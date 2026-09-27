/*  RetroArch - A frontend for libretro.
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Foundation,
 *  either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* Vulkan context driver that binds to an OpenXR session instead of an
 * Android ANativeWindow-backed VkSwapchainKHR.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include <retro_timers.h>
#include <string/stdstring.h>

#include <android/native_activity.h>
#include <android/keycodes.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#ifdef HAVE_OPENGLES
#define XR_USE_GRAPHICS_API_OPENGL_ES
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "../../frontend/drivers/platform_unix.h"
#include "../../verbosity.h"
#include "../common/vulkan_common.h"
#include "../video_driver.h"

#include "vk_android_openxr.h"
#include "../../input/drivers/openxr_input.h"

typedef struct vk_android_openxr
{
   XrInstance instance;
   XrSystemId system;
   XrSession session;
   XrSessionState session_state;
   bool running;                 /* xrBeginSession succeeded, not yet ended */
   bool session_ready_to_exit;
   bool focused;

   XrSpace stage_space;
   XrActionSet input_action_set;
   XrAction input_actions[12];
   bool input_attached;
   bool input_buttons[8];
   int16_t input_axes[6];

   /* Per-eye view configuration (both eyes share resolution). */
   int32_t eye_width, eye_height;
   XrSwapchain eye_swapchain[2];
   uint32_t eye_image_count;      /* same for both eyes */
   XrSwapchainImageVulkanKHR *eye_images[2]; /* [eye_image_count] each */

   VkImage *combined_images;
   unsigned combined_count;

   VkFormat swapchain_format;
   unsigned swapchain_dims;       /* VIDEO_SCALE_PACK(eye_width, eye_height) */

   XrView views[2];
   bool views_valid;

   XrTime predicted_display_time;
   bool frame_began;
   bool should_render;

   gfx_ctx_vulkan_data_t vk;

   struct android_app *android_app;
   unsigned current_eye_image_index[2];

   XrSpace view_space;
} vk_android_openxr_t;

static vk_android_openxr_t vk_android_openxr_ctx;

static XrFrameState vk_android_openxr_frame_state;

bool vk_android_openxr_begin_frame(void)
{
   vk_android_openxr_t *xr = &vk_android_openxr_ctx;
   XrFrameWaitInfo wait_info   = { XR_TYPE_FRAME_WAIT_INFO };
   XrFrameBeginInfo begin_info = { XR_TYPE_FRAME_BEGIN_INFO };
   XrViewLocateInfo locate_info = { XR_TYPE_VIEW_LOCATE_INFO };
   XrViewState view_state = { XR_TYPE_VIEW_STATE };
   uint32_t view_count = 0;
   int eye;

   // reset the state
   xr->frame_began = false;
   xr->views_valid = false;

   RARCH_DBG("[XR] begin_frame: running=%d\n", xr->running);

   if (!xr->running)
   {
      RARCH_DBG("[XR] begin_frame: session is not running.\n");
      return false;
   }

   openxr_input_sync(xr->session);

   vk_android_openxr_frame_state.type = XR_TYPE_FRAME_STATE;

   {
      XrResult result = xrWaitFrame(
            xr->session,
            &wait_info,
            &vk_android_openxr_frame_state);

      if (result != XR_SUCCESS)
      {
         char result_str[XR_MAX_RESULT_STRING_SIZE];
         xrResultToString(xr->instance, result, result_str);

         RARCH_ERR("[XR] xrWaitFrame failed: %s (%d).\n",
               result_str, result);
         return false;
      }
   }

   xr->predicted_display_time =
      vk_android_openxr_frame_state.predictedDisplayTime;
   xr->should_render =
      vk_android_openxr_frame_state.shouldRender;

   RARCH_DBG("[XR] xrWaitFrame succeeded: shouldRender=%d predictedTime=%lld\n",
         xr->should_render,
         (long long)xr->predicted_display_time);

   {
      XrResult result = xrBeginFrame(xr->session, &begin_info);

      if (result != XR_SUCCESS)
      {
         char result_str[XR_MAX_RESULT_STRING_SIZE];
         xrResultToString(xr->instance, result, result_str);

         RARCH_ERR("[XR] xrBeginFrame failed: %s (%d).\n",
            result_str, result);
         return false;
      }
   }

   RARCH_DBG("[XR] xrBeginFrame succeeded.\n");

   xr->frame_began = true;

   if (!xr->should_render)
   {
      RARCH_DBG("[XR] shouldRender=false, sessionState=%d\n",
         xr->session_state);

      /* Still a valid frame - just nothing to submit. */
      xr->views_valid = false;
      return false;
   }

   locate_info.viewConfigurationType =
      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
   locate_info.displayTime = xr->predicted_display_time;
   locate_info.space = xr->stage_space;

   xr->views[0].type = XR_TYPE_VIEW;
   xr->views[1].type = XR_TYPE_VIEW;

   {
      XrResult result = xrLocateViews(
            xr->session,
            &locate_info,
            &view_state,
            2,
            &view_count,
            xr->views);

      if (result != XR_SUCCESS)
      {
         char result_str[XR_MAX_RESULT_STRING_SIZE];
         xrResultToString(xr->instance, result, result_str);

         RARCH_ERR("[XR] xrLocateViews failed: %s (%d).\n",
               result_str, result);

         xr->views_valid = false;
         return false;
      }
   }

   RARCH_DBG("[XR] xrLocateViews succeeded: view_count=%u flags=0x%x.\n",
         view_count,
         view_state.viewStateFlags);

   if (view_count != 2)
   {
      RARCH_ERR("[XR] xrLocateViews returned %u views, expected 2.\n",
            view_count);

      xr->views_valid = false;
      return false;
   }

   struct menu_state *menu_st = menu_state_get_ptr();
   bool menu_active = menu_st->flags & MENU_ST_FLAG_ALIVE;
   bool position_valid = view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT;
   bool orientation_valid = view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT;

   RARCH_DBG("[XR] viewStateFlags = 0x%08x menu active = %d position valid=%d orientation valid=%d\n",
      view_state.viewStateFlags, menu_active, position_valid, orientation_valid);

   if (!position_valid && !menu_active)
   {
      RARCH_ERR("[XR] XR_VIEW_STATE_POSITION_VALID_BIT is not set.\n");

      xr->views_valid = false;
      return false;
   }

   if (!orientation_valid && !menu_active)
   {
      RARCH_ERR("[XR] XR_VIEW_STATE_ORIENTATION_VALID_BIT is not set.\n");

      xr->views_valid = false;
      return false;
   }

   xr->views_valid = true;

   for (eye = 0; eye < 2; eye++)
   {
      XrSwapchainImageAcquireInfo acquire =
         { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
      XrSwapchainImageWaitInfo wait =
         { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
      uint32_t image_index = 0;

      RARCH_DBG("[XR] Acquiring eye %d swapchain image...\n", eye);

      {
         XrResult result = xrAcquireSwapchainImage(
               xr->eye_swapchain[eye],
               &acquire,
               &image_index);

         if (result != XR_SUCCESS)
         {
            char result_str[XR_MAX_RESULT_STRING_SIZE];
            xrResultToString(xr->instance, result, result_str);

            RARCH_ERR(
                  "[XR] xrAcquireSwapchainImage eye %d failed: %s (%d).\n",
                  eye, result_str, result);

            xr->views_valid = false;
            return false;
         }
      }

      RARCH_DBG("[XR] Eye %d acquired image %u.\n",
            eye, image_index);

      wait.timeout = XR_INFINITE_DURATION;

      {
         XrResult result = xrWaitSwapchainImage(
               xr->eye_swapchain[eye],
               &wait);

         if (result != XR_SUCCESS)
         {
            char result_str[XR_MAX_RESULT_STRING_SIZE];
            xrResultToString(xr->instance, result, result_str);

            RARCH_ERR(
                  "[XR] xrWaitSwapchainImage eye %d failed: %s (%d).\n",
                  eye, result_str, result);

            xr->views_valid = false;
            return false;
         }
      }

      RARCH_DBG("[XR] Eye %d swapchain image %u is ready.\n",
            eye, image_index);

      xr->current_eye_image_index[eye] = image_index;
   }

   RARCH_DBG("[XR] OpenXR begin_frame complete: both eye images acquired.\n");

   return true;
}

#define XR_FLAT_QUAD_DISTANCE 1.5f
#define XR_FLAT_QUAD_WIDTH    1.5f

void vk_android_openxr_end_frame(bool stereo_layer)
{
   vk_android_openxr_t *xr = &vk_android_openxr_ctx;
   XrFrameEndInfo end_info = { XR_TYPE_FRAME_END_INFO };
   XrCompositionLayerProjection projection =
      { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
   XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
   XrCompositionLayerProjectionView proj_views[2] =
   {
      { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
      { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }
   };
   const XrCompositionLayerBaseHeader *layers[1];
   int eye;

   if (!xr->frame_began)
   {
      RARCH_DBG("[XR] end_frame: no frame bracket is active.\n");
      return;
   }

   RARCH_DBG("[XR] end_frame: submitting frame, should_render=%d views_valid=%d\n",
         xr->should_render, xr->views_valid);

   xr->frame_began = false;

   end_info.displayTime          = xr->predicted_display_time;
   end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
   end_info.layerCount           = 0;
   end_info.layers               = NULL;

   if (xr->views_valid && xr->should_render)
   {
      for (eye = 0; eye < 2; eye++)
      {
         XrSwapchainImageReleaseInfo release =
            { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
         XrResult result;

         result = xrReleaseSwapchainImage(
               xr->eye_swapchain[eye], &release);

         if (result != XR_SUCCESS)
         {
            char result_str[XR_MAX_RESULT_STRING_SIZE];

            xrResultToString(xr->instance, result, result_str);

            RARCH_ERR(
                  "[XR] xrReleaseSwapchainImage eye %d failed: %s (%d).\n",
                  eye, result_str, result);
         }
         else
            RARCH_DBG("[XR] Released eye %d swapchain image.\n", eye);

         proj_views[eye].pose = xr->views[eye].pose;
         proj_views[eye].fov  = xr->views[eye].fov;

         proj_views[eye].subImage.swapchain =
            xr->eye_swapchain[eye];

         proj_views[eye].subImage.imageRect.offset.x = 0;
         proj_views[eye].subImage.imageRect.offset.y = 0;
         proj_views[eye].subImage.imageRect.extent.width =
            (uint32_t)xr->eye_width;
         proj_views[eye].subImage.imageRect.extent.height =
            (uint32_t)xr->eye_height;
         proj_views[eye].subImage.imageArrayIndex = 0;
      }

      projection.space     = xr->stage_space;
      projection.viewCount = 2;
      projection.views     = proj_views;

      if (stereo_layer)
      {
         /* existing projection setup */
         layers[0] = (const XrCompositionLayerBaseHeader*)&projection;
      }
      else
      {
         quad.layerFlags                          = 0; /* ignore alpha */
         quad.space                               = xr->view_space;
         quad.eyeVisibility                       = XR_EYE_VISIBILITY_BOTH;
         quad.subImage.swapchain                  = xr->eye_swapchain[0];
         quad.subImage.imageRect.offset.x         = 0;
         quad.subImage.imageRect.offset.y         = 0;
         quad.subImage.imageRect.extent.width     = (uint32_t)xr->eye_width;
         quad.subImage.imageRect.extent.height    = (uint32_t)xr->eye_height;
         quad.subImage.imageArrayIndex            = 0;
         quad.pose.orientation.w                  = 1.0f;
         quad.pose.position.z                     = -XR_FLAT_QUAD_DISTANCE;
         quad.size.width                          = XR_FLAT_QUAD_WIDTH;
         quad.size.height                         = XR_FLAT_QUAD_WIDTH
               * (float)xr->eye_height / (float)xr->eye_width;
         layers[0] = (const XrCompositionLayerBaseHeader*)&quad;
      }
      end_info.layerCount = 1;
      end_info.layers     = layers;
   }
   else
   {
      RARCH_DBG("[XR] end_frame: submitting empty frame.\n");
   }

   RARCH_DBG("[XR] end_frame: calling xrEndFrame()\n");
   {
      XrResult result = xrEndFrame(xr->session, &end_info);

      if (result != XR_SUCCESS)
      {
         char result_str[XR_MAX_RESULT_STRING_SIZE];

         xrResultToString(xr->instance, result, result_str);

         RARCH_ERR("[XR] xrEndFrame failed: %s (%d).\n",
               result_str, result);
      }
      else
         RARCH_DBG("[XR] xrEndFrame succeeded.\n");
   }
}

bool vk_android_openxr_get_eye_state(struct retro_vr_eye_state out[2])
{
   vk_android_openxr_t *xr = &vk_android_openxr_ctx;
   int eye;

   if (!xr->views_valid)
      return false;

   for (eye = 0; eye < 2; eye++)
   {
      const XrView *v = &xr->views[eye];

      out[eye].position[0] = v->pose.position.x;
      out[eye].position[1] = v->pose.position.y;
      out[eye].position[2] = v->pose.position.z;

      out[eye].orientation[0] = v->pose.orientation.x;
      out[eye].orientation[1] = v->pose.orientation.y;
      out[eye].orientation[2] = v->pose.orientation.z;
      out[eye].orientation[3] = v->pose.orientation.w;

      /* XrFovf gives signed angles (angleLeft is negative); tan() of
       * each converts directly to the fov_tan convention. */
      out[eye].fov_tan[0] = tanf(v->fov.angleLeft);
      out[eye].fov_tan[1] = tanf(v->fov.angleRight);
      out[eye].fov_tan[2] = tanf(v->fov.angleUp);
      out[eye].fov_tan[3] = tanf(v->fov.angleDown);
   }

   return true;
}

unsigned vk_android_openxr_get_backbuffer_index(int eye)
{
   vk_android_openxr_t *xr = &vk_android_openxr_ctx;

   return xr->current_eye_image_index[eye] * 2 + (unsigned)eye;
}

static void vk_android_openxr_destroy_swapchains(vk_android_openxr_t *xr)
{
   int eye;
   for (eye = 0; eye < 2; eye++)
   {
      if (xr->eye_swapchain[eye] != XR_NULL_HANDLE)
         xrDestroySwapchain(xr->eye_swapchain[eye]);
      xr->eye_swapchain[eye] = XR_NULL_HANDLE;
      if (xr->eye_images[eye])
         free(xr->eye_images[eye]);
      xr->eye_images[eye] = NULL;
   }
   if (xr->combined_images)
      free(xr->combined_images);
   xr->combined_images = NULL;
   xr->combined_count  = 0;
}

static bool vk_android_openxr_init_swapchains(vk_android_openxr_t *xr,
      VkFormat preferred_format)
{
   RARCH_DBG("[XR] Initializing OpenXR eye swapchains...\n");

   uint32_t format_count = 0;
   int64_t *formats      = NULL;
   int64_t chosen_format = 0;
   uint32_t view_count   = 0;
   XrViewConfigurationView views[2] =
   {
      { XR_TYPE_VIEW_CONFIGURATION_VIEW },
      { XR_TYPE_VIEW_CONFIGURATION_VIEW }
   };
   int eye;

   if (xrEnumerateViewConfigurationViews(xr->instance, xr->system,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
            &view_count, NULL) != XR_SUCCESS || view_count != 2)
   {
      RARCH_ERR("[XR] Expected 2 views in primary stereo config, got %u.\n",
            view_count);
      return false;
   }
   xrEnumerateViewConfigurationViews(xr->instance, xr->system,
         XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &view_count, views);

   xr->eye_width  = (int32_t)views[0].recommendedImageRectWidth;
   xr->eye_height = (int32_t)views[0].recommendedImageRectHeight;
   xr->swapchain_dims = VIDEO_SCALE_PACK((unsigned)xr->eye_width,
         (unsigned)xr->eye_height);

   xrEnumerateSwapchainFormats(xr->session, 0, &format_count, NULL);
   if (!format_count)
      return false;
   formats = (int64_t*)malloc(sizeof(*formats) * format_count);
   if (!formats)
      return false;
   xrEnumerateSwapchainFormats(xr->session, format_count, &format_count, formats);

   /* Prefer the format vulkan.c's swapchain init already picked for the
    * flat path (VK_FORMAT_B8G8R8A8_UNORM in the common case); fall back
    * to whatever the runtime offers first. */
   {
      uint32_t i;

      chosen_format = VK_FORMAT_UNDEFINED;

      for (i = 0; i < format_count; i++)
      {
         RARCH_DBG("[XR] Swapchain formats: i:%u (VkFormat)formats[i]:%d\n",
               i, (VkFormat)formats[i]);

         if ((VkFormat)formats[i] == preferred_format)
         {
            chosen_format = formats[i];

            RARCH_DBG("[XR] Swapchain format chosen from preferred format: %d\n",
                  (VkFormat)chosen_format);
            break;
         }
      }

      /*
      * Quest/OpenXR does not necessarily expose the format used by
      * RetroArch's normal Vulkan surface. Prefer a 4-channel UNORM
      * format rather than blindly taking formats[0].
      */
      if (chosen_format == VK_FORMAT_UNDEFINED)
      {
         for (i = 0; i < format_count; i++)
         {
            if ((VkFormat)formats[i] == VK_FORMAT_R8G8B8A8_UNORM)
            {
               chosen_format = formats[i];

               RARCH_DBG("[XR] Swapchain format chosen from RGBA8 UNORM fallback: %d\n",
                     (VkFormat)chosen_format);
               break;
            }
         }
      }

      /*
      * If RGBA8 UNORM isn't available, try RGBA8 SRGB.
      */
      if (chosen_format == VK_FORMAT_UNDEFINED)
      {
         for (i = 0; i < format_count; i++)
         {
            if ((VkFormat)formats[i] == VK_FORMAT_R8G8B8A8_SRGB)
            {
               chosen_format = formats[i];

               RARCH_DBG("[XR] Swapchain format chosen from RGBA8 SRGB fallback: %d\n",
                     (VkFormat)chosen_format);
               break;
            }
         }
      }
   }

   free(formats);

   if (chosen_format == VK_FORMAT_UNDEFINED)
   {
      RARCH_ERR("[XR] No suitable OpenXR swapchain format found!\n");
      return false;
   }

   RARCH_DBG("[XR] Swapchain format chosen: %lld of %u offered (preferred = %d)\n",
      (long long)chosen_format, format_count, (int)preferred_format);

   xr->swapchain_format = (VkFormat)chosen_format;

   for (eye = 0; eye < 2; eye++)
   {
      XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
      uint32_t image_count       = 0;

      info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
                       | XR_SWAPCHAIN_USAGE_SAMPLED_BIT
                       | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
      info.format      = chosen_format;
      info.sampleCount  = 1;
      info.width        = (uint32_t)xr->eye_width;
      info.height       = (uint32_t)xr->eye_height;
      info.faceCount    = 1;
      info.arraySize    = 1;
      info.mipCount     = 1;

      if (xrCreateSwapchain(xr->session, &info, &xr->eye_swapchain[eye])
            != XR_SUCCESS)
      {
         RARCH_ERR("[XR] xrCreateSwapchain failed for eye %d.\n", eye);
         vk_android_openxr_destroy_swapchains(xr);
         return false;
      }

      xrEnumerateSwapchainImages(xr->eye_swapchain[eye], 0, &image_count, NULL);
      if (eye == 0)
         xr->eye_image_count = image_count;
      else if (image_count != xr->eye_image_count)
      {
         /* Runtimes are expected to hand back matching counts for
          * identically-configured swapchains; if not, clamp to the
          * smaller so the interleaved array stays rectangular. */
         if (image_count < xr->eye_image_count)
            xr->eye_image_count = image_count;
      }

      xr->eye_images[eye] = (XrSwapchainImageVulkanKHR*)
         calloc(image_count, sizeof(XrSwapchainImageVulkanKHR));
      if (!xr->eye_images[eye])
      {
         vk_android_openxr_destroy_swapchains(xr);
         return false;
      }
      {
         uint32_t i;
         for (i = 0; i < image_count; i++)
            xr->eye_images[eye][i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
      }
      xrEnumerateSwapchainImages(xr->eye_swapchain[eye], image_count,
            &image_count,
            (XrSwapchainImageBaseHeader*)xr->eye_images[eye]);
   }

   /* Interleave: slot i (frame index i) = [eye0 image i, eye1 image i].
    * vulkan.c's vk->num_swapchain_images becomes xr->eye_image_count,
    * and every place it indexes backbuffers[swapchain_index] instead
    * indexes combined_images[frame_index * 2 + eye] via the driver
    * hook added in vulkan_frame(). */
   xr->combined_count  = xr->eye_image_count * 2;
   xr->combined_images = (VkImage*)malloc(sizeof(VkImage) * xr->combined_count);
   if (!xr->combined_images)
   {
      vk_android_openxr_destroy_swapchains(xr);
      return false;
   }
   {
      uint32_t i;
      for (i = 0; i < xr->eye_image_count; i++)
      {
         xr->combined_images[i * 2 + 0] = xr->eye_images[0][i].image;
         xr->combined_images[i * 2 + 1] = xr->eye_images[1][i].image;
      }
   }

   return true;
}

static bool vk_android_openxr_create_instance(vk_android_openxr_t *xr,
      struct android_app *android_app)
{
   PFN_xrInitializeLoaderKHR init_loader = NULL;
   XrLoaderInitInfoAndroidKHR loader_info = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
   XrInstanceCreateInfoAndroidKHR android_info = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
   XrInstanceCreateInfo create_info = { XR_TYPE_INSTANCE_CREATE_INFO };
   const char *extensions[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
   XrSystemGetInfo system_info = { XR_TYPE_SYSTEM_GET_INFO };

   if (xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
            (PFN_xrVoidFunction*)&init_loader) != XR_SUCCESS || !init_loader)
   {
      RARCH_ERR("[XR] xrInitializeLoaderKHR unavailable.\n");
      return false;
   }

   loader_info.applicationVM      = android_app->activity->vm;
   loader_info.applicationContext = android_app->activity->clazz;
   if (init_loader((XrLoaderInitInfoBaseHeaderKHR*)&loader_info) != XR_SUCCESS)
      return false;

   android_info.applicationVM       = android_app->activity->vm;
   android_info.applicationActivity = android_app->activity->clazz;

   create_info.next = &android_info;
   strlcpy(create_info.applicationInfo.applicationName, "RetroArch",
         sizeof(create_info.applicationInfo.applicationName));
   create_info.applicationInfo.apiVersion            = XR_API_VERSION_1_0;
   create_info.enabledExtensionCount                 = 1;
   create_info.enabledExtensionNames                 = extensions;

   if (xrCreateInstance(&create_info, &xr->instance) != XR_SUCCESS)
   {
      RARCH_ERR("[XR] xrCreateInstance failed.\n");
      return false;
   }

   RARCH_DBG("[XR] Created XrInstance=%p\n", (void *)xr->instance);
   RARCH_DBG("[XR] Global XrInstance=%p\n",
      (void *)vk_android_openxr_ctx.instance);

   system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
   if (xrGetSystem(xr->instance, &system_info, &xr->system) != XR_SUCCESS)
   {
      RARCH_ERR("[XR] xrGetSystem failed (no HMD?).\n");
      return false;
   }

   return true;
}

/* this function binds an XR session to that already-live device via
 * XR_KHR_vulkan_enable2's graphics requirements + binding struct. */
static bool vk_android_openxr_create_session(vk_android_openxr_t *xr,
      vulkan_context_t *vkctx)
{
   PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs = NULL;
   XrGraphicsRequirementsVulkan2KHR reqs =
      { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
   XrGraphicsBindingVulkan2KHR binding   = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
   XrSessionCreateInfo session_info      = { XR_TYPE_SESSION_CREATE_INFO };
   XrReferenceSpaceCreateInfo space_info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
   XrResult result = xrGetInstanceProcAddr(xr->instance,
      "xrGetVulkanGraphicsRequirements2KHR",
      (PFN_xrVoidFunction*)&get_reqs);

   if (result != XR_SUCCESS || !get_reqs)
   {
      RARCH_ERR("[XR] xrGetVulkanGraphicsRequirements2KHR failed: %d\n", result);
      return false;
   }

   result = get_reqs(xr->instance, xr->system, &reqs);

   if (result != XR_SUCCESS)
   {
      RARCH_ERR("[XR] xrGetVulkanGraphicsRequirements2KHR failed: %d\n", result);
      return false;
   }

   binding.instance             = vkctx->instance;
   binding.physicalDevice       = vkctx->gpu;
   binding.device               = vkctx->device;
   binding.queueFamilyIndex     = vkctx->graphics_queue_index;
   binding.queueIndex           = 0;

   session_info.next     = &binding;
   session_info.systemId = xr->system;

   RARCH_DBG("[XR] Creating OpenXR session...\n");

   {
      XrResult result = xrCreateSession(
            xr->instance, &session_info, &xr->session);

      if (result != XR_SUCCESS)
      {
         char result_str[XR_MAX_RESULT_STRING_SIZE];
         xrResultToString(xr->instance, result, result_str);
         RARCH_ERR("[XR] xrCreateSession failed: %s (%d)\n",
            result_str, result);
         return false;
      }
   }

   RARCH_DBG("[XR] OpenXR session created: %p\n", (void *)xr->session);

   space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
   space_info.poseInReferenceSpace.orientation.w = 1.0f;
   if (xrCreateReferenceSpace(xr->session, &space_info, &xr->stage_space)
         != XR_SUCCESS)
   {
      /* STAGE may be unavailable on some Quest configs (no guardian set
       * up yet); LOCAL is guaranteed by the spec. */
      space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
      if (xrCreateReferenceSpace(xr->session, &space_info, &xr->stage_space)
            != XR_SUCCESS)
      {
         RARCH_ERR("[XR] xrCreateReferenceSpace failed.\n");
         return false;
      }
   }

   space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
   if (xrCreateReferenceSpace(xr->session, &space_info, &xr->view_space)
         != XR_SUCCESS)
   {
      RARCH_ERR("[XR] Failed to create view space.\n");
      return false;
   }

   if (!vk_android_openxr_init_swapchains(xr, VK_FORMAT_B8G8R8A8_UNORM))
   {
      xrDestroySpace(xr->stage_space);
      xr->stage_space = XR_NULL_HANDLE;
      xrDestroySession(xr->session);
      xr->session = XR_NULL_HANDLE;
      return false;
   }

   RARCH_DBG("[XR] OpenXR eye swapchains initialized: %dx%d, %u images\n",
      xr->eye_width,
      xr->eye_height,
      xr->eye_image_count);

   if (!openxr_input_attach(xr->session))
      return false;

   {
      unsigned i, n = xr->combined_count;
      if (n > VULKAN_MAX_SWAPCHAIN_IMAGES)
         n = VULKAN_MAX_SWAPCHAIN_IMAGES;
      for (i = 0; i < n; i++)
         vkctx->swapchain_images[i] = xr->combined_images[i];
      vkctx->num_swapchain_images = n;
      vkctx->swapchain_format     = xr->swapchain_format;
      vkctx->swapchain_dims       = xr->swapchain_dims;
      vkctx->flags               |= VK_CTX_FLAG_INVALID_SWAPCHAIN;
   }

   return true;
}

static const char *vk_android_openxr_session_state_name(
      XrSessionState state)
{
   switch (state)
   {
      case XR_SESSION_STATE_UNKNOWN:       return "UNKNOWN";
      case XR_SESSION_STATE_IDLE:          return "IDLE";
      case XR_SESSION_STATE_READY:         return "READY";
      case XR_SESSION_STATE_SYNCHRONIZED:  return "SYNCHRONIZED";
      case XR_SESSION_STATE_VISIBLE:       return "VISIBLE";
      case XR_SESSION_STATE_FOCUSED:       return "FOCUSED";
      case XR_SESSION_STATE_STOPPING:      return "STOPPING";
      case XR_SESSION_STATE_LOSS_PENDING:  return "LOSS_PENDING";
      case XR_SESSION_STATE_EXITING:       return "EXITING";
      default:                             return "UNKNOWN";
   }
}

static void vk_android_openxr_poll_events(vk_android_openxr_t *xr, bool *quit)
{
   XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };

   while (xrPollEvent(xr->instance, &event) == XR_SUCCESS)
   {
      RARCH_DBG("[XR] OpenXR event type: %d\n", event.type);
      switch (event.type)
      {
         case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            *quit = true;
            break;
         case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            {
               XrEventDataSessionStateChanged *e =
                  (XrEventDataSessionStateChanged*)&event;
               xr->session_state = e->state;
               RARCH_DBG("[XR] Session state: %s (%d)\n", vk_android_openxr_session_state_name(e->state),
                  e->state);
               switch (e->state)
               {
                  case XR_SESSION_STATE_READY:
                     RARCH_DBG("[XR] XR_SESSION_STATE_READY -> xrBeginSession()\n");
                     {
                        XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
                        begin.primaryViewConfigurationType =
                           XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        if (xrBeginSession(xr->session, &begin) == XR_SUCCESS)
                        {
                           RARCH_DBG("[XR] xrBeginSession succeeded: session=%p\n",
                              (void *)xr->session);
                           xr->running = true;
                        }
                        else
                           RARCH_ERR("[XR] xrBeginSession failed: session=%p\n",
                              (void *)xr->session);
                     }
                     break;
                  case XR_SESSION_STATE_STOPPING:
                     xrEndSession(xr->session);
                     xr->running = false;
                     break;
                  case XR_SESSION_STATE_FOCUSED:
                     xr->focused = true;
                     break;
                  case XR_SESSION_STATE_VISIBLE:
                  case XR_SESSION_STATE_SYNCHRONIZED:
                     xr->focused = false;
                     break;
                  case XR_SESSION_STATE_EXITING:
                  case XR_SESSION_STATE_LOSS_PENDING:
                     *quit = true;
                     break;
                  default:
                     break;
               }
            }
            break;
         default:
            break;
      }
      event.type = XR_TYPE_EVENT_DATA_BUFFER;
   }
}

/* ===================== gfx_ctx_driver_t entry points ===================== */

// forward declaration
static void vk_android_openxr_gfx_ctx_destroy(void *data);

static void *vk_android_openxr_gfx_ctx_init(void *video_driver)
{
   RARCH_DBG("[XR] *** vk_android_openxr_gfx_ctx_init CALLED ***\n");

   vk_android_openxr_t *xr = &vk_android_openxr_ctx;
   memset(xr, 0, sizeof(*xr));

   xr->android_app = (struct android_app*)g_android;
   if (!xr->android_app)
      return NULL;

   if (!vk_android_openxr_create_instance(xr, xr->android_app))
   {
      vk_android_openxr_gfx_ctx_destroy(xr);
      return NULL;
   }

   if (!openxr_input_init(xr->instance))
   {
      RARCH_ERR("[XR] Touch controller input setup failed.\n");
      vk_android_openxr_gfx_ctx_destroy(xr);
      return NULL;
   }

   /* Capability probe. */
   {
      uint32_t count = 0;
      XrViewConfigurationType types[8];
      uint32_t i;
      bool stereo_supported = false;

      xrEnumerateViewConfigurations(xr->instance, xr->system, 0, &count, NULL);
      if (count == 0 || count > 8)
      {
         vk_android_openxr_gfx_ctx_destroy(xr);
         return NULL;
      }
      xrEnumerateViewConfigurations(xr->instance, xr->system, count, &count, types);
      for (i = 0; i < count; i++)
      {
         if (types[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
         {
            stereo_supported = true;
            break;
         }
      }
      if (!stereo_supported)
      {
         RARCH_WARN("[XR] No PRIMARY_STEREO view configuration; no HMD paired?\n");
         vk_android_openxr_gfx_ctx_destroy(xr);
         return NULL;
      }
   }

   /* reports real dimensions instead of 0x0. */
   {
      uint32_t view_count = 0;
      XrViewConfigurationView views[2] =
      {
         { XR_TYPE_VIEW_CONFIGURATION_VIEW },
         { XR_TYPE_VIEW_CONFIGURATION_VIEW }
      };

      if (xrEnumerateViewConfigurationViews(xr->instance, xr->system,
               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &view_count, views)
            == XR_SUCCESS && view_count == 2)
      {
         xr->eye_width  = (int32_t)views[0].recommendedImageRectWidth;
         xr->eye_height = (int32_t)views[0].recommendedImageRectHeight;
         xr->swapchain_dims = VIDEO_SCALE_PACK((unsigned)xr->eye_width,
               (unsigned)xr->eye_height);
      }
   }

   if (!vulkan_context_init(&xr->vk, VULKAN_WSI_ANDROID)
       || !vulkan_context_init_headless_device(&xr->vk))
   {
      RARCH_ERR("[XR] Vulkan context/device init failed.\n");
      vk_android_openxr_gfx_ctx_destroy(xr);
      return NULL;
   }

   return xr;
}

static enum gfx_ctx_api vk_android_openxr_gfx_ctx_get_api(void *data)
{
   return GFX_CTX_VULKAN_API;
}

static bool vk_android_openxr_gfx_ctx_bind_api(void *data,
      enum gfx_ctx_api api, unsigned major, unsigned minor)
{
   return api == GFX_CTX_VULKAN_API;
}

static void vk_android_openxr_gfx_ctx_get_video_size(void *data, unsigned *dims)
{
   vk_android_openxr_t *xr = (vk_android_openxr_t*)data;
   *dims = (xr && xr->eye_width > 0) ? xr->swapchain_dims : 0;
}

static bool vk_android_openxr_gfx_ctx_set_video_mode(void *data,
      unsigned dims, bool fullscreen)
{
   return true;
}

static bool vk_android_openxr_gfx_ctx_set_resize(void *data,
      unsigned dims)
{
   return true;
}

static void vk_android_openxr_gfx_ctx_set_flags(void *data, uint32_t flags)
{
}

static void vk_android_openxr_gfx_ctx_check_window(void *data,
      bool *quit, bool *resize, unsigned *dims)
{
   vk_android_openxr_t *xr = (vk_android_openxr_t*)data;
   bool local_quit = false;

   if (!xr)
      return;

   vk_android_openxr_poll_events(xr, &local_quit);

   *quit   = local_quit;
   *resize = false;
}

static void vk_android_openxr_gfx_ctx_swap_buffers(void *data)
{
   /* Deliberately empty: vulkan_frame() calls xrEndFrame() directly
    * once both eyes are recorded. */
   (void)data;
}

static void vk_android_openxr_gfx_ctx_destroy(void *data)
{
   RARCH_DBG("[XR] vk_android_openxr_gfx_ctx_destroy called..\n");
   vk_android_openxr_t *xr = (vk_android_openxr_t*)data;
   if (!xr)
      return;

   vk_android_openxr_destroy_swapchains(xr);
   if (xr->stage_space != XR_NULL_HANDLE)
      xrDestroySpace(xr->stage_space);
   if (xr->view_space != XR_NULL_HANDLE)
      xrDestroySpace(xr->view_space);
   if (xr->session != XR_NULL_HANDLE)
      xrDestroySession(xr->session);
   openxr_input_deinit();
   if (xr->instance != XR_NULL_HANDLE)
      xrDestroyInstance(xr->instance);
   memset(xr, 0, sizeof(*xr));
}

static bool vk_android_openxr_gfx_ctx_has_focus(void *data)
{
   vk_android_openxr_t *xr = (vk_android_openxr_t*)data;
   return xr && xr->focused;
}

static bool vk_android_openxr_gfx_ctx_suppress_screensaver(void *data, bool enable)
{
   return false; /* not meaningful under a VR compositor */
}

static void vk_android_openxr_gfx_ctx_input_driver(void *data,
      const char *joypad_name,
      input_driver_t **input, void **input_data)
{
   void *androidinput = input_driver_init_wrap(&input_android, joypad_name);
   *input      = androidinput ? &input_android : NULL;
   *input_data = androidinput;
}

static gfx_ctx_proc_t vk_android_openxr_gfx_ctx_get_proc_address(const char *symbol)
{
   return NULL; /* Vulkan uses vkGetInstanceProcAddr/vkGetDeviceProcAddr, not this */
}

static void vk_android_openxr_gfx_ctx_show_mouse(void *data, bool state) { }

static uint32_t vk_android_openxr_gfx_ctx_get_flags(void *data)
{
   return 0;
}

static void *vk_android_openxr_gfx_ctx_get_context_data(void *data)
{
   vk_android_openxr_t *xr = (vk_android_openxr_t*)data;
   if (!xr)
      return NULL;
   if (xr->session == XR_NULL_HANDLE
         && !vk_android_openxr_create_session(xr, &xr->vk.context))
      return NULL;
   return &xr->vk.context;
}

bool vk_android_openxr_is_session_ready(void)
{
   vk_android_openxr_t *xr = &vk_android_openxr_ctx;

   return xr->session != XR_NULL_HANDLE
       && xr->combined_images != NULL;
}

const gfx_ctx_driver_t gfx_ctx_vk_android_openxr = {
   vk_android_openxr_gfx_ctx_init,
   vk_android_openxr_gfx_ctx_destroy,
   vk_android_openxr_gfx_ctx_get_api,
   vk_android_openxr_gfx_ctx_bind_api,
   NULL, /* swap_interval: no vsync under the XR compositor */
   vk_android_openxr_gfx_ctx_set_video_mode,
   vk_android_openxr_gfx_ctx_get_video_size,
   NULL, /* get_refresh_rate: query via XR_FB_display_refresh_rate if needed later */
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_metrics */
   NULL, /* translate_aspect */
   NULL, /* update_window_title */
   vk_android_openxr_gfx_ctx_check_window,
   vk_android_openxr_gfx_ctx_set_resize,
   vk_android_openxr_gfx_ctx_has_focus,
   vk_android_openxr_gfx_ctx_suppress_screensaver,
   true, /* has_windowed: XR presentation, no Android window */
   vk_android_openxr_gfx_ctx_swap_buffers,
   vk_android_openxr_gfx_ctx_input_driver,
   vk_android_openxr_gfx_ctx_get_proc_address,
   NULL, /* image_buffer_init */
   NULL, /* image_buffer_write */
   vk_android_openxr_gfx_ctx_show_mouse,
   "vk_android_openxr",
   vk_android_openxr_gfx_ctx_get_flags,
   vk_android_openxr_gfx_ctx_set_flags,
   NULL, /* bind_hw_render */
   vk_android_openxr_gfx_ctx_get_context_data,
   NULL, /* make_current */
   NULL, /* create_surface */
   NULL, /* destroy_surface */
   NULL  /* presentable */
};
