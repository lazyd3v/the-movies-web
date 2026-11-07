#include "directinput-api.h"
#include "window-api.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
#include <pthread.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
int movies_process_ticks(uint32_t *out){*out=10000;return 1;}
int movies_window_map_input_time(uint32_t tick,uint32_t now,uint32_t time,uint32_t*out){uint32_t age=now-time;if(age>=0x80000000u)return 0;*out=tick-age;return 1;}
EM_JS(void,install_js_backend,(int active),{
 Module.moviesDirectInputAvailable=()=>!!active;
 Module.moviesDirectInputFocused=h=>h===42;
 Module.diFixtureEvents=[];
 Module.moviesPollDirectInputEvents=max=>Module.diFixtureEvents.splice(0,max);
});
EM_JS(void,queue_js_event,(void),{Module.diFixtureEvents.push({deviceKind:1,offset:31,data:128,time:Math.floor(performance.timeOrigin+performance.now())>>>0});});
#endif
uint8_t*g_mem;static GuestFn functions[11];static jmp_buf trap;static int focus=1,ready=1;
void register_function(uint32_t a,GuestFn f){assert(a>=0xe000000&&(a-0xe000000)/16<11);functions[(a-0xe000000)/16]=f;}
void guest_trap(CPU*c,uint32_t p,const char*s){(void)c;(void)p;(void)s;longjmp(trap,1);}
int movies_window_get(uint32_t h,MoviesWindowEvent*s){(void)s;return h==42;}
static int avail(void*p){(void)p;return ready;}static int focused(void*p,uint32_t h){(void)p;return focus&&h==42;}
static uint32_t invoke(GuestFn f,unsigned n,const uint32_t*a){CPU c={0};c.esp=0x100;W32(c.esp,0x123456);for(unsigned i=0;i<n;i++)W32(c.esp+4+4*i,a[i]);f(&c);assert(c.esp==0x100+4*(n+1));return c.eax;}
static uint32_t method(uint32_t o,unsigned slot,unsigned n,uint32_t a,uint32_t b,uint32_t d,uint32_t e){uint32_t args[]={o,a,b,d,e};return invoke(functions[(R32(R32(o)+slot*4)-0xe000000)/16],n,args);}
static void putguid(uint32_t p,const uint8_t*g){memcpy(HP(p),g,16);}
static const uint8_t inputiid[]={0x30,0x80,0x79,0xbf,0x3a,0x48,0xa2,0x4d,0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0};
static const uint8_t key[]={0x61,0x2b,0x1d,0x6f,0xa0,0xd5,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static const uint8_t keyobj[]={0x20,0x82,0x72,0x55,0x3c,0xd3,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static void setup(uint32_t o,int mouse){W32(0x1000,24);W32(0x1004,16);W32(0x1008,2);W32(0x100c,mouse?16:256);W32(0x1010,mouse?7:256);W32(0x1014,0x2000);putguid(0x1200,keyobj);
 for(unsigned i=0;i<(mouse?7:256);i++){uint32_t p=0x2000+i*16,g=0x1200,off=i,t=0x8000000cu|(i<<8);if(mouse){static const uint8_t axis[]={0xe0,2,0x6d,0xa3,0xf3,0xc9,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};g=i<3?0x1300+i*16:0;if(g){putguid(g,axis);W8(g,0xe0+i);}off=i<3?i*4:i+9;t=i<3?(i==2?0x80ffff03:0xffff03):(i>=5?0x80ffff0c:0xffff0c);}W32(p,g);W32(p+4,off);W32(p+8,t);W32(p+12,0);}
 assert(!method(o,11,2,0x1000,0,0,0));assert(!method(o,13,3,42,5,0,0));W32(0x1100,20);W32(0x1104,16);W32(0x1108,0);W32(0x110c,0);W32(0x1110,128);assert(!method(o,6,3,1,0x1100,0,0));}
#ifndef __EMSCRIPTEN__
static void *foreign_thread(void *unused){(void)unused;assert(!movies_directinput_push_event(1,1,128,10));assert(!movies_directinput_init());return NULL;}
#endif
int main(void){g_mem=calloc(1,0x3010000);assert(g_mem);assert(movies_directinput_init());
#ifndef __EMSCRIPTEN__
 pthread_t foreign;assert(!pthread_create(&foreign,NULL,foreign_thread,NULL));assert(!pthread_join(foreign,NULL));
#endif
MoviesDirectInputHost h={avail,focused,0,0};movies_directinput_set_host(&h,0);uint32_t n;ImportHandler*handlers=movies_directinput_handlers(&n);assert(n==1&&!strcmp(handlers[0].name,"DINPUT8.DLL!DirectInput8Create"));putguid(0x500,inputiid);uint32_t args[]={1,0x800,0x500,0x600,0};assert(!invoke(handlers[0].fn,5,args));uint32_t f=R32(0x600);putguid(0x520,key);assert(!method(f,3,4,0x520,0x604,0,0));uint32_t k=R32(0x604);W8(0x520,0x60);assert(!method(f,3,4,0x520,0x608,0,0));uint32_t m=R32(0x608);setup(k,0);setup(m,1);
 movies_directinput_set_host(0,0);assert(method(k,7,1,0,0,0,0)==0x80070015);movies_directinput_set_host(&h,0);
 W32(0x1000,23);assert(method(k,11,2,0x1000,0,0,0)==0x80070057);W32(0x1000,24);assert(method(k,13,3,999,5,0,0)==0x80070057);assert(method(k,13,3,42,6,0,0)==0x80070057);
 ready=0;assert(method(k,7,1,0,0,0,0)==0x80070015);ready=1;focus=0;assert(method(k,7,1,0,0,0,0)==0x80070005);focus=1;assert(!method(k,7,1,0,0,0,0));assert(method(k,7,1,0,0,0,0)==1);assert(!method(m,7,1,0,0,0,0));assert(method(k,11,2,0x1000,0,0,0)==0x800700aa);
 assert(movies_directinput_push_event(1,30,128,123));assert(movies_directinput_push_event(2,0,(uint32_t)-3,124));assert(!movies_directinput_push_event(1,256,128,123));W32(0x700,128);assert(!method(k,10,5,20,0x4000,0x700,1));assert(R32(0x700)==1&&R32(0x4000)==30&&R32(0x4004)==128&&R32(0x4008)==123);W32(0x700,128);assert(!method(k,10,5,20,0x4000,0x700,0));assert(R32(0x700)==1);W32(0x700,128);assert(!method(k,10,5,20,0,0x700,0)&&!R32(0x700));W32(0x700,128);assert(!method(m,10,5,20,0x4000,0x700,0)&&R32(0x4004)==(uint32_t)-3);
 for(unsigned i=0;i<130;i++)assert(movies_directinput_push_event(1,i,128,i));W32(0x700,0xffffffff);assert(method(k,10,5,20,0,0x700,1)==1&&R32(0x700)==127);W32(0x700,0xffffffff);assert(method(k,10,5,20,0,0x700,0)==1&&R32(0x700)==127);W32(0x700,0);assert(!method(k,10,5,20,0,0x700,0));
 focus=0;W32(0x700,128);assert(method(k,10,5,20,0,0x700,0)==0x8007001e);focus=1;assert(!method(k,7,1,0,0,0,0));assert(!method(k,8,1,0,0,0,0));assert(method(k,8,1,0,0,0,0)==1);assert(method(k,10,5,20,0,0x700,0)==0x8007000c);
 assert(!method(f,0,3,0x500,0x600,0,0));assert(method(f,2,1,0,0,0,0)==1);if(!setjmp(trap)){method(m,9,3,16,0x4000,0,0);assert(0);}assert(!method(k,2,1,0,0,0,0));assert(!method(m,2,1,0,0,0,0));assert(!method(f,2,1,0,0,0,0));
#ifdef __EMSCRIPTEN__
 /* Exercise actual linked EM_JS backend: discard pre-acquisition event,
  * then deliver an acquired event with process-epoch time conversion. */
 assert(!invoke(handlers[0].fn,5,args));f=R32(0x600);putguid(0x520,key);assert(!method(f,3,4,0x520,0x604,0,0));k=R32(0x604);setup(k,0);
 movies_directinput_set_host(0,0);install_js_backend(1);for(unsigned i=0;i<600;i++)queue_js_event();assert(!method(k,7,1,0,0,0,0));W32(0x700,128);assert(!method(k,10,5,20,0x4000,0x700,0)&&!R32(0x700));queue_js_event();W32(0x700,128);assert(!method(k,10,5,20,0x4000,0x700,0)&&R32(0x700)==1&&R32(0x4000)==31&&R32(0x4008)<=10000);install_js_backend(0);W32(0x700,128);assert(method(k,10,5,20,0,0x700,0)==0x8007001e);
#endif
 assert(movies_directinput_init());for(unsigned i=0;i<32;i++)assert(!invoke(handlers[0].fn,5,args));assert(invoke(handlers[0].fn,5,args)==0x8007000e&&R32(0x600)==0);free(g_mem);puts("DirectInput tests passed");}
