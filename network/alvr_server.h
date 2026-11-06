/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RARCH_ALVR_SERVER_H
#define RARCH_ALVR_SERVER_H
#include <boolean.h>
#include <stdint.h>
#include <retro_common_api.h>
RETRO_BEGIN_DECLS
#ifdef HAVE_ALVR
void alvr_server_poll(void);
void alvr_server_input_poll(float threshold);
unsigned alvr_server_input_buttons(void);
int16_t alvr_server_input_analog(unsigned idx, unsigned id);
bool alvr_server_input_menu(void);
void alvr_server_stop(void);
bool alvr_server_pending(void);
bool alvr_server_capture_active(void);
#else
#define alvr_server_poll() ((void)0)
#define alvr_server_stop() ((void)0)
#define alvr_server_pending() false
#define alvr_server_capture_active() false
#endif
RETRO_END_DECLS
#endif
