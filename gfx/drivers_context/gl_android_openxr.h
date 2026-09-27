#ifndef GL_ANDROID_OPENXR_H
#define GL_ANDROID_OPENXR_H

#include <stdint.h>
#include <boolean.h>
#include <libretro.h>

/* True while the OpenXR GLES context driver is the live context */
bool gl_android_openxr_active(void);

/* xrWaitFrame / xrBeginFrame / xrLocateViews */
bool gl_android_openxr_begin_frame(void);

uint32_t gl_android_openxr_get_framebuffer(void);

/* Copies this frame's per-eye pose/FOV (from the xrLocateViews call
 * in begin_frame) into out[RETRO_VR_EYE_LEFT] / out[RETRO_VR_EYE_RIGHT].
 * Returns false when begin_frame hasn't produced a valid sample. */
bool gl_android_openxr_get_eye_state(struct retro_vr_eye_state out[2]);

bool gl_android_openxr_is_session_ready(void);

#endif
