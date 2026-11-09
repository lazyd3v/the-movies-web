#include "time-api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <setjmp.h>
uint8_t *g_mem;
static uint32_t error;
static jmp_buf trapped;
void movies_set_last_error(uint32_t value) { error=value; }
void guest_trap(CPU *c,uint32_t at,const char *why) { (void)c;(void)at;(void)why;longjmp(trapped,1); }
static uint32_t call(const char *name,unsigned argc,uint32_t a,uint32_t b) {
    CPU c={0};c.esp=0x100;c.ebx=0x12345678;W32(c.esp,0xdeadbeef);W32(c.esp+4,a);W32(c.esp+8,b);
    uint32_t count;ImportHandler *h=movies_time_handlers(&count);
    for(unsigned i=0;i<count;i++) if(!strcmp(name,h[i].name+13)) {
        h[i].fn(&c);assert(c.esp==0x104+4*argc && c.ebx==0x12345678);return c.eax;
    }
    assert(0);return 0;
}
static void system_date(unsigned y,unsigned m,unsigned d,unsigned h,unsigned min,unsigned sec,unsigned ms) {
    unsigned fields[]={y,m,999,d,h,min,sec,ms}; /*invalid weekday must be ignored*/
    for(unsigned i=0;i<8;i++)W16(0x1000+2*i,fields[i]);
}
int main(void) {
    g_mem=calloc(1,0x10000);assert(g_mem);
    system_date(1601,1,1,0,0,0,0);error=321;
    assert(call("SystemTimeToFileTime",2,0x1000,0x2000)==1 && R64(0x2000)==0 && error==321);
    assert(call("FileTimeToSystemTime",2,0x2000,0x1100)==1);
    assert(R16(0x1100)==1601 && R16(0x1102)==1 && R16(0x1104)==1 && R16(0x1106)==1);
    system_date(1970,1,1,0,0,0,0);assert(call("SystemTimeToFileTime",2,0x1000,0x2000));
    assert(R64(0x2000)==116444736000000000ull);
    assert(call("FileTimeToSystemTime",2,0x2000,0x1100));assert(R16(0x1104)==4);
    system_date(2000,2,29,12,34,56,789);assert(call("SystemTimeToFileTime",2,0x1000,0x2000));
    assert(R64(0x2000)==125963012967890000ull);
    W64(0x2000,R64(0x2000)+9999);assert(call("FileTimeToSystemTime",2,0x2000,0x1100));
    for(unsigned i=0;i<8;i++) if(i!=2)assert(R16(0x1100+2*i)==R16(0x1000+2*i));
    assert(R16(0x1104)==2); /*Tuesday*/
    system_date(1900,2,29,0,0,0,0);W64(0x2000,0x123456789abcdef0ull);
    assert(!call("SystemTimeToFileTime",2,0x1000,0x2000) && error==87 && R64(0x2000)==0x123456789abcdef0ull);
    system_date(2100,2,29,0,0,0,0);assert(!call("SystemTimeToFileTime",2,0x1000,0x2000));
    system_date(2400,2,29,0,0,0,0);assert(call("SystemTimeToFileTime",2,0x1000,0x2000));
    for(unsigned y=1601;y<=30827;y+=137) {
        system_date(y,12,31,23,59,59,999);assert(call("SystemTimeToFileTime",2,0x1000,0x2000));
        assert(call("FileTimeToSystemTime",2,0x2000,0x1100));
        for(unsigned i=0;i<8;i++) if(i!=2)assert(R16(0x1100+2*i)==R16(0x1000+2*i));
    }
    const unsigned invalid[][7]={{1600,1,1,0,0,0,0},{30828,1,1,0,0,0,0},{2000,0,1,0,0,0,0},
        {2000,13,1,0,0,0,0},{2000,4,31,0,0,0,0},{2000,1,0,0,0,0,0},{2000,1,1,24,0,0,0},
        {2000,1,1,0,60,0,0},{2000,1,1,0,0,60,0},{2000,1,1,0,0,0,1000}};
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++) {
        const unsigned *f=invalid[i];system_date(f[0],f[1],f[2],f[3],f[4],f[5],f[6]);
        assert(!call("SystemTimeToFileTime",2,0x1000,0x2000) && error==87);
    }
    W64(0x2000,0x7fffffffffffffffull);assert(call("FileTimeToSystemTime",2,0x2000,0x1100));
    W64(0x2000,0x8000000000000000ull);memset(HP(0x1100),0xa5,16);
    assert(!call("FileTimeToSystemTime",2,0x2000,0x1100) && error==87 && R32(0x1100)==0xa5a5a5a5);
    W64(0x2000,116444736000000000ull+12345);
    assert(call("FileTimeToLocalFileTime",2,0x2000,0x2010) && R64(0x2010)==R64(0x2000));
    assert(call("LocalFileTimeToFileTime",2,0x2010,0x2020) && R64(0x2020)==R64(0x2000));
    assert(!call("FileTimeToLocalFileTime",2,0x2000,0x2000) && error==87);
    assert(!call("FileTimeToLocalFileTime",2,0,0x2010) && error==87);
    assert(!call("FileTimeToSystemTime",2,0xfffc,0x1100) && error==87);
    assert(!call("SystemTimeToFileTime",2,0x1000,0xfffc) && error==87);
    assert(call("CompareFileTime",2,0x2000,0x2020)==0);
    W64(0x2020,R64(0x2000)+1);assert(call("CompareFileTime",2,0x2000,0x2020)==0xffffffffu);
    assert(call("CompareFileTime",2,0x2020,0x2000)==1);
    memset(HP(0x3000),0xa5,176);error=123;assert(call("GetTimeZoneInformation",1,0x3000,0)==0 && error==123);
    assert(R32(0x3000)==0 && R16(0x3004)=='U' && R16(0x3006)=='T' && R16(0x3008)=='C');
    assert(R32(0x3000+84)==0 && R32(0x3000+168)==0 && R32(0x3000+172)==0xa5a5a5a5);
    assert(R16(0x3000+68+2)==0 && R16(0x3000+152+2)==0);
    assert(call("GetTimeZoneInformation",1,0xfff0,0)==0xffffffffu && error==87);
    time_t before=time(NULL);call("GetLocalTime",1,0x1000,0);time_t after=time(NULL);
    assert(call("SystemTimeToFileTime",2,0x1000,0x2000));
    uint64_t unix_seconds=R64(0x2000)/10000000ull-11644473600ull;
    assert(unix_seconds>=(uint64_t)before && unix_seconds<=(uint64_t)after);
    if(!setjmp(trapped)) { call("CompareFileTime",2,0,0x2000);assert(0); }
    if(!setjmp(trapped)) { call("GetLocalTime",1,0,0);assert(0); }
    /* timeBeginPeriod/timeEndPeriod: TIMERR_NOERROR within [1,1000000], else TIMERR_NOCANDO. */
    {uint32_t count;ImportHandler *h=movies_time_handlers(&count);const char *names[]={"WINMM.DLL!timeBeginPeriod","WINMM.DLL!timeEndPeriod"};
     for(unsigned n=0;n<2;n++){GuestFn fn=NULL;for(unsigned i=0;i<count;i++)if(!strcmp(h[i].name,names[n]))fn=h[i].fn;assert(fn);
      const uint32_t periods[]={1,10,1000000,0,1000001};const uint32_t expected[]={0,0,0,97,97};
      for(unsigned i=0;i<5;i++){CPU c={0};c.esp=0x100;W32(0x100,0xdeadbeef);W32(0x104,periods[i]);error=55;fn(&c);assert(c.esp==0x108&&c.eax==expected[i]&&error==55);}}}
    free(g_mem);puts("UTC FILETIME, Gregorian calendar, bounds and timezone tests passed");
}
