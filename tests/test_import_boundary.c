#include "runtime.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static uint32_t outer_slot,inner_slot;static unsigned hooks,outer_returned,inner_calls;static int enabled=1;
#ifndef TEST_DEFAULT_DISABLED
int movies_import_needs_callback(uint32_t target){return enabled&&target==outer_slot;}
#endif
int dyncode_contains(uint32_t a){(void)a;return 0;}void interp_call(CPU*c,uint32_t a){(void)c;(void)a;abort();}
static void inner(CPU*c){assert(g_import_target==inner_slot);inner_calls++;c->eax=9;c->esp+=4;}
static void outer(CPU*c){assert(g_import_target==outer_slot);assert(!hooks);c->eax=77;c->edx=88;c->cf=1;c->zf=1;c->pf_byte=0x46;c->esp+=8;outer_returned=1;}
static void ordinary(CPU*c){c->eax=12;c->esp+=4;}
void movies_after_import(CPU*c){
 assert(outer_returned&&g_import_target==outer_slot);assert(c->esp==0x808&&c->eax==77&&c->edx==88&&c->cf==1&&c->zf==1&&c->pf_byte==0x46);hooks++;
 CPU nested=*c;nested.esp=0x1000;W32(nested.esp,0x1234);call_indirect(&nested,inner_slot);assert(nested.eax==9&&nested.esp==0x1004&&g_import_target==outer_slot&&hooks==1);
 W32(0x2000,123); /* callback side effect persists */
 c->eax=0;c->edx=0;c->esp=0;c->cf=0;c->zf=0;c->pf_byte=0; /* dispatcher protects original return */
}
int main(void){g_mem=calloc(1,0x4000);assert(g_mem);outer_slot=add_dynamic_import("TEST.DLL!Outer",outer);inner_slot=add_dynamic_import("TEST.DLL!Inner",inner);register_function(0x5000,ordinary);
#ifdef TEST_DEFAULT_DISABLED
 CPU disabled={0};disabled.esp=0x800;W32(disabled.esp,0x1234);call_indirect(&disabled,outer_slot);assert(!hooks&&disabled.eax==77&&disabled.esp==0x808);free(g_mem);puts("default-disabled import hook PASS");return 0;
#endif
 for(unsigned tail=0;tail<2;tail++){CPU c={0};c.esp=0x800;W32(c.esp,0x1234);W32(c.esp+4,3);g_import_target=0xabcdef;hooks=outer_returned=0;if(tail)jump_indirect(&c,outer_slot,0x6000);else call_indirect(&c,outer_slot);assert(hooks==1&&c.esp==0x808&&c.eax==77&&c.edx==88&&c.cf==1&&c.zf==1&&c.pf_byte==0x46&&g_import_target==0xabcdef&&R32(0x2000)==123);}
 CPU c={0};c.esp=0x800;hooks=0;enabled=0;call_indirect(&c,outer_slot);assert(!hooks&&c.eax==77&&c.esp==0x808);c.esp=0x800;call_indirect(&c,0x5000);assert(!hooks&&c.eax==12&&c.esp==0x804);assert(inner_calls==2);free(g_mem);puts("actual runtime post-import and tail boundary CPU/flags/reentrancy/identity PASS");return 0;}
