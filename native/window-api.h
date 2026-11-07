#ifndef MOVIES_WINDOW_API_H
#define MOVIES_WINDOW_API_H
#include "runtime.h"
typedef struct MoviesWindowEvent {
    const char *type,*title,*cursor;
    uint32_t hwnd,style,ex_style;
    int32_t x,y,outer_width,outer_height,client_width,client_height;
    int visible;
    int32_t client_x,client_y; /*virtual screen origin of client area*/
    int minimized; /*visible but iconic; host retains surface and restore dimensions*/
} MoviesWindowEvent;
typedef int (*MoviesWindowHost)(const MoviesWindowEvent *event);
/* Initialize before guest UI execution. One guest UI thread, bounded classes
 * and windows. A host must acknowledge real surface mutations synchronously.
 * Virtual desktop metrics determine maximized size; no host-screen discovery. */
int movies_window_init(uint32_t desktop_width,uint32_t desktop_height);
void movies_window_set_host(MoviesWindowHost host);
/* Return1 with an actually focused guest HWND (or0 for no guest foreground).
 * Return0 when no focus provider exists; no visibility-based focus inference. */
typedef int (*MoviesWindowFocusHost)(uint32_t *hwnd);
void movies_window_set_focus_host(MoviesWindowFocusHost host);
/* Observed VK_ESCAPE/SPACE/RETURN, PageUp/Down, arrows and L/R/M mouse only. State bits:0x8000 current down,
 * bit0 pressed since prior query. Provider atomically consumes the latter.
 * Return0 means unavailable, not a fabricated all-keys-up response. */
typedef int (*MoviesWindowKeyHost)(uint32_t vk,uint16_t *state);
void movies_window_set_key_host(MoviesWindowKeyHost host);
typedef int (*MoviesWindowCursorHost)(int32_t *x,int32_t *y);
void movies_window_set_cursor_host(MoviesWindowCursorHost host);
/* Acknowledge only a real captured virtual-cursor update. No OS pointer warp. */
typedef int (*MoviesWindowCursorWarpHost)(int32_t x,int32_t y);
void movies_window_set_cursor_warp_host(MoviesWindowCursorWarpHost host);
/* Copies live state, no host mutation. Returned string pointers stay owned. */
int movies_window_get(uint32_t hwnd,MoviesWindowEvent *out);
/* Host input timestamps are low32 floor(performance.timeOrigin+performance.now()).
 * Convert generation time, not delivery time, to the process GetTickCount epoch.
 * Both clocks may wrap; ages >=2^31ms (or future stamps) are rejected. */
int movies_window_map_input_time(uint32_t process_tick,uint32_t source_now,uint32_t source_time,uint32_t *out);
ImportHandler *movies_window_handlers(uint32_t *count);
#endif
