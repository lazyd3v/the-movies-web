/* Original game's mutex/event/wait imports. Contract references:
 * https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createmutexa
 * https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-releasemutex
 * https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobjectex
 * https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep
 * Names are case-sensitive ANSI bytes in a private process namespace. No host
 * named mutexes, APC delivery, or guest thread creation. Thread exit abandons owned
 * objects; the next acquirer receives WAIT_ABANDONED and owns the mutex.
 */
#define _POSIX_C_SOURCE 200809L
#include "sync-api.h"
#include "wait-profile.h"
#include "startup-api.h"
#ifdef MOVIES_GUEST_THREADS
#include "guest-thread-api.h"
#endif
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifndef MOVIES_SYNC_SPACE
#define MOVIES_SYNC_SPACE GUEST_SPACE
#endif
/* Standalone API fixtures omit the optional profiler; production links strong implementations. */
__attribute__((weak)) int movies_wait_profile_active(void){return 0;}
__attribute__((weak)) uint64_t movies_wait_profile_begin(void){return 0;}
__attribute__((weak)) void movies_wait_profile_record(uint32_t k,uint32_t c,uint32_t t,uint32_t s,uint64_t b){(void)k;(void)c;(void)t;(void)s;(void)b;}
#define WAIT_FAILED 0xffffffffu
#define INFINITE 0xffffffffu
#define HANDLE_BASE 0x0b000000u
#define HANDLE_END  0x0c000000u
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
typedef struct Mutex {
    char *name;
    void *owner;
    uint32_t depth,handles,waiters;
    int abandoned;
    unsigned kind; int signaled,manual; uint32_t exit_code,running;
    struct Mutex *next;
} Mutex;
typedef struct Handle { uint32_t id; Mutex *object; struct Handle *next; } Handle;
static Mutex *objects;
static Handle *handles;
static uint32_t next_handle=HANDLE_BASE;
static pthread_key_t owner_key;
static pthread_once_t owner_once=PTHREAD_ONCE_INIT;
static int owner_key_error;
static void ret(CPU *c,unsigned argc,uint32_t value) { c->eax=value;c->esp+=4*(argc+1); }
#define ARG(n) R32(c->esp+4+4*(n))
static void abandon(void *owner) {
    pthread_mutex_lock(&guard);
    for(Mutex *m=objects;m;m=m->next) if(m->owner==owner) {
        m->owner=NULL;m->depth=0;m->abandoned=1;
    }
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&guard);
    free(owner);
}
static void make_owner_key(void) { owner_key_error=pthread_key_create(&owner_key,abandon); }
static void *current_owner(void) {
    pthread_once(&owner_once,make_owner_key);
    if(owner_key_error) return NULL;
    void *p=pthread_getspecific(owner_key);
    if(!p) {
        p=malloc(1);
        if(p && pthread_setspecific(owner_key,p)) { free(p);p=NULL; }
    }
    return p;
}
void movies_sync_abandon_current(void){void*p=current_owner();if(p){pthread_setspecific(owner_key,NULL);abandon(p);}}
static Handle *lookup(uint32_t id) {
    for(Handle *h=handles;h;h=h->next) if(h->id==id) return h;
    return NULL;
}
static void collect(Mutex *m) {
    if(m->handles || m->waiters || m->running) return;
    Mutex **p=&objects;
    while(*p && *p!=m) p=&(*p)->next;
    if(*p) *p=m->next;
    free(m->name);free(m);
}
void movies_sync_init(void) {
    pthread_mutex_lock(&guard);
    while(handles) { Handle *h=handles;handles=h->next;free(h); }
    while(objects) { Mutex *m=objects;objects=m->next;free(m->name);free(m); }
    /* Do not reuse old handle values across resets. */
    pthread_mutex_unlock(&guard);
}
int movies_sync_close_handle(uint32_t id) {
    pthread_mutex_lock(&guard);
    Handle **p=&handles;
    while(*p && (*p)->id!=id) p=&(*p)->next;
    if(!*p) { pthread_mutex_unlock(&guard);return 0; }
    Handle *h=*p;*p=h->next;
    Mutex *m=h->object;--m->handles;free(h);collect(m);
    pthread_mutex_unlock(&guard);return 1;
}
static void create_waitable(CPU *c,unsigned kind) {
    uint32_t argc=kind==2?4:3;
    uint32_t security=ARG(0),initial=ARG(kind==2?2:1),address=ARG(kind==2?3:2),failure=0;
    char name[261];name[0]=0;
    if(security) { movies_set_last_error(50);ret(c,argc,0);return; }
    if(address) {
        unsigned i;
        for(i=0;i<260;i++) {
            if((uint64_t)address+i>=MOVIES_SYNC_SPACE) { failure=87;break; }
            name[i]=(char)R8(address+i);
            if(!name[i]) break;
        }
        if(!failure && i==260) failure=206;
        if(!failure) {
            const char *tail=name;
            if(!strncmp(tail,"Global\\",7)) tail+=7;
            else if(!strncmp(tail,"Local\\",6)) tail+=6;
            if(strchr(tail,'\\')) failure=123;
            if(!failure && !strncmp(name,"Local\\",6)) memmove(name,name+6,strlen(name+6)+1);
        }
    }
    void *owner=initial&&kind==1?current_owner():NULL;
    if(initial && kind==1 && !owner) failure=8;
    if(failure) { movies_set_last_error(failure);ret(c,argc,0);return; }
    pthread_mutex_lock(&guard);
    Mutex *m=NULL;int existed=0;
    if(address) for(m=objects;m;m=m->next) if(m->name && !strcmp(m->name,name)) break;
    existed=m!=NULL;
    if(m&&m->kind!=kind)failure=6;
    Handle *h=calloc(1,sizeof *h);
    if(!h || next_handle==HANDLE_END) failure=8;
    if(!failure && !m) {
        m=calloc(1,sizeof *m);
        if(!m) failure=8;
        else {
            if(address) { m->name=strdup(name);if(!m->name) failure=8; }
            if(failure) { free(m);m=NULL; }
            else { m->kind=kind;m->manual=kind==2&&ARG(1)!=0;m->signaled=kind==2&&initial!=0;m->owner=owner;m->depth=kind==1&&initial?1:0;m->next=objects;objects=m; }
        }
    }
    uint32_t result=0;
    if(!failure) {
        h->id=next_handle++;h->object=m;h->next=handles;handles=h;++m->handles;result=h->id;
    } else free(h);
    pthread_mutex_unlock(&guard);
    movies_set_last_error(failure?failure:(existed?183:0));ret(c,argc,result);
}
static void create_mutex(CPU*c){create_waitable(c,1);}
static void create_event(CPU*c){create_waitable(c,2);}
static void change_event(CPU*c,int signaled){pthread_mutex_lock(&guard);Handle*h=lookup(ARG(0));int ok=h&&h->object->kind==2;if(ok){h->object->signaled=signaled;if(signaled)pthread_cond_broadcast(&changed);}pthread_mutex_unlock(&guard);if(!ok)movies_set_last_error(6);ret(c,1,ok);}
static void set_event(CPU*c){change_event(c,1);}static void reset_event(CPU*c){change_event(c,0);}
/* The execution reference outlives CloseHandle and is released only on exit. */
uint32_t movies_sync_create_thread_object(void **token){
 *token=NULL;Mutex*m=calloc(1,sizeof*m);Handle*h=calloc(1,sizeof*h);if(!m||!h){free(m);free(h);return 0;}
 pthread_mutex_lock(&guard);if(next_handle==HANDLE_END){pthread_mutex_unlock(&guard);free(m);free(h);return 0;}
 m->kind=3;m->running=1;m->handles=1;m->exit_code=259;m->next=objects;objects=m;h->object=m;h->id=next_handle++;h->next=handles;handles=h;*token=m;uint32_t id=h->id;pthread_mutex_unlock(&guard);return id;
}
void movies_sync_complete_thread(void*token,uint32_t code){pthread_mutex_lock(&guard);Mutex*m=token;m->exit_code=code;m->running=0;m->signaled=1;pthread_cond_broadcast(&changed);collect(m);pthread_mutex_unlock(&guard);}
static void get_exit_code(CPU*c){uint32_t out=ARG(1);if(!out||(uint64_t)out+4>MOVIES_SYNC_SPACE){movies_set_last_error(87);ret(c,2,0);return;}pthread_mutex_lock(&guard);Handle*h=lookup(ARG(0));int ok=h&&h->object->kind==3;if(ok)W32(out,h->object->exit_code);pthread_mutex_unlock(&guard);if(!ok)movies_set_last_error(6);ret(c,2,ok);}
static void release_mutex(CPU *c) {
    void *owner=current_owner();uint32_t failure=0;
    if(!owner) { movies_set_last_error(8);ret(c,1,0);return; }
    pthread_mutex_lock(&guard);
    Handle *h=lookup(ARG(0));
    if(!h||h->object->kind!=1) failure=6;
    else if(h->object->owner!=owner) failure=288;
    else if(--h->object->depth==0) { h->object->owner=NULL;pthread_cond_broadcast(&changed); }
    pthread_mutex_unlock(&guard);
    if(failure) movies_set_last_error(failure);
    ret(c,1,failure?0:1);
}
static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)) return 0;
    return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
}
static int ready(Mutex*m,void*owner){return m->kind==1?(!m->owner||m->owner==owner):m->signaled;}
static uint32_t acquire(Mutex*m,void*owner){if(m->kind==1){uint32_t abandoned=m->abandoned;m->abandoned=0;m->owner=owner;++m->depth;return abandoned;}if(m->kind==2&&!m->manual)m->signaled=0;return 0;}
static void wait_set(CPU*c,unsigned argc,int multiple){
 uint32_t count=multiple?ARG(0):1,array=multiple?ARG(1):0,all=multiple?ARG(2)!=0:0,timeout=ARG(multiple?3:1),failure=0,result=0;int fatal=0;
 if((!multiple&&argc==3&&ARG(2))||!count||count>64||(multiple&&(!array||(uint64_t)array+count*4>MOVIES_SYNC_SPACE))){movies_set_last_error(!multiple&&argc==3&&ARG(2)?50:87);ret(c,argc,WAIT_FAILED);return;}
 int profiling=movies_wait_profile_active();uint32_t profile_caller=profiling?R32(c->esp):0;
 uint32_t ids[64];Mutex*set[64]={0};for(unsigned i=0;i<count;i++){ids[i]=multiple?R32(array+i*4):ARG(0);for(unsigned j=0;j<i;j++)if(ids[j]==ids[i])failure=87;}
 void*owner=current_owner();if(!owner)failure=8;
 uint64_t started=monotonic_ns(),deadline=started+(uint64_t)timeout*1000000ull;if(!started)failure=31;
 pthread_mutex_lock(&guard);
 if(!failure)for(unsigned i=0;i<count;i++){Handle*h=lookup(ids[i]);if(!h){failure=6;break;}set[i]=h->object;set[i]->waiters++;}
 while(!failure){
#ifdef MOVIES_GUEST_THREADS
  /* NULL query is atomic-only: never acquire lifecycle while holding guard. */
  if(movies_guest_threads_failed(NULL,0)){fatal=1;break;}
#endif
  unsigned eligible=0,first=count;for(unsigned i=0;i<count;i++)if(ready(set[i],owner)){eligible++;if(first==count)first=i;}
  if(all?eligible==count:eligible>0){
   for(unsigned i=0;i<count;i++)if((all||i==first)&&set[i]->kind==1&&set[i]->depth==0x7fffffffu)failure=50;
   if(failure)break;result=all?0:first;
   for(unsigned i=0;i<count;i++)if(all||i==first)if(acquire(set[i],owner)&&result<128)result=128+i;
   break;
  }
  uint64_t now=monotonic_ns();if(!now){failure=31;break;}if(timeout!=INFINITE&&now>=deadline){result=258;break;}
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
  failure=50;break; /* no guest producer can run while this owner blocks */
#endif
  int status;
  uint64_t profile_start=0;
#ifndef MOVIES_GUEST_THREADS
  if(timeout==INFINITE){if(profiling)profile_start=movies_wait_profile_begin();status=pthread_cond_wait(&changed,&guard);}
  else
#endif

  {uint64_t remaining=timeout==INFINITE?50000000ull:deadline-now;if(remaining>50000000ull)remaining=50000000ull;struct timespec until;if(clock_gettime(CLOCK_REALTIME,&until)){failure=31;break;}uint64_t ns=until.tv_nsec+remaining;until.tv_sec+=(time_t)(ns/1000000000ull);until.tv_nsec=(long)(ns%1000000000ull);if(profiling)profile_start=movies_wait_profile_begin();status=pthread_cond_timedwait(&changed,&guard,&until);}
  if(profile_start)movies_wait_profile_record(2,profile_caller,timeout,(uint32_t)status,profile_start);
  if(status&&status!=ETIMEDOUT){failure=31;break;}
 }
 for(unsigned i=0;i<count;i++)if(set[i]){--set[i]->waiters;collect(set[i]);}
 pthread_mutex_unlock(&guard);
#ifdef MOVIES_GUEST_THREADS
 if(fatal){char message[256];movies_guest_threads_failed(message,sizeof message);guest_trap(c,R32(c->esp),message);return;}
#else
 (void)fatal;
#endif
 if(failure){movies_set_last_error(failure);result=WAIT_FAILED;}ret(c,argc,result);
}
static void wait_single(CPU*c){wait_set(c,2,0);}static void wait_single_ex(CPU*c){wait_set(c,3,0);}static void wait_multiple(CPU*c){wait_set(c,4,1);}
#ifdef __EMSCRIPTEN__
/* A private cell has no notifier, so the wait ends only at its timeout. This is
 * synchronous worker blocking, with no Asyncify unwind or busy-spin fallback.
 * Atomics.wait rejects browser main-thread execution instead of faking delay. */
EM_JS(void, movies_worker_sleep, (double milliseconds), {
    var cell = Module['moviesSleepCell'];
    if (!cell) cell = Module['moviesSleepCell'] = new Int32Array(new SharedArrayBuffer(4));
    Atomics.wait(cell, 0, 0, milliseconds === 4294967295 ? Infinity : milliseconds);
});
#endif
static void sleep_thread(CPU *c) {
    uint32_t milliseconds=ARG(0);
    int profiling=movies_wait_profile_active();
    uint32_t profile_caller=profiling?R32(c->esp):0;
    uint64_t profile_start=profiling?movies_wait_profile_begin():0;
#ifdef __EMSCRIPTEN__
    movies_worker_sleep((double)milliseconds);
#else
    if(!milliseconds) sched_yield();
    else {
        do {
            /* Infinite sleeps never return; interrupting host signals restart
             * the remaining interval, rather than delivering fictitious APCs. */
            uint32_t interval=milliseconds==INFINITE?86400000u:milliseconds;
            struct timespec remaining={(time_t)(interval/1000u),(long)(interval%1000u)*1000000L};
            while(nanosleep(&remaining,&remaining)) {
                if(errno!=EINTR) { guest_trap(c,R32(c->esp),"Sleep host wait failed");return; }
            }
        } while(milliseconds==INFINITE);
    }
#endif
    if(profile_start)movies_wait_profile_record(1,profile_caller,milliseconds,0,profile_start);
    c->esp+=8; /* VOID: preserve LastError and guest registers. */
}
ImportHandler *movies_sync_handlers(uint32_t *count) {
    static ImportHandler h[]={
        {"KERNEL32.DLL!CreateEventA",create_event},{"KERNEL32.DLL!SetEvent",set_event},{"KERNEL32.DLL!ResetEvent",reset_event},
        {"KERNEL32.DLL!GetExitCodeThread",get_exit_code},{"KERNEL32.DLL!WaitForMultipleObjects",wait_multiple},
        {"KERNEL32.DLL!Sleep",sleep_thread},
        {"KERNEL32.DLL!CreateMutexA",create_mutex},
        {"KERNEL32.DLL!ReleaseMutex",release_mutex},
        {"KERNEL32.DLL!WaitForSingleObject",wait_single},
        {"KERNEL32.DLL!WaitForSingleObjectEx",wait_single_ex}
    };
    *count=sizeof h/sizeof h[0];return h;
}
