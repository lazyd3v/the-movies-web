#define _DARWIN_C_SOURCE
#include "shell-api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
uint8_t *g_mem;
static uint32_t error;
void movies_set_last_error(uint32_t value) { error=value; }
void guest_trap(CPU *c,uint32_t at,const char *why) { (void)c;(void)at;(void)why;abort(); }
static uint32_t call(unsigned index,uint32_t csidl,uint32_t token,uint32_t flags,uint32_t out) {
    uint32_t count;ImportHandler *h=movies_shell_handlers(&count);assert(count==6);
    CPU c={0};c.esp=0x100;c.ebx=99;
    W32(c.esp,0x1234);W32(c.esp+4,0);W32(c.esp+8,csidl);W32(c.esp+12,token);W32(c.esp+16,flags);W32(c.esp+20,out);
    h[index].fn(&c);assert(c.esp==0x118 && c.ebx==99);return c.eax;
}
static uint32_t special(int wide,uint32_t csidl,uint32_t create,uint32_t out) {
    uint32_t count;ImportHandler *h=movies_shell_handlers(&count);assert(count==6);
    CPU c={0};c.esp=0x100;c.ebx=99;
    W32(c.esp,0x1234);W32(c.esp+4,0);W32(c.esp+8,out);W32(c.esp+12,csidl);W32(c.esp+16,create);
    h[4+wide].fn(&c);assert(c.esp==0x114 && c.ebx==99);return c.eax;
}
int main(void) {
    g_mem=calloc(1,0x10000);assert(g_mem);
    char temporary[]="/private/tmp/movies-shell-XXXXXX";assert(mkdtemp(temporary));
    assert(movies_shell_init(temporary));
    char backend[4096];assert(movies_shell_backend_path(5,backend,sizeof backend));
    struct stat st;assert(!stat(backend,&st) && S_ISDIR(st.st_mode));
    /* The original app requests APPDATA without CREATE and ignores HRESULT.
     * A provisioned profile must give it a real absolute prefix on first call. */
    assert(!call(1,0x1a,0,0,0x1000));
    const char *appdata=movies_shell_virtual_path(0x1a);
    for(unsigned i=0;i<=strlen(appdata);i++) assert(R16(0x1000+2*i)==(uint8_t)appdata[i]);
    assert(movies_shell_backend_path(0x1a,backend,sizeof backend));assert(!stat(backend,&st));assert(!rmdir(backend));
    assert(!call(0,0x1c,0,0,0x1000));
    assert(!strcmp((char *)HP(0x1000),movies_shell_virtual_path(0x1c)));
    assert(movies_shell_backend_path(0x1c,backend,sizeof backend));assert(!stat(backend,&st));assert(!rmdir(backend));
    /* Explicitly remove the initial folders to test create/no-create below. */
    for(unsigned i=0;i<4;i++) {
        memset(HP(0x1000),0xaa,522);assert(!call(i,5,0,0,0x1000));
        const char *expected="C:\\Users\\Player\\Documents";
        if(i&1) { for(size_t j=0;j<=strlen(expected);j++)assert(R16(0x1000+j*2)==(unsigned char)expected[j]); }
        else assert(!strcmp((char *)HP(0x1000),expected));
        assert(R8(0x1000+520)==0xaa);
    }
    error=123;assert(special(0,5,0,0x1000)==1 && error==123);
    assert(!strcmp((char *)HP(0x1000),"C:\\Users\\Player\\Documents"));
    assert(!special(0,0x1a,0,0x1000) && error==3 && !R8(0x1000));
    assert(movies_shell_backend_path(0x1a,backend,sizeof backend));assert(stat(backend,&st));
    assert(special(1,0x1a,2,0x1000)==1); /*any nonzero BOOL creates*/
    assert(!stat(backend,&st) && S_ISDIR(st.st_mode));
    const char *roaming=movies_shell_virtual_path(0x1a);
    for(unsigned j=0;j<=strlen(roaming);j++)assert(R16(0x1000+2*j)==(uint8_t)roaming[j]);
    assert(!rmdir(backend));
    assert(!special(0,0,1,0x1000) && error==87); /*unsupported virtual CSIDL*/
    assert(!special(0,5,0,0) && error==87);
    assert(!special(1,5,0,0xfffffff0) && error==87);
    assert(call(0,0x1a,0,0,0x1000)==0x80070003 && !R8(0x1000));
    assert(!call(0,0x401a,0,0,0x1000));
    assert(movies_shell_backend_path(0x1a,backend,sizeof backend));assert(stat(backend,&st));
    assert(!call(1,0x801a,0,0,0x1000));assert(!stat(backend,&st));
    assert(!call(0,0x101a,0,1,0x1000));
    assert(call(1,0xc01a,0,0,0x1000)==0x80070057 && !R16(0x1000));
    assert(call(0,5,0,2,0x1000)==0x80070057);
    assert(call(0,5,1,0,0x1000)==0x80070006);
    assert(!call(0,0x2005,0,0,0x1000));
    assert(call(0,0x0805,0,0,0x1000)==0x80070057);
    assert(call(0,0,0,0,0x1000)==0x80070057);
    assert(call(1,5,0,0,0xfffffff0)==0x80070057);
    assert(call(1,5,0,0,0)==0x80070057);
    assert(!movies_shell_backend_path(5,backend,2) && !backend[0]);
    assert(movies_shell_backend_path(0x1c,backend,sizeof backend));assert(!symlink("/private/tmp",backend));
    assert(!special(0,0x1c,1,0x1000) && error==5);
    assert(call(0,0x801c,0,0,0x1000)==0x80070005);assert(!unlink(backend));
    assert(movies_shell_backend_path(0x1a,backend,sizeof backend));assert(!rmdir(backend));
    char *last=strrchr(backend,'/');*last=0;assert(!rmdir(backend));
    assert(movies_shell_backend_path(5,backend,sizeof backend));assert(!rmdir(backend));
    assert(call(0,5,0,0,0x1000)==0x80070003);
    assert(!call(0,0x4005,0,0,0x1000));assert(stat(backend,&st));
    assert(!call(0,0x8005,0,0,0x1000));assert(!rmdir(backend));
    last=strrchr(backend,'/');*last=0;assert(!rmdir(backend));last=strrchr(backend,'/');*last=0;assert(!rmdir(backend));assert(!rmdir(temporary));
    assert(!movies_shell_init("/") && !movies_shell_init("relative"));
    free(g_mem);puts("shell folder ABI and backend tests passed");
}
