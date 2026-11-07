#include "sync-api.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
uint8_t *g_mem;
static _Thread_local uint32_t error,stack=0x100;
void movies_set_last_error(uint32_t e) { error=e; }
void guest_trap(CPU *c,uint32_t at,const char *why) { (void)c;(void)at;fprintf(stderr,"%s\n",why);abort(); }
static uint32_t call(const char *name,unsigned n,uint32_t a,uint32_t b,uint32_t d) {
    CPU c={0};c.esp=stack;c.esi=0x12345678;
    W32(stack,0xdeadbeef);W32(stack+4,a);W32(stack+8,b);W32(stack+12,d);
    uint32_t count;ImportHandler *h=movies_sync_handlers(&count);
    for(unsigned i=0;i<count;i++) if(!strcmp(h[i].name+13,name)) {
        h[i].fn(&c);assert(c.esp==stack+4*(n+1) && c.esi==0x12345678);return c.eax;
    }
    assert(0);return 0;
}
static uint32_t event_create(uint32_t manual,uint32_t initial,uint32_t name){
 CPU c={0};c.esp=stack;W32(stack,0x1234);W32(stack+4,0);W32(stack+8,manual);W32(stack+12,initial);W32(stack+16,name);
 uint32_t n;ImportHandler*h=movies_sync_handlers(&n);for(unsigned i=0;i<n;i++)if(!strcmp(h[i].name+13,"CreateEventA")){h[i].fn(&c);assert(c.esp==stack+20);return c.eax;}abort();
}
static uint32_t event_target;
static void *event_signal(void *unused){(void)unused;stack=0x300;struct timespec d={0,10000000};nanosleep(&d,NULL);assert(call("SetEvent",1,event_target,0,0));return NULL;}
static void event_tests(void){
 strcpy((char*)HP(0x1400),"shared-event");uint32_t manual=event_create(1,0,0x1400);assert(manual&&error==0);uint32_t same=event_create(0,1,0x1400);assert(same&&error==183);assert(call("WaitForSingleObject",2,manual,0,0)==258); /* existing state ignored */
 assert(!call("CreateMutexA",3,0,0,0x1400)&&error==6);assert(!call("ReleaseMutex",1,manual,0,0)&&error==6);
 event_target=manual;pthread_t t;assert(!pthread_create(&t,NULL,event_signal,NULL));assert(!call("WaitForSingleObject",2,manual,2000,0));assert(!pthread_join(t,NULL));assert(!call("WaitForSingleObject",2,same,0,0));assert(call("ResetEvent",1,same,0,0));assert(call("WaitForSingleObject",2,manual,0,0)==258);
 assert(movies_sync_close_handle(manual));assert(call("SetEvent",1,same,0,0));assert(!call("WaitForSingleObject",2,same,0,0));assert(movies_sync_close_handle(same));assert(!call("SetEvent",1,same,0,0)&&error==6);
 uint32_t automatic=event_create(0,1,0);assert(automatic);assert(!call("WaitForSingleObject",2,automatic,0,0));assert(call("WaitForSingleObject",2,automatic,0,0)==258);assert(call("SetEvent",1,automatic,0,0));assert(call("SetEvent",1,automatic,0,0));assert(!call("WaitForSingleObject",2,automatic,0,0));assert(call("WaitForSingleObject",2,automatic,10,0)==258);assert(movies_sync_close_handle(automatic));
 uint32_t mutex=call("CreateMutexA",3,0,0,0x1400);assert(mutex);assert(!event_create(0,0,0x1400)&&error==6);assert(!call("ResetEvent",1,mutex,0,0)&&error==6);assert(movies_sync_close_handle(mutex));
}
static void *sleep_worker(void *unused) {
    (void)unused;stack=0x200;
    struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
    error=456;call("Sleep",1,20,0,0);assert(error==456);
    clock_gettime(CLOCK_MONOTONIC,&end);
    assert((end.tv_sec-start.tv_sec)*1000000000ll+end.tv_nsec-start.tv_nsec>=15000000ll);
    return NULL;
}
static uint32_t target;
static void *contend(void *unused) {
    (void)unused;stack=0x200;
    assert(!call("ReleaseMutex",1,target,0,0) && error==288);
    assert(call("WaitForSingleObject",2,target,0,0)==258);
    struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);
    assert(call("WaitForSingleObject",2,target,25,0)==258);
    clock_gettime(CLOCK_MONOTONIC,&b);
    assert((b.tv_sec-a.tv_sec)*1000000000ll+b.tv_nsec-a.tv_nsec>=20000000ll);
    return NULL;
}
static void *acquire_abandon(void *unused) {
    (void)unused;stack=0x200;
    assert(call("WaitForSingleObject",2,target,0xffffffffu,0)==0);
    return NULL; /* key destructor marks abandoned */
}
static void *acquire_release(void *unused) {
    (void)unused;stack=0x200;
    assert(call("WaitForSingleObjectEx",3,target,1000,0)==0);
    assert(call("ReleaseMutex",1,target,0,0));return NULL;
}
int main(void) {
    g_mem=calloc(1,0x10000);assert(g_mem);movies_sync_init();
#ifdef MOVIES_EVENT_POLL_ONLY
    uint32_t manual=event_create(1,0,0),signaled=event_create(1,1,0),automatic=event_create(0,0,0);assert(manual&&signaled&&automatic);
    assert(call("WaitForSingleObject",2,manual,0,0)==258);
    assert(call("WaitForSingleObject",2,manual,0xffffffffu,0)==0xffffffffu&&error==50);
    assert(call("WaitForSingleObject",2,automatic,10,0)==0xffffffffu&&error==50);
    assert(!call("WaitForSingleObject",2,signaled,0xffffffffu,0));assert(!call("WaitForSingleObject",2,signaled,0,0));
    assert(call("SetEvent",1,automatic,0,0));assert(!call("WaitForSingleObject",2,automatic,0,0));assert(call("WaitForSingleObject",2,automatic,0,0)==258);
    assert(movies_sync_close_handle(manual)&&movies_sync_close_handle(signaled)&&movies_sync_close_handle(automatic));free(g_mem);puts("nonpthread event polling/state and explicit unsupported blocking PASS");return 0;
#endif
    struct timespec start,end;
    event_tests();
    error=123;call("Sleep",1,0,0,0);assert(error==123);
    clock_gettime(CLOCK_MONOTONIC,&start);call("Sleep",1,20,0,0);
    clock_gettime(CLOCK_MONOTONIC,&end);
    assert(error==123);
    assert((end.tv_sec-start.tv_sec)*1000000000ll+end.tv_nsec-start.tv_nsec>=15000000ll);
    strcpy((char *)HP(0x1000),"Example");strcpy((char *)HP(0x1100),"example");
    uint32_t one=call("CreateMutexA",3,0,1,0x1000);assert(one && error==0);
    uint32_t two=call("CreateMutexA",3,0,1,0x1000);assert(two && two!=one && error==183);
    uint32_t lower=call("CreateMutexA",3,0,0,0x1100);assert(lower && error==0);
    strcpy((char *)HP(0x1300),"Local\\Example");
    uint32_t local=call("CreateMutexA",3,0,0,0x1300);assert(local && error==183);
    assert(movies_sync_close_handle(local));
    strcpy((char *)HP(0x1300),"Global\\Example");
    uint32_t global=call("CreateMutexA",3,0,0,0x1300);assert(global && error==0);
    assert(movies_sync_close_handle(global));
    target=two;pthread_t thread;assert(!pthread_create(&thread,NULL,sleep_worker,NULL));assert(!pthread_join(thread,NULL));
    assert(!pthread_create(&thread,NULL,contend,NULL));assert(!pthread_join(thread,NULL));
    /* Existing named create ignored initial ownership: only one release needed. */
    assert(call("ReleaseMutex",1,two,0,0));
    assert(!call("ReleaseMutex",1,one,0,0) && error==288);
    assert(call("WaitForSingleObject",2,one,0,0)==0);
    assert(call("WaitForSingleObject",2,two,0,0)==0);
    assert(call("ReleaseMutex",1,one,0,0));
    assert(!pthread_create(&thread,NULL,contend,NULL));assert(!pthread_join(thread,NULL));
    assert(call("ReleaseMutex",1,two,0,0));
    assert(movies_sync_close_handle(one));assert(!movies_sync_close_handle(one));
    assert(call("WaitForSingleObject",2,one,0,0)==0xffffffffu && error==6);
    assert(!pthread_create(&thread,NULL,acquire_abandon,NULL));assert(!pthread_join(thread,NULL));
    assert(call("WaitForSingleObject",2,two,0,0)==128);
    assert(call("WaitForSingleObject",2,two,0,0)==0);
    assert(call("ReleaseMutex",1,two,0,0));assert(call("ReleaseMutex",1,two,0,0));
    assert(call("WaitForSingleObject",2,two,0,0)==0);
    assert(!pthread_create(&thread,NULL,acquire_release,NULL));
    struct timespec delay={0,30000000};nanosleep(&delay,NULL);
    assert(call("ReleaseMutex",1,two,0,0));assert(!pthread_join(thread,NULL));
    assert(call("WaitForSingleObjectEx",3,two,0,1)==0xffffffffu && error==50);
    assert(!call("CreateMutexA",3,123,0,0) && error==50);
    assert(!call("CreateMutexA",3,0,0,0xfffffff0) && error==87);
    memset(HP(0x1200),'a',260);assert(!call("CreateMutexA",3,0,0,0x1200) && error==206);
    strcpy((char *)HP(0x1200),"wrong\\namespace");assert(!call("CreateMutexA",3,0,0,0x1200) && error==123);
    assert(movies_sync_close_handle(two));assert(movies_sync_close_handle(lower));
    uint32_t fresh=call("CreateMutexA",3,0,0,0x1000);assert(fresh && error==0);
    uint32_t u1=call("CreateMutexA",3,0,1,0),u2=call("CreateMutexA",3,0,0,0);assert(u1!=u2);
    assert(!call("ReleaseMutex",1,u2,0,0) && error==288);
    movies_sync_init();assert(!movies_sync_close_handle(fresh));
    assert(!call("ReleaseMutex",1,u1,0,0) && error==6);
    free(g_mem);puts("sync mutex ownership, recursion, timeout, abandonment and handle tests passed");
}
