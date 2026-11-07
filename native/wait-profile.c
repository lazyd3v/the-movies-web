#define _POSIX_C_SOURCE 200809L
#include "wait-profile.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
static int enabled;
static pthread_t owner;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static MoviesWaitProfileReport state;
static MoviesWaitProfileSink output;
static uint64_t boundary;
static uint64_t now_ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static void log_report(const MoviesWaitProfileReport*r){
 fprintf(stderr,"[wait-profile] {\"frames\":%u,\"lastSerial\":%u,\"started\":%u,\"windowMs\":%.3f,\"completedIntervals\":[",r->frames,r->last_serial,r->started,r->window_ns/1e6);
 int comma=0;for(unsigned group=0;group<2;group++)for(unsigned i=0;i<MOVIES_WAIT_PROFILE_SLOTS;i++){const MoviesWaitProfileEntry*e=&r->entries[group][i];if(!e->count)continue;
 fprintf(stderr,"%s{\"thread\":\"%s\",\"kind\":%u,\"caller\":\"%08x\",\"timeout\":%u,\"hostStatus\":%u,\"count\":%llu,\"elapsedMs\":%.3f,\"maxMs\":%.3f}",comma?",":"",group?"child":"main",e->kind,e->caller,e->timeout,e->status,(unsigned long long)e->count,e->ns/1e6,e->max_ns/1e6);comma=1;}
 fprintf(stderr,"]}\n");
}
void movies_wait_profile_init(int active,MoviesWaitProfileSink sink){
 /* Bootstrap calls this before spawning any guest threads. */
 pthread_mutex_lock(&mutex);memset(&state,0,sizeof state);owner=pthread_self();boundary=now_ns();output=sink?sink:log_report;__atomic_store_n(&enabled,!!active,__ATOMIC_RELEASE);pthread_mutex_unlock(&mutex);
}
int movies_wait_profile_active(void){return __atomic_load_n(&enabled,__ATOMIC_ACQUIRE);}
uint64_t movies_wait_profile_begin(void){return __atomic_load_n(&enabled,__ATOMIC_ACQUIRE)?now_ns():0;}
void movies_wait_profile_interval(uint32_t kind,uint32_t caller,uint32_t timeout,uint32_t status,uint64_t begin,uint64_t end){
 if(!begin||end<begin||!__atomic_load_n(&enabled,__ATOMIC_ACQUIRE))return;
 unsigned group=pthread_equal(pthread_self(),owner)?0:1;pthread_mutex_lock(&mutex);
 MoviesWaitProfileEntry*rows=state.entries[group],*e=&rows[MOVIES_WAIT_PROFILE_SLOTS-1];
 for(unsigned i=0;i<MOVIES_WAIT_PROFILE_SLOTS-1;i++)if(!rows[i].count||(rows[i].kind==kind&&rows[i].caller==caller&&rows[i].timeout==timeout&&rows[i].status==status)){e=&rows[i];if(!e->count){e->kind=kind;e->caller=caller;e->timeout=timeout;e->status=status;}break;}
 if(e==&rows[MOVIES_WAIT_PROFILE_SLOTS-1])e->kind=0; /* explicit overflow aggregate */
 e->count++;e->ns+=end-begin;if(end-begin>e->max_ns)e->max_ns=end-begin;pthread_mutex_unlock(&mutex);
}
void movies_wait_profile_record(uint32_t kind,uint32_t caller,uint32_t timeout,uint32_t status,uint64_t begin){if(begin)movies_wait_profile_interval(kind,caller,timeout,status,begin,now_ns());}
static void report(int present){
 if(!__atomic_load_n(&enabled,__ATOMIC_ACQUIRE)||!pthread_equal(pthread_self(),owner))return;
 MoviesWaitProfileReport copy;int emit=0;pthread_mutex_lock(&mutex);uint64_t now=now_ns();
 if(present){state.last_serial++;if(!state.started){state.started=1;memset(state.entries,0,sizeof state.entries);boundary=now;pthread_mutex_unlock(&mutex);return;}state.frames++;}
 int nonempty=state.frames!=0;for(unsigned g=0;g<2;g++)for(unsigned i=0;i<MOVIES_WAIT_PROFILE_SLOTS;i++)nonempty|=state.entries[g][i].count!=0;
 if((!present&&nonempty)||state.frames==120){copy=state;copy.window_ns=now>=boundary?now-boundary:0;memset(state.entries,0,sizeof state.entries);state.frames=0;boundary=now;emit=1;}
 pthread_mutex_unlock(&mutex);if(emit)output(&copy);
}
void movies_wait_profile_present(void){report(1);}
void movies_wait_profile_flush(void){report(0);}
