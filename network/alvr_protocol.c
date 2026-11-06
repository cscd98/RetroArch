/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#include "alvr_protocol.h"
#include <string.h>
#include <stdio.h>
#include <compat/msvc.h>

void alvr_wire_init(alvr_wire_t *w, void *data, size_t size)
{
   w->data = (uint8_t*)data;
   w->size = size;
   w->pos = 0;
   w->error = false;
   w->legacy = false;
}

uint32_t alvr_wire_u32(const uint8_t *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8
      | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void alvr_wire_store32(uint8_t *p, uint32_t value)
{
   unsigned i;
   for (i = 0; i < 4; i++, value >>= 8)
      p[i] = (uint8_t)value;
}

uint32_t alvr_wire_be32(const uint8_t *p)
{
   return (uint32_t)p[3] | (uint32_t)p[2] << 8
      | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}

void alvr_wire_store_be32(uint8_t *p, uint32_t value)
{
   unsigned i;
   for (i = 0; i < 4; i++, value >>= 8)
      p[3 - i] = (uint8_t)value;
}

static bool alvr_wire_room(alvr_wire_t *w, size_t size)
{
   if (w->error || size > w->size - w->pos)
   {
      w->error = true;
      return false;
   }

   return true;
}

uint64_t alvr_wire_uint(alvr_wire_t *w)
{
   uint64_t value = 0;
   unsigned i, count, tag;
   if (!alvr_wire_room(w, 1))
      return 0;

   tag = w->data[w->pos++];
   if (tag < 251)
      return tag;

   if (tag > 253)
   {
      w->error = true;
      return 0;
   }

   count = 1U << (tag - 250);
   if (!alvr_wire_room(w, count))
      return 0;

   for (i = 0; i < count; i++)
      value |= (uint64_t)w->data[w->pos++] << (8 * i);
   return value;
}

uint64_t alvr_wire_integer(alvr_wire_t *w, unsigned bytes)
{
   uint64_t value = 0;
   unsigned i;
   if (!w->legacy)
      return alvr_wire_uint(w);

   if (bytes > 8 || !alvr_wire_room(w, bytes))
   {
      w->error = true;
      return 0;
   }

   for (i = 0; i < bytes; i++)
      value |= (uint64_t)w->data[w->pos++] << (8 * i);
   return value;
}

void alvr_wire_put_integer(alvr_wire_t *w, uint64_t value, unsigned bytes)
{
   unsigned i;
   if (!w->legacy)
   {
      alvr_wire_put_uint(w, value);
      return;
   }

   if (bytes > 8 || !alvr_wire_room(w, bytes))
   {
      w->error = true;
      return;
   }

   for (i = 0; i < bytes; i++, value >>= 8)
      w->data[w->pos++] = (uint8_t)value;
}

float alvr_wire_float(alvr_wire_t *w)
{
   float value = 0;
   uint32_t bits;
   if (!alvr_wire_room(w, 4))
      return 0;

   bits = alvr_wire_u32(w->data + w->pos);
   w->pos += 4;
   memcpy(&value, &bits, 4);
   return value;
}

void alvr_wire_put_bytes(alvr_wire_t *w, const void *data, size_t size)
{
   if (!alvr_wire_room(w, size))
      return;

   if (size)
      memcpy(w->data + w->pos, data, size);
   w->pos += size;
}

void alvr_wire_put_uint(alvr_wire_t *w, uint64_t value)
{
   unsigned count, i;
   uint8_t bytes[9];
   if (value < 251)
   {
      bytes[0] = (uint8_t)value;
      alvr_wire_put_bytes(w, bytes, 1);
      return;
   }

   count = value <= 65535 ? 2 : (value <= UINT32_MAX ? 4 : 8);
   bytes[0] = (uint8_t)(count == 2 ? 251 : count == 4 ? 252 : 253);
   for (i = 0; i < count; i++, value >>= 8)
      bytes[i + 1] = (uint8_t)value;
   alvr_wire_put_bytes(w, bytes, count + 1);
}

void alvr_wire_put_float(alvr_wire_t *w, float value)
{
   uint32_t bits;
   uint8_t bytes[4];
   memcpy(&bits, &value, 4);
   alvr_wire_store32(bytes, bits);
   alvr_wire_put_bytes(w, bytes, 4);
}

void alvr_wire_put_string(alvr_wire_t *w, const char *value)
{
   size_t size = strlen(value);
   alvr_wire_put_integer(w, size, 8);
   alvr_wire_put_bytes(w, value, size);
}

static void alvr_wire_skip_string(alvr_wire_t *w)
{
   uint64_t size = alvr_wire_integer(w, 8);
   if (size > w->size - w->pos)
      w->error = true;
   else
      w->pos += (size_t)size;
}

bool alvr_protocol_caps(void *data, size_t size, alvr_caps_t *caps, bool legacy)
{
   alvr_wire_t w;
   uint64_t protocol, count, width, height, max_width, max_height, address;
   unsigned i;
   float rate, best = 0;
   alvr_wire_init(&w, data, size);
   w.legacy = legacy;
   if (alvr_wire_integer(&w, 4) != 0)
      return false;

   protocol = alvr_wire_integer(&w, 8);
   if (protocol != (legacy
            ? (((uint64_t)0x17667eafU << 32) | 0xcf6d5d67U)
            : (((uint64_t)0xb513cab3U << 32) | 0xaf770970U)))
      return false;

   alvr_wire_skip_string(&w);
   address = alvr_wire_integer(&w, 4);
   if (address > 1 || !alvr_wire_room(&w, address ? 16 : 4))
      return false;

   w.pos += address ? 16 : 4;
   if (!alvr_wire_room(&w, 1) || w.data[w.pos++] != 1)
      return false;

   width = alvr_wire_integer(&w, 4);
   height = alvr_wire_integer(&w, 4);
   max_width = legacy ? 16384 : alvr_wire_integer(&w, 4);
   max_height = legacy ? 16384 : alvr_wire_integer(&w, 4);
   count = alvr_wire_integer(&w, 8);
   if (!width || !height || width > 16384 || height > 16384
         || max_width < width || max_height < height
         || max_width > 16384 || max_height > 16384 || count > (legacy ? 8192 : 64))
      return false;

   for (i = 0; i < (unsigned)count; i++)
   {
      rate = alvr_wire_float(&w);
      if (rate >= 60 && rate <= 144 && (!best || rate < best))
         best = rate;
   }
   alvr_wire_integer(&w, 4);
   if (!legacy)
   {
      if (!alvr_wire_room(&w, 5))
         return false;

      for (i = 0; i < 5; i++)
         if (w.data[w.pos++] > 1)
            return false;

      alvr_wire_float(&w);
      if (!alvr_wire_room(&w, 1) || w.data[w.pos++] > 1)
         return false;

      alvr_wire_skip_string(&w);
   }
   if (w.error || w.pos != w.size || !best)
      return false;

   caps->width = (unsigned)width;
   caps->height = (unsigned)height;
   caps->max_width = (unsigned)max_width;
   caps->max_height = (unsigned)max_height;
   caps->refresh_rate = best;
   return true;
}

size_t alvr_protocol_config(void *data, size_t capacity,
      unsigned width, unsigned height, float rate, unsigned audio_rate,
      bool legacy)
{
   alvr_wire_t w;
   const char *session =
      "{\"server_version\":\"21.0.0-dev14\",\"session_settings\":{"
      "\"connection\":{\"stream_protocol\":{\"variant\":\"Udp\"},"
      "\"stream_port\":9944,\"packet_size\":1400},"
      "\"audio\":{\"microphone\":{\"enabled\":false}},"
      "\"video\":{\"foveated_encoding\":{\"enabled\":false}}}}";
   uint8_t zero = 0;
   alvr_wire_init(&w, data, capacity);
   w.legacy = legacy;
   if (legacy)
   {
      char negotiated[384];
      /* ALVR 20 adds four bytes to the configured packet size. */
      const char *legacy_session =
         "{\"server_version\":\"20.14.0\",\"session_settings\":{"
         "\"connection\":{\"stream_protocol\":{\"variant\":\"Udp\"},"
         "\"stream_port\":9944,\"packet_size\":1396},"
         "\"audio\":{\"microphone\":{\"enabled\":false}},"
         "\"video\":{\"foveated_encoding\":{\"enabled\":false}}}}";
      snprintf(negotiated, sizeof(negotiated),
            "{\"view_resolution\":[%u,%u],\"refresh_rate_hint\":%u,"
            "\"game_audio_sample_rate\":%u,\"enable_foveated_encoding\":false,"
            "\"use_multimodal_protocol\":false,\"use_full_range\":false,"
            "\"encoding_gamma\":1.0,\"enable_hdr\":false,\"wired\":false}",
            width, height, (unsigned)rate, audio_rate);
      alvr_wire_put_string(&w, legacy_session);
      alvr_wire_put_string(&w, negotiated);
      return w.error ? 0 : w.pos;
   }

   alvr_wire_put_string(&w, session);
   alvr_wire_put_uint(&w, width);
   alvr_wire_put_uint(&w, height);
   alvr_wire_put_float(&w, rate);
   alvr_wire_put_uint(&w, audio_rate);
   alvr_wire_put_bytes(&w, &zero, 1);
   alvr_wire_put_float(&w, 1.0f);
   alvr_wire_put_bytes(&w, &zero, 1);
   alvr_wire_put_bytes(&w, &zero, 1);
   alvr_wire_put_string(&w, "{}");
   return w.error ? 0 : w.pos;
}

size_t alvr_protocol_shard(uint8_t *out, size_t capacity,
      unsigned stream, uint32_t packet, const uint8_t *payload,
      size_t size, unsigned shard, bool legacy)
{
   size_t header = legacy ? 18 : ALVR_SHARD_HEADER_SIZE;
   size_t chunk = ALVR_PACKET_SIZE - header;
   size_t count, offset, length;
   if (!size || size > 16 * 1024 * 1024 || stream > 4)
      return 0;

   count = (size + chunk - 1) / chunk;
   if (shard >= count)
      return 0;

   offset = shard * chunk;
   length = size - offset;
   if (length > chunk)
      length = chunk;
   if (capacity < header + length)
      return 0;

   if (legacy)
   {
      alvr_wire_store_be32(out, (uint32_t)(length + header - 4));
      out[4] = 0;
      out[5] = (uint8_t)stream;
      alvr_wire_store_be32(out + 6, packet);
      alvr_wire_store_be32(out + 10, (uint32_t)count);
      alvr_wire_store_be32(out + 14, shard);
      memcpy(out + header, payload + offset, length);
      return length + header;
   }

   out[0] = (uint8_t)stream;
   out[1] = 0;
   alvr_wire_store32(out + 2, packet);
   alvr_wire_store32(out + 6, (uint32_t)count);
   alvr_wire_store32(out + 10, shard);
   memcpy(out + ALVR_SHARD_HEADER_SIZE, payload + offset, length);
   return length + ALVR_SHARD_HEADER_SIZE;
}

#define ALVR_PATH(hi, lo) (((uint64_t)(hi) << 32) | (lo))

typedef struct alvr_button_map
{
   uint64_t path;
   int button, axis;
   bool menu;
} alvr_button_map_t;

/* ALVR's DefaultHasher path IDs; Touch's combined RetroPad layout. */
static const alvr_button_map_t alvr_button_map[] = {
   { ALVR_PATH(0xc72a0ba6U, 0xca52fba7U), 0, -1, false }, /* right/a/click */
   { ALVR_PATH(0x95504746U, 0x6e34669bU), 8, -1, false }, /* right/b/click */
   { ALVR_PATH(0xb1a13c48U, 0x5685e33cU), 1, -1, false }, /* left/x/click */
   { ALVR_PATH(0x521496d2U, 0x3e3e9883U), 9, -1, false }, /* left/y/click */
   { ALVR_PATH(0x6268a669U, 0xe87cf910U), 3, -1, true }, /* left/menu/click */
   { ALVR_PATH(0xed68cbf3U, 0x84f9f033U), 3, -1, false }, /* right/menu/click */
   { ALVR_PATH(0xefe61b05U, 0xbfc00886U), 2, -1, false }, /* left/back/click */
   { ALVR_PATH(0x01f1b9c6U, 0x89804b82U), 10, -1, false }, /* left/squeeze/value */
   { ALVR_PATH(0xdd5935ccU, 0x005136f1U), 11, -1, false }, /* right/squeeze/value */
   { ALVR_PATH(0x0b66bbe3U, 0xdc2dc49fU), 12, 4, false }, /* left/trigger/value */
   { ALVR_PATH(0xede7e982U, 0xd5eedf21U), 13, 5, false }, /* right/trigger/value */
   { ALVR_PATH(0xdfc8a612U, 0x73cf2569U), 14, -1, false }, /* left/thumbstick/click */
   { ALVR_PATH(0x00ac9a92U, 0xf4453d61U), 15, -1, false }, /* right/thumbstick/click */
   { ALVR_PATH(0xc3d709edU, 0x0d794ea1U), -1, 0, false }, /* left/thumbstick/x */
   { ALVR_PATH(0xcb7d9287U, 0x06263554U), -1, 1, false }, /* left/thumbstick/y */
   { ALVR_PATH(0xd7c23648U, 0xf060c6dfU), -1, 2, false }, /* right/thumbstick/x */
   { ALVR_PATH(0xcee8c338U, 0xe02e0131U), -1, 3, false }, /* right/thumbstick/y */
};

bool alvr_protocol_buttons(alvr_wire_t *w, alvr_input_state_t *state)
{
   alvr_input_state_t next = *state;
   uint64_t count = alvr_wire_integer(w, 8);
   unsigned i;
   if (count > 128)
      return false;
   for (i = 0; i < (unsigned)count; i++)
   {
      uint64_t path = alvr_wire_integer(w, 8);
      uint64_t kind = alvr_wire_integer(w, 4);
      float value;
      unsigned j;
      if (!kind)
      {
         uint64_t binary = alvr_wire_integer(w, 1);
         if (binary > 1)
            return false;
         value = binary ? 1.0f : 0.0f;
      }
      else if (kind == 1)
      {
         value = alvr_wire_float(w);
         if (!(value >= -1.0f && value <= 1.0f))
            return false;
      }
      else
         return false;
      if (w->error)
         return false;
      for (j = 0; j < sizeof(alvr_button_map) / sizeof(alvr_button_map[0]); j++)
      {
         const alvr_button_map_t *map = &alvr_button_map[j];
         if (map->path != path)
            continue;
         if (map->axis >= 0)
         {
            if (!kind)
               return false;
            next.analog[map->axis] = (int16_t)(value * 32767.0f);
            if (map->axis == 1 || map->axis == 3)
               next.analog[map->axis] = -next.analog[map->axis];
         }
         if (map->button >= 0)
         {
            uint16_t bit = (uint16_t)(1u << map->button);
            next.buttons &= (uint16_t)~bit;
            if (value > 0.5f)
               next.buttons |= bit;
         }
         if (map->menu)
            next.menu = value > 0.5f;
         break;
      }
   }
   if (w->error || w->pos != w->size)
      return false;
   *state = next;
   return true;
}
