/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#define VK_NO_PROTOTYPES
#include "vulkan_video_encode.h"
#include "../include/vulkan/vulkan.h"
#include <dynamic/dylib.h>
#include <stdlib.h>
#include <string.h>

#define VVE_BUFFER_SIZE (4 * 1024 * 1024)

typedef struct vve_buffer
{
   VkBuffer buffer;
   VkDeviceMemory memory;
   void *mapped;
} vve_buffer_t;

typedef struct vve_image
{
   VkImage image;
   VkImageView view;
   VkDeviceMemory memory;
} vve_image_t;

struct vulkan_video_encoder
{
   dylib_t library;
   VkInstance instance;
   VkPhysicalDevice gpu;
   VkDevice device;
   VkQueue queue, upload_queue;
   VkVideoSessionKHR session;
   VkVideoSessionParametersKHR parameters;
   VkDeviceMemory session_memory[16];
   VkCommandPool pool, upload_pool;
   VkCommandBuffer command, upload_command;
   VkFence fence, upload_fence;
   VkSemaphore upload_ready;
   VkQueryPool query;
   VkPhysicalDeviceMemoryProperties memory;
   VkVideoEncodeH264ProfileInfoKHR h264_profile;
   VkVideoEncodeH265ProfileInfoKHR h265_profile;
   VkVideoProfileInfoKHR profile;
   VkVideoProfileListInfoKHR profiles;
   VkVideoCapabilitiesKHR caps;
   VkVideoEncodeCapabilitiesKHR encode_caps;
   VkVideoEncodeH264CapabilitiesKHR h264_caps;
   VkVideoEncodeH265CapabilitiesKHR h265_caps;
   vve_buffer_t upload, output;
   vve_image_t input, dpb;
   uint32_t family, upload_family, session_memory_count;
   unsigned width, height, fps, bitrate, picture_id;
   bool pending, upload_pending, initialized, failed, keyframe, hevc;
   PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
   PFN_vkDestroyInstance DestroyInstance;
   PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
   PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
   PFN_vkGetPhysicalDeviceQueueFamilyProperties2 GetPhysicalDeviceQueueFamilyProperties2;
   PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
   PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR GetPhysicalDeviceVideoCapabilitiesKHR;
   PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR GetPhysicalDeviceVideoFormatPropertiesKHR;
   PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2;
   PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
   PFN_vkCreateDevice CreateDevice;
   PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
   PFN_vkCreateSemaphore CreateSemaphore;
   PFN_vkDestroySemaphore DestroySemaphore;
   PFN_vkDestroyDevice DestroyDevice;
   PFN_vkGetDeviceQueue GetDeviceQueue;
   PFN_vkCreateVideoSessionKHR CreateVideoSessionKHR;
   PFN_vkDestroyVideoSessionKHR DestroyVideoSessionKHR;
   PFN_vkGetVideoSessionMemoryRequirementsKHR GetVideoSessionMemoryRequirementsKHR;
   PFN_vkBindVideoSessionMemoryKHR BindVideoSessionMemoryKHR;
   PFN_vkCreateVideoSessionParametersKHR CreateVideoSessionParametersKHR;
   PFN_vkDestroyVideoSessionParametersKHR DestroyVideoSessionParametersKHR;
   PFN_vkGetEncodedVideoSessionParametersKHR GetEncodedVideoSessionParametersKHR;
   PFN_vkAllocateMemory AllocateMemory;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkCreateBuffer CreateBuffer;
   PFN_vkDestroyBuffer DestroyBuffer;
   PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
   PFN_vkBindBufferMemory BindBufferMemory;
   PFN_vkMapMemory MapMemory;
   PFN_vkUnmapMemory UnmapMemory;
   PFN_vkCreateImage CreateImage;
   PFN_vkDestroyImage DestroyImage;
   PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
   PFN_vkBindImageMemory BindImageMemory;
   PFN_vkCreateImageView CreateImageView;
   PFN_vkDestroyImageView DestroyImageView;
   PFN_vkCreateCommandPool CreateCommandPool;
   PFN_vkDestroyCommandPool DestroyCommandPool;
   PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
   PFN_vkResetCommandBuffer ResetCommandBuffer;
   PFN_vkBeginCommandBuffer BeginCommandBuffer;
   PFN_vkEndCommandBuffer EndCommandBuffer;
   PFN_vkCreateFence CreateFence;
   PFN_vkDestroyFence DestroyFence;
   PFN_vkGetFenceStatus GetFenceStatus;
   PFN_vkResetFences ResetFences;
   PFN_vkQueueSubmit QueueSubmit;
   PFN_vkCreateQueryPool CreateQueryPool;
   PFN_vkDestroyQueryPool DestroyQueryPool;
   PFN_vkGetQueryPoolResults GetQueryPoolResults;
   PFN_vkCmdResetQueryPool CmdResetQueryPool;
   PFN_vkCmdBeginQuery CmdBeginQuery;
   PFN_vkCmdEndQuery CmdEndQuery;
   PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2;
   PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
   PFN_vkCmdBeginVideoCodingKHR CmdBeginVideoCodingKHR;
   PFN_vkCmdControlVideoCodingKHR CmdControlVideoCodingKHR;
   PFN_vkCmdEncodeVideoKHR CmdEncodeVideoKHR;
   PFN_vkCmdEndVideoCodingKHR CmdEndVideoCodingKHR;
};

static bool vve_load_instance(vulkan_video_encoder_t *e)
{
   e->DestroyInstance = (PFN_vkDestroyInstance)e->GetInstanceProcAddr(e->instance, "vkDestroyInstance");
   if (!e->DestroyInstance)
      return false;

   e->EnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)e->GetInstanceProcAddr(e->instance, "vkEnumeratePhysicalDevices");
   if (!e->EnumeratePhysicalDevices)
      return false;

   e->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)e->GetInstanceProcAddr(e->instance, "vkGetPhysicalDeviceMemoryProperties");
   if (!e->GetPhysicalDeviceMemoryProperties)
      return false;

   e->GetPhysicalDeviceQueueFamilyProperties2 = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2)e->GetInstanceProcAddr(e->instance, "vkGetPhysicalDeviceQueueFamilyProperties2");
   if (!e->GetPhysicalDeviceQueueFamilyProperties2)
      return false;

   e->EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)e->GetInstanceProcAddr(e->instance, "vkEnumerateDeviceExtensionProperties");
   if (!e->EnumerateDeviceExtensionProperties)
      return false;

   e->GetPhysicalDeviceVideoCapabilitiesKHR = (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)e->GetInstanceProcAddr(e->instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
   if (!e->GetPhysicalDeviceVideoCapabilitiesKHR)
      return false;

   e->GetPhysicalDeviceVideoFormatPropertiesKHR = (PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR)e->GetInstanceProcAddr(e->instance, "vkGetPhysicalDeviceVideoFormatPropertiesKHR");
   if (!e->GetPhysicalDeviceVideoFormatPropertiesKHR)
      return false;

   e->GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)e->GetInstanceProcAddr(e->instance, "vkGetPhysicalDeviceProperties");
   if (!e->GetPhysicalDeviceProperties)
      return false;

   e->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)e->GetInstanceProcAddr(e->instance, "vkGetPhysicalDeviceFeatures2");
   if (!e->GetPhysicalDeviceFeatures2)
      return false;

   e->CreateDevice = (PFN_vkCreateDevice)e->GetInstanceProcAddr(e->instance, "vkCreateDevice");
   if (!e->CreateDevice)
      return false;

   e->GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)e->GetInstanceProcAddr(e->instance, "vkGetDeviceProcAddr");
   if (!e->GetDeviceProcAddr)
      return false;

   return true;
}

static bool vve_load_device(vulkan_video_encoder_t *e)
{
   e->DestroyDevice = (PFN_vkDestroyDevice)e->GetDeviceProcAddr(e->device, "vkDestroyDevice");
   if (!e->DestroyDevice)
      return false;

   e->CreateSemaphore = (PFN_vkCreateSemaphore)e->GetDeviceProcAddr(e->device, "vkCreateSemaphore");
   e->DestroySemaphore = (PFN_vkDestroySemaphore)e->GetDeviceProcAddr(e->device, "vkDestroySemaphore");
   if (!e->CreateSemaphore || !e->DestroySemaphore)
      return false;

   e->GetDeviceQueue = (PFN_vkGetDeviceQueue)e->GetDeviceProcAddr(e->device, "vkGetDeviceQueue");
   if (!e->GetDeviceQueue)
      return false;

   e->CreateVideoSessionKHR = (PFN_vkCreateVideoSessionKHR)e->GetDeviceProcAddr(e->device, "vkCreateVideoSessionKHR");
   if (!e->CreateVideoSessionKHR)
      return false;

   e->DestroyVideoSessionKHR = (PFN_vkDestroyVideoSessionKHR)e->GetDeviceProcAddr(e->device, "vkDestroyVideoSessionKHR");
   if (!e->DestroyVideoSessionKHR)
      return false;

   e->GetVideoSessionMemoryRequirementsKHR = (PFN_vkGetVideoSessionMemoryRequirementsKHR)e->GetDeviceProcAddr(e->device, "vkGetVideoSessionMemoryRequirementsKHR");
   if (!e->GetVideoSessionMemoryRequirementsKHR)
      return false;

   e->BindVideoSessionMemoryKHR = (PFN_vkBindVideoSessionMemoryKHR)e->GetDeviceProcAddr(e->device, "vkBindVideoSessionMemoryKHR");
   if (!e->BindVideoSessionMemoryKHR)
      return false;

   e->CreateVideoSessionParametersKHR = (PFN_vkCreateVideoSessionParametersKHR)e->GetDeviceProcAddr(e->device, "vkCreateVideoSessionParametersKHR");
   if (!e->CreateVideoSessionParametersKHR)
      return false;

   e->DestroyVideoSessionParametersKHR = (PFN_vkDestroyVideoSessionParametersKHR)e->GetDeviceProcAddr(e->device, "vkDestroyVideoSessionParametersKHR");
   if (!e->DestroyVideoSessionParametersKHR)
      return false;

   e->GetEncodedVideoSessionParametersKHR = (PFN_vkGetEncodedVideoSessionParametersKHR)e->GetDeviceProcAddr(e->device, "vkGetEncodedVideoSessionParametersKHR");
   if (!e->GetEncodedVideoSessionParametersKHR)
      return false;

   e->AllocateMemory = (PFN_vkAllocateMemory)e->GetDeviceProcAddr(e->device, "vkAllocateMemory");
   if (!e->AllocateMemory)
      return false;

   e->FreeMemory = (PFN_vkFreeMemory)e->GetDeviceProcAddr(e->device, "vkFreeMemory");
   if (!e->FreeMemory)
      return false;

   e->CreateBuffer = (PFN_vkCreateBuffer)e->GetDeviceProcAddr(e->device, "vkCreateBuffer");
   if (!e->CreateBuffer)
      return false;

   e->DestroyBuffer = (PFN_vkDestroyBuffer)e->GetDeviceProcAddr(e->device, "vkDestroyBuffer");
   if (!e->DestroyBuffer)
      return false;

   e->GetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)e->GetDeviceProcAddr(e->device, "vkGetBufferMemoryRequirements");
   if (!e->GetBufferMemoryRequirements)
      return false;

   e->BindBufferMemory = (PFN_vkBindBufferMemory)e->GetDeviceProcAddr(e->device, "vkBindBufferMemory");
   if (!e->BindBufferMemory)
      return false;

   e->MapMemory = (PFN_vkMapMemory)e->GetDeviceProcAddr(e->device, "vkMapMemory");
   if (!e->MapMemory)
      return false;

   e->UnmapMemory = (PFN_vkUnmapMemory)e->GetDeviceProcAddr(e->device, "vkUnmapMemory");
   if (!e->UnmapMemory)
      return false;

   e->CreateImage = (PFN_vkCreateImage)e->GetDeviceProcAddr(e->device, "vkCreateImage");
   if (!e->CreateImage)
      return false;

   e->DestroyImage = (PFN_vkDestroyImage)e->GetDeviceProcAddr(e->device, "vkDestroyImage");
   if (!e->DestroyImage)
      return false;

   e->GetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)e->GetDeviceProcAddr(e->device, "vkGetImageMemoryRequirements");
   if (!e->GetImageMemoryRequirements)
      return false;

   e->BindImageMemory = (PFN_vkBindImageMemory)e->GetDeviceProcAddr(e->device, "vkBindImageMemory");
   if (!e->BindImageMemory)
      return false;

   e->CreateImageView = (PFN_vkCreateImageView)e->GetDeviceProcAddr(e->device, "vkCreateImageView");
   if (!e->CreateImageView)
      return false;

   e->DestroyImageView = (PFN_vkDestroyImageView)e->GetDeviceProcAddr(e->device, "vkDestroyImageView");
   if (!e->DestroyImageView)
      return false;

   e->CreateCommandPool = (PFN_vkCreateCommandPool)e->GetDeviceProcAddr(e->device, "vkCreateCommandPool");
   if (!e->CreateCommandPool)
      return false;

   e->DestroyCommandPool = (PFN_vkDestroyCommandPool)e->GetDeviceProcAddr(e->device, "vkDestroyCommandPool");
   if (!e->DestroyCommandPool)
      return false;

   e->AllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)e->GetDeviceProcAddr(e->device, "vkAllocateCommandBuffers");
   if (!e->AllocateCommandBuffers)
      return false;

   e->ResetCommandBuffer = (PFN_vkResetCommandBuffer)e->GetDeviceProcAddr(e->device, "vkResetCommandBuffer");
   if (!e->ResetCommandBuffer)
      return false;

   e->BeginCommandBuffer = (PFN_vkBeginCommandBuffer)e->GetDeviceProcAddr(e->device, "vkBeginCommandBuffer");
   if (!e->BeginCommandBuffer)
      return false;

   e->EndCommandBuffer = (PFN_vkEndCommandBuffer)e->GetDeviceProcAddr(e->device, "vkEndCommandBuffer");
   if (!e->EndCommandBuffer)
      return false;

   e->CreateFence = (PFN_vkCreateFence)e->GetDeviceProcAddr(e->device, "vkCreateFence");
   if (!e->CreateFence)
      return false;

   e->DestroyFence = (PFN_vkDestroyFence)e->GetDeviceProcAddr(e->device, "vkDestroyFence");
   if (!e->DestroyFence)
      return false;

   e->GetFenceStatus = (PFN_vkGetFenceStatus)e->GetDeviceProcAddr(e->device, "vkGetFenceStatus");
   if (!e->GetFenceStatus)
      return false;

   e->ResetFences = (PFN_vkResetFences)e->GetDeviceProcAddr(e->device, "vkResetFences");
   if (!e->ResetFences)
      return false;

   e->QueueSubmit = (PFN_vkQueueSubmit)e->GetDeviceProcAddr(e->device, "vkQueueSubmit");
   if (!e->QueueSubmit)
      return false;

   e->CreateQueryPool = (PFN_vkCreateQueryPool)e->GetDeviceProcAddr(e->device, "vkCreateQueryPool");
   if (!e->CreateQueryPool)
      return false;

   e->DestroyQueryPool = (PFN_vkDestroyQueryPool)e->GetDeviceProcAddr(e->device, "vkDestroyQueryPool");
   if (!e->DestroyQueryPool)
      return false;

   e->GetQueryPoolResults = (PFN_vkGetQueryPoolResults)e->GetDeviceProcAddr(e->device, "vkGetQueryPoolResults");
   if (!e->GetQueryPoolResults)
      return false;

   e->CmdResetQueryPool = (PFN_vkCmdResetQueryPool)e->GetDeviceProcAddr(e->device, "vkCmdResetQueryPool");
   if (!e->CmdResetQueryPool)
      return false;

   e->CmdBeginQuery = (PFN_vkCmdBeginQuery)e->GetDeviceProcAddr(e->device, "vkCmdBeginQuery");
   if (!e->CmdBeginQuery)
      return false;

   e->CmdEndQuery = (PFN_vkCmdEndQuery)e->GetDeviceProcAddr(e->device, "vkCmdEndQuery");
   if (!e->CmdEndQuery)
      return false;

   e->CmdPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)e->GetDeviceProcAddr(e->device, "vkCmdPipelineBarrier2");
   if (!e->CmdPipelineBarrier2)
      return false;

   e->CmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)e->GetDeviceProcAddr(e->device, "vkCmdCopyBufferToImage");
   if (!e->CmdCopyBufferToImage)
      return false;

   e->CmdBeginVideoCodingKHR = (PFN_vkCmdBeginVideoCodingKHR)e->GetDeviceProcAddr(e->device, "vkCmdBeginVideoCodingKHR");
   if (!e->CmdBeginVideoCodingKHR)
      return false;

   e->CmdControlVideoCodingKHR = (PFN_vkCmdControlVideoCodingKHR)e->GetDeviceProcAddr(e->device, "vkCmdControlVideoCodingKHR");
   if (!e->CmdControlVideoCodingKHR)
      return false;

   e->CmdEncodeVideoKHR = (PFN_vkCmdEncodeVideoKHR)e->GetDeviceProcAddr(e->device, "vkCmdEncodeVideoKHR");
   if (!e->CmdEncodeVideoKHR)
      return false;

   e->CmdEndVideoCodingKHR = (PFN_vkCmdEndVideoCodingKHR)e->GetDeviceProcAddr(e->device, "vkCmdEndVideoCodingKHR");
   if (!e->CmdEndVideoCodingKHR)
      return false;

   return true;
}

static bool vve_allocate(vulkan_video_encoder_t *e,
      const VkMemoryRequirements *requirements, VkMemoryPropertyFlags flags,
      VkDeviceMemory *memory)
{
   VkMemoryAllocateInfo info;
   unsigned i;
   for (i = 0; i < e->memory.memoryTypeCount; i++)
      if ((requirements->memoryTypeBits & (1U << i))
            && (e->memory.memoryTypes[i].propertyFlags & flags) == flags)
         break;
   if (i == e->memory.memoryTypeCount)
      return false;

   info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   info.pNext = NULL;
   info.allocationSize = requirements->size;
   info.memoryTypeIndex = i;
   return e->AllocateMemory(e->device, &info, NULL, memory) == VK_SUCCESS;
}

static bool vve_buffer(vulkan_video_encoder_t *e, vve_buffer_t *buffer,
      VkDeviceSize size, VkBufferUsageFlags usage)
{
   VkBufferCreateInfo info;
   VkMemoryRequirements requirements;
   info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   info.pNext = (usage & VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR)
      ? &e->profiles : NULL;
   info.flags = 0;
   info.size = size;
   info.usage = usage;
   info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   info.queueFamilyIndexCount = 0;
   info.pQueueFamilyIndices = NULL;
   if (e->CreateBuffer(e->device, &info, NULL, &buffer->buffer) != VK_SUCCESS)
      return false;

   e->GetBufferMemoryRequirements(e->device, buffer->buffer, &requirements);
   if (!vve_allocate(e, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buffer->memory))
      return false;

   return e->BindBufferMemory(e->device, buffer->buffer, buffer->memory, 0)
         == VK_SUCCESS
      && e->MapMemory(e->device, buffer->memory, 0, VK_WHOLE_SIZE, 0,
            &buffer->mapped) == VK_SUCCESS;
}

static bool vve_image(vulkan_video_encoder_t *e, vve_image_t *image,
      VkImageUsageFlags usage)
{
   VkPhysicalDeviceVideoFormatInfoKHR query;
   VkVideoFormatPropertiesKHR *formats;
   VkImageCreateInfo info;
   uint32_t families[2];
   VkImageViewCreateInfo view;
   VkMemoryRequirements requirements;
   uint32_t count = 0, i;
   bool supported = false;
   query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
   query.pNext = &e->profiles;
   query.imageUsage = usage;
   if (e->GetPhysicalDeviceVideoFormatPropertiesKHR(e->gpu, &query,
            &count, NULL) != VK_SUCCESS || !count || count > 256)
      return false;

   formats = (VkVideoFormatPropertiesKHR*)calloc(count, sizeof(*formats));
   if (!formats)
      return false;

   for (i = 0; i < count; i++)
   {
      formats[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
      formats[i].pNext = NULL;
   }
   if (e->GetPhysicalDeviceVideoFormatPropertiesKHR(e->gpu, &query,
            &count, formats) == VK_SUCCESS)
      for (i = 0; i < count; i++)
         if (formats[i].format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
               && formats[i].imageType == VK_IMAGE_TYPE_2D
               && formats[i].imageTiling == VK_IMAGE_TILING_OPTIMAL)
            supported = true;
   free(formats);
   if (!supported)
      return false;

   info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   info.pNext = &e->profiles;
   info.flags = 0;
   info.imageType = VK_IMAGE_TYPE_2D;
   info.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   info.extent.width = e->width;
   info.extent.height = e->height;
   info.extent.depth = 1;
   info.mipLevels = 1;
   info.arrayLayers = 1;
   info.samples = VK_SAMPLE_COUNT_1_BIT;
   info.tiling = VK_IMAGE_TILING_OPTIMAL;
   info.usage = usage;
   info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   info.queueFamilyIndexCount = 0;
   info.pQueueFamilyIndices = NULL;
   families[0] = e->family;
   families[1] = e->upload_family;
   if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) && e->family != e->upload_family)
   {
      info.sharingMode = VK_SHARING_MODE_CONCURRENT;
      info.queueFamilyIndexCount = 2;
      info.pQueueFamilyIndices = families;
   }
   info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   if (e->CreateImage(e->device, &info, NULL, &image->image) != VK_SUCCESS)
      return false;

   e->GetImageMemoryRequirements(e->device, image->image, &requirements);
   if (!vve_allocate(e, &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            &image->memory)
         || e->BindImageMemory(e->device, image->image, image->memory, 0)
            != VK_SUCCESS)
      return false;

   view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   view.pNext = NULL;
   view.flags = 0;
   view.image = image->image;
   view.viewType = VK_IMAGE_VIEW_TYPE_2D;
   view.format = info.format;
   view.components.r = view.components.g = view.components.b
      = view.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
   view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   view.subresourceRange.baseMipLevel = 0;
   view.subresourceRange.levelCount = 1;
   view.subresourceRange.baseArrayLayer = 0;
   view.subresourceRange.layerCount = 1;
   return e->CreateImageView(e->device, &view, NULL, &image->view) == VK_SUCCESS;
}

static bool vve_select(vulkan_video_encoder_t *e, VkPhysicalDevice gpu)
{
   VkExtensionProperties *extensions;
   VkQueueFamilyProperties2 *families;
   VkQueueFamilyVideoPropertiesKHR *video;
   VkPhysicalDeviceSynchronization2Features sync;
   VkPhysicalDeviceFeatures2 features;
   VkPhysicalDeviceProperties properties;
   uint32_t count = 0, i, found = 0;
   bool selected = false;
   e->GetPhysicalDeviceProperties(gpu, &properties);
   if (properties.apiVersion < VK_API_VERSION_1_3)
      return false;

   if (e->EnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL)
         != VK_SUCCESS || !count || count > 4096)
      return false;

   extensions = (VkExtensionProperties*)calloc(count, sizeof(*extensions));
   if (!extensions)
      return false;

   if (e->EnumerateDeviceExtensionProperties(gpu, NULL, &count, extensions)
         == VK_SUCCESS)
      for (i = 0; i < count; i++)
      {
         if (!strcmp(extensions[i].extensionName, "VK_KHR_video_queue"))
            found |= 1;
         if (!strcmp(extensions[i].extensionName, "VK_KHR_video_encode_queue"))
            found |= 2;
         if (!strcmp(extensions[i].extensionName, e->hevc
                  ? "VK_KHR_video_encode_h265" : "VK_KHR_video_encode_h264"))
            found |= 4;
      }
   free(extensions);
   if (found != 7)
      return false;

   sync.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
   sync.pNext = NULL;
   features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
   features.pNext = &sync;
   e->GetPhysicalDeviceFeatures2(gpu, &features);
   if (!sync.synchronization2)
      return false;

   e->GetPhysicalDeviceQueueFamilyProperties2(gpu, &count, NULL);
   if (!count || count > 256)
      return false;

   families = (VkQueueFamilyProperties2*)calloc(count, sizeof(*families));
   video = (VkQueueFamilyVideoPropertiesKHR*)calloc(count, sizeof(*video));
   if (!families || !video)
   {
      free(families);
      free(video);
      return false;
   }

   for (i = 0; i < count; i++)
   {
      families[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
      families[i].pNext = &video[i];
      video[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
      video[i].pNext = NULL;
   }
   e->GetPhysicalDeviceQueueFamilyProperties2(gpu, &count, families);
   for (i = 0; i < count; i++)
      if (families[i].queueFamilyProperties.queueCount
            && (video[i].videoCodecOperations & e->profile.videoCodecOperation))
      {
         e->family = i;
         selected = true;
         break;
      }
   e->upload_family = UINT32_MAX;
   for (i = 0; i < count; i++)
      if (families[i].queueFamilyProperties.queueCount
            && (families[i].queueFamilyProperties.queueFlags & VK_QUEUE_TRANSFER_BIT))
      {
         e->upload_family = i;
         break;
      }
   free(families);
   free(video);
   if (!selected || e->upload_family == UINT32_MAX)
      return false;

   e->caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
   e->caps.pNext = &e->encode_caps;
   e->encode_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;
   e->encode_caps.pNext = e->hevc ? (void*)&e->h265_caps : (void*)&e->h264_caps;
   e->h265_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR;
   e->h265_caps.pNext = NULL;
   e->h264_caps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR;
   e->h264_caps.pNext = NULL;
   if (e->GetPhysicalDeviceVideoCapabilitiesKHR(gpu, &e->profile, &e->caps)
            != VK_SUCCESS
         || e->width < e->caps.minCodedExtent.width
         || e->height < e->caps.minCodedExtent.height
         || e->width > e->caps.maxCodedExtent.width
         || e->height > e->caps.maxCodedExtent.height
         || !e->caps.maxDpbSlots || !e->caps.maxActiveReferencePictures
         || !e->encode_caps.maxRateControlLayers
         || e->bitrate > e->encode_caps.maxBitrate
         || !(e->encode_caps.supportedEncodeFeedbackFlags
               & VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR)
         || !(e->encode_caps.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR))
      return false;

   e->gpu = gpu;
   e->GetPhysicalDeviceMemoryProperties(gpu, &e->memory);
   return true;
}

static bool vve_device(vulkan_video_encoder_t *e)
{
   const char *extensions[] = { "VK_KHR_video_queue",
      "VK_KHR_video_encode_queue", "VK_KHR_video_encode_h264" };
   VkPhysicalDevice *devices;
   VkPhysicalDeviceSynchronization2Features sync;
   VkDeviceQueueCreateInfo queues[2];
   VkDeviceCreateInfo info;
   uint32_t count = 0, i;
   float priority = 1.0f;
   if (e->EnumeratePhysicalDevices(e->instance, &count, NULL) != VK_SUCCESS
         || !count || count > 256)
      return false;

   devices = (VkPhysicalDevice*)calloc(count, sizeof(*devices));
   if (!devices)
      return false;

   if (e->EnumeratePhysicalDevices(e->instance, &count, devices) == VK_SUCCESS)
      for (i = 0; i < count; i++)
         if (vve_select(e, devices[i]))
            break;
   free(devices);
   if (!e->gpu)
      return false;

   sync.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
   sync.pNext = NULL;
   sync.synchronization2 = VK_TRUE;
   for (i = 0; i < 2; i++)
   {
      queues[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
      queues[i].pNext = NULL;
      queues[i].flags = 0;
      queues[i].queueFamilyIndex = i ? e->upload_family : e->family;
      queues[i].queueCount = 1;
      queues[i].pQueuePriorities = &priority;
   }
   info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
   info.pNext = &sync;
   info.flags = 0;
   info.queueCreateInfoCount = e->family == e->upload_family ? 1 : 2;
   info.pQueueCreateInfos = queues;
   info.enabledLayerCount = 0;
   info.ppEnabledLayerNames = NULL;
   info.enabledExtensionCount = 3;
   extensions[2] = e->hevc ? "VK_KHR_video_encode_h265" : "VK_KHR_video_encode_h264";
   info.ppEnabledExtensionNames = extensions;
   info.pEnabledFeatures = NULL;
   if (e->CreateDevice(e->gpu, &info, NULL, &e->device) != VK_SUCCESS
         || !vve_load_device(e))
      return false;

   e->GetDeviceQueue(e->device, e->family, 0, &e->queue);
   e->GetDeviceQueue(e->device, e->upload_family, 0, &e->upload_queue);
   return true;
}

static bool vve_session(vulkan_video_encoder_t *e)
{
   VkVideoSessionCreateInfoKHR info;
   VkVideoSessionMemoryRequirementsKHR *requirements;
   VkBindVideoSessionMemoryInfoKHR *bindings;
   uint32_t count = 0, i;
   bool ok = false;
   info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
   info.pNext = NULL;
   info.queueFamilyIndex = e->family;
   info.flags = 0;
   info.pVideoProfile = &e->profile;
   info.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
   info.maxCodedExtent.width = e->width;
   info.maxCodedExtent.height = e->height;
   info.referencePictureFormat = info.pictureFormat;
   info.maxDpbSlots = 1;
   info.maxActiveReferencePictures = 1;
   info.pStdHeaderVersion = &e->caps.stdHeaderVersion;
   if (e->CreateVideoSessionKHR(e->device, &info, NULL, &e->session) != VK_SUCCESS
         || e->GetVideoSessionMemoryRequirementsKHR(e->device, e->session,
               &count, NULL) != VK_SUCCESS || count > 16)
      return false;

   requirements = (VkVideoSessionMemoryRequirementsKHR*)calloc(
         count ? count : 1, sizeof(*requirements));
   bindings = (VkBindVideoSessionMemoryInfoKHR*)calloc(
         count ? count : 1, sizeof(*bindings));
   if (!requirements || !bindings)
      goto done;
   for (i = 0; i < count; i++)
   {
      requirements[i].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
      requirements[i].pNext = NULL;
   }
   if (e->GetVideoSessionMemoryRequirementsKHR(e->device, e->session,
            &count, requirements) != VK_SUCCESS)
      goto done;
   for (i = 0; i < count; i++)
   {
      if (!vve_allocate(e, &requirements[i].memoryRequirements, 0,
               &e->session_memory[i]))
         goto done;
      e->session_memory_count++;
      bindings[i].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
      bindings[i].pNext = NULL;
      bindings[i].memoryBindIndex = requirements[i].memoryBindIndex;
      bindings[i].memory = e->session_memory[i];
      bindings[i].memoryOffset = 0;
      bindings[i].memorySize = requirements[i].memoryRequirements.size;
   }
   ok = e->BindVideoSessionMemoryKHR(e->device, e->session, count, bindings)
      == VK_SUCCESS;
done:
   free(requirements);
   free(bindings);
   return ok;
}

static bool vve_parameters_h265(vulkan_video_encoder_t *e)
{
   StdVideoH265VideoParameterSet vps;
   StdVideoH265SequenceParameterSet sps;
   StdVideoH265PictureParameterSet pps;
   StdVideoH265ProfileTierLevel profile;
   StdVideoH265DecPicBufMgr dpb;
   StdVideoH265SequenceParameterSetVui vui;
   VkVideoEncodeH265SessionParametersAddInfoKHR add;
   VkVideoEncodeH265SessionParametersCreateInfoKHR h265;
   VkVideoSessionParametersCreateInfoKHR info;
   unsigned ctb;
   memset(&vps, 0, sizeof(vps));
   memset(&sps, 0, sizeof(sps));
   memset(&pps, 0, sizeof(pps));
   memset(&profile, 0, sizeof(profile));
   memset(&dpb, 0, sizeof(dpb));
   memset(&vui, 0, sizeof(vui));
   if (e->h265_caps.ctbSizes & VK_VIDEO_ENCODE_H265_CTB_SIZE_64_BIT_KHR)
      ctb = 3;
   else if (e->h265_caps.ctbSizes & VK_VIDEO_ENCODE_H265_CTB_SIZE_32_BIT_KHR)
      ctb = 2;
   else if (e->h265_caps.ctbSizes & VK_VIDEO_ENCODE_H265_CTB_SIZE_16_BIT_KHR)
      ctb = 1;
   else
      return false;

   profile.general_profile_idc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
   profile.general_level_idc = e->h265_caps.maxLevelIdc;
   profile.flags.general_progressive_source_flag = 1;
   profile.flags.general_frame_only_constraint_flag = 1;
   dpb.max_dec_pic_buffering_minus1[0] = 1;
   vps.flags.vps_temporal_id_nesting_flag = 1;
   vps.pProfileTierLevel = &profile;
   vps.pDecPicBufMgr = &dpb;
   sps.flags.sps_temporal_id_nesting_flag = 1;
   sps.flags.strong_intra_smoothing_enabled_flag = 1;
   sps.flags.vui_parameters_present_flag = 1;
   sps.chroma_format_idc = STD_VIDEO_H265_CHROMA_FORMAT_IDC_420;
   sps.pic_width_in_luma_samples = e->width;
   sps.pic_height_in_luma_samples = e->height;
   sps.log2_diff_max_min_luma_coding_block_size = (uint8_t)ctb;
   sps.log2_diff_max_min_luma_transform_block_size = 3;
   sps.max_transform_hierarchy_depth_inter = 2;
   sps.max_transform_hierarchy_depth_intra = 2;
   sps.pProfileTierLevel = &profile;
   sps.pDecPicBufMgr = &dpb;
   sps.pSequenceParameterSetVui = &vui;
   vui.flags.video_signal_type_present_flag = 1;
   vui.flags.colour_description_present_flag = 1;
   vui.video_format = 5;
   vui.colour_primaries = vui.transfer_characteristics = vui.matrix_coeffs = 1;
   pps.flags.deblocking_filter_control_present_flag = 1;
   add.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR;
   add.pNext = NULL;
   add.stdVPSCount = add.stdSPSCount = add.stdPPSCount = 1;
   add.pStdVPSs = &vps;
   add.pStdSPSs = &sps;
   add.pStdPPSs = &pps;
   h265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR;
   h265.pNext = NULL;
   h265.maxStdVPSCount = h265.maxStdSPSCount = h265.maxStdPPSCount = 1;
   h265.pParametersAddInfo = &add;
   info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
   info.pNext = &h265;
   info.flags = 0;
   info.videoSessionParametersTemplate = VK_NULL_HANDLE;
   info.videoSession = e->session;
   return e->CreateVideoSessionParametersKHR(e->device, &info, NULL,
         &e->parameters) == VK_SUCCESS;
}

static bool vve_parameters(vulkan_video_encoder_t *e)
{
   StdVideoH264SequenceParameterSet sps;
   StdVideoH264PictureParameterSet pps;
   StdVideoH264SequenceParameterSetVui vui;
   VkVideoEncodeH264SessionParametersAddInfoKHR add;
   VkVideoEncodeH264SessionParametersCreateInfoKHR h264;
   VkVideoSessionParametersCreateInfoKHR info;
   memset(&sps, 0, sizeof(sps));
   memset(&pps, 0, sizeof(pps));
   memset(&vui, 0, sizeof(vui));
   sps.flags.frame_mbs_only_flag = 1;
   sps.flags.direct_8x8_inference_flag = 1;
   sps.flags.vui_parameters_present_flag = 1;
   sps.profile_idc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
   sps.level_idc = e->h264_caps.maxLevelIdc;
   sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
   sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
   sps.pic_width_in_mbs_minus1 = e->width / 16 - 1;
   sps.pic_height_in_map_units_minus1 = e->height / 16 - 1;
   sps.max_num_ref_frames = 1;
   sps.pSequenceParameterSetVui = &vui;
   vui.flags.video_signal_type_present_flag = 1;
   vui.flags.color_description_present_flag = 1;
   vui.video_format = 5;
   vui.colour_primaries = 1;
   vui.transfer_characteristics = 1;
   vui.matrix_coefficients = 1;
   pps.flags.entropy_coding_mode_flag = 1;
   pps.flags.deblocking_filter_control_present_flag = 1;
   add.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR;
   add.pNext = NULL;
   add.stdSPSCount = 1;
   add.pStdSPSs = &sps;
   add.stdPPSCount = 1;
   add.pStdPPSs = &pps;
   h264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR;
   h264.pNext = NULL;
   h264.maxStdSPSCount = 1;
   h264.maxStdPPSCount = 1;
   h264.pParametersAddInfo = &add;
   info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
   info.pNext = &h264;
   info.flags = 0;
   info.videoSessionParametersTemplate = VK_NULL_HANDLE;
   info.videoSession = e->session;
   return e->CreateVideoSessionParametersKHR(e->device, &info, NULL,
         &e->parameters) == VK_SUCCESS;
}

static bool vve_commands(vulkan_video_encoder_t *e)
{
   VkCommandPoolCreateInfo pool;
   VkCommandBufferAllocateInfo allocate;
   VkFenceCreateInfo fence;
   VkSemaphoreCreateInfo semaphore;
   VkQueryPoolVideoEncodeFeedbackCreateInfoKHR feedback;
   VkQueryPoolCreateInfo query;
   pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   pool.pNext = NULL;
   pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
   pool.queueFamilyIndex = e->family;
   if (e->CreateCommandPool(e->device, &pool, NULL, &e->pool) != VK_SUCCESS)
      return false;

   allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   allocate.pNext = NULL;
   allocate.commandPool = e->pool;
   allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   allocate.commandBufferCount = 1;
   if (e->AllocateCommandBuffers(e->device, &allocate, &e->command) != VK_SUCCESS)
      return false;

   pool.queueFamilyIndex = e->upload_family;
   if (e->CreateCommandPool(e->device, &pool, NULL, &e->upload_pool) != VK_SUCCESS)
      return false;

   allocate.commandPool = e->upload_pool;
   if (e->AllocateCommandBuffers(e->device, &allocate, &e->upload_command) != VK_SUCCESS)
      return false;

   semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
   semaphore.pNext = NULL;
   semaphore.flags = 0;
   if (e->CreateSemaphore(e->device, &semaphore, NULL, &e->upload_ready) != VK_SUCCESS)
      return false;

   fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
   fence.pNext = NULL;
   fence.flags = 0;
   if (e->CreateFence(e->device, &fence, NULL, &e->fence) != VK_SUCCESS)
      return false;

   if (e->CreateFence(e->device, &fence, NULL, &e->upload_fence) != VK_SUCCESS)
      return false;

   feedback.sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR;
   feedback.pNext = &e->profile;
   feedback.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
   query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
   query.pNext = &feedback;
   query.flags = 0;
   query.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
   query.queryCount = 1;
   query.pipelineStatistics = 0;
   return e->CreateQueryPool(e->device, &query, NULL, &e->query) == VK_SUCCESS;
}

bool vulkan_video_encoder_headers(vulkan_video_encoder_t *e,
      void *data, size_t capacity, size_t *size)
{
   VkVideoEncodeH264SessionParametersGetInfoKHR h264;
   VkVideoEncodeH265SessionParametersGetInfoKHR h265;
   VkVideoEncodeSessionParametersGetInfoKHR info;
   VkVideoEncodeSessionParametersFeedbackInfoKHR feedback;
   if (!e || !data || !size)
      return false;

   h264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR;
   h264.pNext = NULL;
   h264.writeStdSPS = h264.writeStdPPS = VK_TRUE;
   h264.stdSPSId = h264.stdPPSId = 0;
   info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR;
   memset(&h265, 0, sizeof(h265));
   h265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_KHR;
   h265.writeStdVPS = h265.writeStdSPS = h265.writeStdPPS = VK_TRUE;
   info.pNext = e->hevc ? (void*)&h265 : (void*)&h264;
   info.videoSessionParameters = e->parameters;
   feedback.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR;
   feedback.pNext = NULL;
   *size = capacity;
   return e->GetEncodedVideoSessionParametersKHR(e->device, &info,
         &feedback, size, data) == VK_SUCCESS;
}

vulkan_video_encoder_t *vulkan_video_encoder_new(unsigned width,
      unsigned height, unsigned bitrate, unsigned fps, bool hevc)
{
   vulkan_video_encoder_t *e;
   PFN_vkCreateInstance create_instance;
   VkApplicationInfo app;
   VkInstanceCreateInfo info;
   if (!width || !height || width > 4096 || height > 4096
         || (width & 15) || (height & 15) || !fps || fps > 144)
      return NULL;

   e = (vulkan_video_encoder_t*)calloc(1, sizeof(*e));
   if (!e)
      return NULL;

   e->hevc = hevc;
   e->width = width;
   e->height = height;
   e->fps = fps;
   e->bitrate = bitrate;
#ifdef _WIN32
   e->library = dylib_load("vulkan-1.dll");
#else
   e->library = dylib_load("libvulkan.so.1");
#endif
   if (!e->library)
      goto error;
   e->GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
      dylib_proc(e->library, "vkGetInstanceProcAddr");
   if (!e->GetInstanceProcAddr)
      goto error;
   create_instance = (PFN_vkCreateInstance)e->GetInstanceProcAddr(
         VK_NULL_HANDLE, "vkCreateInstance");
   if (!create_instance)
      goto error;
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.pNext = NULL;
   app.pApplicationName = "RetroArch";
   app.applicationVersion = 0;
   app.pEngineName = NULL;
   app.engineVersion = 0;
   app.apiVersion = VK_API_VERSION_1_3;
   info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   info.pNext = NULL;
   info.flags = 0;
   info.pApplicationInfo = &app;
   info.enabledLayerCount = 0;
   info.ppEnabledLayerNames = NULL;
   info.enabledExtensionCount = 0;
   info.ppEnabledExtensionNames = NULL;
   if (create_instance(&info, NULL, &e->instance) != VK_SUCCESS
         || !vve_load_instance(e))
      goto error;
   e->h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR;
   e->h264_profile.pNext = NULL;
   e->h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
   e->profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
   e->h265_profile.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR;
   e->h265_profile.pNext = NULL;
   e->h265_profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
   e->profile.pNext = hevc ? (void*)&e->h265_profile : (void*)&e->h264_profile;
   e->profile.videoCodecOperation = hevc ? VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR
      : VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
   e->profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
   e->profile.lumaBitDepth = e->profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
   e->profiles.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
   e->profiles.pNext = NULL;
   e->profiles.profileCount = 1;
   e->profiles.pProfiles = &e->profile;
   if (!vve_device(e) || !vve_session(e) || !(e->hevc ? vve_parameters_h265(e) : vve_parameters(e))
         || !vve_buffer(e, &e->upload, (VkDeviceSize)width * height * 3 / 2,
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT)
         || !vve_buffer(e, &e->output, VVE_BUFFER_SIZE,
               VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR)
         || !vve_image(e, &e->input, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR
               | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
         || !vve_image(e, &e->dpb, VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR)
         || !vve_commands(e))
      goto error;
   return e;

error:
   vulkan_video_encoder_free(e);
   return NULL;
}

static void vve_barrier(vulkan_video_encoder_t *e, VkCommandBuffer command, VkImage image,
      VkImageLayout old_layout, VkImageLayout new_layout,
      VkPipelineStageFlags2 source_stage, VkAccessFlags2 source_access,
      VkPipelineStageFlags2 target_stage, VkAccessFlags2 target_access)
{
   VkImageMemoryBarrier2 barrier;
   VkDependencyInfo dependency;
   barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
   barrier.pNext = NULL;
   barrier.srcStageMask = source_stage;
   barrier.srcAccessMask = source_access;
   barrier.dstStageMask = target_stage;
   barrier.dstAccessMask = target_access;
   barrier.oldLayout = old_layout;
   barrier.newLayout = new_layout;
   barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   barrier.image = image;
   barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   barrier.subresourceRange.baseMipLevel = 0;
   barrier.subresourceRange.levelCount = 1;
   barrier.subresourceRange.baseArrayLayer = 0;
   barrier.subresourceRange.layerCount = 1;
   dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
   dependency.pNext = NULL;
   dependency.dependencyFlags = 0;
   dependency.memoryBarrierCount = 0;
   dependency.pMemoryBarriers = NULL;
   dependency.bufferMemoryBarrierCount = 0;
   dependency.pBufferMemoryBarriers = NULL;
   dependency.imageMemoryBarrierCount = 1;
   dependency.pImageMemoryBarriers = &barrier;
   e->CmdPipelineBarrier2(command, &dependency);
}

static void vve_upload(vulkan_video_encoder_t *e)
{
   VkBufferImageCopy copy[2];
   unsigned i;
   vve_barrier(e, e->upload_command, e->input.image,
         e->initialized ? VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_PIPELINE_STAGE_2_NONE, 0,
         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
   for (i = 0; i < 2; i++)
   {
      copy[i].bufferOffset = i ? (VkDeviceSize)e->width * e->height : 0;
      copy[i].bufferRowLength = 0;
      copy[i].bufferImageHeight = 0;
      copy[i].imageSubresource.aspectMask = i ? VK_IMAGE_ASPECT_PLANE_1_BIT
         : VK_IMAGE_ASPECT_PLANE_0_BIT;
      copy[i].imageSubresource.mipLevel = 0;
      copy[i].imageSubresource.baseArrayLayer = 0;
      copy[i].imageSubresource.layerCount = 1;
      copy[i].imageOffset.x = copy[i].imageOffset.y = copy[i].imageOffset.z = 0;
      copy[i].imageExtent.width = e->width >> i;
      copy[i].imageExtent.height = e->height >> i;
      copy[i].imageExtent.depth = 1;
   }
   e->CmdCopyBufferToImage(e->upload_command, e->upload.buffer, e->input.image,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, copy);
   vve_barrier(e, e->upload_command, e->input.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
         VK_PIPELINE_STAGE_2_NONE, 0);

}

static void vve_encode(vulkan_video_encoder_t *e)
{
   VkVideoBeginCodingInfoKHR begin;
   VkVideoEndCodingInfoKHR end;
   VkVideoCodingControlInfoKHR control;
   VkVideoEncodeRateControlInfoKHR rate;
   VkVideoEncodeRateControlLayerInfoKHR layer;
   VkVideoEncodeH264RateControlInfoKHR h264_rate;
   VkVideoEncodeH264GopRemainingFrameInfoKHR remaining;
   VkVideoPictureResourceInfoKHR reconstructed;
   VkVideoReferenceSlotInfoKHR slot;
   VkVideoEncodeH264DpbSlotInfoKHR dpb;
   StdVideoEncodeH264ReferenceInfo reference;
   StdVideoEncodeH264PictureInfo picture;
   StdVideoEncodeH264SliceHeader slice;
   VkVideoEncodeH264NaluSliceInfoKHR nalu;
   VkVideoEncodeH264PictureInfoKHR h264;
   VkVideoEncodeInfoKHR info;
   memset(&reference, 0, sizeof(reference));
   memset(&picture, 0, sizeof(picture));
   memset(&slice, 0, sizeof(slice));
   reference.primary_pic_type = STD_VIDEO_H264_PICTURE_TYPE_IDR;
   dpb.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR;
   dpb.pNext = NULL;
   dpb.pStdReferenceInfo = &reference;
   reconstructed.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
   reconstructed.pNext = NULL;
   reconstructed.codedOffset.x = reconstructed.codedOffset.y = 0;
   reconstructed.codedExtent.width = e->width;
   reconstructed.codedExtent.height = e->height;
   reconstructed.baseArrayLayer = 0;
   reconstructed.imageViewBinding = e->dpb.view;
   slot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
   slot.pNext = &dpb;
   slot.slotIndex = -1;
   slot.pPictureResource = &reconstructed;
   layer.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR;
   layer.pNext = NULL;
   layer.averageBitrate = layer.maxBitrate = e->bitrate;
   layer.frameRateNumerator = e->fps;
   layer.frameRateDenominator = 1;
   h264_rate.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR;
   h264_rate.pNext = NULL;
   h264_rate.flags = VK_VIDEO_ENCODE_H264_RATE_CONTROL_REGULAR_GOP_BIT_KHR;
   h264_rate.gopFrameCount = 1;
   h264_rate.idrPeriod = 1;
   h264_rate.consecutiveBFrameCount = 0;
   h264_rate.temporalLayerCount = 1;
   rate.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR;
   rate.pNext = &h264_rate;
   rate.flags = 0;
   rate.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
   rate.layerCount = 1;
   rate.pLayers = &layer;
   rate.virtualBufferSizeInMs = 100;
   rate.initialVirtualBufferSizeInMs = 50;
   begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
   remaining.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_GOP_REMAINING_FRAME_INFO_KHR;
   remaining.pNext = e->initialized ? &rate : NULL;
   remaining.useGopRemainingFrames = VK_TRUE;
   remaining.gopRemainingI = 1;
   remaining.gopRemainingP = 0;
   remaining.gopRemainingB = 0;
   begin.pNext = &remaining;
   begin.flags = 0;
   begin.videoSession = e->session;
   begin.videoSessionParameters = e->parameters;
   begin.referenceSlotCount = 1;
   begin.pReferenceSlots = &slot;
   vve_barrier(e, e->command, e->dpb.image,
         e->initialized ? VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
         VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
         VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR);
   e->CmdBeginVideoCodingKHR(e->command, &begin);
   if (!e->initialized)
   {
      control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
      control.pNext = &rate;
      control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR
         | VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
      e->CmdControlVideoCodingKHR(e->command, &control);
   }
   picture.flags.IdrPicFlag = 1;
   picture.flags.is_reference = 1;
   picture.flags.no_output_of_prior_pics_flag = 1;
   picture.primary_pic_type = STD_VIDEO_H264_PICTURE_TYPE_IDR;
   picture.idr_pic_id = (uint16_t)(e->picture_id & 1);
   slice.slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
   nalu.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR;
   nalu.pNext = NULL;
   nalu.constantQp = 0;
   nalu.pStdSliceHeader = &slice;
   h264.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR;
   h264.pNext = NULL;
   h264.naluSliceEntryCount = 1;
   h264.pNaluSliceEntries = &nalu;
   h264.pStdPictureInfo = &picture;
   h264.generatePrefixNalu = VK_FALSE;
   slot.slotIndex = 0;
   info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;
   info.pNext = &h264;
   info.flags = 0;
   info.dstBuffer = e->output.buffer;
   info.dstBufferOffset = 0;
   info.dstBufferRange = VVE_BUFFER_SIZE;
   info.srcPictureResource = reconstructed;
   info.srcPictureResource.imageViewBinding = e->input.view;
   info.pSetupReferenceSlot = &slot;
   info.referenceSlotCount = 0;
   info.pReferenceSlots = NULL;
   info.precedingExternallyEncodedBytes = 0;
   e->CmdBeginQuery(e->command, e->query, 0, 0);
   e->CmdEncodeVideoKHR(e->command, &info);
   e->CmdEndQuery(e->command, e->query, 0);
   end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
   end.pNext = NULL;
   end.flags = 0;
   e->CmdEndVideoCodingKHR(e->command, &end);
}

static void vve_encode_h265(vulkan_video_encoder_t *e)
{
   VkVideoBeginCodingInfoKHR begin;
   VkVideoEndCodingInfoKHR end;
   VkVideoCodingControlInfoKHR control;
   VkVideoEncodeRateControlInfoKHR rate;
   VkVideoEncodeRateControlLayerInfoKHR layer;
   VkVideoEncodeH265RateControlInfoKHR h265_rate;
   VkVideoEncodeH265GopRemainingFrameInfoKHR remaining;
   VkVideoPictureResourceInfoKHR reconstructed;
   VkVideoReferenceSlotInfoKHR slot;
   VkVideoEncodeH265DpbSlotInfoKHR dpb;
   StdVideoEncodeH265ReferenceInfo reference;
   StdVideoEncodeH265PictureInfo picture;
   StdVideoEncodeH265SliceSegmentHeader slice;
   VkVideoEncodeH265NaluSliceSegmentInfoKHR nalu;
   VkVideoEncodeH265PictureInfoKHR h265;
   VkVideoEncodeInfoKHR info;
   memset(&reference, 0, sizeof(reference));
   memset(&picture, 0, sizeof(picture));
   memset(&slice, 0, sizeof(slice));
   reference.pic_type = STD_VIDEO_H265_PICTURE_TYPE_IDR;
   dpb.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR;
   dpb.pNext = NULL;
   dpb.pStdReferenceInfo = &reference;
   reconstructed.sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
   reconstructed.pNext = NULL;
   reconstructed.codedOffset.x = reconstructed.codedOffset.y = 0;
   reconstructed.codedExtent.width = e->width;
   reconstructed.codedExtent.height = e->height;
   reconstructed.baseArrayLayer = 0;
   reconstructed.imageViewBinding = e->dpb.view;
   slot.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
   slot.pNext = &dpb;
   slot.slotIndex = -1;
   slot.pPictureResource = &reconstructed;
   layer.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR;
   layer.pNext = NULL;
   layer.averageBitrate = layer.maxBitrate = e->bitrate;
   layer.frameRateNumerator = e->fps;
   layer.frameRateDenominator = 1;
   h265_rate.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR;
   h265_rate.pNext = NULL;
   h265_rate.flags = VK_VIDEO_ENCODE_H265_RATE_CONTROL_REGULAR_GOP_BIT_KHR;
   h265_rate.gopFrameCount = 1;
   h265_rate.idrPeriod = 1;
   h265_rate.consecutiveBFrameCount = 0;
   h265_rate.subLayerCount = 1;
   rate.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR;
   rate.pNext = &h265_rate;
   rate.flags = 0;
   rate.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
   rate.layerCount = 1;
   rate.pLayers = &layer;
   rate.virtualBufferSizeInMs = 100;
   rate.initialVirtualBufferSizeInMs = 50;
   begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
   remaining.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_GOP_REMAINING_FRAME_INFO_KHR;
   remaining.pNext = e->initialized ? &rate : NULL;
   remaining.useGopRemainingFrames = VK_TRUE;
   remaining.gopRemainingI = 1;
   remaining.gopRemainingP = 0;
   remaining.gopRemainingB = 0;
   begin.pNext = &remaining;
   begin.flags = 0;
   begin.videoSession = e->session;
   begin.videoSessionParameters = e->parameters;
   begin.referenceSlotCount = 1;
   begin.pReferenceSlots = &slot;
   vve_barrier(e, e->command, e->dpb.image,
         e->initialized ? VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
         VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR,
         VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR, VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR);
   e->CmdBeginVideoCodingKHR(e->command, &begin);
   if (!e->initialized)
   {
      control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
      control.pNext = &rate;
      control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR
         | VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
      e->CmdControlVideoCodingKHR(e->command, &control);
   }
   picture.flags.IrapPicFlag = 1;
   picture.flags.is_reference = 1;
   picture.flags.no_output_of_prior_pics_flag = 1;
   picture.pic_type = STD_VIDEO_H265_PICTURE_TYPE_IDR;
   picture.flags.pic_output_flag = 1;
   slice.slice_type = STD_VIDEO_H265_SLICE_TYPE_I;
   slice.flags.first_slice_segment_in_pic_flag = 1;
   slice.MaxNumMergeCand = 5;
   nalu.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_KHR;
   nalu.pNext = NULL;
   nalu.constantQp = 0;
   nalu.pStdSliceSegmentHeader = &slice;
   h265.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR;
   h265.pNext = NULL;
   h265.naluSliceSegmentEntryCount = 1;
   h265.pNaluSliceSegmentEntries = &nalu;
   h265.pStdPictureInfo = &picture;
   slot.slotIndex = 0;
   info.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR;
   info.pNext = &h265;
   info.flags = 0;
   info.dstBuffer = e->output.buffer;
   info.dstBufferOffset = 0;
   info.dstBufferRange = VVE_BUFFER_SIZE;
   info.srcPictureResource = reconstructed;
   info.srcPictureResource.imageViewBinding = e->input.view;
   info.pSetupReferenceSlot = &slot;
   info.referenceSlotCount = 0;
   info.pReferenceSlots = NULL;
   info.precedingExternallyEncodedBytes = 0;
   e->CmdBeginQuery(e->command, e->query, 0, 0);
   e->CmdEncodeVideoKHR(e->command, &info);
   e->CmdEndQuery(e->command, e->query, 0);
   end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
   end.pNext = NULL;
   end.flags = 0;
   e->CmdEndVideoCodingKHR(e->command, &end);
}

bool vulkan_video_encoder_submit(vulkan_video_encoder_t *e,
      const uint8_t *pixels, bool keyframe)
{
   VkCommandBufferBeginInfo begin;
   VkSubmitInfo submit;
   VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
   (void)keyframe;
   if (!e || !pixels || e->pending || e->failed)
      return false;

   memcpy(e->upload.mapped, pixels, (size_t)e->width * e->height * 3 / 2);
   if (e->ResetFences(e->device, 1, &e->fence) != VK_SUCCESS
         || e->ResetCommandBuffer(e->command, 0) != VK_SUCCESS
         || e->ResetFences(e->device, 1, &e->upload_fence) != VK_SUCCESS
         || e->ResetCommandBuffer(e->upload_command, 0) != VK_SUCCESS)
      goto error;
   begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   begin.pNext = NULL;
   begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   begin.pInheritanceInfo = NULL;
   if (e->BeginCommandBuffer(e->command, &begin) != VK_SUCCESS)
      goto error;
   if (e->BeginCommandBuffer(e->upload_command, &begin) != VK_SUCCESS)
      goto error;
   vve_upload(e);
   if (e->EndCommandBuffer(e->upload_command) != VK_SUCCESS)
      goto error;
   e->CmdResetQueryPool(e->command, e->query, 0, 1);
   if (e->hevc)
      vve_encode_h265(e);
   else
      vve_encode(e);
   if (e->EndCommandBuffer(e->command) != VK_SUCCESS)
      goto error;
   submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   submit.pNext = NULL;
   submit.waitSemaphoreCount = 0;
   submit.pWaitSemaphores = NULL;
   submit.pWaitDstStageMask = NULL;
   submit.commandBufferCount = 1;
   submit.pCommandBuffers = &e->upload_command;
   submit.signalSemaphoreCount = 1;
   submit.pSignalSemaphores = &e->upload_ready;
   if (e->QueueSubmit(e->upload_queue, 1, &submit, e->upload_fence) != VK_SUCCESS)
      goto error;
   e->upload_pending = true;
   submit.waitSemaphoreCount = 1;
   submit.pWaitSemaphores = &e->upload_ready;
   submit.pWaitDstStageMask = &wait_stage;
   submit.pCommandBuffers = &e->command;
   submit.signalSemaphoreCount = 0;
   submit.pSignalSemaphores = NULL;
   if (e->QueueSubmit(e->queue, 1, &submit, e->fence) != VK_SUCCESS)
      goto error;
   e->pending = true;
   e->initialized = true;
   e->picture_id++;
   e->keyframe = true;
   return true;

error:
   e->failed = true;
   return false;
}

int vulkan_video_encoder_poll(vulkan_video_encoder_t *e,
      const uint8_t **data, size_t *size, bool *keyframe)
{
   uint64_t result[2];
   VkResult status;
   if (!e || e->failed || !data || !size || !keyframe)
      return -1;

   if (!e->pending)
      return 0;

   status = e->GetFenceStatus(e->device, e->fence);
   if (status == VK_NOT_READY)
      return 0;

   if (status != VK_SUCCESS)
      goto error;
   e->pending = false;
   e->upload_pending = false;
   status = e->GetQueryPoolResults(e->device, e->query, 0, 1,
         sizeof(result), result, sizeof(result), VK_QUERY_RESULT_64_BIT
         | VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
   if (status != VK_SUCCESS || result[1] != VK_QUERY_RESULT_STATUS_COMPLETE_KHR
         || !result[0] || result[0] > VVE_BUFFER_SIZE)
      goto error;
   *data = (const uint8_t*)e->output.mapped;
   *size = (size_t)result[0];
   *keyframe = e->keyframe;
   return 1;

error:
   e->failed = true;
   return -1;
}

bool vulkan_video_encoder_free(vulkan_video_encoder_t *e)
{
   unsigned i;
   if (!e)
      return true;

   if (e->pending && e->GetFenceStatus(e->device, e->fence) == VK_NOT_READY)
      return false;

   if (e->upload_pending && e->GetFenceStatus(e->device, e->upload_fence) == VK_NOT_READY)
      return false;

   if (e->device)
   {
      vve_image_t *images[2];
      vve_buffer_t *buffers[2];
      images[0] = &e->input;
      images[1] = &e->dpb;
      buffers[0] = &e->upload;
      buffers[1] = &e->output;
      if (e->upload_pool)
         e->DestroyCommandPool(e->device, e->upload_pool, NULL);
      if (e->upload_fence)
         e->DestroyFence(e->device, e->upload_fence, NULL);
      if (e->upload_ready)
         e->DestroySemaphore(e->device, e->upload_ready, NULL);
      if (e->query)
         e->DestroyQueryPool(e->device, e->query, NULL);
      if (e->fence)
         e->DestroyFence(e->device, e->fence, NULL);
      if (e->pool)
         e->DestroyCommandPool(e->device, e->pool, NULL);
      for (i = 0; i < 2; i++)
      {
         if (images[i]->view)
            e->DestroyImageView(e->device, images[i]->view, NULL);
         if (images[i]->image)
            e->DestroyImage(e->device, images[i]->image, NULL);
         if (images[i]->memory)
            e->FreeMemory(e->device, images[i]->memory, NULL);
         if (buffers[i]->mapped)
            e->UnmapMemory(e->device, buffers[i]->memory);
         if (buffers[i]->buffer)
            e->DestroyBuffer(e->device, buffers[i]->buffer, NULL);
         if (buffers[i]->memory)
            e->FreeMemory(e->device, buffers[i]->memory, NULL);
      }
      if (e->parameters)
         e->DestroyVideoSessionParametersKHR(e->device, e->parameters, NULL);
      if (e->session)
         e->DestroyVideoSessionKHR(e->device, e->session, NULL);
      for (i = 0; i < e->session_memory_count; i++)
         e->FreeMemory(e->device, e->session_memory[i], NULL);
      if (e->DestroyDevice)
         e->DestroyDevice(e->device, NULL);
   }
   if (e->instance && e->DestroyInstance)
      e->DestroyInstance(e->instance, NULL);
   if (e->library)
      dylib_close(e->library);
   free(e);
   return true;
}
