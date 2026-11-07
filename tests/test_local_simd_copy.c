#include "runtime.h"
#include "simd.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
uint8_t*g_mem;volatile uint8_t g_guest_lock;__thread unsigned g_guest_lock_depth;int g_preempt_on;
void guest_preempt(CPU*c){(void)c;abort();}
static unsigned copies,guest_copies,zeros;
static int is_guest(const void*p){uintptr_t a=(uintptr_t)p,b=(uintptr_t)g_mem;return a>=b&&a-b<4096;}
static void*checked_copy(void*d,const void*s,size_t n){copies++;int guest=is_guest(d)||is_guest(s);if(guest){guest_copies++;guest_lock();}void*r=__builtin_memcpy(d,s,n);if(guest)guest_unlock();return r;}
static void*checked_zero(void*d,int v,size_t n){zeros++;int guest=is_guest(d);if(guest)guest_lock();void*r=__builtin_memset(d,v,n);if(guest)guest_unlock();return r;}
#undef memcpy
#undef memset
#define memcpy checked_copy
#define memset checked_zero
#include "generated.inc"
#include "functions.inc"
#undef memcpy
#undef memset
static uint32_t rng=123;static uint8_t random_byte(void){rng=rng*1664525u+1013904223u;return (uint8_t)(rng>>24);}
int main(void){g_mem=calloc(1,4096);assert(g_mem);unsigned total_baseline=0,total_candidate=0,total_guest=0;uint8_t initial[4096],expected[4096];
 for(unsigned test=0;test<sizeof baselines/sizeof*baselines;test++)for(unsigned iteration=0;iteration<100;iteration++){
  CPU input;uint8_t*b=(uint8_t*)&input;for(unsigned j=0;j<sizeof input;j++)b[j]=random_byte();input.esp=0x800;input.eax=0x200;input.preempt_ticks=0;
  for(unsigned j=0;j<sizeof initial;j++)initial[j]=random_byte();__builtin_memcpy(g_mem,initial,4096);CPU a=input;copies=guest_copies=zeros=0;baselines[test](&a);unsigned baseline_copies=copies,baseline_guest=guest_copies;__builtin_memcpy(expected,g_mem,4096);
  __builtin_memcpy(g_mem,initial,4096);CPU c=input;copies=guest_copies=zeros=0;candidates[test](&c);assert(!memcmp(&a,&c,sizeof a));assert(!memcmp(expected,g_mem,4096));assert(guest_copies==baseline_guest);assert(copies<=baseline_copies);total_baseline+=baseline_copies;total_candidate+=copies;total_guest+=guest_copies;
 }
 assert(total_candidate<total_baseline&&total_guest);printf("SIMD emitter semantic equality PASS: baseline checked copies %u, candidate %u, guest copies preserved %u\n",total_baseline,total_candidate,total_guest);free(g_mem);return 0;}
