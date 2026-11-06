/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RARCH_ALVR_PROTOCOL_H
#define RARCH_ALVR_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>
RETRO_BEGIN_DECLS

#define ALVR_CONTROL_PORT 9943
#define ALVR_STREAM_PORT 9944
#define ALVR_PACKET_SIZE 1400
#define ALVR_SHARD_HEADER_SIZE 14
#define ALVR_CONTROL_LIMIT 65536

typedef struct alvr_wire
{
   uint8_t *data;
   size_t size;
   size_t pos;
   bool error, legacy;
} alvr_wire_t;

typedef struct alvr_caps
{
   unsigned width;
   unsigned height;
   unsigned max_width;
   unsigned max_height;
   float refresh_rate;
} alvr_caps_t;

typedef struct alvr_input_state
{
   int16_t analog[6];
   uint16_t buttons;
   bool menu;
} alvr_input_state_t;

/* Applies a complete Buttons control packet's deltas, or leaves state alone. */
bool alvr_protocol_buttons(alvr_wire_t *wire, alvr_input_state_t *state);

void alvr_wire_init(alvr_wire_t *wire, void *data, size_t size);
uint64_t alvr_wire_uint(alvr_wire_t *wire);
/* ALVR 20 uses fixed-width integers; ALVR 21 uses varints. */
uint64_t alvr_wire_integer(alvr_wire_t *wire, unsigned bytes);
void alvr_wire_put_integer(alvr_wire_t *wire, uint64_t value, unsigned bytes);
uint32_t alvr_wire_be32(const uint8_t *data);
void alvr_wire_store_be32(uint8_t *data, uint32_t value);
float alvr_wire_float(alvr_wire_t *wire);
void alvr_wire_put_uint(alvr_wire_t *wire, uint64_t value);
void alvr_wire_put_float(alvr_wire_t *wire, float value);
void alvr_wire_put_bytes(alvr_wire_t *wire, const void *data, size_t size);
void alvr_wire_put_string(alvr_wire_t *wire, const char *value);
uint32_t alvr_wire_u32(const uint8_t *data);
void alvr_wire_store32(uint8_t *data, uint32_t value);
bool alvr_protocol_caps(void *data, size_t size, alvr_caps_t *caps, bool legacy);
size_t alvr_protocol_config(void *data, size_t capacity,
      unsigned width, unsigned height, float rate, unsigned audio_rate, bool legacy);
size_t alvr_protocol_shard(uint8_t *output, size_t capacity,
      unsigned stream, uint32_t packet, const uint8_t *payload,
      size_t size, unsigned shard, bool legacy);

RETRO_END_DECLS
#endif
