/* Link registry-api.c and text-api.c; define MOVIES_REGISTRY_SPACE=0x20000. */
#include "registry-api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static uint8_t memory[0x20000];
uint8_t *g_mem=memory;
static uint32_t last_error;
void movies_set_last_error(uint32_t error) { last_error=error; }
void guest_trap(CPU *c,uint32_t address,const char *reason) { (void)c;(void)address;fprintf(stderr,"trap: %s\n",reason);abort(); }
static uint32_t call(const char *name,unsigned argc,const uint32_t *args) {
    CPU c={0};c.esp=0x100;c.ebx=0x10203040;c.esi=0x50607080;
    W32(c.esp,0xdeadbeef);
    for(unsigned i=0;i<argc;i++)W32(c.esp+4+4*i,args[i]);
    uint32_t count;ImportHandler *handlers=movies_registry_handlers(&count);
    for(uint32_t i=0;i<count;i++)if(!strcmp(handlers[i].name+13,name)) {
        handlers[i].fn(&c);
        assert(c.esp==0x100+4*(argc+1) && R32(0x100)==0xdeadbeef);
        assert(c.ebx==0x10203040 && c.esi==0x50607080);return c.eax;
    }
    assert(0);return 0;
}
#define CALL(name,n,...) call(name,n,(uint32_t[]){__VA_ARGS__})
static void ansi(uint32_t p,const char *text) { strcpy((char *)HP(p),text); }
static void wide(uint32_t p,const uint16_t *text) { do { W16(p,*text);p+=2; } while(*text++); }
static uint32_t create(const char *name,uint32_t rights,uint32_t disposition) {
    ansi(0x1000,name);W32(0x900,0xffffffff);W32(0x904,0xffffffff);
    assert(CALL("RegCreateKeyExA",9,0x80000001,0x1000,0,0,0,rights,0,0x900,0x904)==0);
    assert(R32(0x904)==disposition);return R32(0x900);
}
int main(void) {
    movies_registry_init(NULL);
    last_error=777;
    ansi(0x1000,"Software\\Absent");
    assert(CALL("RegOpenKeyExA",5,0x80000001,0x1000,0,1,0x900)==2);
    uint32_t key=create("Software\\Lionhead\\Caf\xe9",0xf003f,1);
    uint32_t other=create("software\\LIONHEAD\\CAF\xc9",0xf003f,2);
    assert(other!=key && last_error==777);
    assert(CALL("RegQueryValueExA",6,key,0,0,0,0,0x910)==2);
    assert(CALL("RegOpenKeyA",3,0x80000001,0x1000,0x908)==0);
    assert(CALL("RegCloseKey",1,R32(0x908))==0);
    uint32_t write_only=create("Software\\Lionhead\\Caf\xe9",2,2);
    assert(CALL("RegQueryValueExA",6,write_only,0,0,0,0,0x910)==5);
    assert(CALL("RegCloseKey",1,write_only)==0);
    wide(0x2000,(uint16_t[]){'S','O','F','T','W','A','R','E','\\','L','i','o','n','h','e','a','d','\\','C','a','f',0xc9,0});
    assert(CALL("RegOpenKeyExW",5,0x80000001,0x2000,0,1,0x908)==0);
    uint32_t read_only=R32(0x908);
    ansi(0x1100,"Value\x80");
    uint8_t text[]={0x80,'A',0,'B',0,0};memcpy(HP(0x3000),text,sizeof text);
    assert(CALL("RegSetValueExA",6,key,0x1100,0,7,0x3000,sizeof text)==0);
    wide(0x2100,(uint16_t[]){'v','a','l','u','e',0x20ac,0});
    assert(CALL("RegQueryValueExW",6,read_only,0x2100,0,0x90c,0,0x910)==0);
    assert(R32(0x90c)==7 && R32(0x910)==12);
    memset(HP(0x4000),0xa5,32);W32(0x910,11);
    assert(CALL("RegQueryValueExW",6,key,0x2100,0,0x90c,0x4000,0x910)==234);
    assert(R32(0x910)==12 && R8(0x4000)==0xa5);
    assert(CALL("RegQueryValueExW",6,key,0x2100,0,0x90c,0x4000,0x910)==0);
    assert(R16(0x4000)==0x20ac && R16(0x4002)=='A' && R16(0x4004)==0 && R16(0x4006)=='B');
    assert(R16(0x4008)==0 && R16(0x400a)==0 && R8(0x400c)==0xa5);
    W32(0x910,32);
    assert(CALL("RegQueryValueExA",6,key,0x1100,0,0,0x4000,0x910)==0);
    assert(R32(0x910)==sizeof text && !memcmp(HP(0x4000),text,sizeof text));
    assert(CALL("RegSetValueExA",6,read_only,0,0,4,0x3000,4)==5);
    assert(CALL("RegDeleteValueA",2,read_only,0x1100)==5);
    assert(CALL("RegQueryValueExA",6,key,0x1100,0,0,0,0)==0);
    assert(CALL("RegQueryValueExA",6,key,0x1100,0,0,0x4000,0)==87);
    /* Explicit byte counts preserve unterminated REG_SZ, with no added NUL. */
    W8(0x3000,0x80);assert(CALL("RegSetValueExA",6,key,0,0,1,0x3000,1)==0);
    W32(0x910,32);memset(HP(0x4000),0xa5,32);
    assert(CALL("RegQueryValueExW",6,key,0,0,0,0x4000,0x910)==0);
    assert(R32(0x910)==2 && R16(0x4000)==0x20ac && R8(0x4002)==0xa5);
    /* DWORD/binary bytes are not Unicode-converted. */
    W32(0x3000,0xfedcba98);assert(CALL("RegSetValueExA",6,key,0,0,4,0x3000,4)==0);
    W32(0x910,32);
    assert(CALL("RegQueryValueExW",6,key,0,0,0x90c,0x4000,0x910)==0);
    assert(R32(0x910)==4 && R32(0x4000)==0xfedcba98 && R32(0x90c)==4);
    assert(CALL("RegSetValueExA",6,key,0,0,3,0,0)==0);W32(0x910,0);
    assert(CALL("RegQueryValueExA",6,key,0,0,0,0,0x910)==0 && R32(0x910)==0);
    assert(CALL("RegDeleteValueA",2,other,0x1100)==0);
    assert(CALL("RegDeleteValueA",2,key,0x1100)==2);
    assert(CALL("RegQueryValueExA",6,key,0x1100,0,0,0,0x910)==2);
    assert(CALL("RegCloseKey",1,key)==0);
    assert(CALL("RegCloseKey",1,key)==6);
    assert(CALL("RegQueryValueExA",6,key,0,0,0,0,0x910)==6);
    assert(CALL("RegOpenKeyExA",5,0x80000001,0,0,1,0x900)==0 && R32(0x900)==0x80000001);
    assert(CALL("RegCloseKey",1,0x80000001)==0);
    assert(CALL("RegOpenKeyExA",5,0x80000001,0,0,0x100,0x900)==50);
    assert(CALL("RegCreateKeyExA",9,0x80000001,0x1000,0,0,2,3,0,0x900,0)==50);
    assert(CALL("RegSetValueExA",6,other,0,1,3,0,0)==87);
    assert(CALL("RegSetValueExA",6,other,0,0,3,0x1ffff,2)==87);
    assert(last_error==777); /* all registry calls return status directly */
    W32(0x910,6);memset(HP(0x4000),0xa5,32);
    assert(!CALL("GetUserNameA",2,0x4000,0x910) && last_error==122 && R32(0x910)==7);
    assert(R8(0x4000)==0xa5);
    assert(CALL("GetUserNameA",2,0x4000,0x910) && !strcmp((char *)HP(0x4000),"Player"));
    movies_registry_init("/this/path/is/not/accessed");
    assert(CALL("RegCloseKey",1,other)==6);
    /* Persistence: a tree written under one init is reloaded by the next init from the
     * same userdata root (registry.v1), including values on created subkeys. */
    {char dir[]="/tmp/movies-registry-XXXXXX";assert(mkdtemp(dir));
     movies_registry_init(dir);uint32_t k=create("Software\\Lionhead\\Persist",0xf003f,1);
     ansi(0x1100,"Quality");W32(0x1200,3);assert(CALL("RegSetValueExA",6,k,0x1100,0,4,0x1200,4)==0);
     movies_registry_init(dir);ansi(0x1000,"Software\\Lionhead\\Persist");assert(CALL("RegOpenKeyExA",5,0x80000001,0x1000,0,0x20019,0x900)==0);
     W32(0x910,4);W32(0x914,0);W32(0x1300,0);assert(CALL("RegQueryValueExA",6,R32(0x900),0x1100,0,0x914,0x1300,0x910)==0);
     assert(R32(0x914)==4&&R32(0x1300)==3&&R32(0x910)==4);
     /* Host clean-exit policy replaces an existing string value only. */
     uint32_t w=create("Software\\Lionhead\\Persist",0xf003f,2);ansi(0x1100,"Game running:");ansi(0x1200,"Yes");assert(CALL("RegSetValueExA",6,w,0x1100,0,1,0x1200,4)==0);
     assert(movies_registry_replace_string(0x80000001u,"Software\\Lionhead\\Persist","Game running:","No"));
     assert(!movies_registry_replace_string(0x80000001u,"Software\\Lionhead\\Persist","Absent:","No"));
     assert(!movies_registry_replace_string(0x80000001u,"Software\\Missing","Game running:","No"));
     movies_registry_init(dir);ansi(0x1000,"Software\\Lionhead\\Persist");assert(CALL("RegOpenKeyExA",5,0x80000001,0x1000,0,0x20019,0x900)==0);
     W32(0x910,16);assert(CALL("RegQueryValueExA",6,R32(0x900),0x1100,0,0x914,0x1300,0x910)==0);assert(R32(0x914)==1&&!strcmp((char *)HP(0x1300),"No"));
     char path[600];snprintf(path,sizeof path,"%s/registry.v1",dir);remove(path);rmdir(dir);movies_registry_init(NULL);}
    puts("registry API tests passed");
}
