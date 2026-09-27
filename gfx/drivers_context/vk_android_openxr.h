#ifndef VK_ANDROID_OPENXR_H
#define VK_ANDROID_OPENXR_H

#include <stdint.h>
#include <boolean.h>
#include <libretro.h>

/* Direct entry points into the XR frame loop, called from vulkan.c */
bool vk_android_openxr_begin_frame(void);

/* Releases both eye swapchain images and calls xrEndFrame with a stereo
 * projection composition layer. */
void vk_android_openxr_end_frame(bool stereo_layer);

/* Copies this frame's per-eye pose/FOV (from the xrLocateViews call
 * inside begin_frame) into out[RETRO_VR_EYE_LEFT]/out[RETRO_VR_EYE_RIGHT].
 * Returns false if begin_frame hasn't produced a valid sample yet. */
bool vk_android_openxr_get_eye_state(struct retro_vr_eye_state out[2]);

/* Index into the combined_images[]/backbuffers[] array (built at init
 * time as [xr_image_idx * 2 + eye]) that this eye's just-acquired image
 * corresponds to for the current frame. Only valid between a successful
 * begin_frame and the matching end_frame. */
unsigned vk_android_openxr_get_backbuffer_index(int eye);

/* True once xrCreateSession + both eye swapchains have been created
 * successfully and the session has not since been torn down. */
bool vk_android_openxr_is_session_ready(void);

bool vk_android_openxr_button(unsigned button);
int16_t vk_android_openxr_axis(unsigned axis);

#endif
