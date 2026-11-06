/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#include "alvr_connection.h"
#include <net/net_socket.h>
#include <stdlib.h>
#include <string.h>

#define ALVR_MEDIA_LIMIT (4 * 1024 * 1024)
#define ALVR_TRACKING_LIMIT 65536

typedef struct alvr_outgoing
{
   uint8_t *data;
   size_t size;
   uint32_t sequence;
   unsigned shard;
} alvr_outgoing_t;

struct alvr_connection
{
   uint64_t deadline, keepalive;
   uint64_t tracking_shards;
   alvr_receive_t receive;
   void *context;
   uint8_t *input, *output, *tracking;
   size_t input_size, input_needed, output_size, output_sent;
   size_t tracking_size;
   struct sockaddr_in peer;
   alvr_outgoing_t media[2];
   uint32_t tracking_sequence;
   unsigned tracking_count;
   unsigned width, height, audio_rate;
   int control, stream, state;
   bool awaiting_ready, have_tracking, legacy;
};

static void alvr_connection_fail(alvr_connection_t *c)
{
   c->state = ALVR_CONNECTION_ERROR;
   if (c->control >= 0)
      socket_close(c->control);
   if (c->stream >= 0)
      socket_close(c->stream);
   c->control = c->stream = -1;
}

void alvr_connection_free(alvr_connection_t *c)
{
   unsigned i;
   if (!c)
      return;

   alvr_connection_fail(c);
   for (i = 0; i < 2; i++)
      free(c->media[i].data);
   free(c->input);
   free(c->output);
   free(c->tracking);
   free(c);
}

alvr_connection_t *alvr_connection_new(const char *ip,
      unsigned width, unsigned height, unsigned audio_rate,
      uint64_t now, alvr_receive_t receive, void *context)
{
   alvr_connection_t *c;
   int result;
   if (!ip || !*ip || !width || !height
         || width > 4096 || height > 4096 || !network_init())
      return NULL;

   c = (alvr_connection_t*)calloc(1, sizeof(*c));
   if (!c)
      return NULL;

   c->control = c->stream = -1;
   c->peer.sin_family = AF_INET;
   c->peer.sin_port = htons(ALVR_CONTROL_PORT);
   if (inet_pton(AF_INET, ip, &c->peer.sin_addr) != 1)
      goto error;
   c->input = (uint8_t*)malloc(ALVR_CONTROL_LIMIT);
   c->output = (uint8_t*)malloc(ALVR_CONTROL_LIMIT);
   c->tracking = (uint8_t*)malloc(ALVR_TRACKING_LIMIT);
   if (!c->input || !c->output || !c->tracking)
      goto error;
   c->control = socket_create("alvr", SOCKET_DOMAIN_INET,
         SOCKET_TYPE_STREAM, SOCKET_PROTOCOL_TCP);
   if (c->control < 0 || !socket_nonblock(c->control))
      goto error;
#ifndef _WIN32
   if (c->control >= FD_SETSIZE)
      goto error;
#endif
   result = connect(c->control, (struct sockaddr*)&c->peer, sizeof(c->peer));
   if (result < 0 && !isinprogress(result))
      goto error;
   c->state = result == 0 ? ALVR_CONNECTION_NEGOTIATING
      : ALVR_CONNECTION_CONNECTING;
   c->width = width;
   c->height = height;
   c->audio_rate = audio_rate;
   c->input_needed = 4;
   c->deadline = now + 5000000;
   c->keepalive = now;
   c->receive = receive;
   c->context = context;
   return c;

error:
   alvr_connection_free(c);
   return NULL;
}

static bool alvr_connection_queue(alvr_connection_t *c,
      const void *data, size_t size)
{
   size_t remaining = c->output_size - c->output_sent;
   if (size > ALVR_CONTROL_LIMIT - 4 - remaining)
      return false;

   if (c->output_sent && remaining)
      memmove(c->output, c->output + c->output_sent, remaining);
   c->output_sent = 0;
   if (c->legacy)
      alvr_wire_store_be32(c->output + remaining, (uint32_t)size);
   else
      alvr_wire_store32(c->output + remaining, (uint32_t)size);
   memcpy(c->output + remaining + 4, data, size);
   c->output_size = remaining + 4 + size;
   return true;
}

bool alvr_connection_decoder(alvr_connection_t *c,
      const void *headers, size_t size, unsigned codec)
{
   uint8_t *data;
   alvr_wire_t w;
   bool result;
   if (!c || c->state != ALVR_CONNECTION_STREAMING
         || !headers || !size || size > ALVR_CONTROL_LIMIT - 32 || codec > 1)
      return false;

   data = (uint8_t*)malloc(size + 32);
   if (!data)
      return false;

   alvr_wire_init(&w, data, size + 32);
   w.legacy = c->legacy;
   alvr_wire_put_integer(&w, 1, 4);
   alvr_wire_put_integer(&w, codec, 4);
   alvr_wire_put_integer(&w, size, 8);
   alvr_wire_put_bytes(&w, headers, size);
   if (!c->legacy)
      alvr_wire_put_string(&w, "{}");
   result = !w.error && alvr_connection_queue(c, data, w.pos);
   free(data);
   return result;
}

bool alvr_connection_send(alvr_connection_t *c,
      unsigned stream, const void *data, size_t size)
{
   alvr_outgoing_t *out;
   if (!c || c->state != ALVR_CONNECTION_STREAMING
         || stream < 2 || stream > 3 || !data || !size || size > ALVR_MEDIA_LIMIT)
      return false;

   out = &c->media[stream - 2];
   if (out->data)
      return false;

   out->data = (uint8_t*)malloc(size);
   if (!out->data)
      return false;

   memcpy(out->data, data, size);
   out->size = size;
   out->shard = 0;
   return true;
}

static bool alvr_connection_message(alvr_connection_t *c,
      void *data, size_t size, uint64_t now)
{
   alvr_wire_t w;
   uint64_t kind;
   alvr_caps_t caps;
   if (c->state == ALVR_CONNECTION_NEGOTIATING && !c->awaiting_ready)
   {
      uint8_t config[1024], start[4] = {0};
      size_t length;
      if (!alvr_protocol_caps(data, size, &caps, c->legacy)
            || c->width > caps.max_width || c->height > caps.max_height)
         return false;

      length = alvr_protocol_config(config, sizeof(config), c->width,
            c->height, caps.refresh_rate, c->audio_rate, c->legacy);
      if (!length || !alvr_connection_queue(c, config, length)
            || !alvr_connection_queue(c, start, c->legacy ? 4 : 1))
         return false;

      c->awaiting_ready = true;
      c->deadline = now + 5000000;
      return true;
   }

   alvr_wire_init(&w, data, size);
   w.legacy = c->legacy;
   kind = alvr_wire_integer(&w, 4);
   if (w.error || kind > 11)
      return false;

   if (c->state == ALVR_CONNECTION_NEGOTIATING)
   {
      struct sockaddr_in local;
      socklen_t local_size = sizeof(local);
      if (kind != 3 || w.pos != size)
         return false;

      memset(&local, 0, sizeof(local));
      if (getsockname(c->control, (struct sockaddr*)&local, &local_size) < 0
            || local.sin_family != AF_INET)
         return false;

      local.sin_port = htons(ALVR_STREAM_PORT);
      c->stream = socket_create("alvr", SOCKET_DOMAIN_INET,
            SOCKET_TYPE_DATAGRAM, SOCKET_PROTOCOL_UDP);
      if (c->stream < 0 || !socket_nonblock(c->stream)
            || bind(c->stream, (struct sockaddr*)&local, sizeof(local)) < 0)
         return false;

      c->peer.sin_port = htons(ALVR_STREAM_PORT);
      if (connect(c->stream, (struct sockaddr*)&c->peer, sizeof(c->peer)) < 0)
         return false;

      c->state = ALVR_CONNECTION_STREAMING;
      c->deadline = now + 2000000;
   }
   else
   {
      if (kind == 2)
      {
         if (w.pos != size)
            return false;

         c->deadline = now + 2000000;
      }
      if (c->receive)
         c->receive(c->context, 5, data, size, c->legacy);
   }
   return true;
}

static bool alvr_connection_control(alvr_connection_t *c, uint64_t now)
{
   unsigned i;
   for (i = 0; i < 16; i++)
   {
      bool error = false;
      ssize_t received = socket_receive_all_nonblocking(c->control, &error,
            c->input + c->input_size, c->input_needed - c->input_size);
      if (error || received < 0)
         return false;

      c->input_size += (size_t)received;
      if (c->input_size < c->input_needed)
         break;
      if (c->input_needed == 4)
      {
         uint32_t length;
         if (c->state == ALVR_CONNECTION_NEGOTIATING && !c->awaiting_ready)
            c->legacy = alvr_wire_u32(c->input) > ALVR_CONTROL_LIMIT - 4;
         length = c->legacy ? alvr_wire_be32(c->input)
            : alvr_wire_u32(c->input);
         if (!length || length > ALVR_CONTROL_LIMIT - 4)
            return false;

         c->input_needed += length;
      }
      else
      {
         if (!alvr_connection_message(c, c->input + 4,
                  c->input_needed - 4, now))
            return false;

         c->input_size = 0;
         c->input_needed = 4;
      }
   }
   if (c->state == ALVR_CONNECTION_STREAMING && now >= c->keepalive)
   {
      uint8_t keepalive[4] = {3, 0, 0, 0};
      if (!alvr_connection_queue(c, keepalive, c->legacy ? 4 : 1))
         return false;

      c->keepalive = now + 500000;
   }
   if (c->output_size > c->output_sent)
   {
      ssize_t sent = socket_send_all_nonblocking(c->control,
            c->output + c->output_sent, c->output_size - c->output_sent, true);
      if (sent < 0)
         return false;

      c->output_sent += (size_t)sent;
      if (c->output_sent == c->output_size)
         c->output_sent = c->output_size = 0;
   }
   return true;
}

static void alvr_connection_tracking(alvr_connection_t *c,
      const uint8_t *data, size_t size)
{
   uint32_t sequence, count, shard;
   size_t offset, header = c->legacy ? 18 : 14;
   uint64_t bit;
   if (size <= header)
      return;

   if (c->legacy)
   {
      if (alvr_wire_be32(data) != size - 4 || data[4] || data[5])
         return;

      sequence = alvr_wire_be32(data + 6);
      count = alvr_wire_be32(data + 10);
      shard = alvr_wire_be32(data + 14);
   }
   else
   {
      if (data[0] || data[1])
         return;

      sequence = alvr_wire_u32(data + 2);
      count = alvr_wire_u32(data + 6);
      shard = alvr_wire_u32(data + 10);
   }
   if (!count || count > ALVR_TRACKING_LIMIT / (ALVR_PACKET_SIZE - header)
         || shard >= count || (shard + 1 < count && size != ALVR_PACKET_SIZE))
      return;

   if (!c->have_tracking || (sequence != c->tracking_sequence
            && (uint32_t)(sequence - c->tracking_sequence) < 0x80000000U))
   {
      c->have_tracking = true;
      c->tracking_sequence = sequence;
      c->tracking_count = count;
      c->tracking_shards = 0;
      c->tracking_size = 0;
   }
   if (sequence != c->tracking_sequence || count != c->tracking_count)
      return;

   bit = (uint64_t)1 << shard;
   if (c->tracking_shards & bit)
      return;

   offset = shard * (ALVR_PACKET_SIZE - header);
   memcpy(c->tracking + offset, data + header, size - header);
   c->tracking_shards |= bit;
   if (shard + 1 == count)
      c->tracking_size = offset + size - header;
   if (c->tracking_shards == ((uint64_t)1 << count) - 1 && c->receive)
      c->receive(c->context, 0, c->tracking, c->tracking_size, c->legacy);
}

static bool alvr_connection_media(alvr_connection_t *c)
{
   uint8_t packet[ALVR_PACKET_SIZE + 1];
   unsigned i, stream;
   for (i = 0; i < 64; i++)
   {
      int length = recv(c->stream, (char*)packet, sizeof(packet), 0);
      if (length < 0)
      {
         if (!isagain(length))
            return false;

         break;
      }
      if (length <= ALVR_PACKET_SIZE)
         alvr_connection_tracking(c, packet, (size_t)length);
   }
   for (i = 0; i < 64; i++)
   {
      bool pending = false;
      for (stream = 0; stream < 2; stream++)
      {
         size_t size;
         int sent;
         alvr_outgoing_t *out = &c->media[stream];
         if (!out->data)
            continue;
         pending = true;
         size = alvr_protocol_shard(packet, sizeof(packet), stream + 2,
               out->sequence, out->data, out->size, out->shard, c->legacy);
         if (!size)
            return false;

         sent = send(c->stream, (const char*)packet, (int)size, 0);
         if (sent < 0 && isagain(sent))
            return true;

         if (sent != (int)size)
            return false;

         if (++out->shard * (ALVR_PACKET_SIZE - (c->legacy ? 18 : 14)) >= out->size)
         {
            free(out->data);
            out->data = NULL;
            out->size = 0;
            out->sequence++;
         }
      }
      if (!pending)
         break;
   }
   return true;
}

int alvr_connection_poll(alvr_connection_t *c, uint64_t now)
{
   if (!c || c->state == ALVR_CONNECTION_ERROR)
      return ALVR_CONNECTION_ERROR;

   if (now >= c->deadline)
      goto error;
   if (c->state == ALVR_CONNECTION_CONNECTING)
   {
      struct timeval timeout;
      fd_set writefds, errors;
      int result, error = 0;
      socklen_t length = sizeof(error);
      FD_ZERO(&writefds);
      FD_ZERO(&errors);
      FD_SET(c->control, &writefds);
      FD_SET(c->control, &errors);
      timeout.tv_sec = timeout.tv_usec = 0;
      result = socket_select(c->control + 1, NULL, &writefds, &errors, &timeout);
      if (result < 0)
         goto error;
      if (!result)
         return c->state;

      if (getsockopt(c->control, SOL_SOCKET, SO_ERROR,
               (char*)&error, &length) < 0 || error)
         goto error;
      c->state = ALVR_CONNECTION_NEGOTIATING;
   }
   if (!alvr_connection_control(c, now))
      goto error;
   if (c->state == ALVR_CONNECTION_STREAMING && !alvr_connection_media(c))
      goto error;
   return c->state;

error:
   alvr_connection_fail(c);
   return c->state;
}
