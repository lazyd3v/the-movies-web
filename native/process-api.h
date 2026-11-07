#ifndef MOVIES_PROCESS_API_H
#define MOVIES_PROCESS_API_H
#include "runtime.h"
/* Exclusive process-data reservation, below the separately owned guest heap.
 * Call init after runtime_init and before guest execution. No concurrent reinit.
 * The virtual process has PID 1, a quoted executable-only command line, and an
 * initially empty private environment. No host environment variables are exposed.
 * At most256 variables/65536 ANSI block bytes and256 owned snapshots, bounded
 * by the reserved process memory. Unsupported capacity returns out-of-memory. */
#define MOVIES_PROCESS_DATA_BASE 0x02000000u
#define MOVIES_PROCESS_DATA_END  0x02100000u
#define MOVIES_STD_INPUT_HANDLE  0x08000100u
#define MOVIES_STD_OUTPUT_HANDLE 0x08000101u
#define MOVIES_STD_ERROR_HANDLE  0x08000102u
int movies_process_init(uint32_t image_base, const char *virtual_exe_path);
/* Resolver returns 1 for a valid external handle and writes its Win32 file type. */
void movies_process_set_file_resolver(int (*resolver)(uint32_t,uint32_t *));
/* Snapshot is newest-first; returns total count even if capacity is smaller.
 * No callbacks are invoked here. NULL handler registration toggles ignore only. */
size_t movies_process_console_snapshot(uint32_t *out,size_t capacity,int *ignore);
/* Shared GetTickCount timebase, modulo 2^32 milliseconds. Preserves LastError.
 * Returns false before init, for NULL output, or on host clock failure. */
int movies_process_ticks(uint32_t *out);
/* Pure epoch conversion shared by both millisecond APIs; rejects backwards time. */
int movies_process_elapsed_ticks(uint64_t now_ns,uint64_t epoch_ns,uint32_t *out);
/* ExitProcess terminates the native diagnostic or Emscripten runtime; its full
 * DWORD status is logged before native process-status narrowing. No DLL detach
 * callbacks are fabricated by this host compatibility layer. */
/* GlobalMemoryStatus describes the implemented heap/backend allocation budget,
 * with no swap extension, not physical host hardware or unsupported VirtualAlloc. */
/* SYSTEM_INFO is the 36-byte x86 ABI: CPUID family6/model8/stepping3,
 * one virtual CPU/mask1, 4KiB page/64KiB allocation granularity, and
 * GUEST_TIB..GUEST_SPACE-1 address bounds. This is not host CPU/RAM reporting.
 * Current-process pseudo handle is -1; affinity accepts only that handle and
 * mask1. No host affinity mutation or additional VirtualAlloc support exists. */
/* GetSystemPowerStatus reports documented unknown physical-power fields;
 * legacy Reserved1 is zero. No host battery or battery-saver state is exposed. */
ImportHandler *movies_process_handlers(uint32_t *count);
#endif
