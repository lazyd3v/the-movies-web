/* Bounded Win32 window lifecycle for the original main window. Contracts:
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerclassa
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createwindowexa
 * https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-nccreate
 * https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-create
 * https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-nccalcsize
 * No rendering success is implied: a real host surface must acknowledge create.
 * Virtual decorated-frame metrics:4px border and23px caption; popup has none.
 * This module is confined to the initializing UI thread, including callbacks.
 */
#include "window-api.h"
#include "startup-api.h"
#include "process-api.h"
#include "resource-api.h"
#include "text-api.h"
#include "sync-api.h"
#include "thread-api.h"
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/em_macros.h>
#endif
#ifndef MOVIES_WINDOW_SPACE
#define MOVIES_WINDOW_SPACE GUEST_SPACE
#endif
#define CLASS_LIMIT 16u
#define WINDOW_LIMIT 16u
#define WINDOW_BASE 0x0d000000u
#define STRING_CAP 1024u
#define ARG(n) R32(c->esp+4+4*(n))
typedef struct Class {
    uint16_t atom;uint32_t proc,instance,icon,cursor,brush;char name[STRING_CAP];
} Class;
typedef struct Window {
    MoviesWindowEvent state;Class *class_;uint32_t handle;int creating;
    int32_t normal_x,normal_y,normal_width,normal_height;char title[STRING_CAP*3];
} Window;
#define MESSAGE_LIMIT 256u
typedef struct Message { uint32_t hwnd,message,wparam,lparam,time;int32_t x,y; } Message;
_Static_assert(sizeof(Message)==28,"32-bit MSG wire layout");
static Message messages[MESSAGE_LIMIT];static unsigned message_count;
static int quit_pending;static uint32_t quit_code,quit_time;
static int32_t last_pointer_x,last_pointer_y;
/* GetQueueStatus "new since last query" bits; classes derive from message numbers. */
static uint32_t queue_new_bits;
static int clip_active;static int32_t clip_rect[4];
static uint32_t capture_hwnd;
#define REGISTERED_LIMIT 64u
static char registered_messages[REGISTERED_LIMIT][256];static unsigned registered_count;
/* Fixtures that do not link the wait/thread owners get explicit failures. */
__attribute__((weak)) uint32_t movies_sync_wait(CPU *c,uint32_t count,const uint32_t *ids,int all,uint32_t timeout,uint32_t *error){(void)c;(void)count;(void)ids;(void)all;(void)timeout;*error=50;return 0xffffffffu;}
__attribute__((weak)) uint32_t movies_thread_current_id(void){return 0;}

static Class classes[CLASS_LIMIT];static Window windows[WINDOW_LIMIT];
static uint32_t class_count,next_handle=WINDOW_BASE,current_cursor,desktop_width,desktop_height;
static pthread_t ui_thread;static int initialized;static MoviesWindowHost host;static MoviesWindowFocusHost focus_host;static MoviesWindowKeyHost key_host;
static MoviesWindowCursorHost cursor_host;
static MoviesWindowCursorWarpHost cursor_warp_host;
void movies_window_set_cursor_warp_host(MoviesWindowCursorWarpHost callback){cursor_warp_host=callback;}
void movies_window_set_cursor_host(MoviesWindowCursorHost callback) { cursor_host=callback; }
static int range(uint32_t p,uint32_t n) { return p && (uint64_t)p+n<=MOVIES_WINDOW_SPACE; }
static void ret(CPU *c,unsigned n,uint32_t v) { c->eax=v;c->esp+=4*(n+1); }
static int on_thread(CPU *c) {
    if(!initialized || !pthread_equal(ui_thread,pthread_self())) { guest_trap(c,R32(c->esp),"window API requires initialized guest UI thread");return 0; }return 1;
}
int movies_window_init(uint32_t width,uint32_t height) {
    if(!width || !height || width>8192 || height>8192)return 0;
    memset(classes,0,sizeof classes);memset(windows,0,sizeof windows);class_count=0;
    message_count=0;quit_pending=0;quit_code=0;last_pointer_x=last_pointer_y=0;
    queue_new_bits=0;clip_active=0;capture_hwnd=0;registered_count=0;memset(registered_messages,0,sizeof registered_messages);
    uint32_t tick;if(!movies_process_ticks(&tick))return 0;
    desktop_width=width;desktop_height=height;current_cursor=0;ui_thread=pthread_self();initialized=1;return 1;
}
void movies_window_set_host(MoviesWindowHost callback) { host=callback; }
void movies_window_set_focus_host(MoviesWindowFocusHost callback) { focus_host=callback; }
void movies_window_set_key_host(MoviesWindowKeyHost callback) { key_host=callback; }
static Window *lookup(uint32_t hwnd) { for(unsigned i=0;i<WINDOW_LIMIT;i++)if(windows[i].handle==hwnd && hwnd)return &windows[i];return NULL; }
int movies_window_get(uint32_t hwnd,MoviesWindowEvent *out) {
    if(!initialized || !pthread_equal(ui_thread,pthread_self()) || !out)return 0;
    Window *w=lookup(hwnd);if(!w)return 0;*out=w->state;return 1;
}
static int read_a(uint32_t p,char *out,size_t capacity,int utf8) {
    if(!p) { out[0]=0;return 1; }
    size_t used=0;
    for(unsigned i=0;i<STRING_CAP;i++) {
        if((uint64_t)p+i>0xffffffffull || !range(p+i,1))return 0;
        uint8_t b=R8(p+i);if(!b) { out[used]=0;return 1; }
        uint32_t u=utf8?movies_cp1252_decode(b):b;
        unsigned n=u<0x80?1:u<0x800?2:3;if(!utf8)n=1;
        if(used+n>=capacity)return 0;
        if(n==1)out[used++]=(char)u;
        else if(n==2) { out[used++]=(char)(0xc0|(u>>6));out[used++]=(char)(0x80|(u&63)); }
        else { out[used++]=(char)(0xe0|(u>>12));out[used++]=(char)(0x80|((u>>6)&63));out[used++]=(char)(0x80|(u&63)); }
    }
    return 0;
}
static int same_name(const char *a,const char *b) {
    while(*a && *b)if(movies_western_upper(movies_cp1252_decode((uint8_t)*a++))!=movies_western_upper(movies_cp1252_decode((uint8_t)*b++)))return 0;
    return !*a && !*b;
}
static int metrics(uint32_t style,uint32_t ex,int32_t *border,int32_t *caption) {
    if((style&~0x90ca0000u) || (ex!=0 && ex!=8 && ex!=9))return 0;
    if(style&0x80000000u) { *border=*caption=0;return 1; }
    if((style&0x00c00000u)!=0x00c00000u)return 0;
    *border=4;*caption=23;return 1;
}
#ifdef __EMSCRIPTEN__
EM_JS_DEPS(movies_window_dependencies,"$UTF8ToString");
EM_JS(int, window_bridge,(const char *type,uint32_t hwnd,const char *title,int x,int y,int ow,int oh,int cw,int ch,uint32_t style,uint32_t ex,int visible,const char *cursor,int clientX,int clientY,int minimized),{
    var callback=Module['moviesWindowEvent'];if(typeof callback!=='function')return -1;
    var result=callback({type:UTF8ToString(type),hwnd:hwnd>>>0,title:UTF8ToString(title),x:x,y:y,outerWidth:ow,outerHeight:oh,
        clientWidth:cw,clientHeight:ch,style:style>>>0,exStyle:ex>>>0,visible:!!visible,cursor:UTF8ToString(cursor),clientX:clientX,clientY:clientY,minimized:!!minimized});
    return result===1?1:0;
});
#endif
static int emit(CPU *c,Window *w,const char *type) {
    MoviesWindowEvent event=w?w->state:(MoviesWindowEvent){0};event.type=type;
    if(!event.title)event.title="";event.cursor=current_cursor?"default":"none";
    int result;
    if(host)result=host(&event);
    else {
#ifdef __EMSCRIPTEN__
        result=window_bridge(type,event.hwnd,event.title,event.x,event.y,event.outer_width,event.outer_height,
            event.client_width,event.client_height,event.style,event.ex_style,event.visible,event.cursor,event.client_x,event.client_y,event.minimized);
#else
        result=-1;
#endif
    }
    if(result<0) { guest_trap(c,R32(c->esp),"window operation requires synchronous host surface bridge");return 0; }
    if(result!=1) { movies_set_last_error(31);return 0; }return 1;
}
static uint32_t dispatch(CPU *c,Window *w,uint32_t message,uint32_t wp,uint32_t lp) {
    GuestFn fn=lookup_function(w->class_->proc);
    if(!fn) { guest_trap(c,w->class_->proc,"untranslated original window procedure");return 0; }
    CPU nested=*c;
    if(nested.esp<256 || !range(nested.esp-256,256)) { guest_trap(c,R32(c->esp),"window callback stack outside guest memory");return 0; }
    nested.esp-=128;uint32_t before=nested.esp,args[]={w->handle,message,wp,lp};
    guest_call(&nested,fn,4,args);
    if(nested.esp!=before) { guest_trap(c,w->class_->proc,"window callback violated stdcall stack contract");return 0; }
    return nested.eax;
}
static void register_class(CPU *c) {
    if(!on_thread(c))return;uint32_t p=ARG(0);Class next={0};
    if(!range(p,40) || !R32(p+36) || !read_a(R32(p+36),next.name,sizeof next.name,0) || !next.name[0]) { movies_set_last_error(87);ret(c,1,0);return; }
    next.proc=R32(p+4);next.instance=R32(p+16);next.icon=R32(p+20);next.cursor=R32(p+24);next.brush=R32(p+28);
    if(R32(p) || R32(p+8) || R32(p+12) || R32(p+32)) { movies_set_last_error(50);ret(c,1,0);return; }
    if(!next.proc || !lookup_function(next.proc)) { guest_trap(c,next.proc,"untranslated original window procedure");return; }
    if((next.icon && !movies_resource_icon(next.icon)) || (next.cursor && !movies_resource_cursor(next.cursor)) || (next.brush && !movies_resource_brush(next.brush))) { movies_set_last_error(6);ret(c,1,0);return; }
    unsigned slot=class_count;
    for(unsigned i=0;i<class_count;i++) {
        if(!classes[i].atom) { if(slot==class_count)slot=i;continue; } /*unregistered slot*/
        if(classes[i].instance==next.instance && same_name(classes[i].name,next.name)) { movies_set_last_error(1410);ret(c,1,0);return; }
    }
    if(slot==CLASS_LIMIT) { movies_set_last_error(8);ret(c,1,0);return; }
    next.atom=(uint16_t)(0xc000+slot);classes[slot]=next;if(slot==class_count)class_count++;ret(c,1,next.atom);
}
static void create_window(CPU *c) {
    if(!on_thread(c))return;uint32_t a[12];for(unsigned i=0;i<12;i++)a[i]=ARG(i);
    int32_t border,caption;
    if(a[8] || a[9] || (a[3]&0x10000000u) || !metrics(a[3],a[0],&border,&caption)) { movies_set_last_error(50);ret(c,12,0);return; }
    if(!a[6] || !a[7] || a[6]>8192 || a[7]>8192 || a[4]==0x80000000u || a[5]==0x80000000u) { movies_set_last_error(87);ret(c,12,0);return; }
    if((int64_t)(int32_t)a[4]+a[6]>INT32_MAX || (int64_t)(int32_t)a[5]+a[7]>INT32_MAX) {
        movies_set_last_error(87);ret(c,12,0);return;
    }
    Class *cl=NULL;char name[STRING_CAP];
    if(a[1]>0xffff && !read_a(a[1],name,sizeof name,0)) { movies_set_last_error(87);ret(c,12,0);return; }
    for(unsigned i=0;i<class_count;i++)if(classes[i].atom && classes[i].instance==a[10] && (a[1]<=0xffff?classes[i].atom==a[1]:same_name(classes[i].name,name))) { cl=&classes[i];break; }
    if(!cl) { movies_set_last_error(1407);ret(c,12,0);return; }
    Window *w=NULL;for(unsigned i=0;i<WINDOW_LIMIT;i++)if(!windows[i].handle) { w=&windows[i];break; }
    if(!w) { movies_set_last_error(8);ret(c,12,0);return; }
    memset(w,0,sizeof *w);if(!read_a(a[2],w->title,sizeof w->title,1)) { movies_set_last_error(87);ret(c,12,0);return; }
    if((int32_t)a[6]<=border*2 || (int32_t)a[7]<=border*2+caption) { movies_set_last_error(87);ret(c,12,0);return; }
    w->handle=++next_handle;w->class_=cl;w->creating=1;
    w->state=(MoviesWindowEvent){"create",w->title,NULL,w->handle,a[3],a[0],(int32_t)a[4],(int32_t)a[5],(int32_t)a[6],(int32_t)a[7],(int32_t)a[6]-border*2,(int32_t)a[7]-border*2-caption,0,(int32_t)a[4]+border,(int32_t)a[5]+border+caption,0};
    w->normal_x=w->state.x;w->normal_y=w->state.y;w->normal_width=w->state.outer_width;w->normal_height=w->state.outer_height;
    if(!emit(c,w,"create")) { w->handle=0;ret(c,12,0);return; }
    uint32_t saved_sp=c->esp;
    if(saved_sp<512 || !range(saved_sp-256,256)) { guest_trap(c,R32(c->esp),"window creation scratch outside guest memory");return; }
    uint32_t scratch=saved_sp-128;c->esp=saved_sp-256;
    memset(HP(scratch),0,112);
    W32(scratch+8,desktop_width);W32(scratch+12,desktop_height);W32(scratch+24,64);W32(scratch+28,64);W32(scratch+32,16384);W32(scratch+36,16384);
    dispatch(c,w,0x24,0,scratch); /*WM_GETMINMAXINFO; original callback really runs*/
    uint32_t cs=scratch+40,rect=scratch+88;
    uint32_t create[]={a[11],a[10],a[9],a[8],a[7],a[6],a[5],a[4],a[3],a[2],a[1],a[0]};
    for(unsigned i=0;i<12;i++)W32(cs+4*i,create[i]);
    int accepted=dispatch(c,w,0x81,0,cs)!=0;
    if(accepted) {
        W32(rect,a[4]);W32(rect+4,a[5]);W32(rect+8,a[4]+a[6]);W32(rect+12,a[5]+a[7]);
        dispatch(c,w,0x83,0,rect);
        int64_t cw=(int64_t)(int32_t)R32(rect+8)-(int32_t)R32(rect),ch=(int64_t)(int32_t)R32(rect+12)-(int32_t)R32(rect+4);
        if(cw<=0 || ch<=0 || cw>8192 || ch>8192)accepted=0;
        else {
            int changed=cw!=w->state.client_width || ch!=w->state.client_height || (int32_t)R32(rect)!=w->state.client_x || (int32_t)R32(rect+4)!=w->state.client_y;
            w->state.client_x=(int32_t)R32(rect);w->state.client_y=(int32_t)R32(rect+4);
            w->state.client_width=(int32_t)cw;w->state.client_height=(int32_t)ch;
            if(changed && !emit(c,w,"show"))accepted=0;
            else accepted=dispatch(c,w,1,0,cs)!=0xffffffffu;
        }
    }
    if(!accepted) { dispatch(c,w,0x82,0,0);emit(c,w,"destroy");w->handle=0; }
    else w->creating=0;
    c->esp=saved_sp;ret(c,12,accepted?w->handle:0);
}
static void set_rect(CPU *c) {
    uint32_t out=ARG(0);if(!range(out,16)) { movies_set_last_error(87);ret(c,5,0);return; }
    uint32_t values[4];for(unsigned i=0;i<4;i++)values[i]=ARG(i+1);for(unsigned i=0;i<4;i++)W32(out+4*i,values[i]);ret(c,5,1);
}
static void adjust_rect(CPU *c) {
    uint32_t out=ARG(0),style=ARG(1),menu=ARG(2);int32_t border,caption;
    if(!range(out,16)) { movies_set_last_error(87);ret(c,3,0);return; }
    if(menu || !metrics(style,0,&border,&caption)) { movies_set_last_error(50);ret(c,3,0);return; }
    int64_t values[4]={(int64_t)(int32_t)R32(out)-border,(int64_t)(int32_t)R32(out+4)-border-caption,
        (int64_t)(int32_t)R32(out+8)+border,(int64_t)(int32_t)R32(out+12)+border};
    for(unsigned i=0;i<4;i++)if(values[i]<INT32_MIN || values[i]>INT32_MAX) { movies_set_last_error(87);ret(c,3,0);return; }
    for(unsigned i=0;i<4;i++)W32(out+4*i,(uint32_t)values[i]);ret(c,3,1);
}
static void client_rect(CPU *c) {
    if(!on_thread(c))return;Window *w=lookup(ARG(0));uint32_t out=ARG(1);
    if(!w || !range(out,16)) { movies_set_last_error(w?87:1400);ret(c,2,0);return; }
    W32(out,0);W32(out+4,0);W32(out+8,w->state.client_width);W32(out+12,w->state.client_height);ret(c,2,1);
}
static int apply_show(CPU *c,Window *w,uint32_t mode) {
    MoviesWindowEvent old=w->state;int was_visible=old.visible;w->state.visible=mode!=0;
    if(mode==7)w->state.minimized=1;
    else if(mode==1 || mode==3 || mode==9)w->state.minimized=0;
    int32_t border,caption;metrics(w->state.style,w->state.ex_style,&border,&caption);
    if(mode==3) {
        w->state.x=0;w->state.y=0;w->state.outer_width=(int32_t)desktop_width;w->state.outer_height=(int32_t)desktop_height;
        w->state.client_width=(int32_t)desktop_width-2*border;w->state.client_height=(int32_t)desktop_height-2*border-caption;
    } else if(mode==1 || mode==9) {
        w->state.x=w->normal_x;w->state.y=w->normal_y;w->state.outer_width=w->normal_width;w->state.outer_height=w->normal_height;
        w->state.client_width=w->normal_width-2*border;w->state.client_height=w->normal_height-2*border-caption;
    }
    w->state.client_x=w->state.x+border;w->state.client_y=w->state.y+border+caption;
    if(!emit(c,w,"show")) { w->state=old;return 0; }
    if(mode!=7 || !was_visible)dispatch(c,w,0x18,w->state.visible,0); /*WM_SHOWWINDOW*/
    if(w->state.visible) {
        if(w->state.minimized)dispatch(c,w,5,1,0); /*SIZE_MINIMIZED; restore geometry retained*/
        else {
        dispatch(c,w,5,mode==3?2:0,((uint32_t)w->state.client_height<<16)|(uint32_t)w->state.client_width);
        dispatch(c,w,3,0,((uint32_t)(w->state.y+border+caption)<<16)|((uint32_t)(w->state.x+border)&0xffffu));
        }
    }
    return 1;
}
static void show_window(CPU *c) {
    if(!on_thread(c))return;Window *w=lookup(ARG(0));uint32_t mode=ARG(1);
    if(!w) { movies_set_last_error(1400);ret(c,2,0);return; }
    if(mode!=0 && mode!=1 && mode!=3 && mode!=5 && mode!=7 && mode!=9) { movies_set_last_error(50);ret(c,2,0);return; }
    int was_visible=w->state.visible;uint32_t caller=R32(c->esp);
    int accepted=apply_show(c,w,mode);
    static unsigned show_logs;
    if(show_logs++<64)fprintf(stderr,"[window] ShowWindow caller=%08x hwnd=%08x mode=%u accepted=%d visible=%d minimized=%d\n",caller,w->handle,mode,accepted,w->state.visible,w->state.minimized);
    ret(c,2,accepted?(uint32_t)was_visible:0);
}
static void set_cursor(CPU *c) {
    if(!on_thread(c))return;uint32_t cursor=ARG(0),old=current_cursor;
    if(cursor && !movies_resource_cursor(cursor)) { movies_set_last_error(6);ret(c,1,0);return; }
    current_cursor=cursor;Window *w=NULL;for(unsigned i=0;i<WINDOW_LIMIT;i++)if(windows[i].handle) { w=&windows[i];break; }
    if(!emit(c,w,"cursor")) { current_cursor=old;ret(c,1,0);return; }ret(c,1,old);
}
static void default_proc(CPU *c) {
    if(!on_thread(c))return;Window *w=lookup(ARG(0));uint32_t message=ARG(1),wp=ARG(2),lp=ARG(3),result=0;
    if(!w) { movies_set_last_error(1400);ret(c,4,0);return; }
    switch(message) {
    case 0x112: /*WM_SYSCOMMAND: low four bits reserved by Windows.*/
        if((wp&0xfff0u)!=0xf120u) { fprintf(stderr,"[window] unsupported DefWindowProcA caller=%08x hwnd=%08x message=%08x wParam=%08x lParam=%08x\n",R32(c->esp),w->handle,message,wp,lp);guest_trap(c,R32(c->esp),"unsupported WM_SYSCOMMAND action");return; }
        (void)apply_show(c,w,9); /*SC_RESTORE; host rejection preserves previous state.*/
        break;
    case 0x81:result=1;break; /*WM_NCCREATE accepts the real created surface*/
    case 0x83: { /*WM_NCCALCSIZE: observed creation form, one RECT in screen coords*/
        if(wp || !range(lp,16)) { guest_trap(c,R32(c->esp),"unsupported WM_NCCALCSIZE form");return; }
        int32_t b,t;metrics(w->state.style,w->state.ex_style,&b,&t);
        int64_t rect[4]={(int64_t)(int32_t)R32(lp)+b,(int64_t)(int32_t)R32(lp+4)+b+t,
            (int64_t)(int32_t)R32(lp+8)-b,(int64_t)(int32_t)R32(lp+12)-b};
        for(unsigned i=0;i<4;i++)if(rect[i]<INT32_MIN || rect[i]>INT32_MAX) { guest_trap(c,R32(c->esp),"WM_NCCALCSIZE overflow");return; }
        for(unsigned i=0;i<4;i++)W32(lp+4*i,(uint32_t)rect[i]);break;
    }
    case 0x24: /*WM_GETMINMAXINFO initialized with virtual desktop constraints*/
    case 0:case 1:case 2:case 3:case 5:case 6:case 7:case 8:case 0x18:case 0x1c:case 0x82:
        break; /*Defined notifications need no further default action in this subset.*/
    /* Original WndProc 0x00542380 switches only on 7,8,0xF,0x10,0x12,0x20 and
     * 0x100/0x101/0x104/0x105 (table 0x54263c/index 0x54265c); everything else,
     * including host-injected client mouse input, reaches DefWindowProcA at
     * 0x0054262a unless the UI hook 0x00a097f0 claims it. For a top-level window
     * without a parent, DefWindowProc performs no action for client-area mouse
     * messages (WM_MOUSEWHEEL/HWHEEL forward only to a parent) and plain
     * character/key messages; all return 0. WM_XBUTTONUP's WM_APPCOMMAND and
     * WM_SYSKEYDOWN/WM_SYSKEYUP/WM_SYSCHAR menu semantics are not claimed and keep trapping.
     * https://learn.microsoft.com/en-us/windows/win32/inputdev/mouse-input-notifications */
    case 0x100:case 0x101:case 0x102:case 0x103:case 0x109:
    case 0x200:case 0x201:case 0x202:case 0x203:case 0x204:case 0x205:case 0x206:
    case 0x207:case 0x208:case 0x209:case 0x20a:case 0x20b:case 0x20e:
        break;
    default:fprintf(stderr,"[window] unsupported DefWindowProcA caller=%08x hwnd=%08x message=%08x wParam=%08x lParam=%08x\n",R32(c->esp),w->handle,message,wp,lp);guest_trap(c,R32(c->esp),"unsupported DefWindowProcA message");return;
    }
    ret(c,4,result);
}
static uint32_t message_time(CPU *c) {
    uint32_t tick=0;
    if(!movies_process_ticks(&tick))guest_trap(c,R32(c->esp),"window process clock unavailable");
    return tick;
}
/* Event times come from the page's clock and source_now from the game worker's;
 * the two can disagree by a few milliseconds, so an event may appear slightly
 * newer than "now". Up to one minute of such skew counts as age 0; larger
 * differences are rejected as corrupt. */
#define MOVIES_INPUT_CLOCK_SKEW 60000u
int movies_window_map_input_time(uint32_t tick,uint32_t source_now,uint32_t source_time,uint32_t *out) {
    uint32_t age=source_now-source_time;
    if(!out)return 0;
    if(age>=0x80000000u){if(source_time-source_now>MOVIES_INPUT_CLOCK_SKEW)return 0;age=0;}
    *out=tick-age;return 1;
}
#ifdef __EMSCRIPTEN__
/* Main-thread DOM events must arrive through a shared ring polled here. A
 * postMessage-only transport cannot service a worker blocked in guest code. */
EM_JS(int, poll_host_messages,(uint32_t *out,unsigned capacity,uint32_t *source_now), {
    var poll=Module['moviesPollWindowEvents'];if(typeof poll!=='function')return 0;
    var batch=poll(capacity);
    if(!Array.isArray(batch) || batch.length>capacity)throw new Error('Invalid window event batch');
    for(var i=0;i<batch.length;i++) {
        var e=batch[i],fields=[e.hwnd,e.message,e.wParam,e.lParam,e.time,e.x,e.y];
        for(var j=0;j<7;j++) {
            var v=fields[j];
            if(!Number.isInteger(v) || v< -2147483648 || v>4294967295)throw new Error('Invalid window event field');
            HEAPU32[(out>>>2)+i*7+j]=v>>>0;
        }
    }
    HEAPU32[source_now>>>2]=Math.floor(performance.timeOrigin+performance.now())>>>0;
    return batch.length;
});
#endif
/* QS_KEY 1, QS_MOUSEMOVE 2, QS_MOUSEBUTTON 4, QS_POSTMESSAGE 8|QS_ALLPOSTMESSAGE 0x100.
 * Classification is by message number; no message is ever a sent message. */
static uint32_t queue_class(uint32_t message) {
    if(message>=0x100 && message<=0x109)return 1;
    if(message==0x200)return 2;
    if(message>=0x201 && message<=0x20e)return 4;
    return 0x108;
}
static void poll_input(CPU *c) {
#ifdef __EMSCRIPTEN__
    Message batch[32];unsigned available=MESSAGE_LIMIT-message_count;
    if(available>32)available=32;if(!available)return;
    uint32_t source_now=0;
    int count=poll_host_messages((uint32_t *)batch,available,&source_now);
    uint32_t tick=message_time(c);
    for(int i=0;i<count;i++) {
        Message *m=&batch[i];
        if(!lookup(m->hwnd) || !((m->message>=0x100 && m->message<=0x109) ||
            (m->message>=0x200 && m->message<=0x20e) || m->message==0x10 ||
            /* WM_SETFOCUS/WM_KILLFOCUS from page focus changes; wParam is the other hwnd or 0. */
            ((m->message==7 || m->message==8) && !m->lparam && (!m->wparam || lookup(m->wparam))) ||
            (m->message==0x112 && (m->wparam&0xfff0u)==0xf120u))) {
            guest_trap(c,R32(c->esp),"unsupported host window event");return;
        }
        if(!movies_window_map_input_time(tick,source_now,m->time,&m->time)) {
            guest_trap(c,R32(c->esp),"host window event timestamp outside supported age");return;
        }
        if(m->message!=7 && m->message!=8){last_pointer_x=m->x;last_pointer_y=m->y;}messages[message_count++]=*m;
        queue_new_bits|=queue_class(m->message);
    }
#else
    (void)c;
#endif
}
static int read_message(uint32_t p,Message *out) {
    if(!range(p,28))return 0;
    *out=(Message){R32(p),R32(p+4),R32(p+8),R32(p+12),R32(p+16),(int32_t)R32(p+20),(int32_t)R32(p+24)};return 1;
}
static void write_message(uint32_t p,const Message *m) {
    uint32_t fields[]={m->hwnd,m->message,m->wparam,m->lparam,m->time,(uint32_t)m->x,(uint32_t)m->y};
    for(unsigned i=0;i<7;i++)W32(p+4*i,fields[i]);
}
static void post_message(CPU *c) {
    if(!on_thread(c))return;
    uint32_t hwnd=ARG(0),message=ARG(1),wp=ARG(2),lp=ARG(3);
    if(hwnd && !lookup(hwnd)) { movies_set_last_error(hwnd==0xffffu?50:1400);ret(c,4,0);return; }
    /* Asynchronous messages with guest pointers need capture/marshalling; only
     * observed scalar notification/key/mouse messages and WM_USER+ are admitted. */
    if(message<0x400 && message!=0 && message!=0x10 && message!=0x12 && !(message==0x112 && (wp&0xfff0u)==0xf120u) &&
       !(message>=0x100 && message<=0x109) && !(message>=0x200 && message<=0x20e)) {
        movies_set_last_error(50);ret(c,4,0);return;
    }
    if(message_count==MESSAGE_LIMIT) { movies_set_last_error(1816);ret(c,4,0);return; }
    messages[message_count++]=(Message){hwnd,message,wp,lp,message_time(c),last_pointer_x,last_pointer_y};queue_new_bits|=queue_class(message);ret(c,4,1);
}
static void post_quit(CPU *c) {
    if(!on_thread(c))return;quit_code=ARG(0);quit_time=message_time(c);quit_pending=1;queue_new_bits|=0x108;c->esp+=8;
}
static void peek_message(CPU *c) {
    if(!on_thread(c))return;
    uint32_t out=ARG(0),hwnd=ARG(1),min=ARG(2),max=ARG(3),flags=ARG(4);
    if(!range(out,28) || min>max) { movies_set_last_error(87);ret(c,5,0);return; }
    if(flags&~3u) { movies_set_last_error(50);ret(c,5,0);return; }
    if(hwnd && hwnd!=0xffffffffu && !lookup(hwnd)) { movies_set_last_error(1400);ret(c,5,0);return; }
    poll_input(c);queue_new_bits=0;
    for(unsigned i=0;i<message_count;i++) {
        Message *m=&messages[i];int quit=m->message==0x12;
        int window_match=!hwnd || (hwnd==0xffffffffu?m->hwnd==0:m->hwnd==hwnd);
        if(!quit && (!window_match || ((min||max) && (m->message<min || m->message>max))))continue;
        write_message(out,m);
        if(flags&1) { memmove(messages+i,messages+i+1,(message_count-i-1)*sizeof *messages);--message_count; }
        ret(c,5,1);return;
    }
    if(quit_pending) {
        Message quit={0,0x12,quit_code,0,quit_time,last_pointer_x,last_pointer_y};write_message(out,&quit);
        if(flags&1)quit_pending=0;ret(c,5,1);return;
    }
    ret(c,5,0);
}
static void dispatch_message(CPU *c) {
    if(!on_thread(c))return;Message m;
    if(!read_message(ARG(0),&m)) { movies_set_last_error(87);ret(c,1,0);return; }
    if(!m.hwnd || m.message==0x12) { ret(c,1,0);return; }
    Window *w=lookup(m.hwnd);if(!w) { movies_set_last_error(1400);ret(c,1,0);return; }
    if((m.message==0x113 || m.message==0x118) && m.lparam) { guest_trap(c,R32(c->esp),"timer callback dispatch unsupported");return; }
    uint32_t result=dispatch(c,w,m.message,m.wparam,m.lparam);ret(c,1,result);
}
static void translate_message(CPU *c) {
    if(!on_thread(c))return;Message m;
    if(!read_message(ARG(0),&m)) { movies_set_last_error(87);ret(c,1,0);return; }
    /* No keyboard-layout or dead-key conversion is claimed. Host text events
     * can be posted separately; this must not invent/consume a WM_CHAR. */
    if(m.message==0x100 || m.message==0x101 || m.message==0x104 || m.message==0x105)movies_set_last_error(50);
    ret(c,1,0);
}
#ifdef __EMSCRIPTEN__
EM_JS(int, foreground_bridge,(uint32_t *out),{
    const query=Module.moviesForegroundWindow;if(typeof query!=='function')return 0;
    const hwnd=query();if(!Number.isInteger(hwnd)||hwnd<0||hwnd>0xffffffff)return 0;
    HEAPU32[out>>>2]=hwnd;return 1;
});
#endif
/* Original GetForegroundWindow call returns at0045DF3D. The host shared input
 * control records actual canvas/document focus; showing a window is insufficient.
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getforegroundwindow */
static void foreground_window(CPU *c) {
    if(!on_thread(c))return;
    uint32_t hwnd=0;int available=focus_host?focus_host(&hwnd):0;
#ifdef __EMSCRIPTEN__
    if(!focus_host)available=foreground_bridge(&hwnd);
#endif
    if(!available){guest_trap(c,R32(c->esp),"GetForegroundWindow requires actual host focus state");return;}
    Window *w=lookup(hwnd);
    ret(c,0,w&&w->state.visible&&!w->state.minimized&&!w->creating?hwnd:0);
}
#ifdef __EMSCRIPTEN__
EM_JS(int, async_key_bridge,(uint32_t vk,uint16_t *out),{
    const query=Module.moviesAsyncKeyState;if(typeof query!=='function')return 0;
    const state=query(vk);if(!Number.isInteger(state)||state<0||state>0xffff)return 0;
    HEAPU16[out>>>1]=state;return 1;
});
#endif
/* Intro45DF30 tests AX sign for Escape/Space/Return, not the low edge bit.
 * Drawing/update9E9C70 additionally queries PageUp/Down, arrows and physical
 * left/right/middle mouse buttons (first new return9E9D5B, VK_PRIOR).
 * No global host key interception: browser canvas focus scopes actual events.
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getasynckeystate */
static void async_key_state(CPU *c) {
    if(!on_thread(c))return;uint32_t vk=ARG(0);
    if(vk!=0x1b && vk!=0x20 && vk!=0x0d && vk!=0x21 && vk!=0x22 && vk!=0x25 && vk!=0x27 && vk!=0x26 && vk!=0x28 && vk!=1 && vk!=2 && vk!=4){char message[80];snprintf(message,sizeof message,"unsupported GetAsyncKeyState virtual key 0x%08x",vk);guest_trap(c,R32(c->esp),message);return;}
    uint16_t state=0;int available=key_host?key_host(vk,&state):0;
#ifdef __EMSCRIPTEN__
    if(!key_host)available=async_key_bridge(vk,&state);
#endif
    if(!available || (state&~0x8001u)){guest_trap(c,R32(c->esp),"GetAsyncKeyState requires actual bounded host key state");return;}
    ret(c,1,(uint32_t)(int32_t)(int16_t)state);
}
/* POINT coordinates use the virtual desktop, never host screen coordinates.
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getcursorpos
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-screentoclient */
#ifdef __EMSCRIPTEN__
EM_JS(int,host_cursor,(int32_t *out),{
    const query=Module.moviesCursorPosition;if(typeof query!=='function')return 0;
    const p=query();if(!p)return 0;
    if(!Number.isInteger(p.x)||!Number.isInteger(p.y)||p.x<-2147483648||p.x>2147483647||p.y<-2147483648||p.y>2147483647)return 0;
    HEAP32[out>>2]=p.x;HEAP32[(out>>2)+1]=p.y;return 1;
});
#endif
/* ToUnicode/ToAscii for the US keyboard layout, driven by the key-state array the
 * caller passes (the original keeps its own at 0104c7f0): Shift, Caps Lock, Ctrl.
 * Returns 1 and one character, or 0 for keys without a character (dead keys and
 * Ctrl/Alt chords are not produced). https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-tounicode */
static int us_character(uint32_t vk,uint32_t state,uint16_t *out) {
    uint8_t ks[256];memset(ks,0,sizeof ks);if(state && range(state,256))for(unsigned i=0;i<256;i++)ks[i]=R8(state+i);
    int shift=((ks[0x10]|ks[0xa0]|ks[0xa1])&0x80)!=0,caps=(ks[0x14]&1)!=0,ctrl=((ks[0x11]|ks[0xa2]|ks[0xa3])&0x80)!=0,alt=((ks[0x12]|ks[0xa4]|ks[0xa5])&0x80)!=0;
    if(ctrl || alt)return 0;
    if(vk>='A' && vk<='Z') { *out=(uint16_t)((shift^caps)?vk:vk+32);return 1; }
    if(vk>='0' && vk<='9') { static const char shifted[]=")!@#$%^&*(";*out=(uint16_t)(shift?shifted[vk-'0']:vk);return 1; }
    if(vk>=0x60 && vk<=0x69) { *out=(uint16_t)('0'+vk-0x60);return 1; }
    char c=0;
    switch(vk) {
        case 0x6a:c='*';break;case 0x6b:c='+';break;case 0x6d:c='-';break;case 0x6e:c='.';break;case 0x6f:c='/';break;
        case 0x20:c=' ';break;case 0x0d:c='\r';break;case 0x08:c='\b';break;case 0x09:c='\t';break;case 0x1b:c=0x1b;break;
        case 0xba:c=shift?':':';';break;case 0xbb:c=shift?'+':'=';break;case 0xbc:c=shift?'<':',';break;case 0xbd:c=shift?'_':'-';break;
        case 0xbe:c=shift?'>':'.';break;case 0xbf:c=shift?'?':'/';break;case 0xc0:c=shift?'~':'`';break;case 0xdb:c=shift?'{':'[';break;
        case 0xdc:case 0xe2:c=shift?'|':'\\';break;case 0xdd:c=shift?'}':']';break;case 0xde:c=shift?'"':'\'';break;
    }
    if(!c)return 0;*out=(uint16_t)(uint8_t)c;return 1;
}
static void to_unicode(CPU *c) {
    uint32_t vk=ARG(0),state=ARG(2),buffer=ARG(3),capacity=ARG(4);uint16_t ch;
    int n=us_character(vk,state,&ch);
    if(n && capacity>=1 && range(buffer,2)) { W16(buffer,ch);if(capacity>=2 && range(buffer,4))W16(buffer+2,0); } else n=0;
    ret(c,6,(uint32_t)n);
}
static void to_ascii(CPU *c) {
    uint32_t vk=ARG(0),state=ARG(2),out=ARG(3);uint16_t ch;
    int n=us_character(vk,state,&ch);
    if(n && range(out,2))W16(out,ch);else n=0;
    ret(c,5,(uint32_t)n);
}
/* Windows default double-click interval (ms). */
static void double_click_time(CPU *c) { ret(c,0,500); }
static void cursor_position(CPU *c) {
    if(!on_thread(c))return;uint32_t p=ARG(0);int32_t point[2];
    if(!range(p,8)){movies_set_last_error(87);ret(c,1,0);return;}
    int available=cursor_host?cursor_host(&point[0],&point[1]):0;
#ifdef __EMSCRIPTEN__
    if(!cursor_host)available=host_cursor(point);
#endif
    static unsigned diag_get;if(diag_get++<12||diag_get%2000==0)fprintf(stderr,"[cursor] GetCursorPos caller=%08x ok=%d point=%d,%d calls=%u\n",R32(c->esp),available,available?point[0]:0,available?point[1]:0,diag_get);
    if(!available){movies_set_last_error(21);ret(c,1,0);return;}
    memcpy(HP(p),point,8);ret(c,1,1);
}
#ifdef __EMSCRIPTEN__
EM_JS(int,host_cursor_warp,(int32_t x,int32_t y),{
    const setter=Module.moviesSetCursorPosition;if(typeof setter!=='function')return 0;
    const result=setter(x,y);if(result&&typeof result.then==='function')throw new Error('Cursor warp acknowledgement must be synchronous');
    return result===true||result===1?1:0;
});
#endif
/* Original555220 recenters every active relative-input tick and ignores BOOL.
 * Success means the captured virtual cursor changed, never an OS pointer warp.
 * The default ClipCursor area is our single virtual desktop. No artificial
 * DirectInput motion or button changes are produced by this operation.
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcursorpos */
static void set_cursor_position(CPU *c){
    if(!on_thread(c))return;int32_t x=(int32_t)ARG(0),y=(int32_t)ARG(1);
    if(x<0)x=0;else if((uint32_t)x>=desktop_width)x=(int32_t)desktop_width-1;
    if(y<0)y=0;else if((uint32_t)y>=desktop_height)y=(int32_t)desktop_height-1;
    if(clip_active) {
        if(x<clip_rect[0])x=clip_rect[0];else if(x>=clip_rect[2])x=clip_rect[2]-1;
        if(y<clip_rect[1])y=clip_rect[1];else if(y>=clip_rect[3])y=clip_rect[3]-1;
    }
    int accepted=cursor_warp_host?cursor_warp_host(x,y):0;
#ifdef __EMSCRIPTEN__
    if(!cursor_warp_host)accepted=host_cursor_warp(x,y);
#endif
    static unsigned diag_set,diag_set_failed;if(!accepted)diag_set_failed++;if(diag_set++<12||diag_set%2000==0)fprintf(stderr,"[cursor] SetCursorPos caller=%08x point=%d,%d accepted=%d calls=%u failed=%u\n",R32(c->esp),x,y,accepted,diag_set,diag_set_failed);
    if(!accepted){movies_set_last_error(50);ret(c,2,0);return;}
    last_pointer_x=x;last_pointer_y=y;ret(c,2,1);
}
static void screen_to_client(CPU *c) {
    if(!on_thread(c))return;Window *w=lookup(ARG(0));uint32_t p=ARG(1);int32_t point[2];
    if(!w){movies_set_last_error(1400);ret(c,2,0);return;}
    if(!range(p,8)){movies_set_last_error(87);ret(c,2,0);return;}
    memcpy(point,HP(p),8);int64_t x=(int64_t)point[0]-w->state.client_x,y=(int64_t)point[1]-w->state.client_y;
    if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX){movies_set_last_error(87);ret(c,2,0);return;}
    point[0]=(int32_t)x;point[1]=(int32_t)y;memcpy(HP(p),point,8);ret(c,2,1);
}
static void client_to_screen(CPU *c) {
    if(!on_thread(c))return;Window *w=lookup(ARG(0));uint32_t p=ARG(1);int32_t point[2];
    if(!w){movies_set_last_error(1400);ret(c,2,0);return;}
    if(!range(p,8)){movies_set_last_error(87);ret(c,2,0);return;}
    memcpy(point,HP(p),8);int64_t x=(int64_t)point[0]+w->state.client_x,y=(int64_t)point[1]+w->state.client_y;
    if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX){movies_set_last_error(87);ret(c,2,0);return;}
    point[0]=(int32_t)x;point[1]=(int32_t)y;memcpy(HP(p),point,8);ret(c,2,1);
}
/* Virtual-desktop confinement only: later SetCursorPos warps are clamped to the
 * rectangle (right/bottom exclusive). Browser pointer lock is not changed here.
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-clipcursor */
static void clip_cursor(CPU *c) {
    if(!on_thread(c))return;uint32_t p=ARG(0);
    if(!p){clip_active=0;ret(c,1,1);return;}
    if(!range(p,16)){movies_set_last_error(87);ret(c,1,0);return;}
    int32_t r[4];memcpy(r,HP(p),16);
    if(r[0]<0)r[0]=0;if(r[1]<0)r[1]=0;
    if(r[2]>(int32_t)desktop_width)r[2]=(int32_t)desktop_width;if(r[3]>(int32_t)desktop_height)r[3]=(int32_t)desktop_height;
    if(r[2]<=r[0] || r[3]<=r[1]){clip_active=0;ret(c,1,1);return;} /*degenerate: whole desktop*/
    memcpy(clip_rect,r,16);clip_active=1;ret(c,1,1);
}
/* Capture is bookkeeping: host pointer capture already follows button presses. */
static void set_capture(CPU *c) {
    if(!on_thread(c))return;uint32_t hwnd=ARG(0),old=capture_hwnd;
    if(hwnd && !lookup(hwnd)){movies_set_last_error(1400);ret(c,1,0);return;}
    capture_hwnd=hwnd;ret(c,1,old);
}
static void release_capture(CPU *c) { if(!on_thread(c))return;capture_hwnd=0;ret(c,0,1); }
/* Device contexts are identities for metric queries; no GDI drawing exists.
 * Screen DC = DC_BASE, window DC = DC_BASE+1+slot. */
#define DC_BASE 0x0d200000u
static void get_dc(CPU *c) {
    if(!on_thread(c))return;uint32_t hwnd=ARG(0);
    if(!hwnd){ret(c,1,DC_BASE);return;}
    for(unsigned i=0;i<WINDOW_LIMIT;i++)if(windows[i].handle==hwnd){ret(c,1,DC_BASE+1+i);return;}
    movies_set_last_error(1400);ret(c,1,0);
}
/* Virtual 32bpp display at the current virtual desktop size, 96 DPI, 60Hz.
 * https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getdevicecaps */
static void device_caps(CPU *c) {
    uint32_t dc=ARG(0),index=ARG(1),value=0;
    if(dc) switch(index) {
        case 8:value=desktop_width;break;   /*HORZRES*/
        case 10:value=desktop_height;break; /*VERTRES*/
        case 12:value=32;break;             /*BITSPIXEL*/
        case 14:value=1;break;              /*PLANES*/
        case 88:case 90:value=96;break;     /*LOGPIXELSX/Y*/
        case 116:value=60;break;            /*VREFRESH*/
    }
    ret(c,2,value);
}
/* Stock objects ignore deletion (TRUE). No other GDI objects are created. */
static void delete_object(CPU *c) {
    uint32_t object=ARG(0);
    if(object && movies_resource_brush(object)){ret(c,1,1);return;}
    movies_set_last_error(6);ret(c,1,0);
}
static void set_rect_empty(CPU *c) {
    uint32_t out=ARG(0);if(!range(out,16)){movies_set_last_error(87);ret(c,1,0);return;}
    memset(HP(out),0,16);ret(c,1,1);
}
/* No update region is tracked (no WM_PAINT generation), so validation succeeds. */
static void validate_rect(CPU *c) {
    if(!on_thread(c))return;uint32_t hwnd=ARG(0);
    if(hwnd && !lookup(hwnd)){movies_set_last_error(1400);ret(c,2,0);return;}
    ret(c,2,1);
}
/* Updates the stored caption; the host surface has no caption to repaint. */
static void set_window_text(CPU *c) {
    if(!on_thread(c))return;Window *w=lookup(ARG(0));char title[STRING_CAP*3];
    if(!w){movies_set_last_error(1400);ret(c,2,0);return;}
    if(!read_a(ARG(1),title,sizeof title,1)){movies_set_last_error(87);ret(c,2,0);return;}
    memcpy(w->title,title,sizeof title);w->state.title=w->title;ret(c,2,1);
}
/* TITLEBARINFO (44 bytes): screen title-bar RECT and six STATE_SYSTEM_* words.
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-titlebarinfo */
static void title_bar_info(CPU *c) {
    if(!on_thread(c))return;Window *w=lookup(ARG(0));uint32_t p=ARG(1);
    if(!w){movies_set_last_error(1400);ret(c,2,0);return;}
    if(!range(p,44) || R32(p)!=44){movies_set_last_error(87);ret(c,2,0);return;}
    int32_t border,caption;metrics(w->state.style,w->state.ex_style,&border,&caption);
    const uint32_t invisible=0x8000u,unavailable=1u,focusable=0x100000u;
    uint32_t style=w->state.style,state[6]={0};
    int32_t rect[4]={w->state.x+border,w->state.y+border,w->state.x+w->state.outer_width-border,w->state.y+border+caption};
    if(!caption){state[0]=invisible;rect[3]=rect[1];}
    else state[0]=focusable|(w->state.visible?0:invisible);
    int sysmenu=caption && (style&0x80000u);
    state[2]=sysmenu && (style&0x20000u)?0:invisible; /*minimize*/
    state[3]=sysmenu && (style&0x10000u)?0:invisible; /*maximize*/
    state[4]=invisible; /*help*/
    state[5]=sysmenu?0:invisible; /*close*/
    if(!w->state.visible)for(unsigned i=2;i<6;i++)if(!state[i])state[i]=unavailable;
    for(unsigned i=0;i<4;i++)W32(p+4+4*i,(uint32_t)rect[i]);
    for(unsigned i=0;i<6;i++)W32(p+20+4*i,state[i]);
    ret(c,2,1);
}
static void unregister_class(CPU *c) {
    if(!on_thread(c))return;uint32_t id=ARG(0),instance=ARG(1);char name[STRING_CAP];
    if(id>0xffff && !read_a(id,name,sizeof name,0)){movies_set_last_error(87);ret(c,2,0);return;}
    for(unsigned i=0;i<class_count;i++) {
        Class *cl=&classes[i];
        if(!cl->atom || cl->instance!=instance || (id<=0xffff?cl->atom!=id:!same_name(cl->name,name)))continue;
        for(unsigned j=0;j<WINDOW_LIMIT;j++)if(windows[j].handle && windows[j].class_==cl){movies_set_last_error(1412);ret(c,2,0);return;}
        memset(cl,0,sizeof *cl);ret(c,2,1);return;
    }
    movies_set_last_error(1411);ret(c,2,0);
}
/* Process-wide registered message ids, case-insensitive, stable for the run.
 * Range 0xC400.. is disjoint from this module's class atoms (0xC000+slot).
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerwindowmessagea */
static void register_message(CPU *c) {
    char name[256];
    if(!ARG(0) || !read_a(ARG(0),name,sizeof name,0) || !name[0]){movies_set_last_error(87);ret(c,1,0);return;}
    for(unsigned i=0;i<registered_count;i++)if(same_name(registered_messages[i],name)){ret(c,1,0xc400u+i);return;}
    if(registered_count==REGISTERED_LIMIT){movies_set_last_error(8);ret(c,1,0);return;}
    memcpy(registered_messages[registered_count],name,sizeof name);ret(c,1,0xc400u+registered_count++);
}
static uint32_t queue_present(void) {
    uint32_t bits=quit_pending?0x108u:0;
    for(unsigned i=0;i<message_count;i++)bits|=queue_class(messages[i].message);
    return bits;
}
static int ui_caller(void) { return initialized && pthread_equal(ui_thread,pthread_self()); }
/* High word: classes now queued; low word: classes added since the last
 * GetQueueStatus/PeekMessage. Other threads own empty queues. */
static void queue_status(CPU *c) {
    uint32_t flags=ARG(0)&0x5ffu; /*QS_ALLINPUT|QS_ALLPOSTMESSAGE*/
    if(!ui_caller()){ret(c,1,0);return;}
    poll_input(c);
    uint32_t result=((queue_present()&flags)<<16)|(queue_new_bits&flags);
    queue_new_bits&=~flags;ret(c,1,result);
}
/* Only the UI thread's own queue exists; cross-thread posting is unsupported. */
static void post_thread_message(CPU *c) {
    uint32_t thread=ARG(0),message=ARG(1),wp=ARG(2),lp=ARG(3);
    if(!ui_caller() || !thread || thread!=movies_thread_current_id()){movies_set_last_error(ui_caller()?1444:50);ret(c,4,0);return;}
    if(message<0x400 && message!=0 && message!=0x12 && !(message>=0x100 && message<=0x109) && !(message>=0x200 && message<=0x20e)){movies_set_last_error(50);ret(c,4,0);return;}
    if(message_count==MESSAGE_LIMIT){movies_set_last_error(1816);ret(c,4,0);return;}
    messages[message_count++]=(Message){0,message,wp,lp,message_time(c),last_pointer_x,last_pointer_y};queue_new_bits|=queue_class(message);ret(c,4,1);
}
static uint64_t now_ms(void) { struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000u+(uint64_t)ts.tv_nsec/1000000u; }
/* Object waits use the shared sync core in <=10ms slices on the UI thread,
 * polling host input between slices; queued messages matching dwWakeMask
 * return WAIT_OBJECT_0+nCount (present, not only newly arrived, messages).
 * fWaitAll is satisfied by all objects alone. Other threads have no queue.
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjects */
static void msg_wait(CPU *c) {
    uint32_t count=ARG(0),array=ARG(1),all=ARG(2)!=0,timeout=ARG(3),mask=ARG(4),ids[64],error=0,result;
    if(count>63 || (count && !range(array,count*4)) || (mask&~0x5ffu)){movies_set_last_error(87);ret(c,5,0xffffffffu);return;}
    for(unsigned i=0;i<count;i++)ids[i]=R32(array+4*i);
    if(!ui_caller()) {
        result=movies_sync_wait(c,count,ids,all,timeout,&error);
        if(result==0xffffffffu)movies_set_last_error(error);ret(c,5,result);return;
    }
    uint64_t start=now_ms();uint32_t slice=0;
    for(;;) {
        if(count || slice) {
            result=movies_sync_wait(c,count,ids,all,slice,&error);
            if(result!=258){if(result==0xffffffffu)movies_set_last_error(error);ret(c,5,result);return;}
        }
        poll_input(c);
        if(queue_present()&mask){ret(c,5,count);return;}
        uint64_t elapsed=now_ms()-start;
        if(timeout!=0xffffffffu && elapsed>=timeout){ret(c,5,258);return;}
        slice=timeout==0xffffffffu || timeout-elapsed>10?10:(uint32_t)(timeout-elapsed);
    }
}
ImportHandler *movies_window_handlers(uint32_t *count) {
    static ImportHandler h[]={{"USER32.DLL!GetDoubleClickTime",double_click_time},{"USER32.DLL!ToUnicode",to_unicode},{"USER32.DLL!ToAscii",to_ascii},{"USER32.DLL!SetCursorPos",set_cursor_position},{"USER32.DLL!GetCursorPos",cursor_position},{"USER32.DLL!ScreenToClient",screen_to_client},{"USER32.DLL!GetAsyncKeyState",async_key_state},{"USER32.DLL!GetForegroundWindow",foreground_window},{"USER32.DLL!PeekMessageA",peek_message},{"USER32.DLL!PostMessageA",post_message},
        {"USER32.DLL!PostQuitMessage",post_quit},{"USER32.DLL!DispatchMessageA",dispatch_message},
        {"USER32.DLL!TranslateMessage",translate_message},{"USER32.DLL!RegisterClassA",register_class},{"USER32.DLL!CreateWindowExA",create_window},
        {"USER32.DLL!SetRect",set_rect},{"USER32.DLL!AdjustWindowRect",adjust_rect},{"USER32.DLL!GetClientRect",client_rect},
        {"USER32.DLL!ShowWindow",show_window},{"USER32.DLL!SetCursor",set_cursor},{"USER32.DLL!DefWindowProcA",default_proc},
        {"USER32.DLL!ClientToScreen",client_to_screen},{"USER32.DLL!ClipCursor",clip_cursor},{"USER32.DLL!SetCapture",set_capture},
        {"USER32.DLL!ReleaseCapture",release_capture},{"USER32.DLL!GetDC",get_dc},{"GDI32.DLL!GetDeviceCaps",device_caps},
        {"GDI32.DLL!DeleteObject",delete_object},{"USER32.DLL!SetRectEmpty",set_rect_empty},{"USER32.DLL!ValidateRect",validate_rect},
        {"USER32.DLL!SetWindowTextA",set_window_text},{"USER32.DLL!RegisterWindowMessageA",register_message},
        {"USER32.DLL!GetTitleBarInfo",title_bar_info},{"USER32.DLL!UnregisterClassA",unregister_class},
        {"USER32.DLL!GetQueueStatus",queue_status},{"USER32.DLL!PostThreadMessageA",post_thread_message},
        {"USER32.DLL!MsgWaitForMultipleObjects",msg_wait}};
    *count=sizeof h/sizeof h[0];return h;
}
