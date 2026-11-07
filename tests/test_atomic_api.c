#include "atomic-api.h"
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <sys/mman.h>
uint8_t *g_mem;
static _Thread_local jmp_buf trapped;
static _Thread_local uint32_t stack=0x100;
static GuestFn fn[5];
static unsigned argc[]={1,1,2,3,2};
void guest_trap(CPU *c,uint32_t at,const char *why) {
    (void)c;(void)at;assert(why);longjmp(trapped,1);
}
static uint32_t call(unsigned op,uint32_t address,uint32_t value,uint32_t expected) {
    CPU c={0};c.esp=stack;c.ebx=0x12345678;c.esi=0xabcdef01;c.edi=7;c.ebp=8;
    W32(stack,0xabcdef);W32(stack+4,address);W32(stack+8,value);W32(stack+12,expected);
    CPU unchanged=c;
    fn[op](&c);
    uint32_t result=c.eax;
    unchanged.eax=result;unchanged.esp+=4*(argc[op]+1);
    assert(!memcmp(&c,&unchanged,sizeof c));return result;
}
static void *race(void *arg) {
    stack=0x1000+0x100*(uint32_t)(uintptr_t)arg;
    for(unsigned i=0;i<20000;i++) {
        call(0,0x8000,0,0);
        call(1,0x8004,0,0);
        call(4,0x8008,3,0);
        uint32_t old=call(3,0x800c,0,0);
        while(call(3,0x800c,old+1,old)!=old) old=call(3,0x800c,0,0);
    }
    return NULL;
}
int main(void) {
    g_mem=mmap(NULL,GUEST_SPACE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(g_mem!=MAP_FAILED);
    uint32_t count;ImportHandler *handlers=movies_atomic_handlers(&count);assert(count==5);
    for(unsigned i=0;i<count;i++) { assert(!strncmp(handlers[i].name,"KERNEL32.DLL!Interlocked",23));fn[i]=handlers[i].fn; }
    W32(0x400,0x7fffffff);assert(call(0,0x400,0,0)==0x80000000 && R32(0x400)==0x80000000);
    assert(call(1,0x400,0,0)==0x7fffffff);
    W32(0x400,0xffffffff);assert(!call(0,0x400,0,0));assert(call(1,0x400,0,0)==0xffffffff);
    assert(call(2,0x400,123,0)==0xffffffff && R32(0x400)==123);
    assert(call(3,0x400,456,122)==123 && R32(0x400)==123);
    assert(call(3,0x400,456,123)==123 && R32(0x400)==456);
    assert(call(4,0x400,0xffffffff,0)==456 && R32(0x400)==455);
    W32(0x400,0xffffffff);assert(call(4,0x400,2,0)==0xffffffff && R32(0x400)==1);
    assert(call(0,(uint32_t)(GUEST_SPACE-4),0,0)==1);
    uint32_t invalid[]={0,1,0x401,0xfffffffe};
    for(unsigned op=0;op<5;op++)for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        W32(0x400,0x12345678);
        if(!setjmp(trapped)) { call(op,invalid[i],99,99);assert(0); }
        assert(R32(0x400)==0x12345678);
    }
    pthread_t threads[4];
    for(uintptr_t i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,race,(void *)i));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
    assert(R32(0x8000)==80000 && R32(0x8004)==(uint32_t)-80000);
    assert(R32(0x8008)==240000 && R32(0x800c)==80000);
    assert(!munmap(g_mem,GUEST_SPACE));puts("atomic ABI and race tests passed");
}
