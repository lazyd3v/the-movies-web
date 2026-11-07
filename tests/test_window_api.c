#include "window-api.h"
#include "resource-api.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void,set_cursor_fixture,(int ready),{Module.moviesCursorPosition=()=>ready?{x:-5,y:23}:null;});
EM_JS(void,set_key_fixture,(uint32_t state),{let current=state;Module.moviesAsyncKeyState=()=>{const result=current;current&=~1;return result;};});
EM_JS(void,set_focus_fixture,(uint32_t hwnd),{Module.moviesForegroundWindow=()=>hwnd;});
EM_JS(void, set_input_fixture, (uint32_t hwnd,int restore), {
    var pending=true;Module["moviesPollWindowEvents"]=function(maxCount) {
        if(!pending || !maxCount)return [];pending=false;
        return [{hwnd:hwnd,message:restore?0x112:512,wParam:restore?0xf120:0,lParam:(20<<16)|10,time:(Math.floor(performance.timeOrigin+performance.now())-50)>>>0,x:10,y:20}];
    };
});
#endif
uint8_t *g_mem;
static uint32_t process_tick=4242;
int movies_process_ticks(uint32_t *out) { if(!out)return 0;*out=process_tick;return 1; }
static uint32_t error,messages[32],message_count,host_events,host_creates,host_destroys;
static uint32_t size_kind,size_payload,focused_hwnd;
static int focus_query(uint32_t *out) { *out=focused_hwnd;return 1; }
static int cursor_available,cursor_calls,warp_locked;static int32_t warp_x=-5,warp_y=23;
static int cursor_warp(int32_t x,int32_t y){if(!warp_locked)return 0;warp_x=x;warp_y=y;return 1;}
static int cursor_query(int32_t *x,int32_t *y){cursor_calls++;*x=warp_x;*y=warp_y;return cursor_available;}
static uint16_t key_state;
static int key_query(uint32_t vk,uint16_t *out) { assert(vk==0x1b||vk==0x20||vk==0x0d||vk==0x21||vk==0x22||vk==0x25||vk==0x27||vk==0x26||vk==0x28||vk==1||vk==2||vk==4);*out=key_state;key_state&=~1u;return 1; }
static int reject_nc,reject_create,reject_host,reject_show,consume_restore;
static jmp_buf trapped;
static char trap_message[256];
static MoviesWindowEvent last_event;
void movies_set_last_error(uint32_t e) { error=e; }
void guest_trap(CPU *c,uint32_t at,const char *s) { (void)c;(void)at;snprintf(trap_message,sizeof trap_message,"%s",s);longjmp(trapped,1); }
static const MoviesCursorResource arrow={32512,"default"};
const MoviesCursorResource *movies_resource_cursor(uint32_t h) { return h==123?&arrow:NULL; }
const MoviesIconResource *movies_resource_icon(uint32_t h) { (void)h;return NULL; }
const MoviesBrushResource *movies_resource_brush(uint32_t h) { (void)h;return NULL; }
static ImportHandler *handlers;static uint32_t handler_count;
static void invoke(CPU *c,const char *name) {
    for(unsigned i=0;i<handler_count;i++)if(!strcmp(strchr(handlers[i].name,'!')+1,name)) { handlers[i].fn(c);return; }assert(0);
}
static void wndproc(CPU *c) {
    uint32_t h=R32(c->esp+4),m=R32(c->esp+8),lp=R32(c->esp+16);messages[message_count++]=m;
    MoviesWindowEvent state;assert(movies_window_get(h,&state));
    if(m==0x112 && consume_restore) { c->eax=0;c->esp+=20;return; }
    if(m==5) { size_kind=R32(c->esp+12);size_payload=lp; }
    if(m==0x81 || m==1) {
        assert(R32(lp)==0x1234 && R32(lp+4)==0x400000 && R32(lp+32)==0xca0000);
        assert(R32(lp+16)==631 && R32(lp+20)==808);
        if((m==0x81 && reject_nc) || (m==1 && reject_create)) { c->eax=m==1?0xffffffffu:0;c->esp+=20;return; }
    }
    if(m>=0x400) { c->eax=m^R32(c->esp+12)^lp;c->esp+=20;return; }
    invoke(c,"DefWindowProcA");
}
GuestFn lookup_function(uint32_t address) { return address==0x542380?wndproc:NULL; }
void guest_call(CPU *c,GuestFn fn,int argc,const uint32_t *args) {
    for(int i=argc-1;i>=0;i--)PUSH32(c,args[i]);PUSH32(c,0xdeadbeef);fn(c);
}
static int host_event(const MoviesWindowEvent *event) {
    ++host_events;last_event=*event;
    if(!strcmp(event->type,"create")) {
        ++host_creates;assert(!strcmp(event->title,"The Movies\xe2\x82\xac"));
        assert(event->client_width==800 && event->client_height==600);
        assert(event->client_x==16 && event->client_y==39);
        if(reject_host)return 0;
    }
    if(!strcmp(event->type,"show") && reject_show)return 0;
    if(!strcmp(event->type,"destroy"))++host_destroys;
    return 1;
}
static uint32_t call(const char *name,unsigned n,const uint32_t *args) {
    CPU c={0};c.esp=0x9000;c.ebx=0x12345678;c.esi=0x88888888;c.fs_base=0x8000;
    W32(c.esp,0xdeadbeef);for(unsigned i=0;i<n;i++)W32(c.esp+4+i*4,args[i]);
    invoke(&c,name);assert(c.esp==0x9004+n*4 && c.ebx==0x12345678 && c.esi==0x88888888 && c.fs_base==0x8000);return c.eax;
}
#define CALL(name,...) call(name,sizeof((uint32_t[]){__VA_ARGS__})/4,(uint32_t[]){__VA_ARGS__})
static void setup(void) {
    assert(movies_window_init(1024,768));movies_window_set_host(host_event);movies_window_set_focus_host(focus_query);focused_hwnd=0;movies_window_set_key_host(key_query);key_state=0;
    message_count=host_events=host_creates=host_destroys=0;reject_nc=reject_create=reject_host=reject_show=consume_restore=0;
    memset(HP(0x5000),0,40);W32(0x5004,0x542380);W32(0x5010,0x400000);W32(0x5024,0x6000);
    strcpy((char *)HP(0x6000),"The Movies");strcpy((char *)HP(0x6100),"The Movies\x80");
}
static uint32_t create(uint32_t cl) { return CALL("CreateWindowExA",0,cl,0x6100,0xca0000,12,12,808,631,0,0,0x400000,0x1234); }
int main(void) {
    uint32_t mapped=0;
    assert(movies_window_map_input_time(1000,2000,1750,&mapped) && mapped==750);
    assert(movies_window_map_input_time(1000,5,0xfffffff0u,&mapped) && mapped==979);
    assert(movies_window_map_input_time(5,100,90,&mapped) && mapped==0xfffffffbu);
    assert(!movies_window_map_input_time(100,20,21,&mapped));
    assert(!movies_window_map_input_time(100,0x80000000u,0,&mapped));
    assert(!movies_window_map_input_time(100,20,20,NULL));
    g_mem=calloc(1,0x10000);assert(g_mem);handlers=movies_window_handlers(&handler_count);setup();
    error=123;
    const uint32_t supported_keys[]={0x1b,0x20,0x0d,0x21,0x22,0x25,0x27,0x26,0x28,1,2,4};
    for(unsigned i=0;i<sizeof supported_keys/sizeof supported_keys[0];i++) {
        key_state=0x8001;assert(CALL("GetAsyncKeyState",supported_keys[i])==0xffff8001u&&error==123);
        assert(CALL("GetAsyncKeyState",supported_keys[i])==0xffff8000u&&error==123);
        key_state=1;assert(CALL("GetAsyncKeyState",supported_keys[i])==1&&error==123);
        assert(!CALL("GetAsyncKeyState",supported_keys[i])&&error==123);
    }
    if(!setjmp(trapped)){CALL("GetAsyncKeyState",0x41);assert(0);}assert(error==123&&strstr(trap_message,"0x00000041"));
    key_state=4;if(!setjmp(trapped)){CALL("GetAsyncKeyState",0x1b);assert(0);}key_state=0;
    movies_window_set_key_host(NULL);
#ifdef __EMSCRIPTEN__
    set_key_fixture(0x8001);assert(CALL("GetAsyncKeyState",0x1b)==0xffff8001u);
    assert(CALL("GetAsyncKeyState",0x1b)==0xffff8000u);
#else
    if(!setjmp(trapped)){CALL("GetAsyncKeyState",0x1b);assert(0);}
#endif
    movies_window_set_key_host(key_query);
    error=123;uint32_t atom=CALL("RegisterClassA",0x5000);assert(atom && error==123);
    assert(!CALL("RegisterClassA",0x5000) && error==1410);
    assert(CALL("SetRect",0x1000,0,0,800,600));assert(CALL("AdjustWindowRect",0x1000,0xca0000,0));
    assert((int32_t)R32(0x1000)==-4 && (int32_t)R32(0x1004)==-27 && R32(0x1008)==804 && R32(0x100c)==604);
    uint32_t hwnd=create(atom);assert(hwnd && host_creates==1 && message_count==4);
    movies_window_set_cursor_host(cursor_query);cursor_available=0;W32(0x1000,0xdeadbeef);W32(0x1004,0xcafebabe);
    assert(!CALL("GetCursorPos",0x1000)&&error==21&&R32(0x1000)==0xdeadbeef&&R32(0x1004)==0xcafebabe);
    cursor_available=1;int calls=cursor_calls;assert(!CALL("GetCursorPos",0xfffc)&&error==87&&cursor_calls==calls);
    error=123;assert(CALL("GetCursorPos",0x1001)&&error==123);int32_t point[2];memcpy(point,HP(0x1001),8);assert(point[0]==-5&&point[1]==23);
    assert(CALL("ScreenToClient",hwnd,0x1001)&&error==123);memcpy(point,HP(0x1001),8);assert(point[0]==-21&&point[1]==-16);
    assert(!CALL("ScreenToClient",0,0x1001)&&error==1400);assert(!CALL("ScreenToClient",hwnd,0xfffc)&&error==87);
    W32(0x1000,16);W32(0x1004,39);error=123;assert(CALL("ScreenToClient",hwnd,0x1000)&&!R32(0x1000)&&!R32(0x1004)&&error==123);
    W32(0x1000,0x80000000);assert(!CALL("ScreenToClient",hwnd,0x1000)&&error==87&&R32(0x1000)==0x80000000);
#ifdef __EMSCRIPTEN__
    movies_window_set_cursor_host(NULL);set_cursor_fixture(1);error=123;assert(CALL("GetCursorPos",0x1000)&&R32(0x1000)==(uint32_t)-5&&R32(0x1004)==23&&error==123);
    set_cursor_fixture(0);assert(!CALL("GetCursorPos",0x1000)&&error==21&&R32(0x1000)==(uint32_t)-5);
#endif
    movies_window_set_cursor_host(cursor_query);movies_window_set_cursor_warp_host(cursor_warp);cursor_available=1;warp_locked=0;warp_x=-5;warp_y=23;
    assert(!CALL("SetCursorPos",512u,384u)&&error==50);assert(CALL("GetCursorPos",0x1000)&&R32(0x1000)==(uint32_t)-5&&R32(0x1004)==23);
    warp_locked=1;error=123;assert(CALL("SetCursorPos",512u,384u)&&error==123);assert(CALL("GetCursorPos",0x1000)&&R32(0x1000)==512&&R32(0x1004)==384&&error==123);
    assert(CALL("SetCursorPos",0x80000000u,0x7fffffffu)&&error==123);assert(warp_x==0&&warp_y==767);
    assert(CALL("SetCursorPos",0x7fffffffu,0xffffffffu)&&warp_x==1023&&warp_y==0);
    warp_locked=0;assert(!CALL("SetCursorPos",20u,30u)&&error==50&&warp_x==1023&&warp_y==0);
    movies_window_set_cursor_warp_host(NULL);
    focused_hwnd=hwnd;error=123;assert(!CALL("GetForegroundWindow")&&error==123); /* invisible is not foreground */
    assert(messages[0]==0x24 && messages[1]==0x81 && messages[2]==0x83 && messages[3]==1);
    assert(CALL("GetClientRect",hwnd,0x1000) && R32(0x1008)==800 && R32(0x100c)==600);
    assert(!CALL("ShowWindow",hwnd,1));assert(last_event.visible && !strcmp(last_event.type,"show"));
    error=123;assert(CALL("GetForegroundWindow")==hwnd&&error==123);
    focused_hwnd=0;assert(!CALL("GetForegroundWindow")&&error==123);
    focused_hwnd=0xdeadbeef;assert(!CALL("GetForegroundWindow")&&error==123);focused_hwnd=hwnd;
#ifdef __EMSCRIPTEN__
    movies_window_set_focus_host(NULL);set_focus_fixture(hwnd);assert(CALL("GetForegroundWindow")==hwnd);
    set_focus_fixture(0);assert(!CALL("GetForegroundWindow"));movies_window_set_focus_host(focus_query);
#else
    movies_window_set_focus_host(NULL);if(!setjmp(trapped)){CALL("GetForegroundWindow");assert(0);}movies_window_set_focus_host(focus_query);
#endif
    assert(CALL("ShowWindow",hwnd,3)==1);assert(CALL("GetClientRect",hwnd,0x1000));
    assert(R32(0x1008)==1016 && R32(0x100c)==737);
    assert(CALL("ShowWindow",hwnd,9)==1);assert(CALL("GetClientRect",hwnd,0x1000) && R32(0x1008)==800);
    message_count=0;
    assert(CALL("ShowWindow",hwnd,7)==1 && last_event.visible && last_event.minimized);
    assert(!CALL("GetForegroundWindow"));
    assert(size_kind==1 && size_payload==0 && message_count==1 && messages[0]==5);
    MoviesWindowEvent minimized;assert(movies_window_get(hwnd,&minimized));
    assert(minimized.hwnd==hwnd && minimized.x==12 && minimized.client_width==800);
    assert(CALL("ShowWindow",hwnd,5)==1 && last_event.minimized); /*SHOW retains iconic state*/
    assert(CALL("ShowWindow",hwnd,9)==1 && !last_event.minimized && size_kind==0);
    assert(CALL("GetForegroundWindow")==hwnd);
    assert(last_event.client_width==800 && last_event.client_height==600 && last_event.x==12);
    message_count=0;
    assert(CALL("ShowWindow",hwnd,7)==1);
    message_count=0;reject_show=1;error=0;
    assert(!CALL("DefWindowProcA",hwnd,0x112,0xf12f,0)); /*reserved low bits masked*/
    MoviesWindowEvent failed_restore;assert(movies_window_get(hwnd,&failed_restore));
    assert(error==31 && failed_restore.minimized && failed_restore.visible && message_count==0);
    reject_show=0;consume_restore=1;
    assert(CALL("PostMessageA",hwnd,0x112,0xf120,0));
    assert(CALL("PeekMessageA",0x3000,hwnd,0x112,0x112,1));
    assert(!CALL("DispatchMessageA",0x3000));
    assert(movies_window_get(hwnd,&failed_restore) && failed_restore.minimized); /*WndProc can consume*/
    consume_restore=0;message_count=0;
    assert(CALL("PostMessageA",hwnd,0x112,0xf12f,0));
    assert(CALL("PeekMessageA",0x3000,hwnd,0x112,0x112,1));
    assert(!CALL("DispatchMessageA",0x3000));
    assert(messages[0]==0x112 && messages[1]==0x18 && messages[2]==5 && messages[3]==3);
    assert(size_kind==0 && size_payload==((600u<<16)|800));
    assert(movies_window_get(hwnd,&failed_restore) && !failed_restore.minimized && failed_restore.visible);
    assert(failed_restore.x==12 && failed_restore.client_width==800 && failed_restore.client_height==600);
    if(!setjmp(trapped)) { CALL("DefWindowProcA",hwnd,0x112,0xf060,0);assert(0); }
    /* Original WndProc 0x00542380 forwards client mouse/char input to DefWindowProcA. */
    message_count=0;
    {static const uint32_t passive[]={0x100,0x101,0x102,0x103,0x109,0x200,0x201,0x202,0x203,0x204,0x205,0x206,0x207,0x208,0x209,0x20a,0x20b,0x20e};
     for(unsigned i=0;i<sizeof passive/sizeof *passive;i++)assert(!CALL("DefWindowProcA",hwnd,passive[i],1,(20u<<16)|10));}
    assert(message_count==0);
    if(!setjmp(trapped)) { CALL("DefWindowProcA",hwnd,0x20c,0x10001,0);assert(0); } /*WM_XBUTTONUP/WM_APPCOMMAND unclaimed*/
    if(!setjmp(trapped)) { CALL("DefWindowProcA",hwnd,0x106,'x',0);assert(0); } /*WM_SYSCHAR menu semantics unclaimed*/
    message_count=0;
    assert(CALL("ShowWindow",hwnd,0)==1 && !last_event.visible);assert(!CALL("GetForegroundWindow"));
    message_count=0;
    assert(!CALL("SetCursor",123) && !strcmp(last_event.cursor,"default"));
    assert(CALL("SetCursor",0)==123 && !strcmp(last_event.cursor,"none"));
    assert(!CALL("SetCursor",999) && error==6);
    assert(!CALL("GetClientRect",999,0x1000) && error==1400);
    assert(!CALL("SetRect",0xfff8,0,0,1,1) && error==87);
    assert(!CALL("AdjustWindowRect",0x1000,0xca0000,1) && error==50);
    assert(!CALL("CreateWindowExA",0,atom,0x6100,0xca0000,0,0,808,631,1,0,0x400000,0) && error==50);
    if(!setjmp(trapped)) { CALL("DefWindowProcA",hwnd,0x7777,0,0);assert(0); }
    assert(CALL("PostMessageA",hwnd,0x401,7,9));
    assert(CALL("PostMessageA",0,0x402,11,13));
    memset(HP(0x3000),0xa5,32);
    assert(CALL("PeekMessageA",0x3000,hwnd,0x401,0x401,0));
    assert(R32(0x3000)==hwnd && R32(0x3004)==0x401 && R32(0x3008)==7 && R32(0x300c)==9 && R32(0x301c)==0xa5a5a5a5);
    assert(R32(0x3010)==4242);
    error=456;assert(!CALL("TranslateMessage",0x3000) && error==456);
    assert(CALL("DispatchMessageA",0x3000)==(0x401^7^9));
    assert(CALL("PeekMessageA",0x3000,hwnd,0,0,1)); /*NOREMOVE retained it*/
    assert(!CALL("PeekMessageA",0x3000,hwnd,0,0,1)); /*thread message excluded*/
    assert(CALL("PeekMessageA",0x3000,0xffffffffu,0,0,1) && R32(0x3000)==0);
    assert(!CALL("DispatchMessageA",0x3000));
    assert(!CALL("PeekMessageA",0x3000,0,0,0,0));
    assert(CALL("PostMessageA",hwnd,0x100,65,0));
    assert(CALL("PeekMessageA",0x3000,0,0,0,1));
    assert(!CALL("TranslateMessage",0x3000) && error==50);
    assert(!CALL("PeekMessageA",0x3000,0,0,0,0)); /*no invented WM_CHAR*/
    assert(!CALL("PostMessageA",hwnd,0xc,0,0x6000) && error==50); /*pointer capture unsupported*/
    for(unsigned i=0;i<256;i++)assert(CALL("PostMessageA",hwnd,0x400,i,0));
    assert(!CALL("PostMessageA",hwnd,0x400,0,0) && error==1816);
    CALL("PostQuitMessage",27);process_tick=4342;
    assert(CALL("PeekMessageA",0x3000,hwnd,0x500,0x500,0) && R32(0x3004)==0x12 && R32(0x3008)==27);
    assert(R32(0x3010)==4242);
    assert(CALL("PeekMessageA",0x3000,hwnd,0x500,0x500,1) && R32(0x3004)==0x12);
    for(unsigned i=0;i<256;i++) { assert(CALL("PeekMessageA",0x3000,hwnd,0,0,1));assert(R32(0x3008)==i); }
    assert(!CALL("PeekMessageA",0x3000,0,0,0,1));
    assert(!CALL("PeekMessageA",0xfff0,0,0,0,1) && error==87);
    assert(!CALL("PeekMessageA",0x3000,0,2,1,1) && error==87);
    assert(!CALL("PostMessageA",999,0x400,0,0) && error==1400);
#ifdef __EMSCRIPTEN__
    set_input_fixture(hwnd,0);
    assert(CALL("PeekMessageA",0x3000,0,0,0,1) && R32(0x3004)==0x200);
    assert(R32(0x3010)<=4292 && R32(0x3010)>=4282 && R32(0x3014)==10 && R32(0x3018)==20);
    message_count=0;CALL("ShowWindow",hwnd,7);set_input_fixture(hwnd,1);
    assert(CALL("PeekMessageA",0x3000,0,0,0,1) && R32(0x3004)==0x112 && R32(0x3008)==0xf120);
    assert(!CALL("DispatchMessageA",0x3000));
    assert(movies_window_get(hwnd,&failed_restore) && !failed_restore.minimized);
#endif
    setup();atom=CALL("RegisterClassA",0x5000);reject_nc=1;
    assert(!create(atom) && host_destroys==1 && messages[message_count-1]==0x82);
    setup();atom=CALL("RegisterClassA",0x5000);reject_create=1;
    assert(!create(atom) && host_destroys==1);
    setup();atom=CALL("RegisterClassA",0x5000);reject_host=1;
    assert(!create(atom) && error==31 && message_count==0);
    setup();atom=CALL("RegisterClassA",0x5000);movies_window_set_host(NULL);
    if(!setjmp(trapped)) { create(atom);assert(0); }
    free(g_mem);puts("window lifecycle, real callbacks, host acknowledgement and geometry tests passed");
}
