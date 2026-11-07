#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <emscripten.h>
#include <emscripten/threading.h>
#include "cpu.h"
uint8_t*g_mem;volatile uint8_t g_guest_lock;__thread unsigned g_guest_lock_depth;int g_preempt_on;
static _Atomic int control[8]; /* heartbeat, parent blocked, arm, trap depth, trap seen, address, stop, child ready */
void guest_preempt(CPU*c){(void)c;assert(!g_guest_lock_depth);}
void guest_bounds_fail(uint32_t address,int bytes){(void)bytes;atomic_store(&control[3],g_guest_lock_depth);atomic_store(&control[5],address);atomic_store(&control[4],1);__builtin_trap();}
#include "candidate.inc"
static void*beat(void*p){(void)p;while(!atomic_load(&control[6])){atomic_fetch_add(&control[0],1);emscripten_thread_sleep(5);}return NULL;}
static void*fault(void*p){(void)p;atomic_store(&control[7],1);while(!atomic_load(&control[2]))emscripten_thread_sleep(2);CPU c={0};c.ecx=4096;c.esp=240000;c.fpu_cw=0x37f;f_009e9ab0(&c);abort();}
EM_JS(void,publish,(int *ptr),{Module.probeControl({buffer:HEAP32.buffer,offset:ptr,words:8});});
EM_JS(void,block_parent,(int *ptr),{Atomics.wait(HEAP32,ptr>>2,0);throw new Error('Blocked parent unexpectedly resumed');});
int main(void){g_mem=calloc(1,262144);assert(g_mem);W32(4096+0x2978c,0x03000000); /* first vertex write computes 0x5400105c */
 pthread_t a,b;assert(!pthread_create(&a,NULL,beat,NULL));assert(!pthread_create(&b,NULL,fault,NULL));publish((int*)control);atomic_store(&control[1],1);block_parent((int*)&control[6]);return 9;}
