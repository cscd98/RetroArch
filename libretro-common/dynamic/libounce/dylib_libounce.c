#if defined(__SWITCH2__) && defined(HAVE_DYNAMIC)

#include <dynamic/dylib.h>
#include <switch.h>
#include <elf.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct ounce_image
{
   u64 base;
   u64 init_array;
   u64 init_size;
   u64 fini_array;
   u64 fini_size;
   void *image;
   void *bss;
#ifdef HAVE_LIBOUNCE_MODULE_TRACE
   FILE *trace;
#endif
   const Elf64_Sym *symbols;
   const char *strings;
   NroHeader header;
   u32 mod_offset;
   u32 symbol_count;
   u32 tls_size;
   u64 tls_init;
   u64 tls_close;
   int tls_initialized;
   int initialized;
} ounce_image_t;

#ifdef HAVE_THREADS
static __thread char ounce_dylib_error[256];
#else
static char ounce_dylib_error[256];
#endif
extern const unsigned char _start[];

static void ounce_trace(ounce_image_t *module, const char *format, ...)
{
#ifdef HAVE_LIBOUNCE_MODULE_TRACE
   va_list args;
   if (!module->trace)
      return;

   va_start(args, format);
   vfprintf(module->trace, format, args);
   va_end(args);

   fflush(module->trace);
#else
   (void)module;
   (void)format;
#endif
}

static u64 ounce_host_symbol(const char *name)
{
   const Elf64_Dyn *entry;
   const Elf64_Sym *symbols = NULL;
   const char *strings = NULL;
   const u32 *hash = NULL;
   uintptr_t base = (uintptr_t)_start;
   const NroStart *start = (const NroStart*)base;
   const u32 *mod = (const u32*)(base + start->mod_offset);
   u32 i;

   /* __start__ is absolute zero; _start carries the runtime load address. */
   entry = (const Elf64_Dyn*)((const u8*)mod + (s32)mod[1]);

   for (; entry->d_tag != DT_NULL; entry++)
   {
      switch (entry->d_tag)
      {
         case DT_SYMTAB: symbols = (const Elf64_Sym*)(base + entry->d_un.d_ptr); break;
         case DT_STRTAB: strings = (const char*)(base + entry->d_un.d_ptr); break;
         case DT_HASH: hash = (const u32*)(base + entry->d_un.d_ptr); break;
      }
   }

   if (!symbols || !strings || !hash)
      return 0;

   for (i = 1; i < hash[1]; i++)
   {
      if (symbols[i].st_shndx != SHN_UNDEF &&
          ELF64_ST_TYPE(symbols[i].st_info) != STT_TLS &&
          !strcmp(strings + symbols[i].st_name, name))
         return symbols[i].st_shndx == SHN_ABS ? symbols[i].st_value :
            base + symbols[i].st_value;
   }

   return 0;
}

static int ounce_range(u64 offset, u64 length, u64 limit)
{
   return offset <= limit && length <= limit - offset;
}

static int ounce_read(ounce_image_t *module, const char *path)
{
   NroStart start;
   NroHeader *h = &module->header;
   FILE *file = fopen(path, "rb");
   int ok = 0;

   if (!file)
      return 0;

   if (fread(&start, sizeof(start), 1, file) != 1 ||
       fread(h, sizeof(*h), 1, file) != 1)
      goto done;

   if (h->magic != NROHEADER_MAGIC || h->unk1 || h->unk2 ||
       h->size < 0x3000 || h->size > 0x10000000 ||
       h->bss_size > 0x10000000 ||
       ((h->size | h->bss_size) & 0xfff) ||
       h->segments[0].file_off ||
       h->segments[1].file_off != h->segments[0].size ||
       (u64)h->segments[2].file_off !=
          (u64)h->segments[1].file_off + h->segments[1].size ||
       (u64)h->size != (u64)h->segments[2].file_off + h->segments[2].size ||
       !ounce_range(start.mod_offset, 28, h->segments[0].size) ||
       (start.mod_offset & 3))
      goto done;

   module->mod_offset = start.mod_offset;
   module->image = memalign(0x1000, h->size);
   module->bss = h->bss_size ? memalign(0x1000, h->bss_size) : NULL;
   if (!module->image || (h->bss_size && !module->bss))
      goto done;

   if (module->bss)
      memset(module->bss, 0, h->bss_size);

   if (fseek(file, 0, SEEK_SET) ||
       fread(module->image, 1, h->size, file) != h->size)
      goto done;

   ok = 1;
done:
   fclose(file);

   return ok;
}

static int ounce_relocate(ounce_image_t *module, u64 table, u64 bytes,
      const Elf64_Sym *symbols, u32 count, const char *strings)
{
   u8 *base = (u8*)(uintptr_t)module->base;
   u64 offset;

   if ((table & 7) || bytes % sizeof(Elf64_Rela) ||
       !ounce_range(table, bytes, module->header.size))
      return 0;

   for (offset = 0; offset < bytes; offset += sizeof(Elf64_Rela))
   {
      const Elf64_Rela *r = (const Elf64_Rela*)(base + table + offset);
      u64 value;
      u32 index = ELF64_R_SYM(r->r_info);

      if (ELF64_R_TYPE(r->r_info) == R_AARCH64_NONE)
         continue;

      if (r->r_offset < module->header.segments[2].file_off ||
          (r->r_offset & 7) || !ounce_range(r->r_offset, 8,
             (u64)module->header.size + module->header.bss_size))
         return 0;

      if (ELF64_R_TYPE(r->r_info) == R_AARCH64_RELATIVE)
      {
         if (index || r->r_addend < 0 || (u64)r->r_addend >=
               (u64)module->header.size + module->header.bss_size)
            return 0;

         value = module->base + r->r_addend;
      }
      else if (ELF64_R_TYPE(r->r_info) == 1028 ||
               ELF64_R_TYPE(r->r_info) == 1029)
      {
         /* Traditional AArch64 TLS: DTPMOD64 and DTPREL64. */
         if (!module->tls_init || index >= count ||
             (index && (ELF64_ST_TYPE(symbols[index].st_info) != STT_TLS ||
                        symbols[index].st_shndx == SHN_UNDEF)))
            return 0;

         if (ELF64_R_TYPE(r->r_info) == 1028)
         {
            if (r->r_addend)
               return 0;

            value = 1;
         }
         else
         {
            value = (index ? symbols[index].st_value : 0) + r->r_addend;

            if (value >= module->tls_size)
               return 0;
         }
      }
      else
      {
         const Elf64_Sym *symbol;
         unsigned type = ELF64_R_TYPE(r->r_info);

         if ((type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT &&
              type != R_AARCH64_ABS64) || !index || index >= count)
            return 0;

         symbol = symbols + index;

         if (ELF64_ST_TYPE(symbol->st_info) == STT_TLS)
            return 0;

         if (symbol->st_shndx == SHN_UNDEF)
         {
            value = ounce_host_symbol(strings + symbol->st_name);
            if (!value && ELF64_ST_BIND(symbol->st_info) != STB_WEAK)
            {
               snprintf(ounce_dylib_error, sizeof(ounce_dylib_error),
                     "Unresolved import: %s", strings + symbol->st_name);

               return 0;
            }
         }
         else if (symbol->st_shndx == SHN_ABS)
            value = symbol->st_value;
         else
            value = module->base + symbol->st_value;

         value += r->r_addend;
      }
      *(u64*)(base + r->r_offset) = value;
   }

   return 1;
}

static int ounce_array_valid(ounce_image_t *module, u64 offset, u64 size)
{
   const u64 *entries;
   u64 i;

   if (!size)
      return offset == 0;

   if (!offset || (offset & 7) || (size & 7) ||
       !ounce_range(offset, size, module->header.size))
      return 0;

   entries = (const u64*)(uintptr_t)(module->base + offset);

   for (i = 0; i < size / 8; i++)
   {
      if (entries[i] < module->base || (entries[i] & 3) ||
          !ounce_range(entries[i] - module->base, 4,
             module->header.segments[0].size))
         return 0;
   }

   return 1;
}

static void ounce_array_call(ounce_image_t *module, int finalize)
{
   u64 offset = finalize ? module->fini_array : module->init_array;
   u64 count = (finalize ? module->fini_size : module->init_size) / 8;
   const u64 *entries = (const u64*)(uintptr_t)(module->base + offset);
   u64 i;

   for (i = 0; i < count; i++)
   {
      u64 index = finalize ? count - i - 1 : i;
      void (*function)(void) = (void (*)(void))(uintptr_t)entries[index];
      ounce_trace(module, "%s %lu: module+0x%lx begin\n",
            finalize ? "fini" : "init", (unsigned long)index,
            (unsigned long)(entries[index] - module->base));

      function();

      ounce_trace(module, "%s %lu: complete\n",
            finalize ? "fini" : "init", (unsigned long)index);
   }
}

static int ounce_link(ounce_image_t *module)
{
   u8 *base = (u8*)(uintptr_t)module->base;
   u64 size = module->header.size;
   u32 *mod = (u32*)(base + module->mod_offset);
   s64 dynamic_offset = (s64)module->mod_offset + (s32)mod[1];
   u64 symtab = 0, strtab = 0, strsz = 0, hash = 0;
   u64 rela = 0, relasz = 0, syment = 0, relaent = 0;
   u64 offset, jmprel = 0, pltsz = 0, pltrel = 0;

   u32 count, i;
   int terminated = 0;

   if (mod[0] != NROMOD0_MAGIC || dynamic_offset < 0 ||
       ((u64)dynamic_offset & 7))
      return 0;

   if (mod[6])
   {
      s64 info_offset = (s64)module->mod_offset + (s32)mod[6];
      const u32 *info;
      s64 template_offset, init_offset, close_offset;

      if (info_offset < 0 || (info_offset & 3) ||
          !ounce_range(info_offset, 28, module->header.segments[0].size))
         return 0;

      info = (const u32*)(base + info_offset);
      template_offset = info_offset + (s32)info[1];
      init_offset = info_offset + (s32)info[5];
      close_offset = info_offset + (s32)info[6];

      if (info[0] != 0x30534c54 || info[2] > info[3] ||
          info[3] > 0x10000000 || info[4] < 16 || info[4] > 0x100000 ||
          (info[4] & (info[4] - 1)) || template_offset < 0 ||
          !ounce_range(template_offset, info[2], size) ||
          init_offset < 0 || close_offset < 0 ||
          ((init_offset | close_offset) & 3) ||
          !ounce_range(init_offset, 4, module->header.segments[0].size) ||
          !ounce_range(close_offset, 4, module->header.segments[0].size))
         return 0;

      module->tls_size = info[3];
      module->tls_init = module->base + init_offset;
      module->tls_close = module->base + close_offset;
   }

   for (offset = (u64)dynamic_offset;
        ounce_range(offset, sizeof(Elf64_Dyn), size);
        offset += sizeof(Elf64_Dyn))
   {
      const Elf64_Dyn *entry = (const Elf64_Dyn*)(base + offset);

      if (entry->d_tag == DT_NULL)
      {
         terminated = 1;
         break;
      }

      switch (entry->d_tag)
      {
         case DT_SYMTAB: symtab = entry->d_un.d_ptr; break;
         case DT_STRTAB: strtab = entry->d_un.d_ptr; break;
         case DT_STRSZ: strsz = entry->d_un.d_val; break;
         case DT_SYMENT: syment = entry->d_un.d_val; break;
         case DT_HASH: hash = entry->d_un.d_ptr; break;
         case DT_RELA: rela = entry->d_un.d_ptr; break;
         case DT_RELASZ: relasz = entry->d_un.d_val; break;
         case DT_RELAENT: relaent = entry->d_un.d_val; break;
         case DT_JMPREL: jmprel = entry->d_un.d_ptr; break;
         case DT_PLTRELSZ: pltsz = entry->d_un.d_val; break;
         case DT_PLTREL: pltrel = entry->d_un.d_val; break;
         case DT_INIT_ARRAY: module->init_array = entry->d_un.d_ptr; break;
         case DT_INIT_ARRAYSZ: module->init_size = entry->d_un.d_val; break;
         case DT_FINI_ARRAY: module->fini_array = entry->d_un.d_ptr; break;
         case DT_FINI_ARRAYSZ: module->fini_size = entry->d_un.d_val; break;
         case DT_NEEDED:
         case DT_REL:
         case DT_RELR:
         case DT_INIT:
         case DT_FINI:
         case DT_PREINIT_ARRAY:
         case DT_TEXTREL:
            return 0;
      }
   }

   if (!terminated || !hash || (hash & 3) ||
       !ounce_range(hash, 8, size) || !symtab || (symtab & 7) ||
       syment != sizeof(Elf64_Sym) || !strtab || !strsz ||
       !ounce_range(strtab, strsz, size) || (rela & 7) ||
       (relasz && relaent != sizeof(Elf64_Rela)) ||
       relasz % sizeof(Elf64_Rela) || !ounce_range(rela, relasz, size))
      return 0;

   count = ((const u32*)(base + hash))[1];

   if (!ounce_range(symtab, (u64)count * sizeof(Elf64_Sym), size))
      return 0;

   for (i = 1; i < count; i++)
   {
      const Elf64_Sym *symbol = (const Elf64_Sym*)(base + symtab) + i;
      const char *name;

      if (symbol->st_name >= strsz)
         return 0;

      name = (const char*)(base + strtab + symbol->st_name);

      if (!memchr(name, 0, strsz - symbol->st_name))
         return 0;

      if (ELF64_ST_TYPE(symbol->st_info) == STT_TLS)
      {
         if (!module->tls_init || symbol->st_shndx == SHN_UNDEF ||
             symbol->st_shndx >= SHN_LORESERVE ||
             !ounce_range(symbol->st_value, symbol->st_size, module->tls_size))
            return 0;
         continue;
      }

      if (symbol->st_shndx != SHN_UNDEF && symbol->st_shndx != SHN_ABS &&
           (symbol->st_shndx >= SHN_LORESERVE ||
            !ounce_range(symbol->st_value, symbol->st_size,
               size + module->header.bss_size)))
         return 0;

      if (symbol->st_shndx == SHN_UNDEF &&
          ELF64_ST_BIND(symbol->st_info) != STB_WEAK && !ounce_host_symbol(name))
      {
         snprintf(ounce_dylib_error, sizeof(ounce_dylib_error),
               "Unresolved import: %s", name);
         return 0;
      }
   }
   if ((pltsz && (!jmprel || pltrel != DT_RELA)) ||
       !ounce_relocate(module, rela, relasz,
          (const Elf64_Sym*)(base + symtab), count, (const char*)(base + strtab)) ||
       !ounce_relocate(module, jmprel, pltsz,
          (const Elf64_Sym*)(base + symtab), count, (const char*)(base + strtab)) ||
       !ounce_array_valid(module, module->init_array, module->init_size) ||
       !ounce_array_valid(module, module->fini_array, module->fini_size))
      return 0;

   module->symbols = (const Elf64_Sym*)(base + symtab);
   module->strings = (const char*)(base + strtab);
   module->symbol_count = count;

   return 1;
}


function_t dylib_proc(dylib_t handle, const char *name)
{
   ounce_image_t *module = (ounce_image_t*)handle;
   u32 i;

   if (!module || !name)
      return NULL;

   for (i = 1; i < module->symbol_count; i++)
   {
      const Elf64_Sym *symbol = module->symbols + i;
      if (symbol->st_shndx != SHN_UNDEF &&
          ELF64_ST_TYPE(symbol->st_info) == STT_FUNC &&
          (ELF64_ST_BIND(symbol->st_info) == STB_GLOBAL ||
           ELF64_ST_BIND(symbol->st_info) == STB_WEAK) &&
          !strcmp(module->strings + symbol->st_name, name))
      {
         ounce_trace(module, "export %s: module+0x%lx\n", name,
               (unsigned long)symbol->st_value);

         return (function_t)(uintptr_t)(module->base + symbol->st_value);
      }
   }

   snprintf(ounce_dylib_error, sizeof(ounce_dylib_error),
         "Missing export: %s", name);

   return NULL;
}

void dylib_close(dylib_t handle)
{
   ounce_image_t *module = (ounce_image_t*)handle;
   Result rc;

   if (!module)
      return;

   if (module->tls_initialized)
   {
      int (*close_tls)(int) = (int (*)(int))(uintptr_t)module->tls_close;

      if (!close_tls(0))
      {
         /* Live threads still own storage and callbacks in this mapping. */
         snprintf(ounce_dylib_error, sizeof(ounce_dylib_error),
               "Module retained: other threads still own TLS");

         return;
      }
   }

   if (module->initialized)
   {
      ounce_array_call(module, 1);
      module->initialized = 0;
   }

   if (module->tls_initialized)
   {
      int (*close_tls)(int) = (int (*)(int))(uintptr_t)module->tls_close;

      if (!close_tls(1))
         return;

      module->tls_initialized = 0;
   }

   ounce_trace(module, "unmap begin\n");
   rc = ldrRoUnloadNro(module->base);
   ounce_trace(module, "unmap result %08lx\n", (unsigned long)rc);

   if (R_FAILED(rc))
   {
      /* RO still owns the backing pages; retain them until process exit. */
      snprintf(ounce_dylib_error, sizeof(ounce_dylib_error),
            "UnloadNro: %08lx", (unsigned long)rc);

      return;
   }
#ifdef HAVE_LIBOUNCE_MODULE_TRACE
   if (module->trace)
      fclose(module->trace);
#endif

   free(module->bss);
   free(module->image);
   free(module);

   ldrRoExit();
}

dylib_t dylib_load(const char *path)
{
   ounce_image_t *module;
   Result rc;

   if (!path)
      return NULL;

   module = (ounce_image_t*)calloc(1, sizeof(*module));

   if (!module)
      return NULL;

#ifdef HAVE_LIBOUNCE_MODULE_TRACE
   module->trace = fopen("sdmc:/retroarch/module-loader.log", "a");
#endif
   ounce_trace(module, "\nload %s\n", path);
   ounce_dylib_error[0] = 0;
   ounce_trace(module, "error storage initialized\n");

   if (!ounce_read(module, path))
   {
      snprintf(ounce_dylib_error, sizeof(ounce_dylib_error),
            "Invalid or unreadable NRO module: %s", path);

      goto fail;
   }

   ounce_trace(module, "image read: %lu bytes, BSS %lu bytes\n",
         (unsigned long)module->header.size,
         (unsigned long)module->header.bss_size);

   rc = ldrRoInitialize();
   ounce_trace(module, "RO initialize %08lx\n", (unsigned long)rc);

   if (R_FAILED(rc))
   {
      snprintf(ounce_dylib_error, sizeof(ounce_dylib_error),
            "ldrRoInitialize: %08lx", (unsigned long)rc);

      goto fail;
   }

   rc = ldrRoLoadNro(&module->base, (u64)(uintptr_t)module->image,
         module->header.size, (u64)(uintptr_t)module->bss,
         module->header.bss_size);
   ounce_trace(module, "map result %08lx, base %lx\n",
         (unsigned long)rc, (unsigned long)module->base);

   if (R_FAILED(rc))
   {
      snprintf(ounce_dylib_error, sizeof(ounce_dylib_error),
            "LoadNro: %08lx", (unsigned long)rc);
      ldrRoExit();

      goto fail;
   }

   /* The mapped pages are borrowed by RO until UnloadNro succeeds. */
   ounce_trace(module, "link begin\n");
   if (!ounce_link(module) || !dylib_proc(module, "retro_api_version"))
   {
      if (!ounce_dylib_error[0])
         strcpy(ounce_dylib_error, "Unsupported NRO module ABI");

      ounce_trace(module, "link failed: %s\n", ounce_dylib_error);
      dylib_close(module);

      return NULL;
   }

   ounce_trace(module, "link complete\n");
   if (module->tls_init)
   {
      int (*init_tls)(void) = (int (*)(void))(uintptr_t)module->tls_init;
      if (!init_tls())
      {
         strcpy(ounce_dylib_error, "No module TLS slot available");
         dylib_close(module);
         return NULL;
      }
      module->tls_initialized = 1;
   }
   ounce_array_call(module, 0);
   module->initialized = 1;
   ounce_trace(module, "load complete\n");

   return module;
fail:
   ounce_trace(module, "load failed: %s\n", ounce_dylib_error);
#ifdef HAVE_LIBOUNCE_MODULE_TRACE
   if (module->trace)
      fclose(module->trace);
#endif
   free(module->bss);
   free(module->image);
   free(module);

   return NULL;
}

char *dylib_error(void)
{
   return ounce_dylib_error[0] ? ounce_dylib_error : NULL;
}
#endif
