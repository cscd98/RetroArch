#ifndef OPENXR_INPUT_H
#define OPENXR_INPUT_H

#include <stdint.h>

#include <boolean.h>
#include <openxr/openxr.h>

#include <libretro.h>

bool openxr_input_init(XrInstance instance);
bool openxr_input_attach(XrSession session);
void openxr_input_sync(XrSession session);
void openxr_input_deinit(void);
bool android_vk_openxr_button(unsigned button);
int16_t android_vk_openxr_axis(unsigned axis);
bool android_vk_openxr_menu_long_press(void);

void openxr_input_head_configure(const struct retro_vr_head_input_config *cfg);
void openxr_input_head_recenter(void);
void openxr_input_head_update(const struct retro_vr_head_pose *pose);
/* MOUSE mode: whole counts of relative motion since the last call. */
bool openxr_input_head_mouse_take(int16_t *dx, int16_t *dy);

#endif
