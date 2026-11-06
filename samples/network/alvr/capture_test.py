#!/usr/bin/env python3
"""Exercise the ALVR composed-frame capture with a nonblocking video reader."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / 'network/alvr_server.c').read_text()

def function(name):
    match = re.search(r'^(?:static )?[\w \t*]+\b' + name + r'\([^;{]*\)\s*\{', source, re.M)
    assert match, 'Missing composed-frame capture: ' + name
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int bool;
#define true 1
#define false 0
#define HAVE_THREADS 1
#define RETRO_PIXEL_FORMAT_XRGB8888 1
#define VIDEO_SCALE_PACK(w,h) ((w) | ((h) << 16))
#define VIDEO_SCALE_W(d) ((d) & 65535)
#define VIDEO_SCALE_H(d) ((d) >> 16)
struct video_viewport { unsigned dims, full_dims; };
typedef struct { bool (*record_read)(void*,uint8_t*,bool,bool,bool*); } poke_t;
typedef struct { void (*viewport_info)(void*,struct video_viewport*); } driver_t;
typedef struct {
   void *data;
   poke_t *poke;
   driver_t *current_video;
   bool thread_wrapper_active;
} video_driver_state_t;
static video_driver_state_t video;
static struct { bool enable; } recording;
static void *alvr_server_stream = &video;
static bool alvr_server_stopping;
static uint8_t *alvr_server_pixels;
static uint32_t *alvr_server_scaled;
static size_t alvr_server_pixel_capacity;
static unsigned width, height, deliveries, binds, stops;
static bool inverted, stripes, ready = true;
static uint32_t threaded_pixels[2048 * 2048];
static video_driver_state_t *video_state_get_ptr(void) { return &video; }
#define recording_state_get_ptr() (&recording)
static bool video_thread_record_has_reader(void *data) { return data != NULL; }
static void video_thread_record_stop(void *data) { assert(data); stops++; }
static void video_context_driver_bind_hw_render(bool enable)
{
   assert(enable == (bool)(binds & 1));
   binds++;
}
static void fill(uint32_t *pixels, unsigned w, unsigned h)
{
   unsigned x, y;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
         pixels[(size_t)y * w + x] = stripes
            ? (x % 4 == 1 ? 0xffffff : 0)
            : x | ((inverted ? h - 1 - y : y) << 16);
}
static bool read_pixels(void *data, uint8_t *pixels, bool bgrx, bool full, bool *bottom_up)
{
   assert(data && bgrx && full);
   if (!ready) return false;
   *bottom_up = inverted;
   fill((uint32_t*)pixels, width, height);
   return true;
}
static void viewport(void *data, struct video_viewport *vp)
{
   assert(data);
   vp->dims = VIDEO_SCALE_PACK(width / 2, height);
   vp->full_dims = VIDEO_SCALE_PACK(width, height);
}
static int video_thread_record_take(void *data, unsigned dims, bool bgrx, bool full,
      const uint8_t **pixels, bool *bottom_up)
{
   assert(data && bgrx && full);
   if (!ready) return 0;
   fill(threaded_pixels, VIDEO_SCALE_W(dims), VIDEO_SCALE_H(dims));
   *pixels = (const uint8_t*)threaded_pixels;
   *bottom_up = inverted;
   return 1;
}
static void alvr_stream_video(void *stream, const void *pixels,
      unsigned w, unsigned h, size_t pitch, unsigned format,
      unsigned rotation, float aspect)
{
   const uint32_t *data = (const uint32_t*)pixels;
   unsigned x, y;
   assert(stream == alvr_server_stream && w <= 2048 && h <= 2048);
   assert(pitch == w * 4 && format == RETRO_PIXEL_FORMAT_XRGB8888);
   assert(!rotation && aspect == (float)w / h);
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         unsigned sx = video.thread_wrapper_active ? x : x * width / w;
         unsigned sy = video.thread_wrapper_active ? y : y * height / h;
         if (stripes)
            assert(data[(size_t)y * w + x] == 0x3f3f3f);
         else
            assert(data[(size_t)y * w + x] == (sx | (sy << 16)));
      }
   deliveries++;
}
'''
tests = r'''
int main(void)
{
   poke_t poke = {read_pixels};
   driver_t driver = {viewport};
   unsigned thread, flip, before;
   video.data = &video;
   video.poke = &poke;
   video.current_video = &driver;
   assert(alvr_server_capture_active());
   recording.enable = true;
   assert(!alvr_server_capture_active());
   recording.enable = false;
   for (thread = 0; thread < 2; thread++)
      for (flip = 0; flip < 2; flip++)
      {
         unsigned before = deliveries;
         video.thread_wrapper_active = thread;
         inverted = flip;
         width = 2048; height = 576;
         alvr_server_capture();
         width = 320; height = 240;
         alvr_server_capture();
         assert(deliveries == before + 2);
         ready = false;
         alvr_server_capture();
         assert(deliveries == before + 2);
         ready = true;
      }
   video.thread_wrapper_active = false;
   stripes = true;
   width = 8192; height = 2160;
   before = deliveries;
   alvr_server_capture();
   assert(deliveries == before + 1);
   video.thread_wrapper_active = true;
   alvr_server_capture_free();
   assert(!alvr_server_pixels && !alvr_server_scaled);
   assert(!alvr_server_pixel_capacity && stops == 1 && !(binds & 1));
   puts("ALVR composed-frame capture passed");
   return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='alvr_capture_') as tmp:
    src = Path(tmp) / 'capture.c'
    exe = Path(tmp) / 'capture_test.exe'
    src.write_text(fixture + '\n'.join(function(name) for name in (
        'alvr_server_capture_active', 'alvr_server_capture_free', 'alvr_server_capture')) + tests)
    cc = shlex.split(os.environ.get('CC', 'cc'))
    subprocess.run(cc + ['-std=c89', '-pedantic', '-Wall', '-Wextra', '-Werror']
                   + shlex.split(os.environ.get('CFLAGS', '')) + [str(src), '-o', str(exe)], check=True)
    runner = shlex.split(os.environ.get('RUNNER', ''))
    if runner or not any('mingw' in part for part in cc):
        subprocess.run(runner + [str(exe)], check=True)

source = (root / 'network/alvr_stream.c').read_text()
pixel = function('alvr_pixel')
projection = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define RETRO_PIXEL_FORMAT_XRGB8888 1
#define RETRO_PIXEL_FORMAT_RGB565 2
typedef struct { uint8_t *source; } alvr_stream_t;
typedef struct { unsigned width, height, format, rotation; } alvr_source_t;
'''
projection += pixel + r'''
int main(void)
{
   uint32_t pixels32[16];
   uint16_t pixels16[16];
   alvr_stream_t stream;
   alvr_source_t frame;
   unsigned format, rotation, i;
   int r, g, b;
   frame.width = frame.height = 4;
   for (format = 0; format < 3; format++)
   {
      frame.format = format;
      for (i = 0; i < 16; i++)
      {
         pixels32[i] = i % 4 == 1 ? 0xffffff : 0;
         pixels16[i] = i % 4 == 1 ? (format == 2 ? 0xffff : 0x7fff) : 0;
      }
      stream.source = format == 1 ? (uint8_t*)pixels32 : (uint8_t*)pixels16;
      for (rotation = 0; rotation < 4; rotation++)
      {
         frame.rotation = rotation;
         alvr_pixel(&stream, &frame, 0.5f, 0.5f, 1.0f, 1.0f, &r, &g, &b);
         assert(r == 63 && g == 63 && b == 63);
      }
   }
   puts("ALVR projected thin strokes passed");
   return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='alvr_projection_') as tmp:
    src = Path(tmp) / 'projection.c'
    exe = Path(tmp) / 'projection_test.exe'
    src.write_text(projection)
    subprocess.run(cc + ['-std=c89', '-pedantic', '-Wall', '-Wextra', '-Werror']
                   + shlex.split(os.environ.get('CFLAGS', ''))
                   + [str(src), '-o', str(exe), '-lm'], check=True)
    if runner or not any('mingw' in part for part in cc):
        subprocess.run(runner + [str(exe)], check=True)

geometry = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <retro_spsc.h>
#include "gfx/video_xr.h"
#define RETRO_PIXEL_FORMAT_XRGB8888 1
#define RETRO_PIXEL_FORMAT_RGB565 2
typedef struct { float distance, width; unsigned placement, recenter; } alvr_placement_t;
typedef struct {
   uint64_t tracking_seconds, frame_seconds, placement_time;
   uint8_t *source, *nv12;
   retro_spsc_t placement_queue;
   alvr_placement_t placement;
   video_xr_pose_t anchor;
   unsigned recenter, view_size, view_height, tracking_nanos, frame_nanos;
   float local_views[22], head[7], frame_views[22];
   bool anchor_valid, gliding;
} alvr_stream_t;
typedef struct { unsigned width, height, format, rotation; float aspect; } alvr_source_t;
static uint64_t cpu_features_get_time_usec(void) { return 1000000; }
'''
geometry += function('alvr_views') + pixel + function('alvr_render') + r'''
static unsigned lit(const alvr_stream_t *s)
{
   unsigned i, count = 0;
   for (i = 0; i < 64 * 128; i++)
      if (s->nv12[i] > 16) count++;
   return count;
}
int main(void)
{
   alvr_stream_t s;
   alvr_source_t f;
   uint32_t pixels[16 * 16];
   uint8_t image[64 * 64 * 3];
   uint8_t initial[64 * 64 * 2];
   unsigned eye, count;
   memset(&s, 0, sizeof(s));
   memset(&f, 0, sizeof(f));
   memset(pixels, 255, sizeof(pixels));
   s.source = (uint8_t*)pixels;
   s.nv12 = image;
   s.view_size = s.view_height = 64;
   s.head[3] = 1;
   s.placement.distance = 1.8f;
   s.placement.width = 1.6f;
   f.width = f.height = 16;
   f.format = 1;
   f.aspect = 1;
   assert(retro_spsc_init(&s.placement_queue, 1024));
   for (eye = 0; eye < 2; eye++)
   {
      float *v = s.local_views + eye * 11;
      v[3] = 1;
      v[7] = v[10] = -0.78539816f;
      v[8] = v[9] = 0.78539816f;
   }
   alvr_views(&s);
   alvr_render(&s, &f);
   count = lit(&s);
   assert(count > 1000 && count < 2000);
   memcpy(initial, image, sizeof(initial));
   s.head[1] = (float)sin(0.4);
   s.head[3] = (float)cos(0.4);
   alvr_views(&s);
   alvr_render(&s, &f);
   assert(image[32 * 128 + 32] == 16);
   assert(memcmp(initial, image, sizeof(initial)));
   s.placement.placement = VIDEO_OPENXR_PLACEMENT_FOLLOW;
   alvr_render(&s, &f);
   assert(!memcmp(initial, image, sizeof(initial)));
   s.placement.placement = VIDEO_OPENXR_PLACEMENT_FIXED;
   s.placement.recenter++;
   alvr_render(&s, &f);
   assert(!memcmp(initial, image, sizeof(initial)));
   s.placement.distance *= 2;
   alvr_render(&s, &f);
   assert(lit(&s) < count / 2);
   retro_spsc_free(&s.placement_queue);
   puts("ALVR world placement, follow, recenter and distance passed");
   return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='alvr_geometry_') as tmp:
    src = Path(tmp) / 'geometry.c'
    exe = Path(tmp) / 'geometry_test.exe'
    src.write_text(geometry)
    subprocess.run(cc + ['-std=c89', '-pedantic', '-Wall', '-Wextra', '-Werror',
                        '-ffunction-sections', '-fdata-sections', '-Wno-type-limits',
                        '-I' + str(root), '-I' + str(root / 'libretro-common/include')]
                   + shlex.split(os.environ.get('CFLAGS', ''))
                   + [str(src), str(root / 'gfx/video_xr.c'), str(root / 'gfx/video_views.c'),
                      str(root / 'libretro-common/queues/retro_spsc.c'), '-Wl,--gc-sections',
                      '-o', str(exe), '-lm'], check=True)
    if runner or not any('mingw' in part for part in cc):
        subprocess.run(runner + [str(exe)], check=True)

input_fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <retro_spsc.h>
#include <libretro.h>
#include "network/alvr_protocol.h"
typedef struct { retro_spsc_t input_queue; } alvr_stream_t;
#define ALVR_STREAM_CONNECTED 2
static int stream_status = ALVR_STREAM_CONNECTED;
static int alvr_stream_status(alvr_stream_t *s) { (void)s; return stream_status; }
static alvr_stream_t *alvr_server_stream;
static bool alvr_server_stopping;
static alvr_input_state_t alvr_server_input;
static unsigned alvr_server_buttons;
static retro_time_t alvr_server_menu_since, clock_time;
static bool alvr_server_menu_held, alvr_server_menu_toggle;
static retro_time_t cpu_features_get_time_usec(void) { return clock_time; }
'''
input_fixture += function('alvr_stream_input')
source = (root / 'network/alvr_server.c').read_text()
input_fixture += '\n'.join(function(name) for name in (
    'alvr_server_input_poll', 'alvr_server_input_buttons',
    'alvr_server_input_analog', 'alvr_server_input_menu'))
input_fixture += r'''
int main(void)
{
   alvr_stream_t stream;
   alvr_input_state_t state;
   memset(&stream, 0, sizeof(stream));
   memset(&state, 0, sizeof(state));
   assert(retro_spsc_init(&stream.input_queue, 4096));
   alvr_server_stream = &stream;
   state.buttons = 1;
   state.analog[0] = 24000;
   state.analog[1] = -24000;
   assert(retro_spsc_write(&stream.input_queue, &state, sizeof(state)) == sizeof(state));
   alvr_server_input_poll(0.5f);
   assert(alvr_server_input_buttons() == (1u | (1u << 4) | (1u << 7)));
   assert(alvr_server_input_analog(0,0) == 24000);
   assert(alvr_server_input_analog(0,1) == -24000);
   assert(!alvr_server_input_analog(4,0));
   state.menu = true;
   retro_spsc_write(&stream.input_queue, &state, sizeof(state));
   alvr_server_input_poll(0.5f);
   assert(!alvr_server_input_menu());
   clock_time = 1000000;
   alvr_server_input_poll(0.5f);
   assert(alvr_server_input_menu());
   memset(&state, 0, sizeof(state));
   retro_spsc_write(&stream.input_queue, &state, sizeof(state));
   alvr_server_input_poll(0.5f);
   assert(!alvr_server_input_buttons() && !alvr_server_input_menu());
   state.buttons = 1;
   retro_spsc_write(&stream.input_queue, &state, sizeof(state));
   alvr_server_input_poll(0.5f);
   assert(alvr_server_input_buttons() == 1);
   stream_status = 3;
   alvr_server_input_poll(0.5f);
   assert(!alvr_server_input_buttons() && !alvr_server_input_analog(0,0));
   retro_spsc_free(&stream.input_queue);
   puts("ALVR controller snapshot, release, menu hold and disconnect passed");
   return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='alvr_input_') as tmp:
    src = Path(tmp) / 'input.c'
    exe = Path(tmp) / 'input_test.exe'
    src.write_text(input_fixture)
    subprocess.run(cc + ['-std=c89', '-pedantic', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(root), '-I' + str(root / 'libretro-common/include')]
                   + [str(src), str(root / 'libretro-common/queues/retro_spsc.c'),
                      '-o', str(exe)], check=True)
    if runner or not any('mingw' in part for part in cc):
        subprocess.run(runner + [str(exe)], check=True)

source = (root / 'network/alvr_server.c').read_text()
# A retired audio tap is recovered independently of the video connection.
assert 'record_audio_tap_failed' not in function('alvr_server_poll')
audio_fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
typedef int bool;
#define true 1
#define false 0
#define AUDIO_FLAG_ACTIVE 1
#define AUDIO_FLAGS_GET(s) ((s)->flags)
typedef struct { bool failed; } record_audio_tap_t;
static record_audio_tap_t tap;
static record_audio_tap_t *alvr_server_tap = &tap;
static void *alvr_server_stream = &tap;
static bool alvr_server_tap_installed = true, alvr_server_audio_warned;
static unsigned alvr_server_audio_rate = 48000;
static int64_t alvr_server_audio_retry, now;
static unsigned allocations, freed, messages;
static bool busy;
static int install_result = 1;
static struct { unsigned flags; } audio_state;
#define audio_state_get_ptr() (&audio_state)
static int64_t cpu_features_get_time_usec(void) { return now; }
static bool record_audio_tap_failed(record_audio_tap_t *t) { return t->failed; }
static bool audio_driver_record_tap_remove(record_audio_tap_t *t)
{ assert(t == &tap); return !busy; }
static bool record_audio_tap_free(record_audio_tap_t *t)
{ assert(t == &tap && !busy); freed++; return true; }
static int alvr_server_audio(void *p, const int16_t *s, size_t n, uint64_t t)
{ (void)p; (void)s; (void)n; (void)t; return 0; }
static record_audio_tap_t *record_audio_tap_new(unsigned rate,
      int (*submit)(void*,const int16_t*,size_t,uint64_t), void *p)
{
   assert(rate == 48000 && submit == alvr_server_audio && p == alvr_server_stream);
   allocations++; tap.failed = false; return &tap;
}
static int audio_driver_record_tap_install(record_audio_tap_t *t)
{ assert(t == &tap); return install_result; }
static void alvr_server_message(const char *s) { assert(s); messages++; }
'''
audio_fixture += function('alvr_server_audio_poll') + r'''
int main(void)
{
   tap.failed = true;
   busy = true;
   alvr_server_audio_poll(48000);
   assert(!freed && !allocations && alvr_server_tap == &tap);
   busy = false;
   alvr_server_audio_poll(48000);
   assert(freed == 1 && !allocations && !alvr_server_tap);
   audio_state.flags = AUDIO_FLAG_ACTIVE;
   alvr_server_audio_poll(44100);
   assert(!allocations);
   alvr_server_audio_poll(48000);
   assert(allocations == 1 && alvr_server_tap_installed);
   tap.failed = true;
   install_result = -1;
   alvr_server_audio_poll(48000);
   assert(allocations == 2 && !alvr_server_tap && messages == 1);
   alvr_server_audio_poll(48000);
   assert(allocations == 2);
   now = 1000000;
   alvr_server_audio_poll(48000);
   assert(allocations == 3 && messages == 1);
   now = 2000000;
   install_result = 0;
   alvr_server_audio_poll(48000);
   assert(allocations == 4 && !alvr_server_tap_installed);
   install_result = 1;
   alvr_server_audio_poll(48000);
   assert(allocations == 4 && alvr_server_tap_installed && !alvr_server_audio_warned);
   puts("ALVR audio reinitialization recovery passed");
   return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='alvr_audio_reinit_') as tmp:
    src = Path(tmp) / 'audio.c'
    exe = Path(tmp) / 'audio_test.exe'
    src.write_text(audio_fixture)
    subprocess.run(cc + ['-std=c89', '-pedantic', '-Wall', '-Wextra', '-Werror']
                   + shlex.split(os.environ.get('CFLAGS', ''))
                   + [str(src), '-o', str(exe)], check=True)
    if runner or not any('mingw' in part for part in cc):
        subprocess.run(runner + [str(exe)], check=True)

source = (root / 'network/alvr_stream.c').read_text()
create = function('alvr_stream_encoder_init')
start = create.index('#if defined(HAVE_VULKAN)')
end = create.index('\n#endif', start)
codec_fixture = r'''
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
typedef int bool;
#define true 1
#define false 0
#define RARCH_LOG printf
#define HAVE_VULKAN 1
typedef struct { void *encoder; unsigned backend, codec, view_size, view_height, bitrate; } alvr_stream_t;
static bool supported;
static unsigned calls;
static bool requested[2];
static void *vulkan_video_encoder_new(unsigned w, unsigned h, unsigned rate, unsigned fps, bool hevc)
{
   assert(w == 1024 && h == 512 && rate == 10000000 && fps == 60);
   assert(calls < 2); requested[calls++] = hevc;
   return !hevc || supported ? &calls : NULL;
}
static void create(alvr_stream_t *s)
{
'''
codec_fixture += create[start:end] + '\n#endif\n}\n' + r'''
int main(void)
{
   alvr_stream_t s = {NULL, 0, 0, 512, 512, 10000000};
   supported = true;
   create(&s);
   assert(calls == 1 && requested[0] && s.codec == 2 && s.backend == 1);
   calls = 0; supported = false; s.encoder = NULL; s.codec = 0;
   create(&s);
   assert(calls == 2 && requested[0] && !requested[1] && s.codec == 1 && s.encoder);
   calls = 0; s.encoder = NULL; s.codec = 1;
   create(&s);
   assert(calls == 1 && !requested[0] && s.encoder);
   calls = 0; s.encoder = NULL; s.codec = 2;
   create(&s);
   assert(calls == 1 && requested[0] && !s.encoder);
   puts("ALVR HEVC preference and H.264 fallback passed");
   return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='alvr_codec_') as tmp:
    src = Path(tmp) / 'codec.c'
    exe = Path(tmp) / 'codec_test.exe'
    src.write_text(codec_fixture)
    subprocess.run(cc + ['-std=c89', '-pedantic', '-Wall', '-Wextra', '-Werror']
                   + shlex.split(os.environ.get('CFLAGS', ''))
                   + [str(src), '-o', str(exe)], check=True)
    if runner or not any('mingw' in part for part in cc):
        subprocess.run(runner + [str(exe)], check=True)
