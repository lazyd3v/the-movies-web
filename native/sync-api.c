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
#include "heap-api.h"
#ifdef MOVIES_GUEST_THREADS
#include "guest-thread-api.h"
#endif
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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
/* Section storage and file snapshots use the heap backend and installed file
 * handlers; isolated fixtures without them simply fail those operations. */
__attribute__((weak)) uint32_t movies_backend_alloc(uint32_t bytes){(void)bytes;return 0;}
__attribute__((weak)) int movies_backend_free(uint32_t address){(void)address;return 0;}
__attribute__((weak)) uint32_t movies_get_last_error(void){return 0;}
__attribute__((weak)) GuestFn find_import_handler(const char *name){(void)name;return NULL;}
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
    unsigned kind; int signaled,manual; uint32_t exit_code,running,count,maximum; /* kind 4: semaphore count/maximum */
    /* kind 5 file mapping: backend storage, live views; kind 6 process; kind 7 tagged inert object. */
    uint32_t map_base,map_size,views,map_protect,tag,value;
    int pulse; /* PulseEvent-style timer signal: reset after the next readiness pass */
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
    if(m->handles || m->waiters || m->running || m->views) return;
    Mutex **p=&objects;
    while(*p && *p!=m) p=&(*p)->next;
    if(*p) *p=m->next;
    if(m->kind==5 && m->map_base) movies_backend_free(m->map_base);
    free(m->name);free(m);
}
/* Event timers (timeSetEvent TIME_CALLBACK_EVENT_SET/PULSE) fire lazily inside
 * waits: every wait first signals due timers and bounds its sleep by the next
 * deadline, so no host timer thread is needed. Function callbacks are refused. */
typedef struct Timer { uint32_t id; Mutex *event; uint64_t due,period; int pulse; } Timer;
#define TIMER_LIMIT 64u
static Timer timers[TIMER_LIMIT];
static unsigned timer_count;
static uint32_t next_timer_id=1;
/* Mapped views: base address -> mapping object (same address may repeat). */
typedef struct View { uint32_t address; Mutex *object; } View;
#define VIEW_LIMIT 256u
static View views[VIEW_LIMIT];
static unsigned view_count;
static _Thread_local Mutex *thread_object;
static Mutex *process_object;
void movies_sync_init(void) {
    pthread_mutex_lock(&guard);
    while(handles) { Handle *h=handles;handles=h->next;free(h); }
    while(objects) { Mutex *m=objects;objects=m->next;if(m->kind==5&&m->map_base)movies_backend_free(m->map_base);free(m->name);free(m); }
    memset(timers,0,sizeof timers);timer_count=0;memset(views,0,sizeof views);view_count=0;
    thread_object=NULL;process_object=NULL;
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
/* Semaphores: CreateSemaphoreA(security, initial, maximum, name) / ReleaseSemaphore.
 * A wait succeeds while count > 0 and decrements it (WaitFor* via ready/acquire).
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createsemaphorea */
static void create_semaphore(CPU*c){
    uint32_t security=ARG(0),initial=ARG(1),maximum=ARG(2),address=ARG(3);char name[261];name[0]=0;
    if(security){movies_set_last_error(50);ret(c,4,0);return;}
    if((int32_t)maximum<=0||(int32_t)initial<0||initial>maximum){movies_set_last_error(87);ret(c,4,0);return;}
    if(address){unsigned i;for(i=0;i<260;i++){if((uint64_t)address+i>=MOVIES_SYNC_SPACE){movies_set_last_error(87);ret(c,4,0);return;}name[i]=(char)R8(address+i);if(!name[i])break;}if(i==260){movies_set_last_error(206);ret(c,4,0);return;}}
    pthread_mutex_lock(&guard);Mutex*m=NULL;int existed=0;uint32_t failure=0;
    if(address&&name[0])for(m=objects;m;m=m->next)if(m->name&&!strcmp(m->name,name))break;
    existed=m!=NULL;if(m&&m->kind!=4)failure=6;
    Handle*h=calloc(1,sizeof *h);if(!h||next_handle==HANDLE_END)failure=8;
    if(!failure&&!m){m=calloc(1,sizeof *m);if(!m)failure=8;else{if(address&&name[0]){m->name=strdup(name);if(!m->name){free(m);m=NULL;failure=8;}}if(m){m->kind=4;m->count=initial;m->maximum=maximum;m->next=objects;objects=m;}}}
    uint32_t result=0;if(!failure){h->id=next_handle++;h->object=m;h->next=handles;handles=h;++m->handles;result=h->id;}else free(h);
    pthread_mutex_unlock(&guard);movies_set_last_error(failure?failure:(existed?183:0));ret(c,4,result);
}
static void release_semaphore(CPU*c){
    uint32_t release=ARG(1),previous=ARG(2),failure=0,before=0;
    if(previous&&(uint64_t)previous+4>MOVIES_SYNC_SPACE){movies_set_last_error(87);ret(c,3,0);return;}
    pthread_mutex_lock(&guard);Handle*h=lookup(ARG(0));
    if(!h||h->object->kind!=4)failure=6;else if((int32_t)release<=0)failure=87;
    else{Mutex*m=h->object;before=m->count;if((uint64_t)m->count+release>m->maximum)failure=298;else{m->count+=release;pthread_cond_broadcast(&changed);}}
    pthread_mutex_unlock(&guard);
    if(failure){movies_set_last_error(failure);ret(c,3,0);return;}
    if(previous)W32(previous,before);ret(c,3,1);
}
/* Thread priorities have no browser equivalent; report success / NORMAL. */
static void set_thread_priority(CPU*c){ret(c,2,1);}
static void get_thread_priority(CPU*c){ret(c,1,0);}
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
static int ready(Mutex*m,void*owner){return m->kind==1?(!m->owner||m->owner==owner):m->kind==4?m->count>0:m->signaled;}
static uint32_t acquire(Mutex*m,void*owner){if(m->kind==1){uint32_t abandoned=m->abandoned;m->abandoned=0;m->owner=owner;++m->depth;return abandoned;}if(m->kind==4){--m->count;return 0;}if(m->kind==2&&!m->manual)m->signaled=0;return 0;}
/* Fire due event timers; caller holds guard. Returns next pending deadline or 0. */
static uint64_t fire_timers_locked(uint64_t now){
 uint64_t next=0;int fired=0;
 for(unsigned i=0;i<timer_count;){
  Timer*t=&timers[i];
  if(t->due<=now){
   t->event->signaled=1;if(t->pulse)t->event->pulse=1;fired=1;
   if(t->period){uint64_t late=now-t->due;t->due+=t->period*(late/t->period+1);}
   else{Mutex*m=t->event;timers[i]=timers[--timer_count];--m->handles;collect(m);continue;}
  }
  if(!next||t->due<next)next=t->due;
  i++;
 }
 if(fired)pthread_cond_broadcast(&changed);
 return next;
}
static void clear_pulses_locked(Mutex*const*set,unsigned count){for(unsigned i=0;i<count;i++)if(set[i]->pulse){set[i]->pulse=0;set[i]->signaled=0;}}
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
static void host_sleep_ms(uint32_t milliseconds){
#ifdef __EMSCRIPTEN__
 movies_worker_sleep((double)milliseconds);
#else
 if(!milliseconds){sched_yield();return;}
 struct timespec remaining={(time_t)(milliseconds/1000u),(long)(milliseconds%1000u)*1000000L};
 while(nanosleep(&remaining,&remaining)&&errno==EINTR){}
#endif
}
/* Shared WaitFor* and MsgWait core. Returns WAIT_OBJECT_0+i, WAIT_ABANDONED_0+i,
 * WAIT_TIMEOUT or WAIT_FAILED with *error. count 0 only sleeps for timeout. */
uint32_t movies_sync_wait(CPU*c,uint32_t count,const uint32_t*ids,int all,uint32_t timeout,uint32_t*error){
 uint32_t failure=0,result=0;int fatal=0;*error=0;
 if(count>64){*error=87;return WAIT_FAILED;}
 if(!count){
  if(timeout==INFINITE)for(;;)host_sleep_ms(86400000u);
  host_sleep_ms(timeout);return 258;
 }
 int profiling=movies_wait_profile_active();uint32_t profile_caller=profiling?R32(c->esp):0;
 Mutex*set[64]={0};for(unsigned i=0;i<count;i++)for(unsigned j=0;j<i;j++)if(ids[j]==ids[i])failure=87;
 void*owner=current_owner();if(!owner)failure=8;
 uint64_t started=monotonic_ns(),deadline=started+(uint64_t)timeout*1000000ull;if(!started)failure=31;
 pthread_mutex_lock(&guard);
 if(!failure)for(unsigned i=0;i<count;i++){Handle*h=lookup(ids[i]);if(!h){failure=6;break;}set[i]=h->object;set[i]->waiters++;}
 while(!failure){
#ifdef MOVIES_GUEST_THREADS
  /* NULL query is atomic-only: never acquire lifecycle while holding guard. */
  if(movies_guest_threads_failed(NULL,0)){fatal=1;break;}
#endif
  uint64_t now=monotonic_ns();if(!now){failure=31;break;}
  uint64_t next_timer=timer_count?fire_timers_locked(now):0;
  unsigned eligible=0,first=count;for(unsigned i=0;i<count;i++)if(ready(set[i],owner)){eligible++;if(first==count)first=i;}
  if(all?eligible==count:eligible>0){
   for(unsigned i=0;i<count;i++)if((all||i==first)&&set[i]->kind==1&&set[i]->depth==0x7fffffffu)failure=50;
   if(failure)break;result=all?0:first;
   for(unsigned i=0;i<count;i++)if(all||i==first)if(acquire(set[i],owner)&&result<128)result=128+i;
   clear_pulses_locked(set,count);
   break;
  }
  clear_pulses_locked(set,count);
  if(timeout!=INFINITE&&now>=deadline){result=258;break;}
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
  failure=50;break; /* no guest producer can run while this owner blocks */
#endif
  int status;
  uint64_t profile_start=0;
#ifndef MOVIES_GUEST_THREADS
  if(timeout==INFINITE&&!next_timer){if(profiling)profile_start=movies_wait_profile_begin();status=pthread_cond_wait(&changed,&guard);}
  else
#endif

  {uint64_t remaining=timeout==INFINITE?50000000ull:deadline-now;if(remaining>50000000ull)remaining=50000000ull;if(next_timer&&next_timer-now<remaining)remaining=next_timer>now?next_timer-now:0;struct timespec until;if(clock_gettime(CLOCK_REALTIME,&until)){failure=31;break;}uint64_t ns=until.tv_nsec+remaining;until.tv_sec+=(time_t)(ns/1000000000ull);until.tv_nsec=(long)(ns%1000000000ull);if(profiling)profile_start=movies_wait_profile_begin();status=pthread_cond_timedwait(&changed,&guard,&until);}
  if(profile_start)movies_wait_profile_record(2,profile_caller,timeout,(uint32_t)status,profile_start);
  if(status&&status!=ETIMEDOUT){failure=31;break;}
 }
 for(unsigned i=0;i<count;i++)if(set[i]){--set[i]->waiters;collect(set[i]);}
 pthread_mutex_unlock(&guard);
#ifdef MOVIES_GUEST_THREADS
 if(fatal){char message[256];movies_guest_threads_failed(message,sizeof message);guest_trap(c,R32(c->esp),message);*error=31;return WAIT_FAILED;}
#else
 (void)fatal;
#endif
 if(failure){*error=failure;return WAIT_FAILED;}
 return result;
}
static void wait_set(CPU*c,unsigned argc,int multiple){
 uint32_t count=multiple?ARG(0):1,array=multiple?ARG(1):0,all=multiple?ARG(2)!=0:0,timeout=ARG(multiple?3:1);
 /* bAlertable: the guest imports no APC sources (QueueUserAPC, *FileEx), so an
  * alertable wait can never return WAIT_IO_COMPLETION and behaves as a plain wait. */
 if(!count||count>64||(multiple&&(!array||(uint64_t)array+count*4>MOVIES_SYNC_SPACE))){movies_set_last_error(87);ret(c,argc,WAIT_FAILED);return;}
 uint32_t ids[64];for(unsigned i=0;i<count;i++)ids[i]=multiple?R32(array+i*4):ARG(0);
 uint32_t error,result=movies_sync_wait(c,count,ids,all,timeout,&error);
 if(result==WAIT_FAILED)movies_set_last_error(error);ret(c,argc,result);
}
static void wait_single(CPU*c){wait_set(c,2,0);}static void wait_single_ex(CPU*c){wait_set(c,3,0);}static void wait_multiple(CPU*c){wait_set(c,4,1);}
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
/* ---- Pseudo handles, duplication and process objects ----
 * https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getcurrentthread
 * https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle
 * One virtual process: its pseudo handle (-1) and duplicated process handles
 * are never signaled and report STILL_ACTIVE. Thread pseudo handle (-2) maps to
 * the bound guest-thread completion object, or a never-ending initial thread. */
#define CURRENT_PROCESS 0xffffffffu
#define CURRENT_THREAD 0xfffffffeu
void movies_sync_bind_current_thread(void*token){thread_object=token;}
static Mutex*new_object_locked(unsigned kind){Mutex*m=calloc(1,sizeof*m);if(!m)return NULL;m->kind=kind;m->next=objects;objects=m;return m;}
static uint32_t new_handle_locked(Mutex*m){
 if(next_handle==HANDLE_END)return 0;Handle*h=calloc(1,sizeof*h);if(!h)return 0;
 h->id=next_handle++;h->object=m;h->next=handles;handles=h;++m->handles;return h->id;
}
static Mutex*current_thread_locked(void){
 if(!thread_object){Mutex*m=new_object_locked(3);if(!m)return NULL;m->running=1;m->handles=1;m->exit_code=259;thread_object=m;}
 return thread_object;
}
static Mutex*current_process_locked(void){
 if(!process_object){Mutex*m=new_object_locked(6);if(!m)return NULL;m->handles=1;m->exit_code=259;process_object=m;}
 return process_object;
}
static int process_ref_locked(uint32_t h){if(h==CURRENT_PROCESS)return 1;Handle*x=lookup(h);return x&&x->object->kind==6;}
int movies_sync_is_process_handle(uint32_t handle){pthread_mutex_lock(&guard);int ok=process_ref_locked(handle);pthread_mutex_unlock(&guard);return ok;}
static void current_thread_handle(CPU*c){ret(c,0,CURRENT_THREAD);}
static void duplicate_handle(CPU*c){
 uint32_t source_process=ARG(0),source=ARG(1),target_process=ARG(2),out=ARG(3),options=ARG(6),failure=0,result=0;
 if(out&&(uint64_t)out+4>MOVIES_SYNC_SPACE){movies_set_last_error(87);ret(c,7,0);return;}
 pthread_mutex_lock(&guard);
 if(!process_ref_locked(source_process)||!process_ref_locked(target_process))failure=6;
 else if(options&~3u)failure=87;
 else if(!out&&!(options&1))failure=87;
 else{
  Mutex*m=NULL;Handle*h=NULL;
  if(source==CURRENT_THREAD)m=current_thread_locked();
  else if(source==CURRENT_PROCESS)m=current_process_locked();
  else if((h=lookup(source)))m=h->object;
  else failure=source?50:6; /* non-sync handles belong to other owners */
  if(!failure&&!m)failure=8;
  if(!failure&&out){result=new_handle_locked(m);if(!result)failure=8;}
 }
 pthread_mutex_unlock(&guard);
 if(failure){movies_set_last_error(failure);ret(c,7,0);return;}
 if(options&1)movies_sync_close_handle(source);
 if(out)W32(out,result);ret(c,7,1);
}
/* Inheritance/protection flags have no meaning without child processes. */
static void set_handle_information(CPU*c){
 uint32_t handle=ARG(0),mask=ARG(1);
 if(!handle||handle==0xffffffffu){movies_set_last_error(6);ret(c,3,0);return;}
 if(mask&~3u){movies_set_last_error(87);ret(c,3,0);return;}
 ret(c,3,1);
}
/* Guest register state is owned by translated code; it cannot be sampled. */
static void get_thread_context(CPU*c){movies_set_last_error(50);ret(c,2,0);}
static void get_exit_code_process(CPU*c){
 uint32_t out=ARG(1);if(!out||(uint64_t)out+4>MOVIES_SYNC_SPACE){movies_set_last_error(87);ret(c,2,0);return;}
 pthread_mutex_lock(&guard);int ok=process_ref_locked(ARG(0));pthread_mutex_unlock(&guard);
 if(!ok){movies_set_last_error(6);ret(c,2,0);return;}
 W32(out,259);ret(c,2,1); /* STILL_ACTIVE: the only process is running this call */
}
/* Inert closable objects for other modules (for example Toolhelp snapshots). */
uint32_t movies_sync_create_tagged(uint32_t tag,uint32_t value){
 pthread_mutex_lock(&guard);Mutex*m=new_object_locked(7);uint32_t id=0;
 if(m){m->tag=tag;m->value=value;id=new_handle_locked(m);if(!id)collect(m);}
 pthread_mutex_unlock(&guard);return id;
}
static Mutex*tagged_locked(uint32_t handle,uint32_t tag){Handle*h=lookup(handle);return h&&h->object->kind==7&&h->object->tag==tag?h->object:NULL;}
int movies_sync_tagged_get(uint32_t handle,uint32_t tag,uint32_t*value){pthread_mutex_lock(&guard);Mutex*m=tagged_locked(handle,tag);if(m&&value)*value=m->value;pthread_mutex_unlock(&guard);return m!=NULL;}
int movies_sync_tagged_set(uint32_t handle,uint32_t tag,uint32_t value){pthread_mutex_lock(&guard);Mutex*m=tagged_locked(handle,tag);if(m)m->value=value;pthread_mutex_unlock(&guard);return m!=NULL;}
/* ---- WINMM event timers ----
 * https://learn.microsoft.com/en-us/previous-versions/dd757634(v=vs.85)
 * Original C8D608 (DirectShow renderer end-of-stream) passes TIME_ONESHOT |
 * TIME_CALLBACK_FUNCTION and falls back to an immediate call on NULL. Running
 * a guest callback would need an asynchronous guest thread, so function
 * callbacks are refused; EVENT_SET/EVENT_PULSE signal the event handle. */
static void time_set_event(CPU*c){
 uint32_t delay=ARG(0),callback=ARG(2),flags=ARG(4),kind=flags&0x30u;
 if(kind!=0x10u&&kind!=0x20u){static int logged;if(!logged++)fprintf(stderr,"[time] timeSetEvent function callback %08x refused (caller %08x)\n",callback,R32(c->esp));ret(c,5,0);return;}
 if(!delay||delay>1000000u||(flags&~0x131u)){ret(c,5,0);return;}
 uint64_t now=monotonic_ns();uint32_t id=0;
 pthread_mutex_lock(&guard);Handle*h=lookup(callback);
 if(now&&h&&h->object->kind==2&&timer_count<TIMER_LIMIT){
  Mutex*m=h->object;++m->handles;id=next_timer_id++;if(!next_timer_id)next_timer_id=1;
  timers[timer_count++]=(Timer){id,m,now+(uint64_t)delay*1000000ull,(flags&1)?(uint64_t)delay*1000000ull:0,kind==0x20u};
 }
 pthread_mutex_unlock(&guard);ret(c,5,id);
}
static void time_kill_event(CPU*c){
 uint32_t id=ARG(0),result=11; /* MMSYSERR_INVALPARAM */
 pthread_mutex_lock(&guard);
 for(unsigned i=0;i<timer_count;i++)if(timers[i].id==id){Mutex*m=timers[i].event;timers[i]=timers[--timer_count];--m->handles;collect(m);result=0;break;}
 pthread_mutex_unlock(&guard);ret(c,1,result);
}
/* ---- File mappings ----
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfilemappinga
 * https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile
 * Pagefile-backed sections are zeroed private backend memory (<=32MiB). File
 * sections are read-only snapshots read through the installed file handlers
 * (D3DX loaders at B1B9C9 map whole files PAGE_READONLY); writable file
 * sections would need write-back and are unsupported. All views of one
 * section share one address range. */
static uint32_t host_call(CPU*c,const char*name,unsigned n,const uint32_t*args,int*ok){
 GuestFn fn=find_import_handler(name);*ok=0;
 if(!fn||c->esp<4096||(uint64_t)c->esp>MOVIES_SYNC_SPACE)return 0;
 CPU nested=*c;uint32_t top=c->esp-1024u;nested.esp=top-4*(n+1);
 W32(nested.esp,0);for(unsigned i=0;i<n;i++)W32(nested.esp+4+4*i,args[i]);
 fn(&nested);*ok=nested.esp==top;return nested.eax;
}
static uint32_t snapshot_file(CPU*c,uint32_t file,uint32_t wanted,uint32_t*base,uint32_t*size){
 uint32_t scratch=c->esp-64,saved=movies_get_last_error();int ok;
 if(c->esp<4096)return 50;
 W32(scratch,0);movies_set_last_error(0);uint32_t a1[]={file,scratch};uint32_t low=host_call(c,"KERNEL32.DLL!GetFileSize",2,a1,&ok);
 if(!ok)return 50;if(low==0xffffffffu&&movies_get_last_error())return movies_get_last_error();
 if(R32(scratch))return 8;
 uint32_t length=wanted?wanted:low;if(!length)return 1006; /* ERROR_FILE_INVALID */
 if(wanted>low)return 8;if(length>(32u<<20))return 8;
 uint32_t a2[]={file,0,0,1};uint32_t position=host_call(c,"KERNEL32.DLL!SetFilePointer",4,a2,&ok);
 if(!ok||position==0xffffffffu)return 50;
 uint32_t memory=movies_backend_alloc(length);if(!memory)return 8;
 uint32_t a3[]={file,0,0,0};host_call(c,"KERNEL32.DLL!SetFilePointer",4,a3,&ok);
 uint32_t done=0,failure=0;
 while(ok&&done<length){W32(scratch,0);uint32_t a4[]={file,memory+done,length-done,scratch,0};
  if(!host_call(c,"KERNEL32.DLL!ReadFile",5,a4,&ok)||!ok){failure=ok?movies_get_last_error():50;break;}
  uint32_t got=R32(scratch);if(!got)break;done+=got;}
 if(!ok&&!failure)failure=50;if(!failure&&done<length)failure=38; /* ERROR_HANDLE_EOF */
 uint32_t a5[]={file,position,0,0};host_call(c,"KERNEL32.DLL!SetFilePointer",4,a5,&ok);
 if(failure){movies_backend_free(memory);return failure;}
 movies_set_last_error(saved);*base=memory;*size=length;return 0;
}
static uint32_t read_object_name(uint32_t address,char name[261]){
 name[0]=0;if(!address)return 0;unsigned i;
 for(i=0;i<260;i++){if((uint64_t)address+i>=MOVIES_SYNC_SPACE)return 87;name[i]=(char)R8(address+i);if(!name[i])break;}
 if(i==260)return 206;
 if(!strncmp(name,"Local\\",6))memmove(name,name+6,strlen(name+6)+1);
 else if(!strncmp(name,"Global\\",7))memmove(name,name+7,strlen(name+7)+1);
 return strchr(name,'\\')?123:0;
}
static void create_file_mapping(CPU*c){
 uint32_t file=ARG(0),security=ARG(1),protect=ARG(2),high=ARG(3),low=ARG(4),address=ARG(5),page=protect&0xffu,failure=0;
 char name[261];
 if(security){movies_set_last_error(50);ret(c,6,0);return;}
 if((page!=2&&page!=4&&page!=8&&page!=0x20&&page!=0x40&&page!=0x80)||(protect&0x01000000u)||(protect&~0x1c0000ffu)){movies_set_last_error((protect&0x01000000u)?50:87);ret(c,6,0);return;}
 int writable=page==4||page==0x40;
 if((failure=read_object_name(address,name))){movies_set_last_error(failure);ret(c,6,0);return;}
 if(high){movies_set_last_error(8);ret(c,6,0);return;}
 /* Opening an existing named section ignores size and file. */
 pthread_mutex_lock(&guard);Mutex*m=NULL;
 if(name[0])for(m=objects;m;m=m->next)if(m->name&&!strcmp(m->name,name))break;
 if(m){uint32_t id=m->kind==5?new_handle_locked(m):0;pthread_mutex_unlock(&guard);movies_set_last_error(m->kind!=5?6:id?183:8);ret(c,6,id);return;}
 pthread_mutex_unlock(&guard);
 uint32_t base=0,size=0;
 if(file==0xffffffffu){
  if(!low)failure=87;else if(low>(32u<<20))failure=8;else if(!(base=movies_backend_alloc(low)))failure=8;else size=low;
 } else if(writable)failure=50;
 else failure=snapshot_file(c,file,low,&base,&size);
 if(failure){movies_set_last_error(failure);ret(c,6,0);return;}
 pthread_mutex_lock(&guard);uint32_t id=0;m=new_object_locked(5);
 if(m){m->map_base=base;m->map_size=size;m->map_protect=page;base=0;
  if(name[0]&&!(m->name=strdup(name)))collect(m);else if(!(id=new_handle_locked(m)))collect(m);}
 pthread_mutex_unlock(&guard);
 if(base)movies_backend_free(base);
 if(!id){movies_set_last_error(8);ret(c,6,0);return;}
 movies_set_last_error(0);ret(c,6,id);
}
static void map_view(CPU*c){
 uint32_t access=ARG(1),high=ARG(2),offset=ARG(3),bytes=ARG(4),failure=0,result=0;
 pthread_mutex_lock(&guard);Handle*h=lookup(ARG(0));Mutex*m=h?h->object:NULL;
 if(!m||m->kind!=5)failure=6;
 else if((access&2u)&&m->map_protect!=4&&m->map_protect!=0x40)failure=5;
 else if(high||offset>m->map_size)failure=87;
 else if(offset&0xffffu)failure=1132; /* ERROR_MAPPED_ALIGNMENT */
 else if(bytes>m->map_size-offset)failure=8;
 else if(view_count==VIEW_LIMIT)failure=8;
 else{result=m->map_base+offset;views[view_count++]=(View){result,m};++m->views;}
 pthread_mutex_unlock(&guard);
 if(failure)movies_set_last_error(failure);ret(c,5,result);
}
static void unmap_view(CPU*c){
 uint32_t address=ARG(0);int ok=0;
 pthread_mutex_lock(&guard);
 for(unsigned i=0;i<view_count;i++)if(views[i].address==address){Mutex*m=views[i].object;views[i]=views[--view_count];--m->views;collect(m);ok=1;break;}
 pthread_mutex_unlock(&guard);
 if(!ok)movies_set_last_error(487);ret(c,1,ok);
}
ImportHandler *movies_sync_handlers(uint32_t *count) {
    static ImportHandler h[]={
        {"KERNEL32.DLL!CreateSemaphoreA",create_semaphore},{"KERNEL32.DLL!ReleaseSemaphore",release_semaphore},{"KERNEL32.DLL!SetThreadPriority",set_thread_priority},{"KERNEL32.DLL!GetThreadPriority",get_thread_priority},
        {"KERNEL32.DLL!CreateEventA",create_event},{"KERNEL32.DLL!SetEvent",set_event},{"KERNEL32.DLL!ResetEvent",reset_event},
        {"KERNEL32.DLL!GetExitCodeThread",get_exit_code},{"KERNEL32.DLL!WaitForMultipleObjects",wait_multiple},
        {"KERNEL32.DLL!Sleep",sleep_thread},
        {"KERNEL32.DLL!CreateMutexA",create_mutex},
        {"KERNEL32.DLL!ReleaseMutex",release_mutex},
        {"KERNEL32.DLL!WaitForSingleObject",wait_single},
        {"KERNEL32.DLL!WaitForSingleObjectEx",wait_single_ex},
        {"KERNEL32.DLL!GetCurrentThread",current_thread_handle},{"KERNEL32.DLL!DuplicateHandle",duplicate_handle},
        {"KERNEL32.DLL!SetHandleInformation",set_handle_information},{"KERNEL32.DLL!GetThreadContext",get_thread_context},
        {"KERNEL32.DLL!GetExitCodeProcess",get_exit_code_process},
        {"KERNEL32.DLL!CreateFileMappingA",create_file_mapping},{"KERNEL32.DLL!MapViewOfFile",map_view},
        {"KERNEL32.DLL!UnmapViewOfFile",unmap_view},
        {"WINMM.DLL!timeSetEvent",time_set_event},{"WINMM.DLL!timeKillEvent",time_kill_event}
    };
    *count=sizeof h/sizeof h[0];return h;
}
