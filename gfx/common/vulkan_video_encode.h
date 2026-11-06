/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RARCH_VULKAN_VIDEO_ENCODE_H
#define RARCH_VULKAN_VIDEO_ENCODE_H
#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>
RETRO_BEGIN_DECLS

typedef struct vulkan_video_encoder vulkan_video_encoder_t;

/* Open on a worker. All subsequent calls have the same serialized owner. */
vulkan_video_encoder_t *vulkan_video_encoder_new(unsigned width,
      unsigned height, unsigned bitrate, unsigned fps, bool hevc);
bool vulkan_video_encoder_headers(vulkan_video_encoder_t *encoder,
      void *data, size_t capacity, size_t *size);
/* Tight NV12 input, borrowed for this call only. False means busy or failed. */
bool vulkan_video_encoder_submit(vulkan_video_encoder_t *encoder,
      const uint8_t *pixels, bool keyframe);
/* -1 failure, 0 pending, 1 packet. Packet stays valid until next submit. */
int vulkan_video_encoder_poll(vulkan_video_encoder_t *encoder,
      const uint8_t **data, size_t *size, bool *keyframe);
/* False while submitted GPU work is still outstanding. Never waits. */
bool vulkan_video_encoder_free(vulkan_video_encoder_t *encoder);
RETRO_END_DECLS
#endif
