/* Copyright (C) 2026 The RetroArch team. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <retro_environment.h>
#include "mf_video_encode.h"
#include <stdlib.h>
#include <string.h>
#include <dynamic/dylib.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <icodecapi.h>

static const GUID mf_force_keyframe = { 0x398c1b98, 0x8353, 0x475a,
   { 0x9e, 0xf2, 0x8f, 0x26, 0x5d, 0x26, 0x03, 0x45 } };

static const GUID mf_b_pictures = { 0x8d390aac, 0xdc5c, 0x4200,
   { 0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2 } };
static const GUID mf_low_latency = { 0x9c27891a, 0xed7a, 0x40e1,
   { 0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee } };

struct mf_video_encoder
{
   dylib_t library;
   IMFTransform *transform;
   IMFMediaEventGenerator *events;
   IMFMediaType *output_type;
   IMFSample *sample;
   IMFMediaBuffer *buffer;
   BYTE *mapped;
   uint64_t frame;
   unsigned width, height, fps;
   bool hevc;
   DWORD input_id, output_id;
   unsigned input_ready, output_ready;
   bool com, startup, asynchronous;
   HRESULT (WINAPI *startup_fn)(ULONG, DWORD);
   HRESULT (WINAPI *shutdown_fn)(void);
   HRESULT (WINAPI *enumerate)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *,
         const MFT_REGISTER_TYPE_INFO *, IMFActivate ***, UINT32 *);
   HRESULT (WINAPI *create_type)(IMFMediaType **);
   HRESULT (WINAPI *create_sample)(IMFSample **);
   HRESULT (WINAPI *create_buffer)(DWORD, IMFMediaBuffer **);
};

bool mf_video_encoder_free(mf_video_encoder_t *e)
{
   if (!e)
      return true;

   if (e->mapped)
      IMFMediaBuffer_Unlock(e->buffer);
   if (e->buffer)
      IMFMediaBuffer_Release(e->buffer);
   if (e->sample)
      IMFSample_Release(e->sample);
   if (e->events)
      IMFMediaEventGenerator_Release(e->events);
   if (e->output_type)
      IMFMediaType_Release(e->output_type);
   if (e->transform)
   {
      IMFTransform_ProcessMessage(e->transform, MFT_MESSAGE_COMMAND_FLUSH, 0);
      IMFTransform_Release(e->transform);
   }
   if (e->startup)
      e->shutdown_fn();
   if (e->library)
      dylib_close(e->library);
   if (e->com)
      CoUninitialize();
   free(e);
   return true;
}

static bool mf_encoder_type(mf_video_encoder_t *e, IMFMediaType *type,
      const GUID *subtype, unsigned bitrate)
{
   return SUCCEEDED(IMFMediaType_SetGUID(type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video))
      && SUCCEEDED(IMFMediaType_SetGUID(type, &MF_MT_SUBTYPE, subtype))
      && SUCCEEDED(IMFMediaType_SetUINT64(type, &MF_MT_FRAME_SIZE,
               ((uint64_t)e->width << 32) | e->height))
      && SUCCEEDED(IMFMediaType_SetUINT64(type, &MF_MT_FRAME_RATE,
               ((uint64_t)e->fps << 32) | 1))
      && SUCCEEDED(IMFMediaType_SetUINT64(type, &MF_MT_PIXEL_ASPECT_RATIO,
               ((uint64_t)1 << 32) | 1))
      && SUCCEEDED(IMFMediaType_SetUINT32(type, &MF_MT_INTERLACE_MODE,
               MFVideoInterlace_Progressive))
      && (!bitrate || SUCCEEDED(IMFMediaType_SetUINT32(type,
               &MF_MT_AVG_BITRATE, bitrate)));
}

static bool mf_encoder_activate(mf_video_encoder_t *e,
      IMFActivate *activate, unsigned bitrate)
{
   ICodecAPI *codec = NULL;
   VARIANT value;
   IMFAttributes *attributes = NULL;
   IMFMediaType *input = NULL;
   DWORD inputs, outputs;
   UINT32 asynchronous = 0;
   HRESULT result;
   if (FAILED(IMFActivate_ActivateObject(activate, &IID_IMFTransform,
               (void**)&e->transform)))
      return false;

   if (SUCCEEDED(IMFTransform_GetAttributes(e->transform, &attributes)))
   {
      IMFAttributes_GetUINT32(attributes, &MF_TRANSFORM_ASYNC, &asynchronous);
      if (asynchronous)
         IMFAttributes_SetUINT32(attributes, &MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
      IMFAttributes_SetUINT32(attributes, &MF_LOW_LATENCY, TRUE);
      IMFAttributes_Release(attributes);
   }
   e->asynchronous = asynchronous != 0;
   if (e->asynchronous
         && FAILED(IMFTransform_QueryInterface(e->transform,
               &IID_IMFMediaEventGenerator, (void**)&e->events)))
      goto error;
   if (FAILED(IMFTransform_GetStreamCount(e->transform, &inputs, &outputs))
         || inputs != 1 || outputs != 1)
      goto error;
   result = IMFTransform_GetStreamIDs(e->transform, 1, &e->input_id, 1, &e->output_id);
   if (result == E_NOTIMPL)
      e->input_id = e->output_id = 0;
   else if (FAILED(result))
      goto error;
   if (FAILED(e->create_type(&e->output_type))
         || !mf_encoder_type(e, e->output_type, e->hevc ? &MFVideoFormat_HEVC : &MFVideoFormat_H264, bitrate)
         || FAILED(IMFMediaType_SetUINT32(e->output_type,
               &MF_MT_MPEG2_PROFILE, e->hevc ? 1 : 77))
         || FAILED(IMFTransform_SetOutputType(e->transform, e->output_id,
               e->output_type, 0))
         || FAILED(e->create_type(&input))
         || !mf_encoder_type(e, input, &MFVideoFormat_NV12, 0)
         || FAILED(IMFTransform_SetInputType(e->transform, e->input_id, input, 0)))
      goto error;
   IMFMediaType_Release(input);
   input = NULL;
   if (FAILED(IMFTransform_QueryInterface(e->transform,
               &IID_ICodecAPI, (void**)&codec)))
      goto error;
   VariantInit(&value);
   V_VT(&value) = VT_UI4;
   V_UI4(&value) = 0;
   result = ICodecAPI_SetValue(codec, &mf_b_pictures, &value);
   V_VT(&value) = VT_BOOL;
   V_BOOL(&value) = VARIANT_TRUE;
   if (SUCCEEDED(result))
      result = ICodecAPI_SetValue(codec, &mf_low_latency, &value);
   ICodecAPI_Release(codec);
   if (FAILED(result))
      goto error;

   if (FAILED(IMFTransform_ProcessMessage(e->transform,
               MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))
         || FAILED(IMFTransform_ProcessMessage(e->transform,
               MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))
      goto error;
   e->input_ready = !e->asynchronous;
   return true;

error:
   if (input)
      IMFMediaType_Release(input);
   if (e->output_type)
      IMFMediaType_Release(e->output_type);
   if (e->events)
      IMFMediaEventGenerator_Release(e->events);
   IMFTransform_Release(e->transform);
   e->output_type = NULL;
   e->events = NULL;
   e->transform = NULL;
   return false;
}

mf_video_encoder_t *mf_video_encoder_new(unsigned width, unsigned height,
      unsigned bitrate, unsigned fps, bool hevc)
{
   mf_video_encoder_t *e;
   MFT_REGISTER_TYPE_INFO input, output;
   IMFActivate **activations = NULL;
   UINT32 count = 0, i, pass;
   if (!width || !height || (width & 1) || (height & 1)
         || width > 8192 || height > 4096 || !fps || fps > 144)
      return NULL;

   e = (mf_video_encoder_t*)calloc(1, sizeof(*e));
   if (!e)
      return NULL;

   if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED)))
      goto error;
   e->com = true;
   e->width = width;
   e->height = height;
   e->fps = fps;
   e->hevc = hevc;
#ifdef __WINRT__
   e->startup_fn = MFStartup;
   e->shutdown_fn = MFShutdown;
   e->enumerate = MFTEnumEx;
   e->create_type = MFCreateMediaType;
   e->create_sample = MFCreateSample;
   e->create_buffer = MFCreateMemoryBuffer;
#else
   e->library = dylib_load("mfplat.dll");
   if (!e->library)
      goto error;
#define MF_LOAD(member, name) do { \
      function_t proc = dylib_proc(e->library, name); \
      memcpy(&e->member, &proc, sizeof(e->member)); \
      if (!e->member) goto error; \
   } while (0)
   MF_LOAD(startup_fn, "MFStartup");
   MF_LOAD(shutdown_fn, "MFShutdown");
   MF_LOAD(enumerate, "MFTEnumEx");
   MF_LOAD(create_type, "MFCreateMediaType");
   MF_LOAD(create_sample, "MFCreateSample");
   MF_LOAD(create_buffer, "MFCreateMemoryBuffer");
#undef MF_LOAD
#endif
   if (FAILED(e->startup_fn(MF_VERSION, MFSTARTUP_FULL)))
      goto error;
   e->startup = true;
   input.guidMajorType = MFMediaType_Video;
   input.guidSubtype = MFVideoFormat_NV12;
   output.guidMajorType = MFMediaType_Video;
   output.guidSubtype = hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
   for (pass = 0; pass < 2; pass++)
   {
      /* Numeric flags keep older target SDK declarations optional. */
      UINT32 flags = pass ? 0x01 | 0x10 | 0x40 : 0x04 | 0x40;
      if (FAILED(e->enumerate(MFT_CATEGORY_VIDEO_ENCODER, flags,
                  &input, &output, &activations, &count)))
         continue;
      for (i = 0; i < count; i++)
      {
         if (!e->transform)
            mf_encoder_activate(e, activations[i], bitrate);
         IMFActivate_Release(activations[i]);
      }
      CoTaskMemFree(activations);
      activations = NULL;
      if (e->transform)
         return e;
   }
error:
   mf_video_encoder_free(e);
   return NULL;
}

static bool mf_encoder_events(mf_video_encoder_t *e)
{
   unsigned i;
   if (!e->events)
      return true;

   for (i = 0; i < 16; i++)
   {
      IMFMediaEvent *event = NULL;
      MediaEventType type;
      HRESULT status, result = IMFMediaEventGenerator_GetEvent(e->events,
            MF_EVENT_FLAG_NO_WAIT, &event);
      if (result == MF_E_NO_EVENTS_AVAILABLE)
         return true;

      if (FAILED(result))
         return false;

      result = IMFMediaEvent_GetStatus(event, &status);
      if (SUCCEEDED(result) && SUCCEEDED(status))
         result = IMFMediaEvent_GetType(event, &type);
      IMFMediaEvent_Release(event);
      if (FAILED(result) || FAILED(status))
         return false;

      if (type == METransformNeedInput)
         e->input_ready++;
      if (type == METransformHaveOutput)
         e->output_ready++;
      if (type == MEError)
         return false;
   }
   return true;
}

bool mf_video_encoder_headers(mf_video_encoder_t *e,
      void *data, size_t capacity, size_t *size)
{
   UINT32 written = 0;
   IMFMediaType *type = NULL;
   HRESULT result;
   if (!e || !data || !size || capacity > UINT32_MAX
         || FAILED(IMFTransform_GetOutputCurrentType(e->transform,
               e->output_id, &type)))
      return false;

   result = IMFMediaType_GetBlob(type, &MF_MT_MPEG_SEQUENCE_HEADER,
         (UINT8*)data, (UINT32)capacity, &written);
   IMFMediaType_Release(type);
   *size = written;
   return SUCCEEDED(result) && written;
}

bool mf_video_encoder_submit(mf_video_encoder_t *e,
      const uint8_t *pixels, bool keyframe)
{
   IMFSample *sample = NULL;
   IMFMediaBuffer *buffer = NULL;
   BYTE *mapped = NULL;
   DWORD bytes;
   bool ok = false;
   if (!e || !pixels || !mf_encoder_events(e)
         || (e->asynchronous && !e->input_ready))
      return false;

   if (keyframe)
   {
      ICodecAPI *codec = NULL;
      if (SUCCEEDED(IMFTransform_QueryInterface(e->transform,
                  &IID_ICodecAPI, (void**)&codec)))
      {
         VARIANT value;
         VariantInit(&value);
         V_VT(&value) = VT_UI4;
         V_UI4(&value) = 1;
         ICodecAPI_SetValue(codec, &mf_force_keyframe, &value);
         ICodecAPI_Release(codec);
      }
   }
   bytes = e->width * e->height * 3 / 2;
   if (FAILED(e->create_sample(&sample))
         || FAILED(e->create_buffer(bytes, &buffer))
         || FAILED(IMFMediaBuffer_Lock(buffer, &mapped, NULL, NULL)))
      goto done;
   memcpy(mapped, pixels, bytes);
   IMFMediaBuffer_Unlock(buffer);
   mapped = NULL;
   if (FAILED(IMFMediaBuffer_SetCurrentLength(buffer, bytes))
         || FAILED(IMFSample_AddBuffer(sample, buffer))
         || FAILED(IMFSample_SetSampleTime(sample,
               (LONGLONG)(e->frame * 10000000 / e->fps)))
         || FAILED(IMFSample_SetSampleDuration(sample, 10000000 / e->fps))
         || FAILED(IMFTransform_ProcessInput(e->transform, e->input_id, sample, 0)))
      goto done;
   if (e->asynchronous)
      e->input_ready--;
   e->frame++;
   ok = true;
done:
   if (mapped)
      IMFMediaBuffer_Unlock(buffer);
   if (buffer)
      IMFMediaBuffer_Release(buffer);
   if (sample)
      IMFSample_Release(sample);
   return ok;
}

int mf_video_encoder_poll(mf_video_encoder_t *e,
      const uint8_t **data, size_t *size, bool *keyframe)
{
   MFT_OUTPUT_STREAM_INFO stream;
   MFT_OUTPUT_DATA_BUFFER output;
   DWORD flags = 0, length;
   UINT32 clean = 0;
   HRESULT result;
   if (!e || !data || !size || !keyframe || !mf_encoder_events(e))
      return -1;

   if (e->asynchronous && !e->output_ready)
      return 0;

   if (e->mapped)
      IMFMediaBuffer_Unlock(e->buffer);
   if (e->buffer)
      IMFMediaBuffer_Release(e->buffer);
   if (e->sample)
      IMFSample_Release(e->sample);
   e->mapped = NULL;
   e->buffer = NULL;
   e->sample = NULL;
   if (FAILED(IMFTransform_GetOutputStreamInfo(e->transform, e->output_id, &stream)))
      return -1;

   if (!(stream.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES))
   {
      if (!stream.cbSize || stream.cbSize > 16 * 1024 * 1024
            || FAILED(e->create_sample(&e->sample))
            || FAILED(e->create_buffer(stream.cbSize, &e->buffer))
            || FAILED(IMFSample_AddBuffer(e->sample, e->buffer)))
         return -1;

      IMFMediaBuffer_Release(e->buffer);
      e->buffer = NULL;
   }
   output.dwStreamID = e->output_id;
   output.pSample = e->sample;
   output.dwStatus = 0;
   output.pEvents = NULL;
   result = IMFTransform_ProcessOutput(e->transform, 0, 1, &output, &flags);
   if (output.pEvents)
      IMFCollection_Release(output.pEvents);
   e->sample = output.pSample;
   if (result == MF_E_TRANSFORM_STREAM_CHANGE)
   {
      IMFMediaType *type = NULL;
      GUID subtype;
      if (FAILED(IMFTransform_GetOutputAvailableType(e->transform,
                  e->output_id, 0, &type)))
         return -1;

      result = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype);
      if (SUCCEEDED(result) && IsEqualGUID(&subtype, e->hevc ? &MFVideoFormat_HEVC : &MFVideoFormat_H264))
         result = IMFTransform_SetOutputType(e->transform, e->output_id, type, 0);
      else
         result = E_FAIL;
      if (FAILED(result))
      {
         IMFMediaType_Release(type);
         return -1;
      }

      IMFMediaType_Release(e->output_type);
      e->output_type = type;
      return 0;
   }

   if (e->asynchronous)
      e->output_ready--;
   if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
      return 0;

   if (FAILED(result) || !e->sample
         || FAILED(IMFSample_ConvertToContiguousBuffer(e->sample, &e->buffer))
         || FAILED(IMFMediaBuffer_Lock(e->buffer, &e->mapped, NULL, &length)))
      return -1;

   IMFSample_GetUINT32(e->sample, &MFSampleExtension_CleanPoint, &clean);
   *data = e->mapped;
   *size = length;
   *keyframe = clean != 0;
   return 1;
}
