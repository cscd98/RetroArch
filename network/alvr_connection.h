/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RARCH_ALVR_CONNECTION_H
#define RARCH_ALVR_CONNECTION_H
#include "alvr_protocol.h"
#include <retro_common_api.h>
RETRO_BEGIN_DECLS

typedef struct alvr_connection alvr_connection_t;
enum alvr_connection_state
{
   ALVR_CONNECTION_ERROR = -1,
   ALVR_CONNECTION_CONNECTING,
   ALVR_CONNECTION_NEGOTIATING,
   ALVR_CONNECTION_STREAMING
};

typedef void (*alvr_receive_t)(void *context, unsigned stream,
      void *data, size_t size, bool legacy);

/* Owner-thread only; numeric IPv4 addresses avoid synchronous DNS. */
alvr_connection_t *alvr_connection_new(const char *ip,
      unsigned width, unsigned height, unsigned audio_rate,
      uint64_t now, alvr_receive_t receive, void *context);
void alvr_connection_free(alvr_connection_t *connection);
int alvr_connection_poll(alvr_connection_t *connection, uint64_t now);
bool alvr_connection_decoder(alvr_connection_t *connection,
      const void *headers, size_t size, unsigned codec);
/* Copies a complete stream payload, including its serialized ALVR header. */
bool alvr_connection_send(alvr_connection_t *connection,
      unsigned stream, const void *data, size_t size);
RETRO_END_DECLS
#endif
