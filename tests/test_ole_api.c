#include "ole-api.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
uint8_t *g_mem;
static jmp_buf trapped;
void guest_trap(CPU *c,uint32_t at,const char *why) { (void)c;(void)at;(void)why;longjmp(trapped,1); }
/* Backend allocator fixture: bump allocation with live tracking. */
static uint32_t next_block=0x8000,live[32];static unsigned live_count,failed_frees;
uint32_t movies_backend_alloc(uint32_t n){if(next_block+n>0xf000||live_count==32)return 0;uint32_t a=next_block;next_block+=(n+7)&~7u;memset(HP(a),0xcc,n);live[live_count++]=a;return a;}
int movies_backend_free(uint32_t a){for(unsigned i=0;i<live_count;i++)if(live[i]==a){live[i]=live[--live_count];return 1;}failed_frees++;return 0;}
static uint32_t released;
static void release_fn(CPU *c){released=R32(c->esp+4);c->eax=0;c->esp+=8;}
GuestFn lookup_function(uint32_t a){return a==0x401230?release_fn:NULL;}
void guest_call(CPU *c,GuestFn fn,int argc,const uint32_t *args){for(int i=argc-1;i>=0;i--){c->esp-=4;W32(c->esp,args[i]);}c->esp-=4;W32(c->esp,0xdeadbeef);fn(c);}
static uint32_t call(const char *name,unsigned argc,uint32_t a,uint32_t b,uint32_t d) {
    uint32_t count;ImportHandler *h=movies_ole_handlers(&count);CPU c={0};c.esp=0x1000;c.ebx=0x12345678;c.eax=0xabcdabcd;
    W32(c.esp,0xdeadbeef);W32(c.esp+4,a);W32(c.esp+8,b);W32(c.esp+12,d);
    for(unsigned i=0;i<count;i++)if(!strcmp(h[i].name,name)){h[i].fn(&c);assert(c.esp==0x1004+4*argc&&c.ebx==0x12345678);return c.eax;}
    assert(0);return 0;
}
int main(void) {
    g_mem=calloc(1,0x10000);assert(g_mem);
    assert(call("OLE32.DLL!CoFreeUnusedLibraries",0,0,0,0)==0xabcdabcd); /* VOID: registers preserved */
    W32(0x2000,0x1111);assert(call("OLE32.DLL!GetRunningObjectTable",2,0,0x2000,0)==0x80004001u&&R32(0x2000)==0);
    W32(0x2000,0x1111);assert(call("OLE32.DLL!CreateItemMoniker",3,0x3000,0x3010,0x2000)==0x80004001u&&R32(0x2000)==0);
    /* BSTR: byte-length prefix, UTF-16 payload, NUL terminator. */
    const char *text="Movies";for(unsigned i=0;i<7;i++)W16(0x3000+2*i,(uint8_t)text[i]);
    uint32_t bstr=call("OLEAUT32.DLL!#2",1,0x3000,0,0);assert(bstr&&R32(bstr-4)==12&&R16(bstr)=='M'&&R16(bstr+10)=='s'&&R16(bstr+12)==0&&live_count==1);
    W16(0x3100,0);uint32_t empty=call("OLEAUT32.DLL!#2",1,0x3100,0,0);assert(empty&&R32(empty-4)==0&&R16(empty)==0);
    assert(!call("OLEAUT32.DLL!#2",1,0,0,0));
    call("OLEAUT32.DLL!#6",1,empty,0,0);assert(live_count==1);
    call("OLEAUT32.DLL!#6",1,0,0,0);assert(live_count==1&&!failed_frees);
    /* VARIANT: init sets VT_EMPTY; clear frees BSTR, releases IUnknown, rejects arrays. */
    memset(HP(0x4000),0xee,16);call("OLEAUT32.DLL!#8",1,0x4000,0,0);assert(R16(0x4000)==0&&R16(0x4006)==0&&R32(0x4008)==0xeeeeeeeeu);
    W16(0x4000,8);W32(0x4008,bstr);assert(call("OLEAUT32.DLL!#9",1,0x4000,0,0)==0&&R16(0x4000)==0&&live_count==0);
    W32(0x5000,0x5100);W32(0x5108,0x401230);W16(0x4000,13);W32(0x4008,0x5000);
    assert(call("OLEAUT32.DLL!#9",1,0x4000,0,0)==0&&released==0x5000&&R16(0x4000)==0);
    W16(0x4000,3);W32(0x4008,77);assert(call("OLEAUT32.DLL!#9",1,0x4000,0,0)==0&&R16(0x4000)==0);
    W16(0x4000,0x4008);assert(call("OLEAUT32.DLL!#9",1,0x4000,0,0)==0&&R16(0x4000)==0); /* BYREF: not owned */
    W16(0x4000,0x2003);assert(call("OLEAUT32.DLL!#9",1,0x4000,0,0)==0x80020008u&&R16(0x4000)==0x2003);
    W16(0x4000,0x24);assert(call("OLEAUT32.DLL!#9",1,0x4000,0,0)==0x80020008u);
    assert(call("OLEAUT32.DLL!#9",1,0,0,0)==0x80070057u);
    W32(0x5108,0x409999);W16(0x4000,9);W32(0x4008,0x5000);if(!setjmp(trapped)){call("OLEAUT32.DLL!#9",1,0x4000,0,0);assert(0);}
    free(g_mem);puts("OLE BSTR, VARIANT and moniker refusal tests passed");
}
