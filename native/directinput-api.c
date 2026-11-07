/* Original game's DirectInput8A buffered keyboard/mouse subset.
 * ABI and IDs: Wine wine-11.0/include/dinput.h.
 * Contracts: Microsoft Learn previous-versions/windows/desktop/
 * ee417818 (Acquire), ee417925 (SetDataFormat), ee416611 (event record),
 * ee416238 (buffered keyboard), ee416239 (buffered mouse).
 * Unsupported vtable entries trap, never report simulated success.
 * UI-thread confined; host callbacks must not reenter guest methods.
 */
#include "directinput-api.h"
#include "window-api.h"
#include "process-api.h"
#include <pthread.h>
#include <stdlib.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifndef MOVIES_DI_SPACE
#define MOVIES_DI_SPACE GUEST_SPACE
#endif
#define BASE 0x03000000u
#define TRAMP 0x0e000000u
#define LIMIT 32u
#define INVALID 0x80070057u
#define NOTINIT 0x80070015u
#define NOTACQUIRED 0x8007000cu
#define INPUTLOST 0x8007001eu
#define ACQUIRED 0x800700aau
#define ARG(n) R32(c->esp+4+4*(n))
typedef struct Event { uint32_t offset,data,time,sequence,app; } Event;
typedef struct Object { uint32_t address,refs,kind,hwnd; unsigned format,buffer,acquired,lost,head,count,overflow;Event events[127]; } Object;
static Object objects[LIMIT];
static uint32_t sequence;
static int initialized;
static pthread_t owner;
static int on_owner(void){return initialized&&pthread_equal(owner,pthread_self());}
static void check_owner(CPU*c){if(!on_owner())guest_trap(c,R32(c->esp),"DirectInput call outside initializing UI thread");}
static MoviesDirectInputHost host;
static void *context;
static const uint8_t iid_unknown[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const uint8_t iid_input[16]={0x30,0x80,0x79,0xbf,0x3a,0x48,0xa2,0x4d,0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0};
static const uint8_t iid_device[16]={0x80,0x10,0xd4,0x54,0x15,0xdc,0x33,0x48,0xa4,0x1b,0x74,0x8f,0x73,0xa3,0x81,0x79};
static const uint8_t sys_keyboard[16]={0x61,0x2b,0x1d,0x6f,0xa0,0xd5,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static const uint8_t sys_mouse[16]={0x60,0x2b,0x1d,0x6f,0xa0,0xd5,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static const uint8_t key_guid[16]={0x20,0x82,0x72,0x55,0x3c,0xd3,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static const uint8_t axis_guid[16]={0xe0,2,0x6d,0xa3,0xf3,0xc9,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0,0};
static int range(uint32_t p,uint64_t n) { return p && (uint64_t)p+n<=MOVIES_DI_SPACE; }
static int guid(uint32_t p,const uint8_t *g) {return range(p,16)&&!memcmp(HP(p),g,16);}
static void ret(CPU*c,unsigned n,uint32_t r){c->eax=r;c->esp+=4*(n+1);}
static Object *find(uint32_t p){for(unsigned i=0;i<LIMIT;i++)if(objects[i].refs&&objects[i].address==p)return &objects[i];return NULL;}
static Object *self(CPU*c){check_owner(c);Object*o=find(ARG(0));if(!o)guest_trap(c,R32(c->esp),"invalid DirectInput object");return o;}
static void clear(Object*o){o->head=o->count=o->overflow=0;}
#ifdef __EMSCRIPTEN__
EM_JS(int,js_available,(void),{return typeof Module.moviesDirectInputAvailable==='function'&&Module.moviesDirectInputAvailable()?1:0;});
EM_JS(int,js_focused,(uint32_t hwnd),{return typeof Module.moviesDirectInputFocused==='function'&&Module.moviesDirectInputFocused(hwnd)?1:0;});
EM_JS(int,js_poll,(uint32_t *out,uint32_t *now),{
 if(typeof Module.moviesPollDirectInputEvents!=='function')return 0;
 const es=Module.moviesPollDirectInputEvents(256);if(!Array.isArray(es)||es.length>256)return -1;
 HEAPU32[now>>>2]=Math.floor(performance.timeOrigin+performance.now())>>>0;
 for(let i=0;i<es.length;i++){const e=es[i];const v=[e.deviceKind,e.offset,e.data,e.time];
 if(v.some(x=>!Number.isInteger(x)||x<0||x>4294967295))return -1;
 for(let j=0;j<4;j++)HEAPU32[(out>>>2)+i*4+j]=v[j];}return es.length;
});
#endif
static int available(void){if(host.available)return host.available(context);
#ifdef __EMSCRIPTEN__
 return js_available();
#else
 return 0;
#endif
}
static int focused(uint32_t hwnd){if(host.focused)return host.focused(context,hwnd);
#ifdef __EMSCRIPTEN__
 return js_focused(hwnd);
#else
 return 0;
#endif
}
static void lose(Object*o){o->acquired=0;o->lost=1;clear(o);}
static void pump(CPU*c){
 for(unsigned i=0;i<LIMIT;i++)if(objects[i].acquired&&(!available()||!focused(objects[i].hwnd)))lose(&objects[i]);
 if(host.pump){host.pump(context);return;}
#ifdef __EMSCRIPTEN__
 /* Drain the bounded host ring before acquisition changes. One batch is
  * insufficient when more than256 events preceded Acquire. */
 for(unsigned batch=0;batch<16;batch++) {
  uint32_t data[256][4],now,tick;int n=js_poll(&data[0][0],&now);
  if(n<0||!movies_process_ticks(&tick)){guest_trap(c,R32(c->esp),"invalid DirectInput host events/clock");return;}
  for(int i=0;i<n;i++){uint32_t time;if(!movies_window_map_input_time(tick,now,data[i][3],&time)||!movies_directinput_push_event(data[i][0],data[i][1],data[i][2],time))guest_trap(c,R32(c->esp),"invalid DirectInput event");}
  if(n<256)return;
 }
 guest_trap(c,R32(c->esp),"DirectInput host queue did not drain within4096 events");
#else
 (void)c;
#endif
}
int movies_directinput_push_event(uint32_t kind,uint32_t off,uint32_t data,uint32_t time){
 if(!on_owner())return 0;
 if((kind!=1&&kind!=2)||(kind==1&&(off>=256||(data!=0&&data!=128)))||(kind==2&&off!=0&&off!=4&&off!=8&&!(off>=12&&off<=15))||(kind==2&&off>=12&&data!=0&&data!=128))return 0;
 uint32_t seq=sequence++;
 for(unsigned i=0;i<LIMIT;i++){Object*o=&objects[i];if(o->kind!=kind||!o->acquired)continue;
 if(!available()||!focused(o->hwnd)){lose(o);continue;}if(!o->buffer)continue;
 if(o->count==127){o->overflow=1;continue;}o->events[(o->head+o->count++)%127]=(Event){off,data,time,seq,0};}
 return 1;
}
static Object*allocate(unsigned kind){for(unsigned i=0;i<LIMIT;i++)if(!objects[i].refs){Object*o=&objects[i];memset(o,0,sizeof *o);o->refs=1;o->kind=kind;o->address=BASE+0x1000+i*16;W32(o->address,BASE+(kind?0x100:0));return o;}return NULL;}
static void query(CPU*c){Object*o=self(c);uint32_t out=ARG(2);if(!range(out,4)){ret(c,3,INVALID);return;}W32(out,0);if(!guid(ARG(1),iid_unknown)&&!guid(ARG(1),o->kind?iid_device:iid_input)){ret(c,3,0x80004002);return;}o->refs++;W32(out,o->address);ret(c,3,0);}
static void addref(CPU*c){Object*o=self(c);ret(c,1,++o->refs);}
static void release(CPU*c){Object*o=self(c);uint32_t refs=--o->refs;if(!refs){W32(o->address,0);memset(o,0,sizeof *o);}ret(c,1,refs);}
static void factory(CPU*c){if(initialized)check_owner(c);uint32_t out=ARG(3);if(!range(out,4)){ret(c,5,INVALID);return;}W32(out,0);if(!initialized){ret(c,5,NOTINIT);return;}if(ARG(4)){ret(c,5,0x80040110);return;}if(ARG(1)!=0x800||!guid(ARG(2),iid_input)){ret(c,5,ARG(1)!=0x800?INVALID:0x80004002);return;}Object*o=allocate(0);if(o)W32(out,o->address);ret(c,5,o?0:0x8007000e);}
static void create(CPU*c){Object*o=self(c);uint32_t out=ARG(2);if(o->kind||!range(out,4)){ret(c,4,INVALID);return;}W32(out,0);if(ARG(3)){ret(c,4,0x80040110);return;}unsigned kind=guid(ARG(1),sys_keyboard)?1:guid(ARG(1),sys_mouse)?2:0;if(!kind){ret(c,4,0x80040154);return;}Object*d=allocate(kind);if(d)W32(out,d->address);ret(c,4,d?0:0x8007000e);}
static void format(CPU*c){Object*o=self(c);uint32_t p=ARG(1);if(o->acquired){ret(c,2,ACQUIRED);return;}unsigned count=o->kind==1?256:7;
 if(!o->kind||!range(p,24)||R32(p)!=24||R32(p+4)!=16||R32(p+8)!=2||R32(p+12)!=(o->kind==1?256:16)||R32(p+16)!=count||!range(R32(p+20),count*16)){ret(c,2,INVALID);return;}
 p=R32(p+20);for(unsigned i=0;i<count;i++,p+=16){uint32_t g=R32(p),off=R32(p+4),type=R32(p+8);int ok;
 if(o->kind==1)ok=guid(g,key_guid)&&off==i&&type==(0x8000000cu|(i<<8));
 else {uint8_t axis[16];memcpy(axis,axis_guid,16);axis[0]+=i;ok=off==(i<3?i*4:i+9)&&type==(i<3?(i==2?0x80ffff03u:0x00ffff03u):(i>=5?0x80ffff0cu:0x00ffff0cu))&&(i<3?guid(g,axis):g==0);}
 if(!ok||R32(p+12)){ret(c,2,INVALID);return;}}
 o->format=1;clear(o);ret(c,2,0);
}
static void coop(CPU*c){Object*o=self(c);uint32_t hwnd=ARG(1);MoviesWindowEvent state;int valid=host.valid_window?host.valid_window(context,hwnd):movies_window_get(hwnd,&state);
 if(o->acquired){ret(c,3,ACQUIRED);return;}if(!o->kind||ARG(2)!=5||!valid){ret(c,3,INVALID);return;}o->hwnd=hwnd;ret(c,3,0);}
static void property(CPU*c){Object*o=self(c);uint32_t p=ARG(2);if(o->acquired){ret(c,3,ACQUIRED);return;}if(ARG(1)!=1){guest_trap(c,R32(c->esp),"unsupported DirectInput property");return;}
 if(!o->kind||!range(p,20)||R32(p)!=20||R32(p+4)!=16||R32(p+8)||R32(p+12)||R32(p+16)!=128){ret(c,3,INVALID);return;}o->buffer=128;clear(o);ret(c,3,0);}
static void acquire(CPU*c){Object*o=self(c);pump(c);if(!o->kind||!o->format||!o->hwnd){ret(c,1,INVALID);return;}if(!available()){ret(c,1,NOTINIT);return;}if(!focused(o->hwnd)){ret(c,1,0x80070005);return;}if(o->acquired){ret(c,1,1);return;}o->acquired=1;o->lost=0;clear(o);ret(c,1,0);}
static void unacquire(CPU*c){Object*o=self(c);uint32_t result=o->acquired?0:1;o->acquired=o->lost=0;clear(o);ret(c,1,result);}
static void getdata(CPU*c){Object*o=self(c);uint32_t out=ARG(2),count=ARG(3),flags=ARG(4);if(ARG(1)!=20||!range(count,4)||flags>1){ret(c,5,INVALID);return;}pump(c);if(!o->acquired){ret(c,5,o->lost?INPUTLOST:NOTACQUIRED);return;}if(!o->buffer){ret(c,5,0x80040207);return;}uint32_t n=R32(count);if(n>o->count)n=o->count;if(out&&!range(out,(uint64_t)n*20)){ret(c,5,INVALID);return;}for(unsigned i=0;out&&i<n;i++){Event*e=&o->events[(o->head+i)%127];W32(out+i*20,e->offset);W32(out+i*20+4,e->data);W32(out+i*20+8,e->time);W32(out+i*20+12,e->sequence);W32(out+i*20+16,e->app);}W32(count,n);uint32_t result=o->overflow?1:0;if(!flags){o->head=(o->head+n)%127;o->count-=n;o->overflow=0;}ret(c,5,result);}
static void unsupported(CPU*c){guest_trap(c,R32(c->esp),"unsupported DirectInput vtable method");}
int movies_directinput_init(void){if(initialized&&!on_owner())return 0;owner=pthread_self();if((uint64_t)BASE+0x10000>MOVIES_DI_SPACE)return 0;memset(objects,0,sizeof objects);memset(HP(BASE),0,0x10000);sequence=0;GuestFn f[]={query,addref,release,create,property,acquire,unacquire,getdata,format,coop,unsupported};for(unsigned i=0;i<sizeof f/sizeof*f;i++)register_function(TRAMP+i*16,f[i]);for(unsigned i=0;i<11;i++)W32(BASE+i*4,TRAMP+10*16);for(unsigned i=0;i<32;i++)W32(BASE+0x100+i*4,TRAMP+10*16);for(unsigned i=0;i<3;i++){W32(BASE+i*4,TRAMP+i*16);W32(BASE+0x100+i*4,TRAMP+i*16);}W32(BASE+12,TRAMP+3*16);unsigned slots[]={6,7,8,10,11,13};for(unsigned i=0;i<6;i++)W32(BASE+0x100+slots[i]*4,TRAMP+(i+4)*16);initialized=1;return 1;}
void movies_directinput_set_host(const MoviesDirectInputHost*h,void*ctx){if(initialized&&!on_owner())abort();memset(&host,0,sizeof host);if(h)host=*h;context=ctx;for(unsigned i=0;i<LIMIT;i++)if(objects[i].acquired)lose(&objects[i]);}
ImportHandler *movies_directinput_handlers(uint32_t*count){static ImportHandler handlers[]={{"DINPUT8.DLL!DirectInput8Create",factory}};*count=1;return handlers;}
