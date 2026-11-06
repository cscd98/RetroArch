/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#include "../../../network/alvr_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t accepted_20[] = {
   0x00, 0x00, 0x00, 0x00, 0x67, 0x5d, 0x6d, 0xcf, 0xaf, 0x7e, 0x66, 0x17,
   0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x51, 0x75, 0x65, 0x73,
   0x74, 0x00, 0x00, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x01, 0x01, 0x00, 0x08,
   0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x90, 0x42, 0x00, 0x00, 0xb4, 0x42, 0x00, 0x00,
   0xf6, 0xc2, 0x00, 0x00, 0xfa, 0xc2, 0x80, 0xbb, 0x00, 0x00,
};

static void integers(void)
{
   static const uint8_t expected[] = {
      0, 250, 251, 251, 0, 251, 255, 255,
      252, 0, 0, 1, 0, 252, 255, 255, 255, 255,
      253, 0, 0, 0, 0, 1, 0, 0, 0
   };
   uint64_t values[7];
   uint8_t bytes[64];
   alvr_wire_t w;
   unsigned i;
   size_t length;
   values[0] = 0; values[1] = 250; values[2] = 251;
   values[3] = 65535; values[4] = 65536; values[5] = UINT32_MAX;
   values[6] = (uint64_t)UINT32_MAX + 1;
   alvr_wire_init(&w, bytes, sizeof(bytes));
   for (i = 0; i < 7; i++)
      alvr_wire_put_uint(&w, values[i]);
   assert(!w.error && w.pos == sizeof(expected));
   assert(!memcmp(bytes, expected, sizeof(expected)));
   length = w.pos;
   alvr_wire_init(&w, bytes, length);
   for (i = 0; i < 7; i++)
      assert(alvr_wire_uint(&w) == values[i]);
   assert(!w.error && w.pos == length);
   alvr_wire_uint(&w);
   assert(w.error);
   for (i = 0; i < 8; i++)
   {
      bytes[0] = 253;
      alvr_wire_init(&w, bytes, i + 1);
      alvr_wire_uint(&w);
      assert(w.error);
   }
   bytes[0] = 254;
   alvr_wire_init(&w, bytes, sizeof(bytes));
   alvr_wire_uint(&w);
   assert(w.error);
   alvr_wire_init(&w, bytes, 0);
   alvr_wire_put_uint(&w, 0);
   assert(w.error && !w.pos);
}

static void capabilities(void)
{
   /* ConnectionAccepted, bincode 2 standard encoding, 21-dev14. */
   uint8_t packet[] = {
      0, 253, 0x70, 0x09, 0x77, 0xaf, 0xb3, 0xca, 0x13, 0xb5,
      5, 'Q', 'u', 'e', 's', 't',
      0, 127, 0, 0, 1, 1,
      251, 0, 8, 251, 0, 8,
      251, 0, 16, 251, 0, 16,
      2, 0, 0, 0x90, 0x42, 0, 0, 0xb4, 0x42,
      251, 0x80, 0xbb,
      1, 1, 1, 1, 0, 0, 0, 0x80, 0x3f, 0,
      2, '{', '}'
   };
   alvr_caps_t caps;
   size_t i;
   assert(alvr_protocol_caps(packet, sizeof(packet), &caps, false));
   assert(caps.width == 2048 && caps.height == 2048);
   assert(caps.refresh_rate == 72.0f);
   assert(caps.max_width == 4096 && caps.max_height == 4096);
   packet[30] = 4;
   assert(!alvr_protocol_caps(packet, sizeof(packet), &caps, false));
   packet[30] = 16;
   for (i = 0; i < sizeof(packet); i++)
      assert(!alvr_protocol_caps(packet, i, &caps, false));
   packet[2] ^= 1;
   assert(!alvr_protocol_caps(packet, sizeof(packet), &caps, false));
   packet[2] ^= 1;
   packet[0] = 1;
   assert(!alvr_protocol_caps(packet, sizeof(packet), &caps, false));
}

static void legacy_protocol(void)
{
   alvr_caps_t caps;
   uint8_t packet[1024];
   alvr_wire_t w;
   size_t size, i;
   uint64_t length;
   assert(alvr_protocol_caps((void*)accepted_20, sizeof(accepted_20), &caps, true));
   assert(caps.width == 2048 && caps.refresh_rate == 72);
   assert(!alvr_protocol_caps((void*)accepted_20, sizeof(accepted_20), &caps, false));
   for (i = 0; i < sizeof(accepted_20); i++)
      assert(!alvr_protocol_caps((void*)accepted_20, i, &caps, true));
   size = alvr_protocol_config(packet, sizeof(packet), 512, 512, 72, 48000, true);
   assert(size);
   alvr_wire_init(&w, packet, size);
   w.legacy = true;
   length = alvr_wire_integer(&w, 8);
   assert(length < size - w.pos);
   assert(!memcmp(packet + w.pos, "{\"server_version\":\"20.14.0\"", 26));
   w.pos += (size_t)length;
   length = alvr_wire_integer(&w, 8);
   assert(length == size - w.pos);
   assert(!memcmp(packet + w.pos, "{\"view_resolution\":[512,512]", 28));
   for (i = 0; i < size; i++)
      assert(!alvr_protocol_config(packet, i, 512, 512, 72, 48000, true));
}

static void shards(void)
{
   static uint8_t payload[4000], restored[4000];
   uint8_t packet[ALVR_PACKET_SIZE];
   size_t size, offset = 0;
   unsigned i;
   for (i = 0; i < sizeof(payload); i++)
      payload[i] = (uint8_t)i;
   for (i = 0; i < 3; i++)
   {
      size = alvr_protocol_shard(packet, sizeof(packet), 3,
            UINT32_MAX, payload, sizeof(payload), i, false);
      assert(size > 14 && size <= ALVR_PACKET_SIZE);
      assert(packet[0] == 3 && packet[1] == 0);
      assert(alvr_wire_u32(packet + 2) == UINT32_MAX);
      assert(alvr_wire_u32(packet + 6) == 3);
      assert(alvr_wire_u32(packet + 10) == i);
      memcpy(restored + offset, packet + 14, size - 14);
      offset += size - 14;
   }
   assert(offset == sizeof(payload));
   assert(!memcmp(payload, restored, sizeof(payload)));
   assert(!alvr_protocol_shard(packet, sizeof(packet), 3, 0,
            payload, sizeof(payload), 3, false));
   assert(!alvr_protocol_shard(packet, 14, 3, 0,
            payload, sizeof(payload), 0, false));
   assert(!alvr_protocol_shard(packet, sizeof(packet), 3, 0,
            payload, 0, 0, false));
}

static void configuration(void)
{
   uint8_t bytes[1024];
   size_t size, i;
   uint64_t json_size;
   alvr_wire_t w;
   size = alvr_protocol_config(bytes, sizeof(bytes), 1024, 1024, 72, 48000, false);
   assert(size);
   alvr_wire_init(&w, bytes, size);
   json_size = alvr_wire_uint(&w);
   assert(json_size < w.size - w.pos);
   assert(bytes[w.pos] == '{' && bytes[w.pos + (size_t)json_size - 1] == '}');
   w.pos += (size_t)json_size;
   assert(alvr_wire_uint(&w) == 1024);
   assert(alvr_wire_uint(&w) == 1024);
   assert(alvr_wire_float(&w) == 72);
   assert(alvr_wire_uint(&w) == 48000);
   assert(bytes[w.pos++] == 0);
   assert(alvr_wire_float(&w) == 1);
   assert(bytes[w.pos++] == 0 && bytes[w.pos++] == 0);
   assert(alvr_wire_uint(&w) == 2);
   assert(bytes[w.pos++] == '{' && bytes[w.pos++] == '}');
   assert(!w.error && w.pos == w.size);
   for (i = 0; i < size; i++)
      assert(!alvr_protocol_config(bytes, i, 1024, 1024, 72, 48000, false));
}

static void buttons(void)
{
   static const uint8_t touch_a20[] = {
      1,0,0,0,0,0,0,0, 0xa7,0xfb,0x52,0xca,0xa6,0x0b,0x2a,0xc7, 0,0,0,0, 1
   };
   static const uint8_t touch_a21[] = {
      1, 253,0xa7,0xfb,0x52,0xca,0xa6,0x0b,0x2a,0xc7, 0, 1
   };
   uint8_t packet[128];
   unsigned legacy;
   for (legacy = 0; legacy < 2; legacy++)
   {
      alvr_wire_t w;
      alvr_input_state_t state;
      size_t size, cut;
      const uint8_t *fixture = legacy ? touch_a20 : touch_a21;
      size_t length = legacy ? sizeof(touch_a20) : sizeof(touch_a21);
      memset(&state, 0, sizeof(state));
      alvr_wire_init(&w, (void*)fixture, length);
      w.legacy = legacy != 0;
      assert(alvr_protocol_buttons(&w, &state));
      assert(state.buttons == 1);
      alvr_wire_init(&w, packet, sizeof(packet));
      w.legacy = legacy != 0;
      alvr_wire_put_integer(&w, 3, 8);
      alvr_wire_put_integer(&w, ((uint64_t)0xc3d709edU << 32) | 0x0d794ea1U, 8);
      alvr_wire_put_integer(&w, 1, 4);
      alvr_wire_put_float(&w, 0.75f);
      alvr_wire_put_integer(&w, ((uint64_t)0xcb7d9287U << 32) | 0x06263554U, 8);
      alvr_wire_put_integer(&w, 1, 4);
      alvr_wire_put_float(&w, 0.5f);
      alvr_wire_put_integer(&w, ((uint64_t)0xede7e982U << 32) | 0xd5eedf21U, 8);
      alvr_wire_put_integer(&w, 1, 4);
      alvr_wire_put_float(&w, 0.8f);
      assert(!w.error);
      size = w.pos;
      for (cut = 0; cut < size; cut++)
      {
         alvr_wire_init(&w, packet, cut);
         w.legacy = legacy != 0;
         assert(!alvr_protocol_buttons(&w, &state));
         assert(state.buttons == 1 && !state.analog[0] && !state.analog[5]);
      }
      alvr_wire_init(&w, packet, size);
      w.legacy = legacy != 0;
      assert(alvr_protocol_buttons(&w, &state));
      assert(state.analog[0] == 24575 && state.analog[1] == -16383);
      assert(state.analog[5] == 26213);
      assert(state.buttons == ((1u << 13) | 1));
      alvr_wire_init(&w, packet, sizeof(packet));
      w.legacy = legacy != 0;
      alvr_wire_put_integer(&w, 1, 8);
      alvr_wire_put_integer(&w, ((uint64_t)0xc72a0ba6U << 32) | 0xca52fba7U, 8);
      alvr_wire_put_integer(&w, 0, 4);
      alvr_wire_put_integer(&w, 0, 1);
      size = w.pos;
      alvr_wire_init(&w, packet, size);
      w.legacy = legacy != 0;
      assert(alvr_protocol_buttons(&w, &state));
      assert(state.buttons == (1u << 13));
      alvr_wire_init(&w, packet, sizeof(packet));
      w.legacy = legacy != 0;
      alvr_wire_put_integer(&w, 129, 8);
      size = w.pos;
      alvr_wire_init(&w, packet, size);
      w.legacy = legacy != 0;
      assert(!alvr_protocol_buttons(&w, &state));
   }
}

int main(void)
{
   buttons();
   integers();
   capabilities();
   legacy_protocol();
   shards();
   configuration();
   puts("ALVR protocol tests passed");
   return 0;
}
