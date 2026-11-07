/* Win32 TLS/critical-section subset. Official contracts:
 * https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsalloc
 * https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue
 * https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-entercriticalsection
 * No exception dispatch, deadlock timeout, scheduler, or thread creation implied.
 */
#define _POSIX_C_SOURCE 200809L
#include "thread-api.h"
#include "startup-api.h"
#include <pthread.h>
#include <stdlib.h>
#define SLOT_COUNT 1088
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER;
static uint64_t generations[SLOT_COUNT];
static unsigned char allocated[SLOT_COUNT];
static _Thread_local uint64_t local_generations[SLOT_COUNT];
static _Thread_local uint32_t values[SLOT_COUNT], thread_id;
static uint32_t next_thread_id=0x100;
typedef struct Section {
    uint32_t address, refs; /* refs includes waiting and recursive owners */
    pthread_mutex_t mutex;
    struct Section *next;
} Section;
static Section *sections;
#define ARG(n) R32(c->esp+4+4*(n))
static void ret(CPU *c,unsigned n,uint32_t value) { c->eax=value;c->esp+=4*(n+1); }
static void fail(CPU *c,const char *s) { guest_trap(c,R32(c->esp),s); }
static void tls_alloc(CPU *c) {
    uint32_t index=0xffffffffu;
    pthread_mutex_lock(&registry);
    for(uint32_t i=0;i<SLOT_COUNT;i++)if(!allocated[i]) {
        allocated[i]=1; ++generations[i]; index=i;break;
    }
    pthread_mutex_unlock(&registry);
    if(index==0xffffffffu)movies_set_last_error(8);
    ret(c,0,index);
}
static void tls_free(CPU *c) {
    uint32_t i=ARG(0),ok=0;
    pthread_mutex_lock(&registry);
    if(i<SLOT_COUNT && allocated[i]) { allocated[i]=0;ok=1; }
    pthread_mutex_unlock(&registry);
    if(!ok)movies_set_last_error(87); ret(c,1,ok);
}
static void tls_get(CPU *c) {
    uint32_t i=ARG(0),value=0,ok=0;
    pthread_mutex_lock(&registry);
    if(i<SLOT_COUNT && allocated[i]) {
        ok=1;
        if(local_generations[i]==generations[i])value=values[i];
    }
    pthread_mutex_unlock(&registry);
    movies_set_last_error(ok?0:87); ret(c,1,value);
}
static void tls_set(CPU *c) {
    uint32_t i=ARG(0),ok=0;
    pthread_mutex_lock(&registry);
    if(i<SLOT_COUNT && allocated[i]) {
        local_generations[i]=generations[i];values[i]=ARG(1);ok=1;
    }
    pthread_mutex_unlock(&registry);
    if(!ok)movies_set_last_error(87);ret(c,2,ok);
}
uint32_t movies_thread_reserve_id(void){pthread_mutex_lock(&registry);uint32_t id=next_thread_id==0xffffffffu?0:next_thread_id++;pthread_mutex_unlock(&registry);return id;}
void movies_thread_bind_id(uint32_t id){memset(local_generations,0,sizeof local_generations);memset(values,0,sizeof values);thread_id=id;}
static void current_thread(CPU *c) {
    pthread_mutex_lock(&registry);
    if(!thread_id)thread_id=next_thread_id++;
    uint32_t id=thread_id;
    pthread_mutex_unlock(&registry);ret(c,0,id);
}
static Section *find(uint32_t address) {
    for(Section *s=sections;s;s=s->next)if(s->address==address)return s;
    return NULL;
}
static void initialize(CPU *c) {
    uint32_t address=ARG(0);
    if(!address || (uint64_t)address+24>GUEST_SPACE) { fail(c,"invalid critical section address");return; }
    Section *s=calloc(1,sizeof *s);
    if(!s) { fail(c,"critical section OOM: guest SEH unavailable");return; }
    pthread_mutexattr_t attr;
    int error=pthread_mutexattr_init(&attr);
    if(!error) {
        error=pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
        if(!error)error=pthread_mutex_init(&s->mutex,&attr);
        pthread_mutexattr_destroy(&attr);
    }
    if(error) { free(s);fail(c,"critical section mutex initialization failed");return; }
    pthread_mutex_lock(&registry);
    if(find(address)) {
        pthread_mutex_unlock(&registry);pthread_mutex_destroy(&s->mutex);free(s);
        fail(c,"critical section already initialized");return;
    }
    s->address=address;s->next=sections;sections=s;
    memset(HP(address),0,24); W32(address+4,0xffffffffu);
    pthread_mutex_unlock(&registry);ret(c,1,0);
}
static void enter(CPU *c) {
    pthread_mutex_lock(&registry);Section *s=find(ARG(0));
    if(s)s->refs++;
    pthread_mutex_unlock(&registry);
    if(!s) { fail(c,"uninitialized critical section");return; }
    int error=pthread_mutex_lock(&s->mutex);
    if(error) {
        pthread_mutex_lock(&registry);s->refs--;pthread_mutex_unlock(&registry);
        fail(c,"critical section lock failed");return;
    }
    ret(c,1,0);
}
static void leave(CPU *c) {
    pthread_mutex_lock(&registry);Section *s=find(ARG(0));
    int error=(!s || !s->refs)?1:pthread_mutex_unlock(&s->mutex);
    if(!error)s->refs--;
    pthread_mutex_unlock(&registry);
    if(error) { fail(c,"critical section leave without ownership");return; }
    ret(c,1,0);
}
static void delete_section(CPU *c) {
    pthread_mutex_lock(&registry);Section **p=&sections;
    while(*p && (*p)->address!=ARG(0))p=&(*p)->next;
    Section *s=*p;
    if(!s || s->refs) {
        pthread_mutex_unlock(&registry);fail(c,"deleting unknown or active critical section");return;
    }
    int error=pthread_mutex_destroy(&s->mutex);
    if(!error) { *p=s->next;free(s); }
    pthread_mutex_unlock(&registry);
    if(error) { fail(c,"critical section destruction failed");return; }
    ret(c,1,0);
}
ImportHandler *movies_thread_handlers(uint32_t *count) {
    static ImportHandler handlers[]={
        {"KERNEL32.DLL!TlsAlloc",tls_alloc},{"KERNEL32.DLL!TlsFree",tls_free},
        {"KERNEL32.DLL!TlsGetValue",tls_get},{"KERNEL32.DLL!TlsSetValue",tls_set},
        {"KERNEL32.DLL!InitializeCriticalSection",initialize},
        {"KERNEL32.DLL!EnterCriticalSection",enter},
        {"KERNEL32.DLL!LeaveCriticalSection",leave},
        {"KERNEL32.DLL!DeleteCriticalSection",delete_section},
        {"KERNEL32.DLL!GetCurrentThreadId",current_thread}};
    *count=sizeof handlers/sizeof handlers[0];return handlers;
}
