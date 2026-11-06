/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RARCH_MF_VIDEO_ENCODE_H
#define RARCH_MF_VIDEO_ENCODE_H
#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>
RETRO_BEGIN_DECLS

typedef struct mf_video_encoder mf_video_encoder_t;
/* One MTA worker owns the encoder throughout its lifetime. */
mf_video_encoder_t *mf_video_encoder_new(unsigned width, unsigned height,
      unsigned bitrate, unsigned fps, bool hevc);
bool mf_video_encoder_headers(mf_video_encoder_t *encoder,
      void *data, size_t capacity, size_t *size);
bool mf_video_encoder_submit(mf_video_encoder_t *encoder,
      const uint8_t *pixels, bool keyframe);
int mf_video_encoder_poll(mf_video_encoder_t *encoder,
      const uint8_t **data, size_t *size, bool *keyframe);
bool mf_video_encoder_free(mf_video_encoder_t *encoder);
RETRO_END_DECLS
#endif
