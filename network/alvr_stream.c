/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#include "alvr_stream.h"
#include "alvr_connection.h"
#include "../verbosity.h"
#include "../gfx/video_xr.h"
#include <libretro.h>
#include <retro_atomic.h>
#include <retro_spsc.h>
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include <features/features_cpu.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(HAVE_VULKAN) || defined(HAVE_ALVR_VULKAN)
#include "../gfx/common/vulkan_video_encode.h"
#endif
#ifdef _WIN32
#include "../gfx/common/mf_video_encode.h"
#endif

#define ALVR_SOURCE_LIMIT (16 * 1024 * 1024)

typedef struct alvr_placement
{
   float distance, width;
   unsigned placement, recenter;
} alvr_placement_t;

typedef struct alvr_source
{
   size_t bytes;
   unsigned width, height, format, rotation;
   float aspect;
} alvr_source_t;

struct alvr_stream
{
   uint64_t tracking_seconds, frame_seconds;
   alvr_connection_t *connection;
   void *encoder;
   uint8_t *source, *nv12, *packet;
   retro_spsc_t video, audio, placement_queue, input_queue;
   alvr_input_state_t input;
   alvr_placement_t placement, published;
   video_xr_pose_t anchor;
   uint64_t placement_time;
   unsigned recenter;
   retro_eventcount_t wake;
   retro_atomic_int_t epoch, stopped, done, status;
   unsigned view_size, bitrate, audio_rate, backend, codec;
   unsigned tracking_nanos, frame_nanos;
   float local_views[22], head[7], frame_views[22];
   char ip[64];
   bool anchor_valid, gliding;
   bool tracking, pending, decoder_sent, force_idr, legacy;
   bool connected, received, rejected, submitted, streaming;
   bool input_pending, input_received;
};

static bool alvr_encoder_headers(alvr_stream_t *s, void *data,
      size_t capacity, size_t *size)
{
#if defined(HAVE_VULKAN) || defined(HAVE_ALVR_VULKAN)
   if (s->backend == 1)
      return vulkan_video_encoder_headers((vulkan_video_encoder_t*)s->encoder,
            data, capacity, size);

#endif
#ifdef _WIN32
   if (s->backend == 2)
      return mf_video_encoder_headers((mf_video_encoder_t*)s->encoder,
            data, capacity, size);

#endif
   return false;
}

static bool alvr_encoder_submit(alvr_stream_t *s)
{
#if defined(HAVE_VULKAN) || defined(HAVE_ALVR_VULKAN)
   if (s->backend == 1)
      return vulkan_video_encoder_submit((vulkan_video_encoder_t*)s->encoder,
            s->nv12, s->force_idr);

#endif
#ifdef _WIN32
   if (s->backend == 2)
      return mf_video_encoder_submit((mf_video_encoder_t*)s->encoder,
            s->nv12, s->force_idr);

#endif
   return false;
}

static int alvr_encoder_poll(alvr_stream_t *s, const uint8_t **data,
      size_t *size, bool *keyframe)
{
#if defined(HAVE_VULKAN) || defined(HAVE_ALVR_VULKAN)
   if (s->backend == 1)
      return vulkan_video_encoder_poll((vulkan_video_encoder_t*)s->encoder,
            data, size, keyframe);

#endif
#ifdef _WIN32
   if (s->backend == 2)
      return mf_video_encoder_poll((mf_video_encoder_t*)s->encoder,
            data, size, keyframe);

#endif
   return -1;
}

static bool alvr_encoder_free(alvr_stream_t *s)
{
#if defined(HAVE_VULKAN) || defined(HAVE_ALVR_VULKAN)
   if (s->backend == 1)
      return vulkan_video_encoder_free((vulkan_video_encoder_t*)s->encoder);

#endif
#ifdef _WIN32
   if (s->backend == 2)
      return mf_video_encoder_free((mf_video_encoder_t*)s->encoder);

#endif
   return true;
}

static bool alvr_floats(alvr_wire_t *w, float *values, unsigned count)
{
   unsigned i;
   for (i = 0; i < count; i++)
   {
      values[i] = alvr_wire_float(w);
      if (!(values[i] >= -1000000.0f && values[i] <= 1000000.0f))
         return false;
   }
   return !w->error;
}

static void alvr_receive(void *context, unsigned stream, void *data,
      size_t size, bool legacy)
{
   alvr_stream_t *s = (alvr_stream_t*)context;
   alvr_wire_t w;
   uint64_t kind;
   alvr_wire_init(&w, data, size);
   w.legacy = s->legacy = legacy;
   if (stream == 5)
   {
      kind = alvr_wire_integer(&w, 4);
      if (kind == 1)
         s->force_idr = true;
      else if (kind == 7)
      {
         if (alvr_protocol_buttons(&w, &s->input))
         {
            s->input_pending = true;
            if (!s->input_received)
               RARCH_LOG("[ALVR] Headset controller input received.\n");
            s->input_received = true;
         }
      }
      else if (kind == 4)
      {
         float views[22];
         unsigned i;
         if (legacy)
         {
            float ipd = alvr_wire_float(&w);
            if (!(ipd > 0 && ipd < 0.2f))
               return;

            memset(views, 0, sizeof(views));
            views[3] = views[14] = 1.0f;
            views[4] = -ipd * 0.5f;
            views[15] = ipd * 0.5f;
            if (!alvr_floats(&w, views + 7, 4)
                  || !alvr_floats(&w, views + 18, 4))
               return;
         }
         else if (!alvr_floats(&w, views, 22))
            return;

         if (w.error || w.pos != w.size)
            return;

         for (i = 0; i < 2; i++)
            if (!(views[i * 11 + 7] > -1.55f && views[i * 11 + 7] < 0
                     && views[i * 11 + 8] > 0 && views[i * 11 + 8] < 1.55f
                     && views[i * 11 + 9] > 0 && views[i * 11 + 9] < 1.55f
                     && views[i * 11 + 10] > -1.55f && views[i * 11 + 10] < 0))
               return;

         memcpy(s->local_views, views, sizeof(views));
      }
   }
   else if (stream == 0)
   {
      uint64_t seconds = alvr_wire_integer(&w, 8);
      uint64_t nanos = alvr_wire_integer(&w, 4);
      uint64_t count = alvr_wire_integer(&w, 8);
      unsigned i;
      if (count > 64 || seconds > UINT32_MAX || nanos >= 1000000000)
         return;

      for (i = 0; i < count; i++)
      {
         uint64_t id = alvr_wire_integer(&w, 8);
         float motion[13];
         if (!alvr_floats(&w, motion, 13))
            return;

         if (id == (((uint64_t)0x5b90853dU << 32) | 0xc9202538U))
         {
            float norm = motion[0] * motion[0] + motion[1] * motion[1]
               + motion[2] * motion[2] + motion[3] * motion[3];
            if (!(norm > 0.9f && norm < 1.1f))
               return;

            memcpy(s->head, motion, sizeof(s->head));
            s->tracking_seconds = seconds;
            s->tracking_nanos = (unsigned)nanos;
            if (!s->tracking)
               RARCH_LOG("[ALVR] Headset tracking received.\n");
            s->tracking = true;
         }
      }
   }
}

static void alvr_views(alvr_stream_t *s)
{
   unsigned eye, i;
   const float *q = s->head;
   s->frame_seconds = s->tracking_seconds;
   s->frame_nanos = s->tracking_nanos;
   for (eye = 0; eye < 2; eye++)
   {
      const float *p = s->local_views + eye * 11;
      float *v = s->frame_views + eye * 11;
      float tx = 2 * (q[1] * p[6] - q[2] * p[5]);
      float ty = 2 * (q[2] * p[4] - q[0] * p[6]);
      float tz = 2 * (q[0] * p[5] - q[1] * p[4]);
      v[0] = q[3]*p[0] + q[0]*p[3] + q[1]*p[2] - q[2]*p[1];
      v[1] = q[3]*p[1] - q[0]*p[2] + q[1]*p[3] + q[2]*p[0];
      v[2] = q[3]*p[2] + q[0]*p[1] - q[1]*p[0] + q[2]*p[3];
      v[3] = q[3]*p[3] - q[0]*p[0] - q[1]*p[1] - q[2]*p[2];
      v[4] = q[4] + p[4] + q[3]*tx + q[1]*tz - q[2]*ty;
      v[5] = q[5] + p[5] + q[3]*ty + q[2]*tx - q[0]*tz;
      v[6] = q[6] + p[6] + q[3]*tz + q[0]*ty - q[1]*tx;
      for (i = 7; i < 11; i++)
         v[i] = p[i];
   }
}

static void alvr_pixel(const alvr_stream_t *s, const alvr_source_t *source,
      float u, float v, float du, float dv, int *r, int *g, int *b)
{
   unsigned x, y, x0, x1, y0, y1;
   unsigned red = 0, green = 0, blue = 0, count = 0;
   float t, left, right, top, bottom;
   *r = *g = *b = 0;
   if (u < 0 || u >= 1 || v < 0 || v >= 1)
      return;

   if (source->rotation == 1)
   {
      t = u; u = 1 - v; v = t;
   }
   else if (source->rotation == 2)
   {
      u = 1 - u; v = 1 - v;
   }
   else if (source->rotation == 3)
   {
      t = u; u = v; v = 1 - t;
   }
   if (source->rotation & 1)
   {
      t = du; du = dv; dv = t;
   }
   left   = u - du * 0.5f;
   right  = u + du * 0.5f;
   top    = v - dv * 0.5f;
   bottom = v + dv * 0.5f;
   x0 = left > 0 ? (unsigned)(left * source->width) : 0;
   y0 = top > 0 ? (unsigned)(top * source->height) : 0;
   if (x0 >= source->width) x0 = source->width - 1;
   if (y0 >= source->height) y0 = source->height - 1;
   x1 = right < 1 ? (unsigned)ceil(right * source->width) : source->width;
   y1 = bottom < 1 ? (unsigned)ceil(bottom * source->height) : source->height;
   if (x1 <= x0) x1 = x0 + 1;
   if (y1 <= y0) y1 = y0 + 1;
   for (y = y0; y < y1; y++)
      for (x = x0; x < x1; x++)
      {
         size_t offset = (size_t)y * source->width + x;
         if (source->format == RETRO_PIXEL_FORMAT_XRGB8888)
         {
            uint32_t pixel;
            memcpy(&pixel, s->source + offset * 4, 4);
            *r = (pixel >> 16) & 255;
            *g = (pixel >> 8) & 255;
            *b = pixel & 255;
         }
         else
         {
            uint16_t pixel;
            memcpy(&pixel, s->source + offset * 2, 2);
            *b = (pixel & 31) * 255 / 31;
            if (source->format == RETRO_PIXEL_FORMAT_RGB565)
            {
               *r = (pixel >> 11) * 255 / 31;
               *g = ((pixel >> 5) & 63) * 255 / 63;
            }
            else
            {
               *r = ((pixel >> 10) & 31) * 255 / 31;
               *g = ((pixel >> 5) & 31) * 255 / 31;
            }
         }
         red += *r;
         green += *g;
         blue += *b;
         count++;
      }
   if (count == 1)
      return;

   *r = red / count;
   *g = green / count;
   *b = blue / count;
}

static void alvr_render(alvr_stream_t *s, const alvr_source_t *source)
{
   unsigned eye, x, y, size = s->view_size;
   size_t updates = retro_spsc_read_avail(&s->placement_queue) / sizeof(s->placement);
   float aspect = source->aspect;
   video_xr_pose_t head, at, inverse;
   uint64_t now = (uint64_t)cpu_features_get_time_usec();
   uint8_t *chroma = s->nv12 + (size_t)size * size * 2;
   while (updates--)
      retro_spsc_read(&s->placement_queue, &s->placement, sizeof(s->placement));
   memcpy(&head.orientation, s->head, 4 * sizeof(float));
   memcpy(&head.position, s->head + 4, 3 * sizeof(float));
   if ((!s->anchor_valid || s->recenter != s->placement.recenter)
         && video_xr_anchor_from_head(&head, &at))
   {
      s->anchor = at;
      s->anchor_valid = true;
      s->recenter = s->placement.recenter;
      s->gliding = false;
   }
   at = s->anchor;
   if (s->placement.placement == VIDEO_OPENXR_PLACEMENT_FOLLOW)
      at = head;
   else if (s->placement.placement == VIDEO_OPENXR_PLACEMENT_LAZY
         && video_xr_anchor_from_head(&head, &at))
   {
      s->gliding = video_xr_lazy_follow(&s->anchor, &at,
            s->placement_time && now > s->placement_time
            ? (double)(now - s->placement_time) / 1000000.0 : 0.0,
            s->gliding);
      at = s->anchor;
   }
   s->placement_time = now;
   video_xr_pose_invert(&at, &inverse);
   if (!(aspect > 0.1f && aspect < 10.0f))
      aspect = (float)source->width / source->height;
   memset(s->nv12, 16, (size_t)size * size * 2);
   memset(chroma, 128, (size_t)size * size);
   for (eye = 0; eye < 2; eye++)
   {
      const float *view = s->frame_views + eye * 11;
      video_xr_pose_t pose, relative;
      video_xr_vec3_t right_axis, up_axis, forward, ray;
      float left = (float)tan(view[7]), right = (float)tan(view[8]);
      float top = (float)tan(view[9]), bottom = (float)tan(view[10]);
      float dx = (right - left) / size;
      float dy = (bottom - top) / size;
      float width = s->placement.width;
      float height = width / aspect;
      float depth;
      unsigned first_x = 0, first_y = 0, last_x = size, last_y = size;
      memcpy(&pose.orientation, view, 4 * sizeof(float));
      memcpy(&pose.position, view + 4, 3 * sizeof(float));
      video_xr_compose(&inverse, &pose, &relative);
      right_axis.x = 1; right_axis.y = right_axis.z = 0;
      up_axis.y = 1; up_axis.x = up_axis.z = 0;
      forward.z = -1; forward.x = forward.y = 0;
      video_xr_rotate(&relative.orientation, &right_axis, &right_axis);
      video_xr_rotate(&relative.orientation, &up_axis, &up_axis);
      video_xr_rotate(&relative.orientation, &forward, &forward);
      depth = -s->placement.distance - relative.position.z;
      {
         video_xr_pose_t eye_pose;
         float min_x = (float)size, min_y = (float)size;
         float max_x = 0, max_y = 0;
         unsigned corner;
         video_xr_pose_invert(&relative, &eye_pose);
         for (corner = 0; corner < 4; corner++)
         {
            video_xr_vec3_t point;
            float px, py;
            point.x = (corner & 1) ? width * 0.5f : -width * 0.5f;
            point.y = (corner & 2) ? height * 0.5f : -height * 0.5f;
            point.z = -s->placement.distance;
            video_xr_rotate(&eye_pose.orientation, &point, &point);
            point.x += eye_pose.position.x;
            point.y += eye_pose.position.y;
            point.z += eye_pose.position.z;
            if (point.z >= -0.0001f)
               break;
            px = (point.x / -point.z - left) / dx;
            py = (point.y / -point.z - top) / dy;
            if (px < min_x) min_x = px;
            if (px > max_x) max_x = px;
            if (py < min_y) min_y = py;
            if (py > max_y) max_y = py;
         }
         if (corner == 4)
         {
            if (max_x < 0 || max_y < 0 || min_x >= size || min_y >= size)
               continue;
            first_x = min_x > 1 ? (unsigned)min_x - 1 : 0;
            first_y = min_y > 1 ? (unsigned)min_y - 1 : 0;
            last_x = max_x < size - 1 ? (unsigned)max_x + 2 : size;
            last_y = max_y < size - 1 ? (unsigned)max_y + 2 : size;
         }
      }
      for (y = first_y; y < last_y; y++)
         for (x = first_x; x < last_x; x++)
         {
            int r = 0, g = 0, b = 0;
            float rx = left + dx * (x + 0.5f);
            float ry = top + dy * (y + 0.5f);
            size_t offset = (size_t)y * size * 2 + eye * size + x;
            ray.x = forward.x + right_axis.x * rx + up_axis.x * ry;
            ray.y = forward.y + right_axis.y * rx + up_axis.y * ry;
            ray.z = forward.z + right_axis.z * rx + up_axis.z * ry;
            if (ray.z < -0.0001f && depth < 0)
            {
               float t = depth / ray.z;
               float u = 0.5f + (relative.position.x + t * ray.x) / width;
               float v = 0.5f - (relative.position.y + t * ray.y) / height;
               float du = (float)fabs(t * dx / width);
               float dv = (float)fabs(t * dy / height);
               alvr_pixel(s, source, u, v, du, dv, &r, &g, &b);
            }
            s->nv12[offset] = (uint8_t)(16 + (47*r + 157*g + 16*b + 128) / 256);
            if (!(x & 1) && !(y & 1))
            {
               offset = (size_t)(y / 2) * size * 2 + eye * size + x;
               chroma[offset] = (uint8_t)(128 + (-26*r - 87*g + 112*b) / 256);
               chroma[offset + 1] = (uint8_t)(128 + (112*r - 102*g - 10*b) / 256);
            }
         }
   }
}

static bool alvr_stream_step(alvr_stream_t *s)
{
   int state, result;
   uint64_t now = (uint64_t)cpu_features_get_time_usec();
   const uint8_t *data;
   size_t size;
   bool keyframe;
   if (!s->connection)
      s->connection = alvr_connection_new(s->ip, s->view_size, s->view_size,
            s->audio_rate, now, alvr_receive, s);
   if (!s->connection)
   {
      RARCH_ERR("[ALVR] Unable to connect to headset at: %s.\n", s->ip);
      return false;
   }

   state = alvr_connection_poll(s->connection, now);
   if (state == ALVR_CONNECTION_ERROR)
   {
      RARCH_ERR("[ALVR] Headset connection failed or timed out: %s.\n", s->ip);
      return false;
   }

   if (state != ALVR_CONNECTION_STREAMING)
      return true;

   if (s->input_pending
         && retro_spsc_write_avail(&s->input_queue) >= sizeof(s->input))
   {
      retro_spsc_write(&s->input_queue, &s->input, sizeof(s->input));
      s->input_pending = false;
   }
   if (!s->connected)
   {
      RARCH_LOG("[ALVR] Headset connected at: %s.\n", s->ip);
      s->connected = true;
      retro_atomic_store_release_int(&s->status, ALVR_STREAM_CONNECTED);
   }
   if (!s->decoder_sent)
   {
      uint8_t headers[2048];
      if (alvr_encoder_headers(s, headers, sizeof(headers), &size))
      {
         s->decoder_sent = alvr_connection_decoder(s->connection, headers, size, s->codec == 2 ? 1 : 0);
         if (s->decoder_sent)
            RARCH_LOG("[ALVR] %s decoder configuration queued (%u bytes).\n",
                  s->codec == 2 ? "H.265" : "H.264", (unsigned)size);
      }
   }
   result = alvr_encoder_poll(s, &data, &size, &keyframe);
   if (result < 0)
   {
      RARCH_ERR("[ALVR] Video encoder output failed.\n");
      return false;
   }

   if (result)
   {
      alvr_wire_t w;
      unsigned i;
      uint8_t byte;
      if (size > ALVR_SOURCE_LIMIT - 256)
         return false;

      alvr_wire_init(&w, s->packet, ALVR_SOURCE_LIMIT);
      w.legacy = s->legacy;
      alvr_wire_put_integer(&w, s->frame_seconds, 8);
      alvr_wire_put_integer(&w, s->frame_nanos, 4);
      if (!s->legacy)
      {
         for (i = 0; i < 22; i++)
            alvr_wire_put_float(&w, s->frame_views[i]);
         byte = 0;
         alvr_wire_put_bytes(&w, &byte, 1);
      }
      byte = keyframe ? 1 : 0;
      alvr_wire_put_bytes(&w, &byte, 1);
      alvr_wire_put_bytes(&w, data, size);
      if (!s->decoder_sent || w.error
            || !alvr_connection_send(s->connection, 3, s->packet, w.pos))
         s->force_idr = true;
      else if (!s->streaming)
      {
         RARCH_LOG("[ALVR] Streaming started: %s, %ux%u, 60 FPS, %u Mbps.\n",
               s->codec == 2 ? "H.265" : "H.264",
               s->view_size * 2, s->view_size, s->bitrate / 1000000);
         s->streaming = true;
      }
      s->pending = false;
   }
   if (!s->pending && s->tracking)
   {
      alvr_source_t source;
      if (retro_spsc_peek(&s->video, &source, sizeof(source)) == sizeof(source)
            && retro_spsc_read_avail(&s->video) >= sizeof(source) + source.bytes)
      {
         retro_spsc_read(&s->video, &source, sizeof(source));
         retro_spsc_read(&s->video, s->source, source.bytes);
         if (!s->received)
         {
            RARCH_LOG("[ALVR] First captured frame received (%ux%u source).\n",
                  source.width, source.height);
            s->received = true;
         }
         alvr_views(s);
         alvr_render(s, &source);
         if (alvr_encoder_submit(s))
         {
            if (!s->submitted)
            {
               RARCH_LOG("[ALVR] First video frame submitted (%ux%u source).\n",
                     source.width, source.height);
               s->submitted = true;
            }
            s->pending = true;
            s->force_idr = false;
         }
         else if (!s->submitted && !s->rejected)
         {
            RARCH_WARN("[ALVR] Encoder has not accepted its first frame; retrying as capture arrives.\n");
            s->rejected = true;
         }
      }
   }
   size = retro_spsc_read_avail(&s->audio);
   if (size > 8192)
      size = 8192;
   size &= ~(size_t)3;
   if (size)
   {
      retro_spsc_peek(&s->audio, s->packet, size);
      if (alvr_connection_send(s->connection, 2, s->packet, size))
         retro_spsc_skip(&s->audio, size);
   }
   return true;
}

static void alvr_stream_run(void *context)
{
   alvr_stream_t *s = (alvr_stream_t*)context;
   int last, key;
   bool failed = false;
#ifdef _WIN32
   if (!s->backend || s->backend == 2)
   {
      if (s->codec != 1)
      {
         s->encoder = mf_video_encoder_new(s->view_size * 2, s->view_size,
               s->bitrate, 60, true);
         if (s->encoder)
            s->codec = 2;
         else if (!s->codec)
            RARCH_LOG("[ALVR] H.265 encoder unavailable; trying H.264.\n");
      }
      if (!s->encoder && s->codec != 2)
      {
         s->encoder = mf_video_encoder_new(s->view_size * 2, s->view_size,
               s->bitrate, 60, false);
         if (s->encoder)
            s->codec = 1;
      }
      if (s->encoder)
         s->backend = 2;
   }
#endif
#if defined(HAVE_VULKAN) || defined(HAVE_ALVR_VULKAN)
   if (!s->encoder && (!s->backend || s->backend == 1))
   {
      if (s->codec != 1)
      {
         s->encoder = vulkan_video_encoder_new(s->view_size * 2, s->view_size,
               s->bitrate, 60, true);
         if (s->encoder)
            s->codec = 2;
         else if (!s->codec)
            RARCH_LOG("[ALVR] Vulkan H.265 unavailable; trying H.264.\n");
      }
      if (!s->encoder && s->codec != 2)
      {
         s->encoder = vulkan_video_encoder_new(s->view_size * 2, s->view_size,
               s->bitrate, 60, false);
         if (s->encoder)
            s->codec = 1;
      }
      if (s->encoder)
         s->backend = 1;
   }
#endif
   failed = s->encoder == NULL;
   if (failed)
      RARCH_ERR("[ALVR] Unable to initialize video encoder (backend %u).\n",
            s->backend);
   else
      RARCH_LOG("[ALVR] Video encoder ready: %s, %s. Connecting to %s.\n",
            s->backend == 2 ? "Media Foundation" : "Vulkan Video",
            s->codec == 2 ? "H.265" : "H.264", s->ip);
   retro_atomic_store_release_int(&s->status, failed
         ? ALVR_STREAM_FAILED : ALVR_STREAM_CONNECTING);
   for (;;)
   {
      last = retro_atomic_load_acquire_int(&s->epoch);
      if (!failed && !retro_atomic_load_acquire_int(&s->stopped)
            && !alvr_stream_step(s))
      {
         failed = true;
         retro_atomic_store_release_int(&s->status, ALVR_STREAM_FAILED);
      }
      if (failed || retro_atomic_load_acquire_int(&s->stopped))
      {
         if (alvr_encoder_free(s))
            break;
      }
      key = retro_eventcount_prepare_wait(&s->wake);
      if (retro_atomic_load_acquire_int(&s->epoch) != last)
         retro_eventcount_cancel_wait(&s->wake);
      else
         retro_eventcount_commit_wait_timeout(&s->wake, key,
               s->pending ? 1000 : 5000);
   }
   alvr_connection_free(s->connection);
   RARCH_LOG("[ALVR] Stream %s (tracking: %s, video input: %s, decoder: %s, video output: %s).\n",
         failed ? "failed" : "stopped", s->tracking ? "yes" : "no",
         s->submitted ? "yes" : "no", s->decoder_sent ? "yes" : "no",
         s->streaming ? "yes" : "no");
   if (!failed)
      retro_atomic_store_release_int(&s->status, ALVR_STREAM_STOPPED);
   retro_atomic_store_release_int(&s->done, 1);
}

void alvr_stream_input(alvr_stream_t *s, alvr_input_state_t *state)
{
   size_t count;
   if (!s || alvr_stream_status(s) != ALVR_STREAM_CONNECTED)
   {
      memset(state, 0, sizeof(*state));
      return;
   }
   count = retro_spsc_read_avail(&s->input_queue) / sizeof(*state);
   while (count--)
      retro_spsc_read(&s->input_queue, state, sizeof(*state));
}

void alvr_stream_placement(alvr_stream_t *s, unsigned placement,
      float distance, float width, unsigned recenter)
{
   alvr_placement_t p;
   p.placement = placement;
   p.distance = distance;
   p.width = width;
   p.recenter = recenter;
   if (memcmp(&p, &s->published, sizeof(p))
         && retro_spsc_write_avail(&s->placement_queue) >= sizeof(p))
   {
      retro_spsc_write(&s->placement_queue, &p, sizeof(p));
      s->published = p;
   }
}

void alvr_stream_wake(alvr_stream_t *s)
{
   if (!s)
      return;

   retro_atomic_fetch_add_int(&s->epoch, 1);
   retro_eventcount_notify(&s->wake);
}

void alvr_stream_stop(alvr_stream_t *s)
{
   if (!s)
      return;

   retro_atomic_store_release_int(&s->stopped, 1);
   alvr_stream_wake(s);
}

enum alvr_stream_status alvr_stream_status(const alvr_stream_t *s)
{
   return s ? (enum alvr_stream_status)retro_atomic_load_acquire_int(&s->status)
      : ALVR_STREAM_STOPPED;
}

bool alvr_stream_free(alvr_stream_t *s)
{
   if (!s)
      return true;

   if (!retro_atomic_load_acquire_int(&s->done))
      return false;

   retro_eventcount_free(&s->wake);
   retro_spsc_free(&s->video);
   retro_spsc_free(&s->audio);
   retro_spsc_free(&s->placement_queue);
   retro_spsc_free(&s->input_queue);
   free(s->source);
   free(s->nv12);
   free(s->packet);
   free(s);
   return true;
}

alvr_stream_t *alvr_stream_new(const char *ip, unsigned view_size,
      unsigned bitrate, unsigned audio_rate, unsigned backend, unsigned codec)
{
   alvr_stream_t *s;
   sthread_t *thread;
   unsigned eye;
   size_t length;
   if (!ip || !(length = strlen(ip)) || length >= 64
         || view_size < 256 || view_size > 2048 || (view_size & 15)
         || bitrate < 1000000 || bitrate > 100000000
         || audio_rate < 8000 || audio_rate > 192000 || backend > 2 || codec > 2)
      return NULL;

   s = (alvr_stream_t*)calloc(1, sizeof(*s));
   if (!s)
      return NULL;

   memcpy(s->ip, ip, length + 1);
   s->view_size = view_size;
   s->bitrate = bitrate;
   s->audio_rate = audio_rate;
   s->backend = backend;
   s->codec = codec;
   s->placement.distance = 1.8f;
   s->placement.width = 1.6f;
   s->force_idr = true;
   for (eye = 0; eye < 2; eye++)
   {
      float *view = s->local_views + eye * 11;
      view[3] = 1;
      view[4] = eye ? 0.032f : -0.032f;
      view[7] = view[10] = -1;
      view[8] = view[9] = 1;
   }
   retro_atomic_int_init(&s->epoch, 0);
   retro_atomic_int_init(&s->done, 0);
   retro_atomic_int_init(&s->stopped, 0);
   retro_atomic_int_init(&s->status, ALVR_STREAM_STARTING);
   s->source = (uint8_t*)malloc(ALVR_SOURCE_LIMIT);
   s->packet = (uint8_t*)malloc(ALVR_SOURCE_LIMIT);
   s->nv12 = (uint8_t*)malloc((size_t)view_size * view_size * 3);
   if (!s->source || !s->packet || !s->nv12
         || !retro_spsc_init(&s->video, ALVR_SOURCE_LIMIT * 2)
         || !retro_spsc_init(&s->audio, 65536)
         || !retro_spsc_init(&s->placement_queue, 1024)
         || !retro_spsc_init(&s->input_queue, 4096)
         || !retro_eventcount_init(&s->wake))
      goto error;
   thread = sthread_create(alvr_stream_run, s);
   if (!thread)
      goto error;
   sthread_detach(thread);
   return s;

error:
   retro_atomic_store_release_int(&s->done, 1);
   alvr_stream_free(s);
   return NULL;
}

bool alvr_stream_video_ready(const alvr_stream_t *s)
{
   return s && alvr_stream_status(s) == ALVR_STREAM_CONNECTED
      && retro_spsc_write_avail(&s->video) == s->video.capacity;
}

void alvr_stream_video(alvr_stream_t *s, const void *pixels,
      unsigned width, unsigned height, size_t pitch, unsigned pixel_format,
      unsigned rotation, float aspect)
{
   alvr_source_t source;
   unsigned y, bytes = pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
   size_t row = (size_t)width * bytes;
   if (!s || !pixels || pixel_format > RETRO_PIXEL_FORMAT_RGB565
         || !width || !height || width > 2048 || height > 2048
         || pitch < row || row * height > ALVR_SOURCE_LIMIT
         || !alvr_stream_video_ready(s))
      return;

   source.bytes = row * height;
   source.width = width;
   source.height = height;
   source.format = pixel_format;
   source.rotation = rotation & 3;
   source.aspect = aspect;
   retro_spsc_write(&s->video, &source, sizeof(source));
   for (y = 0; y < height; y++)
      retro_spsc_write(&s->video, (const uint8_t*)pixels + y * pitch, row);
   alvr_stream_wake(s);
}

void alvr_stream_audio(alvr_stream_t *s, const int16_t *samples, size_t frames)
{
   size_t bytes;
   if (!s || !samples || frames > 16384
         || alvr_stream_status(s) != ALVR_STREAM_CONNECTED)
      return;

   bytes = frames * 4;
   if (retro_spsc_write_avail(&s->audio) >= bytes)
      retro_spsc_write(&s->audio, samples, bytes);
   alvr_stream_wake(s);
}
