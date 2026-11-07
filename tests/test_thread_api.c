#include "thread-api.h"
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
uint8_t *g_mem;
static _Thread_local uint32_t error,stack=0x100;
static jmp_buf trap;
void movies_set_last_error(uint32_t e) { error=e; }
void guest_trap(CPU *c,uint32_t address,const char *message) {
    (void)c;(void)address;(void)message;longjmp(trap,1);
}
static uint32_t call(const char *name,uint32_t a,uint32_t b) {
    uint32_t n;ImportHandler *h=movies_thread_handlers(&n);CPU c={0};c.esp=stack;
    W32(stack,0xdeadbeef);W32(stack+4,a);W32(stack+8,b);
    for(uint32_t i=0;i<n;i++)if(!strcmp(h[i].name+13,name)) {
        h[i].fn(&c);
        unsigned argc=(!strcmp(name,"TlsAlloc") || !strcmp(name,"GetCurrentThreadId"))?0:
            (!strcmp(name,"TlsSetValue")?2:1);
        assert(c.esp==stack+4*(argc+1));return c.eax;
    }
    assert(0);return 0;
}
static pthread_mutex_t coordination=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static unsigned phase;
static uint32_t index_value,worker_id;
static unsigned counter;
static void *worker(void *unused) {
    (void)unused;stack=0x200;
    worker_id=call("GetCurrentThreadId",0,0);
    assert(worker_id==call("GetCurrentThreadId",0,0));
    error=123;assert(call("TlsGetValue",index_value,0)==0 && error==0);
    assert(call("TlsSetValue",index_value,456));
    pthread_mutex_lock(&coordination);phase=1;pthread_cond_signal(&changed);
    while(phase!=2)pthread_cond_wait(&changed,&coordination);
    pthread_mutex_unlock(&coordination);
    assert(call("TlsGetValue",index_value,0)==0); /* reallocated index clears all threads */
    for(unsigned i=0;i<20000;i++) {
        call("EnterCriticalSection",0x1000,0);call("EnterCriticalSection",0x1000,0);
        counter++;call("LeaveCriticalSection",0x1000,0);call("LeaveCriticalSection",0x1000,0);
    }
    return NULL;
}
int main(void) {
    g_mem=calloc(1,0x10000);assert(g_mem);
    index_value=call("TlsAlloc",0,0);assert(index_value!=0xffffffffu);
    assert(call("TlsSetValue",index_value,123));
    call("InitializeCriticalSection",0x1000,0);
    uint32_t id=call("GetCurrentThreadId",0,0);
    pthread_t thread;assert(!pthread_create(&thread,NULL,worker,NULL));
    pthread_mutex_lock(&coordination);
    while(phase!=1)pthread_cond_wait(&changed,&coordination);
    assert(call("TlsGetValue",index_value,0)==123);
    assert(call("TlsFree",index_value,0));assert(call("TlsAlloc",0,0)==index_value);
    assert(call("TlsGetValue",index_value,0)==0);
    phase=2;pthread_cond_signal(&changed);pthread_mutex_unlock(&coordination);
    for(unsigned i=0;i<20000;i++) {
        call("EnterCriticalSection",0x1000,0);counter++;call("LeaveCriticalSection",0x1000,0);
    }
    assert(!pthread_join(thread,NULL));assert(counter==40000 && worker_id!=id);
    call("DeleteCriticalSection",0x1000,0);
    call("InitializeCriticalSection",0x1000,0); /* address reuse */
    call("EnterCriticalSection",0x1000,0);
    if(!setjmp(trap)) { call("DeleteCriticalSection",0x1000,0);assert(0); }
    call("LeaveCriticalSection",0x1000,0);
    if(!setjmp(trap)) { call("LeaveCriticalSection",0x1000,0);assert(0); }
    call("DeleteCriticalSection",0x1000,0);
    assert(!call("TlsFree",0xffffffffu,0) && error==87);
    assert(!call("TlsGetValue",0xffffffffu,0) && error==87);
    assert(!call("TlsSetValue",0xffffffffu,1) && error==87);
    assert(call("TlsFree",index_value,0));
    uint32_t indices[1088];
    for(unsigned i=0;i<1088;i++) { indices[i]=call("TlsAlloc",0,0);assert(indices[i]!=0xffffffffu); }
    assert(call("TlsAlloc",0,0)==0xffffffffu && error==8);
    for(unsigned i=0;i<1088;i++)assert(call("TlsFree",indices[i],0));
    free(g_mem);puts("thread API tests passed");
}
