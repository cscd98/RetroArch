#include <switch.h>
#include <stddef.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* These offsets are consumed by the test-only exception entry. */
typedef char probe_pc_offset[(offsetof(ThreadExceptionFrameA64, elr_el1) == 88) ? 1 : -1];
typedef char probe_esr_offset[(offsetof(ThreadExceptionFrameA64, esr) == 108) ? 1 : -1];
typedef char probe_far_offset[(offsetof(ThreadExceptionFrameA64, far) == 112) ? 1 : -1];

volatile unsigned probe_fault_count;
extern unsigned long probe_fault_read(void *address);
static FILE *probe_log;
static int worker_ok;

static void report(const char *format, ...)
{
   va_list args;
   va_start(args, format);
   vprintf(format, args);
   va_end(args);
   if (probe_log)
   {
      va_start(args, format);
      vfprintf(probe_log, format, args);
      va_end(args);
      fflush(probe_log);
   }
   consoleUpdate(NULL);
}

static void worker(void *address)
{
   worker_ok = probe_fault_read(address) == 0x1234;
}

static int process_aliases(void *first, void *second)
{
   Handle process = envGetOwnProcessHandle();
   volatile unsigned *source;
   volatile unsigned *a = (volatile unsigned*)first;
   volatile unsigned *b = (volatile unsigned*)second;
   Result rc;
   int pass = 0;
   int mapped_a = 0, mapped_b = 0;

   report("Process alias hints: map=%u unmap=%u handle=%08x\n",
         envIsSyscallHinted(0x74), envIsSyscallHinted(0x75), process);
   if (!process || !envIsSyscallHinted(0x74) || !envIsSyscallHinted(0x75))
   {
      report("UNAVAILABLE: process alias mapping\n");
      return 0;
   }
   source = (volatile unsigned*)memalign(0x1000, 0x2000);
   if (!source)
      return 0;
   source[0] = 0x11223344;
   source[0x1000 / sizeof(*source)] = 0xaabbccdd;
   rc = svcMapProcessMemory(first, process, (u64)(uintptr_t)source, 0x1000);
   report("Map first page: %08x\n", rc);
   if (R_FAILED(rc))
      goto done;
   mapped_a = 1;
   rc = svcMapProcessMemory(second, process, (u64)(uintptr_t)source, 0x1000);
   report("Map alias: %08x\n", rc);
   if (R_FAILED(rc))
      goto done;
   mapped_b = 1;
   if (*a != 0x11223344 || *b != 0x11223344)
      goto done;
   *a = 0x55667788;
   if (*b != 0x55667788 || *source != 0x55667788)
      goto done;
   *b = 0x10203040;
   if (*a != 0x10203040 || *source != 0x10203040)
      goto done;
   rc = svcSetMemoryPermission(first, 0x1000, Perm_R);
   report("Make alias read-only: %08x\n", rc);
   if (R_SUCCEEDED(rc))
   {
      rc = svcSetMemoryPermission(first, 0x1000, Perm_Rw);
      report("Restore alias writable: %08x\n", rc);
      if (R_FAILED(rc))
         goto done;
   }
   rc = svcUnmapProcessMemory(second, process, (u64)(uintptr_t)source, 0x1000);
   report("Unmap alias for remap: %08x\n", rc);
   if (R_FAILED(rc))
      goto done;
   mapped_b = 0;
   rc = svcMapProcessMemory(second, process, (u64)(uintptr_t)source + 0x1000, 0x1000);
   report("Remap second source page: %08x\n", rc);
   if (R_FAILED(rc))
      goto done;
   mapped_b = 2;
   pass = *b == 0xaabbccdd;
done:
   if (mapped_b)
   {
      rc = svcUnmapProcessMemory(second, process, (u64)(uintptr_t)source +
            (mapped_b == 2 ? 0x1000 : 0), 0x1000);
      report("Unmap alias: %08x\n", rc);
      if (R_FAILED(rc))
         return 0;
   }
   if (mapped_a)
   {
      rc = svcUnmapProcessMemory(first, process, (u64)(uintptr_t)source, 0x1000);
      report("Unmap first page: %08x\n", rc);
      if (R_FAILED(rc))
         return 0;
   }
   free((void*)source);
   report("%s: coherent aliases and page remapping\n", pass ? "PASS" : "FAIL");
   return pass;
}

int main(void)
{
   const size_t arena_size = (size_t)14 << 30;
   VirtmemReservation *reservation = NULL;
   void *arena;
   Thread thread;
   PadState pad;
   Result rc;
   int aliases = 0, faults = 0;
   unsigned before;

   consoleInit(NULL);
   padConfigureInput(1, HidNpadStyleSet_NpadStandard);
   padInitializeDefault(&pad);
   probe_log = fopen("sdmc:/retroarch/fastmem-probe.log", "w");
   report("Switch 2 fastmem capability probe\n");
   virtmemLock();
   arena = virtmemFindAslr(arena_size, 0);
   if (arena)
      reservation = virtmemAddReservation(arena, arena_size);
   virtmemUnlock();
   report("14 GiB reservation: %s, base=%p\n", reservation ? "PASS" : "FAIL", arena);
   if (reservation)
   {
      aliases = process_aliases((char*)arena + ((size_t)2 << 30),
            (char*)arena + ((size_t)8 << 30));
      report("About to test recovery at a known unmapped guard address.\n");
      before = probe_fault_count;
      faults = probe_fault_read(arena) == 0x1234 && probe_fault_count == before + 1;
      report("%s: main-thread fault recovery and register preservation\n", faults ? "PASS" : "FAIL");
      rc = threadCreate(&thread, worker, arena, NULL, 0x10000, 0x2c, -2);
      report("Create worker: %08x\n", rc);
      if (R_SUCCEEDED(rc))
      {
         rc = threadStart(&thread);
         report("Start worker: %08x\n", rc);
         if (R_SUCCEEDED(rc))
         {
            rc = threadWaitForExit(&thread);
            faults = faults && R_SUCCEEDED(rc) && worker_ok && probe_fault_count == before + 2;
            report("%s: worker-thread fault recovery\n", faults ? "PASS" : "FAIL");
         }
         else
            faults = 0;
         threadClose(&thread);
      }
      else
         faults = 0;
      /* Keep the reservation until process exit, including on cleanup failure. */
   }
   report("RESULT: aliases=%s recovery=%s\nPress + to exit.\n",
         aliases ? "PASS" : "NOT AVAILABLE", faults ? "PASS" : "FAIL");
   if (probe_log)
      fclose(probe_log);
   while (appletMainLoop())
   {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
         break;
      consoleUpdate(NULL);
   }
   consoleExit(NULL);
   return 0;
}
