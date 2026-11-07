/* Strict bring-up harness for statically recompiled original code.
 * No graphics/menu substitute, no interpreter fallback, no successful API stubs.
 * Unsupported imports stop execution and become the next implementation target.
 */
#include "runtime.h"
#include "wait-profile.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include "startup-api.h"
#include "heap-api.h"
#include "thread-api.h"
#include "process-api.h"
#include "text-api.h"
#include "atomic-api.h"
#include "shell-api.h"
#include "registry-api.h"
#include "file-api.h"
#include "sync-api.h"
#include "time-api.h"
#include "dialog-api.h"
#include "resource-api.h"
#include "window-api.h"
#include "directinput-api.h"
#include "media-com.h"
#include "d3d9-api.h"
#include "audio-api.h"
#ifdef MOVIES_GUEST_THREADS
#include "guest-thread-api.h"
#include <pthread.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#endif
#endif
#include <stdio.h>
#include <stdlib.h>

int dyncode_contains(uint32_t address) { (void)address; return 0; }
void dyncode_register(uint32_t address, uint32_t length) {
    (void)address; (void)length;
    fprintf(stderr, "dynamic code is not supported by the static bootstrap\n");
    exit(5);
}
void interp_call(CPU *cpu, uint32_t address) {
    guest_trap(cpu, address, "interpreter fallback disabled");
}

static CPU cpu;
static int audio_ready;
#ifdef MOVIES_GUEST_THREADS
static pthread_t main_guest_thread;
static int guest_threads_ready;
extern void (*g_preempt_fn)(void);
#ifdef __EMSCRIPTEN__
EM_JS(void, publish_thread_failure_watch, (uint32_t flag, uint32_t message, uint32_t capacity), {
    if (typeof Module.moviesThreadFailureWatch === 'function') {
        Module.moviesThreadFailureWatch({sharedWasmBuffer:HEAPU8.buffer,
            flagOffset:flag,errorOffset:message,errorCapacity:capacity});
    }
});
#endif
/* Only system proxy work is drained here. Arbitrary guest/audio callbacks must
 * not run inside filesystem waits or while a native API holds its own locks. */
static void service_guest_threads(CPU *c) {
    if (!guest_threads_ready || !pthread_equal(main_guest_thread,pthread_self())) return;
#ifdef __EMSCRIPTEN__
    emscripten_main_thread_process_queued_calls();
#endif
    char failure[256];
    if (movies_guest_threads_failed(failure,sizeof failure)) guest_trap(c,0,failure);
}
static void service_guest_preempt(void) { service_guest_threads(&cpu); }
#endif
/* Buffer submission precedes the game's pending-counter increment. Deliver
 * completions at explicit polling boundaries, never immediately after Write
 * or EnterCriticalSection. Audio delivery remains on its owning main thread. */
int movies_import_needs_callback(uint32_t target) {
#ifdef MOVIES_GUEST_THREADS
    if (guest_threads_ready) return pthread_equal(main_guest_thread,pthread_self());
#endif
    if (!audio_ready) return 0;
    const char *name=import_name(target);
    return name && (!strcmp(name,"USER32.DLL!PeekMessageA") ||
                    !strcmp(name,"KERNEL32.DLL!Sleep"));
}
void movies_after_import(CPU *c) {
    uint32_t error=movies_get_last_error();
#ifdef MOVIES_GUEST_THREADS
    service_guest_threads(c);
#endif
    const char *name=import_name(g_import_target);
    if (audio_ready && name && (!strcmp(name,"USER32.DLL!PeekMessageA") ||
                               !strcmp(name,"KERNEL32.DLL!Sleep"))) movies_audio_pump(c);
    movies_set_last_error(error);
}
/* Report dialog text when a noninteractive diagnostic stops without a UI
 * callback. Never choose a response on behalf of the game or its user. */
static void diagnostic_wide(const char *label, uint32_t address) {
    fprintf(stderr, "%s: ", label);
    for (unsigned i = 0; i < 2048; i++) {
        uint64_t p = (uint64_t)address + 2 * i;
        if (!address || p + 2 > GUEST_SPACE) { fputs("<invalid pointer>", stderr); break; }
        uint16_t ch = R16((uint32_t)p);
        if (!ch) break;
        if (ch == '\n' || ch == '\t' || (ch >= 32 && ch < 127)) fputc(ch, stderr);
        else fprintf(stderr, "\\u%04x", ch);
    }
    fputc('\n', stderr);
}
#ifdef __EMSCRIPTEN__
EM_JS(int,wait_profile_option,(void),{return Module['moviesProfileWaits']===true?1:0;});
#endif
int main(int argc, char **argv) {
#ifdef __EMSCRIPTEN__
    movies_wait_profile_init(wait_profile_option(),NULL);
#else
    movies_wait_profile_init(getenv("MOVIES_PROFILE_WAITS")!=NULL,NULL);
#endif
    atexit(movies_wait_profile_flush);

    if (argc != 2) {
        fprintf(stderr, "usage: bootstrap MoviesSE.exe\n");
        return 2;
    }
    LoadedImage image;
    runtime_init(&cpu);
    uint32_t handler_count = 0;
    ImportHandler *handlers = NULL;
    static ImportHandler combined[512];
    if (getenv("MOVIES_STARTUP_APIS")) {
        uint32_t startup_count, heap_count, thread_count, process_count, text_count, atomic_count, shell_count, registry_count, file_count, sync_count, time_count, dialog_count, resource_count, window_count, input_count, media_count;
        ImportHandler *startup = movies_startup_handlers(0x400000, &startup_count);
        ImportHandler *heap = movies_heap_handlers(&heap_count);
        ImportHandler *thread = movies_thread_handlers(&thread_count);
        if (!movies_process_init(0x400000, "C:\\Games\\TheMovies\\MoviesSE.exe")) return 2;
        ImportHandler *process = movies_process_handlers(&process_count);
        ImportHandler *text = movies_text_handlers(&text_count);
        ImportHandler *atomic = movies_atomic_handlers(&atomic_count);
        const char *userdata = getenv("MOVIES_USERDATA_ROOT");
        if (!movies_shell_init(userdata ? userdata : "/userdata")) {
            fprintf(stderr, "Unable to initialize private user-data folders\n");
            return 2;
        }
        ImportHandler *shell = movies_shell_handlers(&shell_count);
        movies_registry_init(userdata ? userdata : "/userdata");
        ImportHandler *registry = movies_registry_handlers(&registry_count);
        char game_root[4096];
        if (!realpath(argv[1], game_root)) { perror("original executable path"); return 2; }
        char *separator = strrchr(game_root, '/');
        if (!separator) return 2;
        if (separator == game_root) game_root[1] = 0; else *separator = 0;
        if (!movies_file_init(game_root, userdata ? userdata : "/userdata")) {
            fprintf(stderr, "Unable to initialize private game filesystem\n"); return 2;
        }
        ImportHandler *file = movies_file_handlers(&file_count);
        movies_sync_init();
        movies_file_set_close_resolver(movies_sync_close_handle);
        ImportHandler *sync = movies_sync_handlers(&sync_count);
        ImportHandler *time = movies_time_handlers(&time_count);
        ImportHandler *dialog = movies_dialog_handlers(&dialog_count);
        ImportHandler *resource = movies_resource_handlers(&resource_count);
        if (!movies_window_init(1024, 768)) return 2;
        ImportHandler *window = movies_window_handlers(&window_count);
        ImportHandler *input = movies_directinput_handlers(&input_count);
        ImportHandler *media = movies_media_handlers(&media_count);
        handler_count = startup_count + heap_count + thread_count + process_count + text_count + atomic_count + shell_count + registry_count + file_count + sync_count + time_count + dialog_count + resource_count + window_count + input_count + media_count;
        if (handler_count > sizeof combined / sizeof combined[0]) return 2;
        memcpy(combined, startup, startup_count * sizeof *startup);
        memcpy(combined + startup_count, heap, heap_count * sizeof *heap);
        memcpy(combined + startup_count + heap_count, thread, thread_count * sizeof *thread);
        memcpy(combined + startup_count + heap_count + thread_count, process, process_count * sizeof *process);
        memcpy(combined + startup_count + heap_count + thread_count + process_count, text, text_count * sizeof *text);
        memcpy(combined + startup_count + heap_count + thread_count + process_count + text_count, atomic, atomic_count * sizeof *atomic);
        memcpy(combined + startup_count + heap_count + thread_count + process_count + text_count + atomic_count, shell, shell_count * sizeof *shell);
        memcpy(combined + startup_count + heap_count + thread_count + process_count + text_count + atomic_count + shell_count, registry, registry_count * sizeof *registry);
        memcpy(combined + startup_count + heap_count + thread_count + process_count + text_count + atomic_count + shell_count + registry_count, file, file_count * sizeof *file);
        memcpy(combined + startup_count + heap_count + thread_count + process_count + text_count + atomic_count + shell_count + registry_count + file_count, sync, sync_count * sizeof *sync);
        memcpy(combined + startup_count + heap_count + thread_count + process_count + text_count + atomic_count + shell_count + registry_count + file_count + sync_count, time, time_count * sizeof *time);
        memcpy(combined + startup_count + heap_count + thread_count + process_count + text_count + atomic_count + shell_count + registry_count + file_count + sync_count + time_count, dialog, dialog_count * sizeof *dialog);
        handlers = combined;
        memcpy(combined + handler_count - resource_count - window_count - input_count - media_count, resource, resource_count * sizeof *resource);
        memcpy(combined + handler_count - window_count - input_count - media_count, window, window_count * sizeof *window);
        memcpy(combined + handler_count - input_count - media_count, input, input_count * sizeof *input);
        memcpy(combined + handler_count - media_count, media, media_count * sizeof *media);
        uint32_t graphics_count;
        ImportHandler *graphics = movies_d3d9_handlers(&graphics_count);
        if (handler_count + graphics_count > sizeof combined / sizeof combined[0]) return 2;
        memcpy(combined + handler_count, graphics, graphics_count * sizeof *graphics);
        handler_count += graphics_count;
        uint32_t audio_count;
        ImportHandler *audio = movies_audio_handlers(&audio_count);
        if (handler_count + audio_count > sizeof combined / sizeof combined[0]) return 2;
        memcpy(combined + handler_count, audio, audio_count * sizeof *audio);
        handler_count += audio_count;
        if (!movies_audio_init_browser()) return 2;
        audio_ready = 1;
#ifdef MOVIES_GUEST_THREADS
        uint32_t guest_thread_count;
        ImportHandler *guest_thread = movies_guest_thread_handlers(&guest_thread_count);
        if (handler_count + guest_thread_count > sizeof combined / sizeof combined[0]) return 2;
        memcpy(combined + handler_count, guest_thread, guest_thread_count * sizeof *guest_thread);
        handler_count += guest_thread_count;
        main_guest_thread = pthread_self();
        if (!movies_guest_threads_init(2)) return 2;
        guest_threads_ready = 1;
#ifdef __EMSCRIPTEN__
        MoviesGuestFailureView failure_view;
        movies_guest_threads_failure_view(&failure_view);
        publish_thread_failure_watch((uint32_t)(uintptr_t)failure_view.ready,
            (uint32_t)(uintptr_t)failure_view.message,failure_view.message_capacity);
#endif
        g_preempt_fn = service_guest_preempt;
        g_preempt_on = 1;
#endif
    }
    set_import_handlers(handlers, handler_count);
    if (load_image(argv[1], &image)) return 2;
    if (getenv("MOVIES_STARTUP_APIS") && !movies_resource_init(image.image_base)) {
        fprintf(stderr, "Unable to parse original executable resources\n");
        return 2;
    }
    register_all_functions();
    if (getenv("MOVIES_STARTUP_APIS") &&
        (!movies_directinput_init() || !movies_media_init() || !movies_d3d9_init() ||
         !movies_startup_register_module("d3d9.dll", "Direct3DCreate9") ||
         !movies_startup_register_module("wmvcore.dll", "WMCreateProfileManager") ||
         !movies_file_register_builtin_module("wmvcore.dll", "WMCreateProfileManager") ||
         !movies_startup_register_module("dinput8.dll", "DirectInput8Create") ||
         !movies_file_register_builtin_module("dinput8.dll", "DirectInput8Create"))) {
        fprintf(stderr, "Unable to initialize compatibility object modules\n");
        return 2;
    }
    GuestFn entry = lookup_function(image.entry);
    if (!entry) {
        fprintf(stderr, "entry %08x was not translated\n", image.entry);
        return 3;
    }
    fprintf(stderr, "original entry=%08x imports=%u; strict bootstrap, %u API handlers\n",
            image.entry, image.num_imports, handler_count);
    jmp_buf trap;
    g_trap_jmp = &trap;
    if (setjmp(trap)) {
        fprintf(stderr, "STOP: %s\n", g_trap_msg);
        fprintf(stderr, "eax=%08x ecx=%08x edx=%08x esp=%08x ebp=%08x\n",
                cpu.eax, cpu.ecx, cpu.edx, cpu.esp, cpu.ebp);
        const char *stopped_import = import_name(g_import_target);
        if (stopped_import && !strcmp(stopped_import, "USER32.DLL!MessageBoxW") &&
            (uint64_t)cpu.esp + 16 <= GUEST_SPACE) {
            diagnostic_wide("Game dialog title", R32(cpu.esp + 12));
            diagnostic_wide("Game dialog text", R32(cpu.esp + 8));
        }
        g_trap_jmp = NULL;
        movies_wait_profile_flush();
        return 4;
    }
    PUSH32(&cpu, GUEST_RETURN_SENTINEL);
    entry(&cpu);
    g_trap_jmp = NULL;
    fprintf(stderr, "original entry returned %08x; this does not establish gameplay\n", cpu.eax);
    movies_wait_profile_flush();
    return 0;
}
