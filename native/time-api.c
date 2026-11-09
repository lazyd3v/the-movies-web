/* Private UTC calendar ABI. Official contracts:
 * https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-filetimetosystemtime
 * https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-systemtimetofiletime
 * https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-gettimezoneinformation
 * https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-filetimetolocalfiletime
 * https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-comparefiletime
 */
#include "time-api.h"
#include "startup-api.h"
#include <time.h>
#ifndef MOVIES_TIME_SPACE
#define MOVIES_TIME_SPACE GUEST_SPACE
#endif
#define TICKS_SECOND 10000000ull
#define TICKS_DAY (86400ull*TICKS_SECOND)
#define ARG(n) R32(c->esp+4+4*(n))
static void ret(CPU *c,unsigned n,uint32_t v) { c->eax=v;c->esp+=4*(n+1); }
static int range(uint32_t p,uint32_t n) { return p && (uint64_t)p+n<=MOVIES_TIME_SPACE; }
static uint64_t before_year(unsigned year) {
    uint64_t previous=year-1;
    return 365*previous+previous/4-previous/100+previous/400;
}
static int leap(unsigned y) { return y%4==0 && (y%100!=0 || y%400==0); }
static unsigned month_days(unsigned y,unsigned m) {
    static const unsigned char days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    return days[m-1]+(m==2 && leap(y));
}
static void decode(uint64_t ticks,uint16_t fields[8]) {
    uint64_t days=ticks/TICKS_DAY,remaining=ticks%TICKS_DAY;
    unsigned lo=1601,hi=65536;
    uint64_t absolute=days+before_year(1601);
    while(lo+1<hi) { unsigned mid=lo+(hi-lo)/2;if(before_year(mid)<=absolute)lo=mid;else hi=mid; }
    unsigned y=lo,m=1,day=(unsigned)(absolute-before_year(y));
    while(day>=month_days(y,m)) { day-=month_days(y,m);++m; }
    fields[0]=(uint16_t)y;fields[1]=(uint16_t)m;fields[2]=(uint16_t)((days+1)%7); /*1601-01-01 Monday*/
    fields[3]=(uint16_t)(day+1);fields[4]=(uint16_t)(remaining/(3600*TICKS_SECOND));
    remaining%=3600*TICKS_SECOND;fields[5]=(uint16_t)(remaining/(60*TICKS_SECOND));
    remaining%=60*TICKS_SECOND;fields[6]=(uint16_t)(remaining/TICKS_SECOND);
    fields[7]=(uint16_t)((remaining%TICKS_SECOND)/10000); /*truncate sub-millisecond ticks*/
}
static void write_system(uint32_t out,const uint16_t f[8]) { for(unsigned i=0;i<8;i++) W16(out+2*i,f[i]); }
static void file_to_system(CPU *c) {
    uint32_t in=ARG(0),out=ARG(1);
    if(!range(in,8) || !range(out,16)) { movies_set_last_error(87);ret(c,2,0);return; }
    uint64_t ticks=R64(in);
    if(ticks>=0x8000000000000000ull) { movies_set_last_error(87);ret(c,2,0);return; }
    uint16_t fields[8];decode(ticks,fields);write_system(out,fields);ret(c,2,1);
}
static void system_to_file(CPU *c) {
    uint32_t in=ARG(0),out=ARG(1);
    if(!range(in,16) || !range(out,8)) { movies_set_last_error(87);ret(c,2,0);return; }
    unsigned f[8];for(unsigned i=0;i<8;i++) f[i]=R16(in+2*i);
    if(f[0]<1601 || f[0]>30827 || f[1]<1 || f[1]>12 || f[3]<1 ||
       (f[1]>=1 && f[1]<=12 && f[3]>month_days(f[0],f[1])) || f[4]>23 || f[5]>59 || f[6]>59 || f[7]>999) {
        movies_set_last_error(87);ret(c,2,0);return;
    }
    uint64_t days=before_year(f[0])-before_year(1601)+f[3]-1;
    for(unsigned m=1;m<f[1];m++) days+=month_days(f[0],m);
    uint64_t ticks=days*TICKS_DAY+((uint64_t)f[4]*3600+f[5]*60+f[6])*TICKS_SECOND+f[7]*10000ull;
    W64(out,ticks);ret(c,2,1); /*wDayOfWeek intentionally ignored*/
}
static void utc_identity(CPU *c) {
    uint32_t in=ARG(0),out=ARG(1);
    /*Microsoft disallows identical input/output pointers for these conversions.*/
    if(!range(in,8) || !range(out,8) || in==out) { movies_set_last_error(87);ret(c,2,0);return; }
    uint64_t ticks=R64(in);W64(out,ticks);ret(c,2,1);
}
static void compare_file(CPU *c) {
    uint32_t a=ARG(0),b=ARG(1);
    if(!range(a,8) || !range(b,8)) { guest_trap(c,R32(c->esp),"CompareFileTime pointer outside guest memory");return; }
    uint64_t x=R64(a),y=R64(b);ret(c,2,x<y?0xffffffffu:x>y?1:0);
}
static void timezone_info(CPU *c) {
    uint32_t out=ARG(0);
    if(!range(out,172)) { movies_set_last_error(87);ret(c,1,0xffffffffu);return; }
    memset(HP(out),0,172); /*Bias, StandardBias, DaylightBias, both dates allzero*/
    const char *name="UTC";
    for(unsigned i=0;name[i];i++) { W16(out+4+2*i,(uint8_t)name[i]);W16(out+88+2*i,(uint8_t)name[i]); }
    ret(c,1,0); /*TIME_ZONE_ID_UNKNOWN means no DST transitions, not an error*/
}
static void local_time(CPU *c) {
    uint32_t out=ARG(0);struct timespec now;
    if(!range(out,16)) { guest_trap(c,R32(c->esp),"GetLocalTime output outside guest memory");return; }
    if(clock_gettime(CLOCK_REALTIME,&now) || now.tv_sec< -11644473600ll || now.tv_sec>910692730085ll) {
        guest_trap(c,R32(c->esp),"GetLocalTime realtime clock outside FILETIME range");return;
    }
    uint64_t ticks=(uint64_t)(now.tv_sec+11644473600ll)*TICKS_SECOND+(uint64_t)now.tv_nsec/100;
    if(ticks>=0x8000000000000000ull) { guest_trap(c,R32(c->esp),"GetLocalTime realtime clock overflow");return; }
    uint16_t f[8];decode(ticks,f);write_system(out,f);c->esp+=8;
}
/* Host timers already have millisecond-or-better resolution; the request is
 * accepted without changing scheduling. Periods outside [1,1000000] are
 * TIMERR_NOCANDO (97), the documented wPeriodMin/wPeriodMax bounds.
 * https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod */
static void time_period(CPU *c) { uint32_t period=ARG(0);ret(c,1,period>=1 && period<=1000000u?0:97); }
ImportHandler *movies_time_handlers(uint32_t *count) {
    static ImportHandler handlers[]={
        {"KERNEL32.DLL!FileTimeToLocalFileTime",utc_identity},
        {"KERNEL32.DLL!LocalFileTimeToFileTime",utc_identity},
        {"KERNEL32.DLL!FileTimeToSystemTime",file_to_system},
        {"KERNEL32.DLL!SystemTimeToFileTime",system_to_file},
        {"KERNEL32.DLL!CompareFileTime",compare_file},
        {"KERNEL32.DLL!GetTimeZoneInformation",timezone_info},
        {"KERNEL32.DLL!GetLocalTime",local_time},
        {"WINMM.DLL!timeBeginPeriod",time_period},{"WINMM.DLL!timeEndPeriod",time_period}
    };
    *count=sizeof handlers/sizeof handlers[0];return handlers;
}
