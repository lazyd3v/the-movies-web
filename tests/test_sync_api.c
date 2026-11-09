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

/* ---- Fixtures for sections, pseudo handles and event timers. */
static uint32_t heap_next=0x9000,heap_live[32];static unsigned heap_count;
uint32_t movies_backend_alloc(uint32_t n){if(heap_next+n>0xf000||heap_count==32)return 0;uint32_t a=heap_next;heap_next+=(n+7)&~7u;memset(HP(a),0,n);heap_live[heap_count++]=a;return a;}
int movies_backend_free(uint32_t a){for(unsigned i=0;i<heap_count;i++)if(heap_live[i]==a){heap_live[i]=heap_live[--heap_count];return 1;}return 0;}
uint32_t movies_get_last_error(void){return error;}
static uint32_t file_position;static const char file_bytes[]="0123456789";
#define ARGN(c,n) R32((c)->esp+4+4*(n))
static void fake_size(CPU*c){if(ARGN(c,0)!=0x777){error=6;c->eax=0xffffffffu;}else{if(ARGN(c,1))W32(ARGN(c,1),0);c->eax=10;}c->esp+=12;}
static void fake_seek(CPU*c){uint32_t d=ARGN(c,1),m=ARGN(c,3);file_position=m==0?d:m==1?file_position+d:10+d;c->eax=file_position;c->esp+=20;}
static void fake_read(CPU*c){uint32_t buf=ARGN(c,1),n=ARGN(c,2),out=ARGN(c,3);if(file_position+n>10)n=file_position<10?10-file_position:0;memcpy(HP(buf),file_bytes+file_position,n);file_position+=n;W32(out,n);c->eax=1;c->esp+=24;}
GuestFn find_import_handler(const char*name){if(!strcmp(name,"KERNEL32.DLL!GetFileSize"))return fake_size;if(!strcmp(name,"KERNEL32.DLL!SetFilePointer"))return fake_seek;if(!strcmp(name,"KERNEL32.DLL!ReadFile"))return fake_read;return NULL;}
static uint32_t callv(const char*name,unsigned n,const uint32_t*args){
 CPU c={0};uint32_t sp=0x8000;c.esp=sp;c.esi=0x12345678;W32(sp,0xdeadbeef);for(unsigned i=0;i<n;i++)W32(sp+4+4*i,args[i]);
 uint32_t count;ImportHandler*h=movies_sync_handlers(&count);
 for(unsigned i=0;i<count;i++)if(!strcmp(strchr(h[i].name,'!')+1,name)){h[i].fn(&c);assert(c.esp==sp+4*(n+1)&&c.esi==0x12345678);return c.eax;}
 assert(0);return 0;
}
static uint32_t dup(uint32_t source,uint32_t options){uint32_t a[]={0xffffffffu,source,0xffffffffu,0x7000,0,0,options};W32(0x7000,0);return callv("DuplicateHandle",7,a)?R32(0x7000):0;}
static uint64_t elapsed_ms(struct timespec a){struct timespec b;clock_gettime(CLOCK_MONOTONIC,&b);return (uint64_t)((b.tv_sec-a.tv_sec)*1000ll+(b.tv_nsec-a.tv_nsec)/1000000ll);}
static void handle_tests(void){
 uint32_t none[1]={0};
 assert(callv("GetCurrentThread",0,none)==0xfffffffeu);
 /* Thread pseudo handle duplicates into a real, unsignaled, STILL_ACTIVE handle. */
 uint32_t thread=dup(0xfffffffeu,2);assert(thread&&thread!=0xfffffffeu);
 assert(call("WaitForSingleObject",2,thread,0,0)==258);
 assert(call("GetExitCodeThread",2,thread,0x7100,0)&&R32(0x7100)==259);
 uint32_t again=dup(0xfffffffeu,2);assert(again&&again!=thread);
 assert(movies_sync_close_handle(thread)&&movies_sync_close_handle(again));
 /* Process duplication; GetExitCodeProcess for pseudo and real handle. */
 uint32_t process=dup(0xffffffffu,2);assert(process);assert(movies_sync_is_process_handle(process)&&movies_sync_is_process_handle(0xffffffffu)&&!movies_sync_is_process_handle(0x1234));
 assert(call("GetExitCodeProcess",2,0xffffffffu,0x7100,0)&&R32(0x7100)==259);
 W32(0x7100,0);assert(call("GetExitCodeProcess",2,process,0x7100,0)&&R32(0x7100)==259);
 assert(!call("GetExitCodeProcess",2,0x1234,0x7100,0)&&error==6);
 assert(!call("GetExitCodeProcess",2,0xffffffffu,0,0)&&error==87);
 /* Event duplication shares state; DUPLICATE_CLOSE_SOURCE closes the source. */
 uint32_t ev=event_create(1,0,0),copy=dup(ev,2);assert(copy&&copy!=ev);
 assert(call("SetEvent",1,ev,0,0));assert(!call("WaitForSingleObject",2,copy,0,0));
 uint32_t moved=dup(copy,3);assert(moved&&!movies_sync_close_handle(copy));
 assert(!call("WaitForSingleObject",2,moved,0,0));
 {uint32_t a[]={0xffffffffu,ev,0xffffffffu,0,0,0,1};assert(callv("DuplicateHandle",7,a)==1&&!movies_sync_close_handle(ev));}
 {uint32_t a[]={0xffffffffu,0x5555,0xffffffffu,0x7000,0,0,2};assert(!callv("DuplicateHandle",7,a)&&error==50);}
 {uint32_t a[]={0x1234,moved,0xffffffffu,0x7000,0,0,2};assert(!callv("DuplicateHandle",7,a)&&error==6);}
 {uint32_t a[]={0xffffffffu,moved,0xffffffffu,0,0,0,2};assert(!callv("DuplicateHandle",7,a)&&error==87);}
 {uint32_t a[]={0xffffffffu,moved,process,0x7000,0,0,2};assert(callv("DuplicateHandle",7,a)&&R32(0x7000)&&movies_sync_close_handle(R32(0x7000)));}
 assert(movies_sync_close_handle(moved)&&movies_sync_close_handle(process));
 /* Handle information, thread context. */
 assert(call("SetHandleInformation",3,0x1234,1,0)==1);assert(!call("SetHandleInformation",3,0,1,0)&&error==6);
 assert(!call("SetHandleInformation",3,0x1234,4,0)&&error==87);
 assert(!call("GetThreadContext",2,0xfffffffeu,0x7200,0)&&error==50);
 /* Tagged inert objects for other owners. */
 uint32_t tagged=movies_sync_create_tagged(77,5),value=0;assert(tagged&&movies_sync_tagged_get(tagged,77,&value)&&value==5);
 assert(!movies_sync_tagged_get(tagged,78,&value)&&movies_sync_tagged_set(tagged,77,9)&&movies_sync_tagged_get(tagged,77,&value)&&value==9);
 assert(call("WaitForSingleObject",2,tagged,0,0)==258);assert(movies_sync_close_handle(tagged)&&!movies_sync_tagged_get(tagged,77,&value));
 /* Shared wait core: count zero sleeps; failure leaves LastError to caller. */
 struct timespec t0;clock_gettime(CLOCK_MONOTONIC,&t0);uint32_t e=0;CPU c={0};c.esp=0x8000;
 assert(movies_sync_wait(&c,0,NULL,0,20,&e)==258&&elapsed_ms(t0)>=15);
 uint32_t bad=0x4444;error=123;assert(movies_sync_wait(&c,1,&bad,0,0,&e)==0xffffffffu&&e==6&&error==123);
}
static void mapping_tests(void){
 /* Anonymous section: zeroed, shared by views, freed after close + unmap. */
 uint32_t a[]={0xffffffffu,0,4,0,0x1000,0};unsigned before=heap_count;
 uint32_t map=callv("CreateFileMappingA",6,a);assert(map&&error==0&&heap_count==before+1);
 uint32_t v1[]={map,6,0,0,0},v2[]={map,4,0,0,0x100};
 uint32_t view=callv("MapViewOfFile",5,v1);assert(view&&R32(view+0xffc)==0);W32(view+0x10,0xabcdef01);
 uint32_t tail=callv("MapViewOfFile",5,v2);assert(tail==view&&R32(tail+0x10)==0xabcdef01);
 {uint32_t b[]={map,4,0,0x100,0};assert(!callv("MapViewOfFile",5,b)&&error==1132);}
 {uint32_t b[]={map,4,0,0x10000,0};assert(!callv("MapViewOfFile",5,b)&&error==87);}
 {uint32_t b[]={map,4,0,0,0x1001};assert(!callv("MapViewOfFile",5,b)&&error==8);}
 {uint32_t b[]={map,4,1,0,0};assert(!callv("MapViewOfFile",5,b)&&error==87);}
 {uint32_t b[]={0x4444,4,0,0,0};assert(!callv("MapViewOfFile",5,b)&&error==6);}
 assert(movies_sync_close_handle(map)&&heap_count==before+1); /* views keep the section alive */
 {uint32_t b[]={tail};assert(callv("UnmapViewOfFile",1,b)==1);assert(heap_count==before+1);}
 {uint32_t b[]={view};assert(callv("UnmapViewOfFile",1,b)==1&&heap_count==before);assert(!callv("UnmapViewOfFile",1,b)&&error==487);}
 /* Named sections reopen with ERROR_ALREADY_EXISTS and share storage. */
 strcpy((char*)HP(0x7400),"Local\\movies-section");
 uint32_t n1[]={0xffffffffu,0,4,0,0x100,0x7400};uint32_t first=callv("CreateFileMappingA",6,n1);assert(first&&error==0);
 uint32_t second=callv("CreateFileMappingA",6,n1);assert(second&&second!=first&&error==183);
 uint32_t m1[]={first,2,0,0,0},m2[]={second,4,0,0,0};uint32_t p1=callv("MapViewOfFile",5,m1),p2=callv("MapViewOfFile",5,m2);assert(p1&&p1==p2);
 {uint32_t b[]={p1};assert(callv("UnmapViewOfFile",1,b));b[0]=p2;assert(callv("UnmapViewOfFile",1,b));}
 assert(movies_sync_close_handle(first)&&movies_sync_close_handle(second)&&heap_count==before);
 /* Size, protection and SEC_IMAGE validation. */
 {uint32_t b[]={0xffffffffu,0,4,0,0,0};assert(!callv("CreateFileMappingA",6,b)&&error==87);}
 {uint32_t b[]={0xffffffffu,0,4,1,0,0};assert(!callv("CreateFileMappingA",6,b)&&error==8);}
 {uint32_t b[]={0xffffffffu,0,1,0,16,0};assert(!callv("CreateFileMappingA",6,b)&&error==87);}
 {uint32_t b[]={0xffffffffu,0,0x01000002u,0,16,0};assert(!callv("CreateFileMappingA",6,b)&&error==50);}
 {uint32_t b[]={0xffffffffu,0x10,4,0,16,0};assert(!callv("CreateFileMappingA",6,b)&&error==50);}
 /* File section: read-only snapshot, file position restored, write views denied. */
 file_position=3;uint32_t f[]={0x777,0,2,0,0,0};uint32_t file=callv("CreateFileMappingA",6,f);assert(file&&file_position==3);
 {uint32_t b[]={file,4,0,0,0};uint32_t p=callv("MapViewOfFile",5,b);assert(p&&!memcmp(HP(p),"0123456789",10));
  uint32_t w[]={file,2,0,0,0};assert(!callv("MapViewOfFile",5,w)&&error==5);uint32_t u[]={p};assert(callv("UnmapViewOfFile",1,u));}
 assert(movies_sync_close_handle(file));
 {uint32_t b[]={0x777,0,2,0,4,0};uint32_t part=callv("CreateFileMappingA",6,b);assert(part);uint32_t m[]={part,4,0,0,0};uint32_t p=callv("MapViewOfFile",5,m);assert(p&&!memcmp(HP(p),"0123",4));uint32_t u[]={p};callv("UnmapViewOfFile",1,u);movies_sync_close_handle(part);}
 {uint32_t b[]={0x777,0,2,0,11,0};assert(!callv("CreateFileMappingA",6,b)&&error==8);}
 {uint32_t b[]={0x777,0,4,0,0,0};assert(!callv("CreateFileMappingA",6,b)&&error==50);}
 {uint32_t b[]={0x778,0,2,0,0,0};assert(!callv("CreateFileMappingA",6,b)&&error==6);}
 assert(heap_count==before);
}
static void timer_tests(void){
 uint32_t ev=event_create(0,0,0);struct timespec t0;
 /* Function callbacks need a guest thread and are refused with NULL. */
 {uint32_t a[]={50,10,0x401000,0,0};assert(!callv("timeSetEvent",5,a));}
 {uint32_t a[]={0,10,ev,0,0x10};assert(!callv("timeSetEvent",5,a));}
 {uint32_t a[]={20,10,0x4444,0,0x10};assert(!callv("timeSetEvent",5,a));}
 /* One-shot EVENT_SET: the wait observes the deadline without a host timer thread. */
 clock_gettime(CLOCK_MONOTONIC,&t0);
 {uint32_t a[]={30,1,ev,0,0x10};uint32_t id=callv("timeSetEvent",5,a);assert(id);
  assert(call("WaitForSingleObject",2,ev,0,0)==258);
  assert(call("WaitForSingleObject",2,ev,2000,0)==0&&elapsed_ms(t0)>=25&&elapsed_ms(t0)<1500);
  uint32_t k[]={id};assert(callv("timeKillEvent",1,k)==11);} /* one-shot already retired */
 /* Event survives CloseHandle while a timer references it. Periodic fires repeatedly. */
 {uint32_t a[]={15,1,ev,0,0x11};uint32_t id=callv("timeSetEvent",5,a);assert(id);
  for(int i=0;i<3;i++)assert(call("WaitForSingleObject",2,ev,1000,0)==0);
  uint32_t k[]={id};assert(callv("timeKillEvent",1,k)==0&&callv("timeKillEvent",1,k)==11);
  assert(call("WaitForSingleObject",2,ev,60,0)==258);}
 /* PULSE on a manual-reset event: releases the waiter, then resets. */
 uint32_t manual=event_create(1,0,0);
 {uint32_t a[]={10,1,manual,0,0x20};assert(callv("timeSetEvent",5,a));
  assert(call("WaitForSingleObject",2,manual,1000,0)==0);assert(call("WaitForSingleObject",2,manual,0,0)==258);}
 {uint32_t a[]={10,1,manual,0,0x10};assert(callv("timeSetEvent",5,a));
  assert(movies_sync_close_handle(manual)); /* timer still owns the object */
  struct timespec d={0,30000000};nanosleep(&d,NULL);}
 assert(movies_sync_close_handle(ev));
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
    assert(call("WaitForSingleObjectEx",3,two,0,1)==0);assert(call("ReleaseMutex",1,two,0,0)); /* alertable == plain wait */
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
    /* Semaphore: initial 1 of max 2; waits decrement, zero times out, release enforces max. */
    {W32(stack+16,0);uint32_t sem=call("CreateSemaphoreA",4,0,1,2);assert(sem);
     assert(call("WaitForSingleObject",2,sem,0,0)==0);assert(call("WaitForSingleObject",2,sem,0,0)==0x102);
     assert(call("ReleaseSemaphore",3,sem,2,0x5000)==1&&R32(0x5000)==0);
     assert(call("ReleaseSemaphore",3,sem,1,0)==0);
     assert(call("WaitForSingleObject",2,sem,0,0)==0&&call("WaitForSingleObject",2,sem,0,0)==0&&call("WaitForSingleObject",2,sem,0,0)==0x102);
     assert(call("CreateSemaphoreA",4,0,3,2)==0);assert(call("SetThreadPriority",2,0x1234,2,0)==1);}
    movies_sync_init();handle_tests();mapping_tests();timer_tests();
    free(g_mem);puts("sync mutex ownership, recursion, timeout, abandonment and handle tests passed");
}
