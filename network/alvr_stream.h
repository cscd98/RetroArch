/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RARCH_ALVR_STREAM_H
#define RARCH_ALVR_STREAM_H
#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>
#include "alvr_protocol.h"
RETRO_BEGIN_DECLS

typedef struct alvr_stream alvr_stream_t;
enum alvr_stream_status
{
   ALVR_STREAM_STARTING,
   ALVR_STREAM_CONNECTING,
   ALVR_STREAM_CONNECTED,
   ALVR_STREAM_FAILED,
   ALVR_STREAM_STOPPED
};

/* Backend: 0 automatic, 1 Vulkan Video, 2 Media Foundation. */
alvr_stream_t *alvr_stream_new(const char *ip, unsigned view_size,
      unsigned bitrate, unsigned audio_rate, unsigned backend, unsigned codec);
void alvr_stream_placement(alvr_stream_t *stream, unsigned placement,
      float distance, float width, unsigned recenter);
/* Main thread: takes the latest controller state; disconnect clears it. */
void alvr_stream_input(alvr_stream_t *stream, alvr_input_state_t *state);
void alvr_stream_wake(alvr_stream_t *stream);
void alvr_stream_stop(alvr_stream_t *stream);
enum alvr_stream_status alvr_stream_status(const alvr_stream_t *stream);
bool alvr_stream_free(alvr_stream_t *stream);
/* One video producer, one audio producer. Full queues drop whole inputs. */
bool alvr_stream_video_ready(const alvr_stream_t *stream);
void alvr_stream_video(alvr_stream_t *stream, const void *pixels,
      unsigned width, unsigned height, size_t pitch, unsigned pixel_format,
      unsigned rotation, float aspect);
void alvr_stream_audio(alvr_stream_t *stream, const int16_t *samples,
      size_t frames);
RETRO_END_DECLS
#endif
