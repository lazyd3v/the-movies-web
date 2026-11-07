#define _POSIX_C_SOURCE 200809L
#include "guest-thread-api.h"
#include "thread-api.h"
#include "sync-api.h"
#include "startup-api.h"
#include "heap-api.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
#ifndef MOVIES_THREAD_SPACE
#define MOVIES_THREAD_SPACE GUEST_SPACE
#endif
#define DEFAULT_STACK (1024u*1024u)
#define MAX_STACK (4u*1024u*1024u)
#define ARG(n) R32(c->esp+4+4*(n))
typedef struct Execution {unsigned slot;CPU cpu;GuestFn start;uint32_t parameter,stack,tib,id,exit_code;void *completion;jmp_buf exit_boundary;} Execution;
static _Thread_local Execution *current;
static pthread_mutex_t lifecycle=PTHREAD_MUTEX_INITIALIZER;
typedef struct HostWorker {pthread_t thread;Execution *job;int busy;} HostWorker;
static HostWorker workers[32];static pthread_cond_t jobs=PTHREAD_COND_INITIALIZER;
static unsigned capacity,active;static int stopping;static uint32_t failure_ready;static char first_failure[256];
static void *pool_loop(void *argument);
int movies_guest_threads_init(unsigned limit){
 if(!limit||limit>32)return 0;pthread_mutex_lock(&lifecycle);if(capacity||active){pthread_mutex_unlock(&lifecycle);return 0;}stopping=0;first_failure[0]=0;__atomic_store_n(&failure_ready,0,__ATOMIC_RELEASE);pthread_mutex_unlock(&lifecycle);
 unsigned made=0;for(;made<limit;made++)if(pthread_create(&workers[made].thread,NULL,pool_loop,(void*)(uintptr_t)made))break;
 if(made!=limit){pthread_mutex_lock(&lifecycle);stopping=1;pthread_cond_broadcast(&jobs);pthread_mutex_unlock(&lifecycle);for(unsigned i=0;i<made;i++)pthread_join(workers[i].thread,NULL);return 0;}
 pthread_mutex_lock(&lifecycle);capacity=limit;pthread_mutex_unlock(&lifecycle);return 1;
}
int movies_guest_threads_shutdown(void){pthread_mutex_lock(&lifecycle);if(active){pthread_mutex_unlock(&lifecycle);return 0;}unsigned count=capacity;stopping=1;pthread_cond_broadcast(&jobs);pthread_mutex_unlock(&lifecycle);for(unsigned i=0;i<count;i++)pthread_join(workers[i].thread,NULL);pthread_mutex_lock(&lifecycle);capacity=0;memset(workers,0,sizeof workers);pthread_mutex_unlock(&lifecycle);return 1;}
int movies_guest_thread_is_child(void){return current!=NULL;}
int movies_guest_threads_failed(char*out,uint32_t size){if(!__atomic_load_n(&failure_ready,__ATOMIC_ACQUIRE)){if(out&&size)out[0]=0;return 0;}if(!out||!size)return 1;pthread_mutex_lock(&lifecycle);if(out&&size)snprintf(out,size,"%s",first_failure);pthread_mutex_unlock(&lifecycle);return 1;}
void movies_guest_threads_failure_view(MoviesGuestFailureView*out){*out=(MoviesGuestFailureView){&failure_ready,first_failure,sizeof first_failure};}
static void drop_slot(unsigned slot){pthread_mutex_lock(&lifecycle);workers[slot].busy=0;--active;pthread_mutex_unlock(&lifecycle);}

static void ret(CPU*c,unsigned n,uint32_t v){c->eax=v;c->esp+=4+4*n;}
static void *execute(void*arg){
 Execution*e=arg;current=e;movies_thread_bind_id(e->id);movies_set_last_error(0);g_import_target=0;g_trace=NULL;g_trace_len=g_trace_cap=0;fpu_sync_rounding(&e->cpu);
 jmp_buf trap;g_trap_jmp=&trap;
 if(setjmp(trap)){fprintf(stderr,"guest child %u failed: %s\n",e->id,g_trap_msg);pthread_mutex_lock(&lifecycle);if(!first_failure[0])snprintf(first_failure,sizeof first_failure,"thread %u: %.220s",e->id,g_trap_msg);__atomic_store_n(&failure_ready,1,__ATOMIC_RELEASE);pthread_mutex_unlock(&lifecycle);e->exit_code=0xc0000005u;}
 else if(!setjmp(e->exit_boundary)){
  fprintf(stderr,"[thread] start id=%u parameter=%08x\n",e->id,e->parameter);
  uint32_t before=e->cpu.esp;guest_call(&e->cpu,e->start,1,&e->parameter);
  if(e->cpu.esp!=before)guest_trap(&e->cpu,0,"guest thread start violated stdcall");
  e->exit_code=e->cpu.eax;
 }
 g_trap_jmp=NULL;current=NULL;
 movies_backend_free(e->stack);movies_backend_free(e->tib);
 movies_sync_abandon_current();
 drop_slot(e->slot);movies_sync_complete_thread(e->completion,e->exit_code);free(e);return NULL;
}
static void *pool_loop(void*argument){unsigned slot=(unsigned)(uintptr_t)argument;for(;;){pthread_mutex_lock(&lifecycle);while(!workers[slot].job&&!stopping)pthread_cond_wait(&jobs,&lifecycle);if(stopping){pthread_mutex_unlock(&lifecycle);return NULL;}Execution*job=workers[slot].job;workers[slot].job=NULL;pthread_mutex_unlock(&lifecycle);execute(job);}}
static void create_thread(CPU*c){
 uint32_t security=ARG(0),stack_size=ARG(1),entry=ARG(2),parameter=ARG(3),flags=ARG(4),id_out=ARG(5),failure=0;
 if(security||flags){movies_set_last_error(50);ret(c,6,0);return;}
 if(id_out&&(uint64_t)id_out+4>MOVIES_THREAD_SPACE){movies_set_last_error(87);ret(c,6,0);return;}
 GuestFn start=lookup_function(entry);if(!start){movies_set_last_error(487);ret(c,6,0);return;}
 if(!stack_size)stack_size=DEFAULT_STACK;if(stack_size<65536)stack_size=65536;if(stack_size>MAX_STACK){movies_set_last_error(8);ret(c,6,0);return;}stack_size=(stack_size+4095)&~4095u;
 pthread_mutex_lock(&lifecycle);unsigned slot=capacity;for(unsigned i=0;i<capacity;i++)if(!workers[i].busy){slot=i;workers[i].busy=1;active++;break;}pthread_mutex_unlock(&lifecycle);if(slot==capacity){movies_set_last_error(8);ret(c,6,0);return;}
 Execution*e=calloc(1,sizeof*e);uint32_t handle=0;
 if(!e)failure=8;
 if(e){e->slot=slot;e->stack=movies_backend_alloc(stack_size);e->tib=movies_backend_alloc(4096);if(!e->stack||!e->tib)failure=8;}
 if(!failure){e->start=start;e->parameter=parameter;e->id=movies_thread_reserve_id();if(!e->id)failure=8;}
 if(!failure){e->cpu.esp=e->stack+stack_size;e->cpu.fs_base=e->tib;e->cpu.fpu_cw=0x027f;e->cpu.mxcsr=0x1f80;
  W32(e->tib,0xffffffffu);W32(e->tib+4,e->cpu.esp);W32(e->tib+8,e->stack);W32(e->tib+0x18,e->tib);W32(e->tib+0x20,1);W32(e->tib+0x24,e->id);
  handle=movies_sync_create_thread_object(&e->completion);if(!handle)failure=8;
 }
 if(!failure){if(id_out)W32(id_out,e->id);fprintf(stderr,"[thread] create id=%u entry=%08x handle=%08x stack=%u\n",e->id,entry,handle,stack_size);pthread_mutex_lock(&lifecycle);workers[slot].job=e;pthread_cond_broadcast(&jobs);pthread_mutex_unlock(&lifecycle);}
 if(failure){drop_slot(slot);if(e){if(handle){movies_sync_complete_thread(e->completion,0);movies_sync_close_handle(handle);}if(e->stack)movies_backend_free(e->stack);if(e->tib)movies_backend_free(e->tib);free(e);}movies_set_last_error(failure);handle=0;}
 ret(c,6,handle);
}
static void exit_thread(CPU*c){if(!current){guest_trap(c,R32(c->esp),"ExitThread on initial thread unsupported; process lifetime not modeled");return;}current->exit_code=ARG(0);longjmp(current->exit_boundary,1);}
ImportHandler*movies_guest_thread_handlers(uint32_t*count){static ImportHandler h[]={{"KERNEL32.DLL!CreateThread",create_thread},{"KERNEL32.DLL!ExitThread",exit_thread}};*count=sizeof h/sizeof*h;return h;}
