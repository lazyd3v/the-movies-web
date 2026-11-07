#include "guest-thread-api.h"
#include "sync-api.h"
#include "thread-api.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int dyncode_contains(uint32_t a){(void)a;return 0;}
void interp_call(CPU*c,uint32_t a){(void)c;(void)a;abort();}
static _Thread_local uint32_t error;
static pthread_mutex_t alloc_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t next=0x10000,allocated[64];static unsigned live;
void movies_set_last_error(uint32_t e){error=e;}
uint32_t movies_backend_alloc(uint32_t n){pthread_mutex_lock(&alloc_lock);unsigned i;for(i=0;i<64;i++)if(!allocated[i])break;assert(i<64);uint32_t a=next;next+=(n+7)&~7u;assert(next<0x2000000);allocated[i]=a;live++;memset(HP(a),0,n);pthread_mutex_unlock(&alloc_lock);return a;}
int movies_backend_free(uint32_t a){pthread_mutex_lock(&alloc_lock);int ok=0;for(unsigned i=0;i<64;i++)if(allocated[i]==a){allocated[i]=0;live--;ok=1;break;}pthread_mutex_unlock(&alloc_lock);return ok;}
static uint32_t call(CPU*c,const char*name,unsigned n,const uint32_t*a){uint32_t old=c->esp;for(int i=(int)n-1;i>=0;i--)PUSH32(c,a[i]);PUSH32(c,0x1234);for(unsigned group=0;group<3;group++){uint32_t count;ImportHandler*h=group==0?movies_guest_thread_handlers(&count):group==1?movies_sync_handlers(&count):movies_thread_handlers(&count);for(unsigned i=0;i<count;i++)if(!strcmp(h[i].name+13,name)){h[i].fn(c);assert(c->esp==old);return c->eax;}}abort();}
#define CALL(c,n,...) call(c,n,sizeof((uint32_t[]){__VA_ARGS__})/4,(uint32_t[]){__VA_ARGS__})
static atomic_uint reached,finished;static uint32_t gate,tls;
static void worker(CPU*c){uint32_t mode=R32(c->esp+4);assert(c->fs_base!=0x10000&&R32(c->fs_base)==0xffffffffu&&R32(c->fs_base+0x18)==c->fs_base);assert(R32(c->fs_base+4)>c->esp&&R32(c->fs_base+8)<c->esp);
 uint32_t id=CALL(c,"GetCurrentThreadId");assert(id==R32(c->fs_base+0x24));assert(CALL(c,"TlsGetValue",tls)==0);assert(CALL(c,"TlsSetValue",tls,id));atomic_fetch_add(&reached,1);
 W32(c->esp-128,gate);assert(CALL(c,"WaitForMultipleObjects",1,c->esp-128,0,0xffffffffu)==0);assert(CALL(c,"TlsGetValue",tls)==id);atomic_fetch_add(&finished,1);
 if(mode==2){struct timespec delay={0,20000000};nanosleep(&delay,NULL);guest_lock();guest_trap(c,0,"isolated child trap");}if(mode==1){CALL(c,"ExitThread",88);abort();}c->eax=77;c->esp+=8;
}
static void wait_reached(unsigned n){struct timespec t={0,1000000};for(unsigned i=0;i<5000&&atomic_load(&reached)<n;i++)nanosleep(&t,NULL);assert(atomic_load(&reached)>=n);}
static void failure_wait_test(int multiple){
 struct timespec pause={0,1000000};int stopped=0;for(unsigned i=0;i<5000&&!stopped;i++){stopped=movies_guest_threads_shutdown();if(!stopped)nanosleep(&pause,NULL);}assert(stopped&&movies_guest_threads_init(2));
 MoviesGuestFailureView view;movies_guest_threads_failure_view(&view);assert(view.message_capacity==256&&!__atomic_load_n(view.ready,__ATOMIC_ACQUIRE));
 CPU c={0};c.esp=0x8000;gate=CALL(&c,"CreateEventA",0,1,1,0);uint32_t unrelated=CALL(&c,"CreateEventA",0,1,0,0),thread=CALL(&c,"CreateThread",0,0,0x5000,2,0,0);assert(thread);W32(0x300,unrelated);
 jmp_buf boundary;jmp_buf*previous=g_trap_jmp;g_trap_jmp=&boundary;
 if(!setjmp(boundary)){if(multiple)CALL(&c,"WaitForMultipleObjects",1,0x300,0,0xffffffffu);else CALL(&c,"WaitForSingleObject",unrelated,0xffffffffu);assert(0);}
 assert(strstr(g_trap_msg,"isolated child trap"));assert(__atomic_load_n(view.ready,__ATOMIC_ACQUIRE)==1&&strstr(view.message,"isolated child trap"));g_trap_jmp=previous;
 assert(movies_sync_close_handle(unrelated)&&movies_sync_close_handle(gate)&&movies_sync_close_handle(thread));
 stopped=0;for(unsigned i=0;i<5000&&!stopped;i++){stopped=movies_guest_threads_shutdown();if(!stopped)nanosleep(&pause,NULL);}assert(stopped);
}
int main(void){g_mem=calloc(1,0x2000000);assert(g_mem);movies_sync_init();register_function(0x5000,worker);assert(movies_guest_threads_init(2));CPU main_cpu={0},*c=&main_cpu;c->esp=0x8000;tls=CALL(c,"TlsAlloc");assert(CALL(c,"TlsSetValue",tls,123));
 gate=CALL(c,"CreateEventA",0,1,0,0);assert(gate);assert(CALL(c,"WaitForSingleObject",gate,0)==258);
 jmp_buf main_trap;g_trap_jmp=&main_trap;assert(!setjmp(main_trap));
 uint32_t a=CALL(c,"CreateThread",0,0,0x5000,0,0,0x100),b=CALL(c,"CreateThread",0,0,0x5000,1,0,0x104);assert(a&&b&&R32(0x100)!=R32(0x104));wait_reached(2);
 assert(!CALL(c,"CreateThread",0,0,0x5000,0,0,0)&&error==8); /* bounded prewarmed capacity exhausted */

 assert(CALL(c,"GetExitCodeThread",a,0x108)&&R32(0x108)==259);assert(CALL(c,"TlsGetValue",tls)==123);assert(CALL(c,"SetEvent",gate));assert(!CALL(c,"WaitForSingleObject",a,5000));assert(!CALL(c,"WaitForSingleObject",b,5000));assert(CALL(c,"GetExitCodeThread",a,0x108)&&R32(0x108)==77);assert(CALL(c,"GetExitCodeThread",b,0x108)&&R32(0x108)==88);assert(movies_sync_close_handle(a)&&movies_sync_close_handle(b));
 assert(CALL(c,"ResetEvent",gate));uint32_t d=CALL(c,"CreateThread",0,0,0x5000,0,0,0);assert(d);wait_reached(3);assert(movies_sync_close_handle(d));assert(CALL(c,"SetEvent",gate));
 struct timespec pause={0,1000000};for(unsigned i=0;i<5000;i++){pthread_mutex_lock(&alloc_lock);unsigned outstanding=live;pthread_mutex_unlock(&alloc_lock);if(!outstanding)break;nanosleep(&pause,NULL);}pthread_mutex_lock(&alloc_lock);assert(!live);pthread_mutex_unlock(&alloc_lock);assert(atomic_load(&finished)==3);
 assert(!CALL(c,"CreateThread",0,0,0x5000,0,4,0)&&error==50);assert(!CALL(c,"CreateThread",0,0,0x9999,0,0,0)&&error==487);
 uint32_t autoevent=CALL(c,"CreateEventA",0,0,1,0);assert(!CALL(c,"WaitForSingleObject",autoevent,0));assert(CALL(c,"WaitForSingleObject",autoevent,0)==258);assert(CALL(c,"SetEvent",autoevent));assert(!CALL(c,"WaitForSingleObject",autoevent,0));assert(movies_sync_close_handle(autoevent));assert(movies_sync_close_handle(gate));uint32_t e1=CALL(c,"CreateEventA",0,0,1,0),e2=CALL(c,"CreateEventA",0,0,0,0);W32(0x200,e1);W32(0x204,e2);
 assert(CALL(c,"WaitForMultipleObjects",2,0x200,1,0)==258);assert(!CALL(c,"WaitForSingleObject",e1,0));assert(CALL(c,"SetEvent",e1));assert(CALL(c,"SetEvent",e2));assert(!CALL(c,"WaitForMultipleObjects",2,0x200,1,0));assert(CALL(c,"WaitForSingleObject",e1,0)==258&&CALL(c,"WaitForSingleObject",e2,0)==258);
 W32(0x204,e1);assert(CALL(c,"WaitForMultipleObjects",2,0x200,0,0)==0xffffffffu&&error==87);assert(movies_sync_close_handle(e1)&&movies_sync_close_handle(e2));failure_wait_test(0);failure_wait_test(1);assert(CALL(c,"TlsFree",tls));g_trap_jmp=NULL;int stopped=0;for(unsigned i=0;i<5000&&!stopped;i++){stopped=movies_guest_threads_shutdown();if(!stopped)nanosleep(&pause,NULL);}assert(stopped);free(g_mem);puts("guest thread contexts/events/return/ExitThread/trap/TLS/close-before-exit PASS");return 0;}
