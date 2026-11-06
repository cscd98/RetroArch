#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <boolean.h>
#define D3D11_RECORD_RING 3
#define VIDEO_SCALE_PACK(w,h) (((w)<<16)|(h))
#define D3D11_USAGE_STAGING 1
#define D3D11_CPU_ACCESS_READ 1
#define FAILED(hr) ((hr)<0)
#define RARCH_ERR printf
#define uuidof(t) 0
#define Release(t) ((void)(t))
typedef struct { unsigned Width,Height,Format,Usage,BindFlags,CPUAccessFlags,MiscFlags; } D3D11_TEXTURE2D_DESC;
struct texture;
struct texture_methods { void (*GetDesc)(struct texture*,D3D11_TEXTURE2D_DESC*); };
struct texture { struct texture_methods *lpVtbl; unsigned ready; };
typedef struct texture *D3D11Texture2D;
typedef D3D11Texture2D ID3D11Texture2D;
typedef D3D11Texture2D D3D11Resource;
struct swapchain;
struct swapchain_methods { int (*GetBuffer)(struct swapchain*,unsigned,unsigned,void**); };
struct swapchain { struct swapchain_methods *lpVtbl; };
struct device;
struct device_methods { int (*CreateTexture2D)(struct device*,D3D11_TEXTURE2D_DESC*,void*,D3D11Texture2D*); };
struct device { struct device_methods *lpVtbl; };
struct context;
struct context_methods { void (*CopyResource)(struct context*,D3D11Resource,D3D11Resource); };
struct context { struct context_methods *lpVtbl; };
typedef struct {
   struct swapchain *swapChain;
   struct device *device;
   struct context *context;
   struct {
      D3D11Texture2D staging[D3D11_RECORD_RING];
      unsigned dims,format,index;
      bool enable,valid[D3D11_RECORD_RING];
   } record;
} d3d11_video_t;
static struct texture textures[4];
static unsigned tick,copies,overwrites,allocations;
static void desc(struct texture *t,D3D11_TEXTURE2D_DESC *d)
{ (void)t;memset(d,0,sizeof(*d));d->Width=3840;d->Height=2160;d->Format=28; }
static int get(struct swapchain *s,unsigned index,unsigned iid,void **out)
{ (void)s;(void)index;(void)iid;*out=&textures[3];return 0; }
static int create(struct device *d,D3D11_TEXTURE2D_DESC *info,void *initial,D3D11Texture2D *out)
{ (void)d;(void)info;(void)initial;assert(allocations<3);*out=&textures[allocations++];return 0; }
static void copy(struct context *c,D3D11Resource dst,D3D11Resource src)
{ (void)c;(void)src;if(dst->ready>tick)overwrites++;dst->ready=tick+5;copies++; }
static void d3d11_record_free(d3d11_video_t *s)
{ memset(&s->record,0,sizeof(s->record)); }
#include "capture.inc"
int main(void)
{
   static struct texture_methods tv={desc};
   static struct swapchain_methods sv={get};
   static struct device_methods dv={create};
   static struct context_methods cv={copy};
   struct swapchain swap={&sv};
   struct device device={&dv};
   struct context context={&cv};
   d3d11_video_t state;
   unsigned i,delivered=0;
   memset(&state,0,sizeof(state));
   state.swapChain=&swap;state.device=&device;state.context=&context;
   for(i=0;i<4;i++)textures[i].lpVtbl=&tv;
   for(tick=0;tick<30;tick++)
   {
      unsigned oldest=state.record.index;
      if(state.record.valid[oldest] && state.record.staging[oldest]->ready<=tick)
      {
         state.record.valid[oldest]=false;
         delivered++;
      }
      d3d11_record_capture(&state);
   }
   if(!delivered || overwrites)
   {
      fprintf(stderr,"Capture starved: %u delivered, %u pending copies overwritten\n",delivered,overwrites);
      return 1;
   }
   assert(allocations==3 && copies<tick);
   puts("D3D11 capture retains pending GPU copies without waiting: passed");
   return 0;
}
