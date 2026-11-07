#include "runtime.h"
#include "atomic-api.h"
#include <pthread.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
int dyncode_contains(uint32_t a){(void)a;return 0;}void interp_call(CPU*c,uint32_t a){(void)c;(void)a;abort();}
static GuestFn increment;
static void*translated(void*p){(void)p;for(unsigned i=0;i<20000;i++){guest_lock();W32(0x100,R32(0x100)+1);guest_unlock();}W8(0x200,1);return NULL;}
static void*native(void*p){(void)p;for(unsigned i=0;i<20000;i++){CPU c={0};c.esp=0x1000;W32(0x1004,0x100);increment(&c);}return NULL;}
int main(void){g_mem=calloc(1,0x4000);assert(g_mem);uint32_t n;ImportHandler*h=movies_atomic_handlers(&n);increment=h[0].fn;W64(0x301,0x123456789abcdef0ull);assert(R64(0x301)==0x123456789abcdef0ull);WF32(0x311,1.25f);assert(RF32(0x311)==1.25f);pthread_t a,b;assert(!pthread_create(&a,NULL,translated,NULL));assert(!pthread_create(&b,NULL,native,NULL));while(!R8(0x200)){}assert(!pthread_join(a,NULL)&&!pthread_join(b,NULL));assert(R32(0x100)==40000);free(g_mem);puts("serialized guest accessor/Interlocked/LOCK mixed RMW and unaligned/spin reads PASS");return 0;}
