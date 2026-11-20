#ifdef LIBOUNCE_MODULE_TLS
#include <switch.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <retro_atomic.h>
#endif

extern void __cxa_finalize(void *dso);
void *__dso_handle = &__dso_handle;

#ifdef LIBOUNCE_MODULE_TLS
extern const unsigned __libounce_tls_info[];

typedef struct libounce_tls_destructor
{
   struct libounce_tls_destructor *next;
   void (*function)(void*);
   void *object;
} libounce_tls_destructor_t;

typedef struct libounce_tls_block
{
   libounce_tls_destructor_t *destructors;
   void *data;
} libounce_tls_block_t;

static int libounce_tls_slot = -1;
static retro_atomic_int_t libounce_tls_users;

static void libounce_tls_destroy(void *value)
{
   libounce_tls_block_t *block = (libounce_tls_block_t*)value;
   /* Destructors may access TLS or register another destructor. */
   threadTlsSet(libounce_tls_slot, block);

   while (block->destructors)
   {
      libounce_tls_destructor_t *entry = block->destructors;
      block->destructors = entry->next;
      entry->function(entry->object);
      free(entry);
   }

   threadTlsSet(libounce_tls_slot, NULL);
   free(block);

   retro_atomic_fetch_sub_int(&libounce_tls_users, 1);
}

int libounce_module_tls_init(void)
{
   libounce_tls_slot = threadTlsAlloc(libounce_tls_destroy);

   return libounce_tls_slot >= 0;
}

/* The caller must stop module execution before requesting unload. */
int libounce_module_tls_close(int release)
{
   void *block;
   if (libounce_tls_slot < 0)
      return 1;

   block = threadTlsGet(libounce_tls_slot);

   if (block)
      libounce_tls_destroy(block);

   if (retro_atomic_load_acquire_int(&libounce_tls_users))
      return 0;

   if (release)
   {
      threadTlsFree(libounce_tls_slot);
      libounce_tls_slot = -1;
   }

   return 1;
}

static libounce_tls_block_t *libounce_tls_get_block(void)
{
   libounce_tls_block_t *block;
   size_t alignment = __libounce_tls_info[4];
   size_t offset = (sizeof(*block) + alignment - 1) & ~(alignment - 1);
   size_t size = __libounce_tls_info[3];

   if (libounce_tls_slot < 0)
      abort();

   block = (libounce_tls_block_t*)threadTlsGet(libounce_tls_slot);

   if (block)
      return block;

   block = (libounce_tls_block_t*)memalign(alignment, offset + size);

   if (!block)
      abort();

   block->destructors = NULL;
   block->data = (unsigned char*)block + offset;

   /* Each thread owns its mutable copy of the module's TLS template. */
   memcpy(block->data, (const unsigned char*)__libounce_tls_info +
         (int)__libounce_tls_info[1], __libounce_tls_info[2]);
   memset((unsigned char*)block->data + __libounce_tls_info[2], 0,
         size - __libounce_tls_info[2]);
   retro_atomic_fetch_add_int(&libounce_tls_users, 1);
   threadTlsSet(libounce_tls_slot, block);

   return block;
}

void *__tls_get_addr(const size_t *index)
{
   if (index[0] != 1 || index[1] >= __libounce_tls_info[3])
      abort();
   return (unsigned char*)libounce_tls_get_block()->data + index[1];
}

int __cxa_thread_atexit_impl(void (*function)(void*), void *object, void *dso)
{
   libounce_tls_block_t *block = libounce_tls_get_block();
   libounce_tls_destructor_t *entry =
      (libounce_tls_destructor_t*)malloc(sizeof(*entry));

   (void)dso;
   if (!entry)
      return -1;

   entry->function = function;
   entry->object = object;
   entry->next = block->destructors;
   block->destructors = entry;

   return 0;
}

int __cxa_thread_atexit(void (*function)(void*), void *object, void *dso)
{
   return __cxa_thread_atexit_impl(function, object, dso);
}
#endif

static void __attribute__((destructor(101))) libounce_module_finalize(void)
{
   __cxa_finalize(__dso_handle);
}
