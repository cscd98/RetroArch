/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#include "alvr_server.h"
#include "alvr_stream.h"
#include "../configuration.h"
#include "../audio/audio_driver.h"
#include "../gfx/video_driver.h"
#include "../record/record_audio_tap.h"
#include "../runloop.h"
#include "../verbosity.h"
#include <string.h>
#include <stdlib.h>
#include <libretro.h>
#include <features/features_cpu.h>
#include "../record/record_driver.h"
#ifdef HAVE_THREADS
#include "../gfx/video_thread_wrapper.h"
#endif

static alvr_stream_t *alvr_server_stream;
static alvr_input_state_t alvr_server_input;
static unsigned alvr_server_buttons;
static unsigned alvr_server_audio_rate;
static retro_time_t alvr_server_audio_retry;
static bool alvr_server_audio_warned;
static retro_time_t alvr_server_menu_since;
static bool alvr_server_menu_held, alvr_server_menu_toggle;
static record_audio_tap_t *alvr_server_tap;
static bool alvr_server_quitting;
static bool alvr_server_stopping;
static bool alvr_server_tap_installed;
static enum alvr_stream_status alvr_server_state;

static uint8_t *alvr_server_pixels;
static uint32_t *alvr_server_scaled;
static size_t alvr_server_pixel_capacity;

void alvr_server_input_poll(float threshold)
{
   int limit = (int)(threshold * 32767.0f);
   if (alvr_server_stopping)
      memset(&alvr_server_input, 0, sizeof(alvr_server_input));
   else
      alvr_stream_input(alvr_server_stream, &alvr_server_input);
   alvr_server_buttons = alvr_server_input.buttons;
   if (alvr_server_input.analog[0] < -limit)
      alvr_server_buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
   if (alvr_server_input.analog[0] > limit)
      alvr_server_buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
   if (alvr_server_input.analog[1] < -limit)
      alvr_server_buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
   if (alvr_server_input.analog[1] > limit)
      alvr_server_buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
   alvr_server_menu_toggle = false;
   if (alvr_server_input.menu)
   {
      retro_time_t now = cpu_features_get_time_usec();
      if (!alvr_server_menu_held)
         alvr_server_menu_since = now;
      alvr_server_menu_toggle = now - alvr_server_menu_since >= 1000000;
   }
   alvr_server_menu_held = alvr_server_input.menu;
}

unsigned alvr_server_input_buttons(void)
{
   return alvr_server_buttons;
}

int16_t alvr_server_input_analog(unsigned idx, unsigned id)
{
   if (idx < 2 && id < 2)
      return alvr_server_input.analog[idx * 2 + id];
   if (idx == RETRO_DEVICE_INDEX_ANALOG_BUTTON
         && id >= RETRO_DEVICE_ID_JOYPAD_L2 && id <= RETRO_DEVICE_ID_JOYPAD_R2)
      return alvr_server_input.analog[4 + id - RETRO_DEVICE_ID_JOYPAD_L2];
   return 0;
}

bool alvr_server_input_menu(void)
{
   return alvr_server_menu_toggle;
}

bool alvr_server_capture_active(void)
{
   video_driver_state_t *video = video_state_get_ptr();
   if (!alvr_server_stream || alvr_server_stopping
         || recording_state_get_ptr()->enable || !video->data)
      return false;

#ifdef HAVE_THREADS
   if (video->thread_wrapper_active)
      return video_thread_record_has_reader(video->data);

#endif
   return video->poke && video->poke->record_read;
}

static void alvr_server_capture_free(void)
{
   video_driver_state_t *video = video_state_get_ptr();
#ifdef HAVE_THREADS
   if (video->thread_wrapper_active && video->data
         && !recording_state_get_ptr()->enable)
      video_thread_record_stop(video->data);
#endif
   free(alvr_server_pixels);
   free(alvr_server_scaled);
   alvr_server_pixels = NULL;
   alvr_server_scaled = NULL;
   alvr_server_pixel_capacity = 0;
}

static void alvr_server_capture(void)
{
   video_driver_state_t *video = video_state_get_ptr();
   struct video_viewport vp;
   const uint8_t *pixels;
   unsigned width, height, out_width, out_height, x, y;
   size_t size;
   bool bottom_up = false;
   memset(&vp, 0, sizeof(vp));
   if (!video->current_video || !video->current_video->viewport_info)
      return;

   video->current_video->viewport_info(video->data, &vp);
   width = VIDEO_SCALE_W(vp.full_dims);
   height = VIDEO_SCALE_H(vp.full_dims);
   if (!width || !height || width > 8192 || height > 8192
         || (size_t)width * height > 16 * 2048 * 2048)
      return;

   out_width = width;
   out_height = height;
   if (width > 2048 || height > 2048)
   {
      unsigned largest = width > height ? width : height;
      out_width = width * 2048 / largest;
      out_height = height * 2048 / largest;
   }
   if (!out_width || !out_height)
      return;

#ifdef HAVE_THREADS
   if (video->thread_wrapper_active)
   {
      if (video_thread_record_take(video->data,
               VIDEO_SCALE_PACK(out_width, out_height), true, true,
               &pixels, &bottom_up) != 1)
         return;

      width = out_width;
      height = out_height;
   }
   else
#endif
   {
      size = (size_t)width * height * 4;
      if (size > alvr_server_pixel_capacity)
      {
         uint8_t *buffer = (uint8_t*)realloc(alvr_server_pixels, size);
         if (!buffer)
            return;

         alvr_server_pixels = buffer;
         alvr_server_pixel_capacity = size;
      }
      video_context_driver_bind_hw_render(false);
      if (!video->poke->record_read(video->data, alvr_server_pixels,
               true, true, &bottom_up))
      {
         video_context_driver_bind_hw_render(true);
         return;
      }

      video_context_driver_bind_hw_render(true);
      pixels = alvr_server_pixels;
   }
   if (!alvr_server_scaled)
      alvr_server_scaled = (uint32_t*)malloc(2048 * 2048 * 4);
   if (!alvr_server_scaled)
      return;

   for (y = 0; y < out_height; y++)
   {
      unsigned first_y = y * height / out_height;
      unsigned last_y = (y + 1) * height / out_height;
      if (width == out_width && height == out_height)
      {
         unsigned row = bottom_up ? height - 1 - y : y;
         memcpy(alvr_server_scaled + (size_t)y * out_width,
               pixels + (size_t)row * width * 4, (size_t)width * 4);
         continue;
      }
      for (x = 0; x < out_width; x++)
      {
         unsigned first_x = x * width / out_width;
         unsigned last_x = (x + 1) * width / out_width;
         unsigned sx, sy, red = 0, green = 0, blue = 0;
         unsigned count = (last_x - first_x) * (last_y - first_y);
         for (sy = first_y; sy < last_y; sy++)
         {
            unsigned row = bottom_up ? height - 1 - sy : sy;
            const uint32_t *src = (const uint32_t*)pixels + (size_t)row * width;
            for (sx = first_x; sx < last_x; sx++)
            {
               uint32_t pixel = src[sx];
               red += (pixel >> 16) & 255;
               green += (pixel >> 8) & 255;
               blue += pixel & 255;
            }
         }
         alvr_server_scaled[(size_t)y * out_width + x]
            = ((red / count) << 16) | ((green / count) << 8) | (blue / count);
      }
   }
   alvr_stream_video(alvr_server_stream, alvr_server_scaled,
         out_width, out_height, (size_t)out_width * 4,
         RETRO_PIXEL_FORMAT_XRGB8888, 0, (float)out_width / out_height);
}

static int alvr_server_audio(void *context, const int16_t *samples,
      size_t frames, uint64_t timestamp)
{
   (void)timestamp;
   alvr_stream_audio((alvr_stream_t*)context, samples, frames);
   return RECORD_AUDIO_OK;
}

static void alvr_server_video(void *context, const void *pixels,
      unsigned dims, size_t pitch)
{
   if (!pixels)
      return;

   alvr_stream_video((alvr_stream_t*)context, pixels,
         VIDEO_SCALE_W(dims), VIDEO_SCALE_H(dims), pitch,
         video_state_get_ptr()->pix_fmt, video_driver_get_rotation_snapshot(),
         video_driver_get_aspect_ratio());
}

static void alvr_server_message(const char *message)
{
   runloop_msg_queue_push(message, strlen(message), 1, 180, false,
         NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}

void alvr_server_stop(void)
{
   alvr_server_quitting = true;
   if (alvr_server_stream)
   {
      alvr_server_stopping = true;
      alvr_stream_stop(alvr_server_stream);
   }
}

bool alvr_server_pending(void)
{
   return alvr_server_stream != NULL;
}

static void alvr_server_audio_poll(unsigned audio_rate)
{
   if (alvr_server_tap && record_audio_tap_failed(alvr_server_tap))
   {
      if (audio_driver_record_tap_remove(alvr_server_tap)
            && record_audio_tap_free(alvr_server_tap))
      {
         alvr_server_tap = NULL;
         alvr_server_tap_installed = false;
         alvr_server_audio_retry = 0;
      }
   }
   if (!alvr_server_tap && audio_rate == alvr_server_audio_rate
         && (AUDIO_FLAGS_GET(audio_state_get_ptr()) & AUDIO_FLAG_ACTIVE)
         && cpu_features_get_time_usec() >= alvr_server_audio_retry)
   {
      alvr_server_audio_retry = cpu_features_get_time_usec() + 1000000;
      alvr_server_tap = record_audio_tap_new(audio_rate,
            alvr_server_audio, alvr_server_stream);
   }

   if (alvr_server_tap && !alvr_server_tap_installed)
   {
      int result = audio_driver_record_tap_install(alvr_server_tap);
      if (result > 0)
      {
         alvr_server_tap_installed = true;
         alvr_server_audio_warned = false;
      }
      else if (result < 0)
      {
         record_audio_tap_free(alvr_server_tap);
         alvr_server_tap = NULL;
         if (!alvr_server_audio_warned)
            alvr_server_message("ALVR audio capture is unavailable.");
         alvr_server_audio_warned = true;
      }
   }
}

void alvr_server_poll(void)
{
   settings_t *settings = config_get_ptr();
   enum alvr_stream_status status;
   unsigned audio_rate = audio_state_get_ptr()->out_rate;
   if (alvr_server_stream)
   {
      status = alvr_stream_status(alvr_server_stream);
      if (status != alvr_server_state)
      {
         if (status == ALVR_STREAM_CONNECTED)
            alvr_server_message("ALVR headset connected.");
         else if (status == ALVR_STREAM_FAILED)
         {
            RARCH_ERR("[ALVR] Stream failed.\n");
            alvr_server_message("ALVR stream failed. Check the encoder, headset address and client version.");
            settings->bools.alvr_enable = false;
         }
         alvr_server_state = status;
      }
      if (!settings->bools.alvr_enable || alvr_server_quitting
            || status == ALVR_STREAM_FAILED)
      {
         alvr_server_stopping = true;
         alvr_stream_stop(alvr_server_stream);
      }
      if (alvr_server_stopping)
      {
         if (alvr_server_tap
               && !audio_driver_record_tap_remove(alvr_server_tap))
            return;

         record_audio_tap_free(alvr_server_tap);
         alvr_server_tap = NULL;
         alvr_server_tap_installed = false;
         alvr_stream_wake(alvr_server_stream);
         if (!alvr_stream_free(alvr_server_stream))
            return;

         alvr_server_capture_free();
         alvr_server_stream = NULL;
         alvr_server_stopping = false;
         return;
      }

      alvr_server_audio_poll(audio_rate);
      alvr_stream_placement(alvr_server_stream,
            settings->uints.video_openxr_placement,
            settings->floats.video_openxr_distance,
            settings->floats.video_openxr_width,
            video_state_get_ptr()->headset_recenter);
      if (alvr_stream_video_ready(alvr_server_stream))
      {
         if (alvr_server_capture_active())
            alvr_server_capture();
         else
            video_driver_cached_frame_read(alvr_server_stream, alvr_server_video);
      }
      alvr_stream_wake(alvr_server_stream);
      return;
   }

   if (alvr_server_quitting || !settings->bools.alvr_enable)
      return;

   RARCH_LOG("[ALVR] Starting headset connection to: %s.\n",
         settings->arrays.alvr_client_address);
   alvr_server_stream = alvr_stream_new(settings->arrays.alvr_client_address,
         settings->uints.alvr_view_size, settings->uints.alvr_bitrate * 1000000,
         audio_rate ? audio_rate : 48000, settings->uints.alvr_encoder,
         settings->uints.alvr_codec);
   if (!alvr_server_stream)
   {
      RARCH_ERR("[ALVR] Unable to start stream. Check the address, encoder and stream settings.\n");
      settings->bools.alvr_enable = false;
      alvr_server_message("Unable to start ALVR. Check the stream settings and audio output.");
      return;
   }

   alvr_server_state = ALVR_STREAM_STARTING;
   alvr_server_audio_rate = audio_rate ? audio_rate : 48000;
   alvr_server_audio_retry = 0;
   alvr_server_audio_warned = false;
   if (audio_rate)
      alvr_server_tap = record_audio_tap_new(audio_rate,
            alvr_server_audio, alvr_server_stream);
}
