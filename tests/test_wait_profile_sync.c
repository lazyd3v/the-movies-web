#include "wait-profile.h"
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

static MoviesWaitProfileReport observed;static unsigned reports;
static void capture(const MoviesWaitProfileReport*r){observed=*r;reports++;}
int main(void){
 g_mem=calloc(1,0x10000);assert(g_mem);movies_sync_init();movies_wait_profile_init(1,capture);movies_wait_profile_present();
 error=123;call("Sleep",1,10,0,0);assert(error==123);
 uint32_t event=event_create(1,0,0);assert(event);assert(call("WaitForSingleObject",2,event,10,0)==258);assert(call("WaitForSingleObject",2,event,0,0)==258);
 movies_wait_profile_flush();assert(reports==1);uint64_t sleep_count=0,wait_count=0;
 for(unsigned i=0;i<MOVIES_WAIT_PROFILE_SLOTS;i++){MoviesWaitProfileEntry*e=&observed.entries[0][i];if(!e->count)continue;assert(e->caller==0xdeadbeef&&e->timeout==10&&e->ns>=8000000);if(e->kind==1)sleep_count+=e->count;else if(e->kind==2)wait_count+=e->count;else assert(0);}
 assert(sleep_count==1&&wait_count>=1);assert(!observed.entries[1][0].count);assert(movies_sync_close_handle(event));free(g_mem);puts("actual Sleep/condvar profiling passed; zero-timeout poll not counted");return 0;
}
