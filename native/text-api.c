/* Contracts: learn.microsoft.com/en-us/windows/win32/api/stringapiset/
 * nf-stringapiset-multibytetowidechar and nf-stringapiset-widechartomultibyte.
 * Supports exact CP1252 conversion and UTF8, Microsoft CP1252 best-fit mappings; no normalization.
 * XP MB UTF8 permissive mode drops malformed input and sets last-error1113.
 * Malformed UTF16 without strict flag traps (XP/Vista behavior differs).
 */
#include "text-api.h"
#include "startup-api.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define ARG(n) R32(c->esp+4+4*(n))
#ifndef MOVIES_TEXT_SPACE
#define MOVIES_TEXT_SPACE GUEST_SPACE
#endif
static const uint16_t cp1252[32]={
 0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,
 0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,
 0x90,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
 0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
static void ret(CPU *c,unsigned n,uint32_t v) { c->eax=v;c->esp+=4*(n+1); }
static void error(CPU *c,unsigned n,uint32_t e) { movies_set_last_error(e);ret(c,n,0); }
static int range(uint32_t p,uint64_t n) { return p && (uint64_t)p+n<=MOVIES_TEXT_SPACE; }
static uint32_t page(uint32_t cp) { return !cp || cp==3 ?1252:cp; }
static int supported(uint32_t cp) { return cp==1252 || cp==65001; }
static int length(uint32_t p,int32_t n,unsigned width,uint32_t *out) {
    if(n==0 || n< -1 || !p)return 0;
    if(n>0) { *out=(uint32_t)n;return range(p,(uint64_t)*out*width); }
    uint64_t i=0;
    while(i<INT_MAX && range(p,(i+1)*width)) {
        uint32_t ch=width==1?R8(p+(uint32_t)i):R16(p+(uint32_t)i*2);
        if(!ch) { *out=(uint32_t)i+1;return 1; } ++i;
    }
    return 0;
}
static int overlap(uint32_t a,uint64_t n,uint32_t b,uint64_t m) {
    return n && m && (uint64_t)a+n>b && (uint64_t)b+m>a;
}
/* Consume one valid scalar. On malformed UTF8 consume only its valid prefix,
 * preserving the next ASCII/start byte for XP's drop-invalid behavior. */
static int decode(uint32_t p,uint32_t remaining,uint32_t *used,uint32_t *scalar) {
    uint32_t a=R8(p),n,min,v;
    *used=1;
    if(a<128) { *scalar=a;return 1; }
    if(a>=0xc2 && a<=0xdf) { n=2;min=0x80;v=a&31; }
    else if(a>=0xe0 && a<=0xef) { n=3;min=0x800;v=a&15; }
    else if(a>=0xf0 && a<=0xf4) { n=4;min=0x10000;v=a&7; }
    else return 0;
    for(uint32_t i=1;i<n;i++) {
        if(i>=remaining || (R8(p+i)&0xc0)!=0x80)return 0;
        v=(v<<6)|(R8(p+i)&63);*used=i+1;
    }
    if(v<min || v>0x10ffff || (v>=0xd800 && v<=0xdfff))return 0;
    *scalar=v;return 1;
}
static void multi(CPU *c) {
    uint32_t cp=page(ARG(0)),f=ARG(1),src=ARG(2),dst=ARG(4),len;
    int32_t capacity=(int32_t)ARG(5);
    if(!supported(cp) || !length(src,(int32_t)ARG(3),1,&len) || capacity<0 ||
       (capacity && !range(dst,(uint64_t)capacity*2))) { error(c,6,87);return; }
    if(f & ~(cp==65001?8u:9u)) { error(c,6,1004);return; }
    if(capacity && overlap(src,len,dst,(uint64_t)capacity*2)) { error(c,6,87);return; }
    uint64_t count=0;int invalid=0;
    /* Validate/size before writing so short buffers and strict errors are atomic. */
    for(unsigned pass=0;pass<2;pass++) {
        uint32_t output=0;
        for(uint32_t i=0;i<len;) {
            uint32_t u=1,v=R8(src+i);
            if(cp==65001 && !decode(src+i,len-i,&u,&v)) {
                if(f&8) { error(c,6,1113);return; }
                invalid=1;i+=u;continue;
            }
            if(cp==1252 && v>=128 && v<160)v=cp1252[v-128];
            i+=u;
            if(pass) {
                if(v>0xffff) { W16(dst+2*output++,0xd800+((v-0x10000)>>10));W16(dst+2*output++,0xdc00+((v-0x10000)&1023)); }
                else W16(dst+2*output++,(uint16_t)v);
            } else count+=v>0xffff?2:1;
        }
        if(!pass) {
            if(count>INT_MAX) { error(c,6,87);return; }
            if(!capacity)break;
            if(count>(uint32_t)capacity) { error(c,6,122);return; }
        }
    }
    if(invalid)movies_set_last_error(1113);
    ret(c,6,(uint32_t)count);
}
static int encode1252(uint32_t v) {
    if(v<128 || (v>=160 && v<=255))return (int)v;
    for(unsigned i=0;i<32;i++)if(cp1252[i]==v)return (int)i+128;
    return -1;
}
static int bestfit1252(uint32_t v) {
    static const struct { uint16_t unicode; uint8_t byte; } table[]={
#include "cp1252-bestfit.inc"
    };
    unsigned lo=0,hi=sizeof table/sizeof table[0];
    while(lo<hi) {
        unsigned mid=lo+(hi-lo)/2;
        if(table[mid].unicode<v)lo=mid+1;else hi=mid;
    }
    return lo<sizeof table/sizeof table[0] && table[lo].unicode==v?table[lo].byte:-1;
}
static void wide(CPU *c) {
    uint32_t cp=page(ARG(0)),f=ARG(1),src=ARG(2),dst=ARG(4),def=ARG(6),used=ARG(7),len;
    int32_t capacity=(int32_t)ARG(5);
    if(!supported(cp) || !length(src,(int32_t)ARG(3),2,&len) || capacity<0 ||
       (capacity && !range(dst,(uint32_t)capacity)) || (def && !range(def,1)) ||
       (used && !range(used,4)) || (cp==65001 && (def || used))) { error(c,8,87);return; }
    if(f & ~(cp==65001?0x80u:0x400u)) { error(c,8,1004);return; }
    if(capacity && overlap(src,(uint64_t)len*2,dst,(uint32_t)capacity)) { error(c,8,87);return; }
    uint64_t count=0;uint32_t default_used=0;
    uint8_t default_byte=def?R8(def):'?';
    for(unsigned pass=0;pass<2;pass++) {
        uint32_t output=0;
        for(uint32_t i=0;i<len;i++) {
            uint32_t v=R16(src+2*i);
            if(v>=0xd800 && v<=0xdbff && i+1<len && R16(src+2*(i+1))>=0xdc00 && R16(src+2*(i+1))<=0xdfff) {
                v=0x10000+((v-0xd800)<<10)+(R16(src+2*(i+1))-0xdc00);i++;
            } else if(v>=0xd800 && v<=0xdfff && cp==65001) {
                if(f&0x80) { error(c,8,1113);return; }
                guest_trap(c,R32(c->esp),"permissive malformed UTF16 conversion unavailable");return;
            }
            uint8_t bytes[4];unsigned n=1;
            if(cp==1252) {
                int b=encode1252(v);
                if(b<0 && !(f&0x400))b=bestfit1252(v);
                if(b<0) { b=default_byte;default_used=1; }
                bytes[0]=(uint8_t)b;
            } else if(v<0x80)bytes[0]=(uint8_t)v;
            else if(v<0x800) { n=2;bytes[0]=0xc0|(v>>6);bytes[1]=0x80|(v&63); }
            else if(v<0x10000) { n=3;bytes[0]=0xe0|(v>>12);bytes[1]=0x80|((v>>6)&63);bytes[2]=0x80|(v&63); }
            else { n=4;bytes[0]=0xf0|(v>>18);bytes[1]=0x80|((v>>12)&63);bytes[2]=0x80|((v>>6)&63);bytes[3]=0x80|(v&63); }
            if(pass)for(unsigned j=0;j<n;j++)W8(dst+output++,bytes[j]);else count+=n;
        }
        if(!pass) {
            if(count>INT_MAX) { error(c,8,87);return; }
            if(!capacity)break;
            if(count>(uint32_t)capacity) { error(c,8,122);return; }
        }
    }
    if(used)W32(used,default_used);ret(c,8,(uint32_t)count);
}
/* CT_CTYPE1 ASCII subset. Microsoft GetStringTypeW contracts plus Windows
 * conformance observations in Wine dlls/kernel32/tests/locale.c: C1_DEFINED
 * is additive on XP and later, including letters/control characters.
 * Actual MoviesSE CRT probe at AEA049 classifies UTF16 NUL at D7E360.
 */
static uint16_t ascii_type(uint16_t v) {
    uint16_t bits=0x200; /* C1_DEFINED */
    if(v<32 || v==127)bits|=0x20;
    if(v==' ' || (v>=9 && v<=13))bits|=8;
    if(v==' ' || v==9)bits|=0x40;
    if(v>='A' && v<='Z')bits|=0x101;
    if(v>='a' && v<='z')bits|=0x102;
    if(v>='0' && v<='9')bits|=4;
    if((v>='0' && v<='9') || (v>='A' && v<='F') || (v>='a' && v<='f'))bits|=0x80;
    if(v>=33 && v<=126 && !(bits&(0x100|4)))bits|=0x10;
    return bits;
}
/* C1 facts checked against Wine 11.0 tools/make_unicode categories and
 * special_categories, https://github.com/wine-mirror/wine/blob/wine-11.0/tools/make_unicode
 * Reference implementation copyright Alexandre Julliard, LGPL-2.1-or-later.
 * These explicit classification facts follow that modern Wine profile, including
 * soft hyphen CNTRL|PUNCT (XP differed); no claim of exhaustive XP NLS parity.
 */
static int western_type(uint16_t v) {
    static const uint16_t extras[32]={
        0x200,0x220,0x210,0x302,0x210,0x210,0x210,0x210,
        0x300,0x210,0x301,0x210,0x301,0x220,0x301,0x220,
        0x220,0x210,0x210,0x210,0x210,0x210,0x210,0x210,
        0x200,0x200,0x302,0x210,0x302,0x220,0x302,0x301,
    };
    if(v<128)return ascii_type(v);
    for(unsigned i=0;i<32;i++)if(v==cp1252[i])return extras[i];
    if(v<160 || v>255)return -1;
    uint16_t bits=0x200;
    if(v==160)return 0x248;
    if((v>=192 && v<=214) || (v>=216 && v<=222))return 0x301;
    if(v>=223 && v!=247)return 0x302;
    if(v==181)bits|=0x102;
    if(v==170 || v==186)bits|=0x102;
    if(v==178 || v==179 || v==185)bits|=4;
    if(v==173)bits|=0x20;
    /* Latin-1 punctuation plus Windows' symbol/numeric punctuation additions. */
    if((v>=161 && v<=191) || v==215 || v==247)bits|=0x10;
    return bits;
}
static void string_type(CPU *c) {
    uint32_t type=ARG(0),src=ARG(1),dst=ARG(3),len;
    int32_t n=(int32_t)ARG(2);
    if(type!=1) {
        if(type==2 || type==4)guest_trap(c,R32(c->esp),"GetStringTypeW CTYPE2/3 unavailable");
        else error(c,4,1004);
        return;
    }
    /* This API treats ANY negative count as NUL terminated. */
    if(!length(src,n<0?-1:n,2,&len) || !range(dst,(uint64_t)len*2) ||
       overlap(src,(uint64_t)len*2,dst,(uint64_t)len*2)) { error(c,4,87);return; }
    for(uint32_t i=0;i<len;i++)if(western_type(R16(src+2*i))<0) {
        guest_trap(c,R32(c->esp),"GetStringTypeW classification outside CP1252 repertoire unavailable");return;
    }
    for(uint32_t i=0;i<len;i++)W16(dst+2*i,(uint16_t)western_type(R16(src+2*i)));
    ret(c,4,1);
}
/* Simple (non-linguistic) case mappings from UnicodeData 17.0 fields 12/13:
 * https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt
 * Unicode data license: https://www.unicode.org/license.txt
 * Wine11.0 kernelbase/locale.c uses its default case table without
 * LCMAP_LINGUISTIC_CASING. No expansion of sharp-s to SS is performed.
 */
static uint16_t western_case(uint16_t v,int upper) {
    if(upper) {
        if((v>='a' && v<='z') || (v>=0xe0 && v<=0xf6) || (v>=0xf8 && v<=0xfe))return v-32;
        switch(v) {
            case 0xff:return 0x178;
            case 0x153:return 0x152;
            case 0x161:return 0x160;
            case 0x17e:return 0x17d;
            case 0x192:return 0x191;
            case 0xb5:return 0x39c;
        }
    } else {
        if((v>='A' && v<='Z') || (v>=0xc0 && v<=0xd6) || (v>=0xd8 && v<=0xde))return v+32;
        switch(v) {
            case 0x178:return 0xff;
            case 0x152:return 0x153;
            case 0x160:return 0x161;
            case 0x17d:return 0x17e;
        }
    }
    return v;
}
static void map_string(CPU *c) {
    uint32_t locale=ARG(0),flags=ARG(1),src=ARG(2),dst=ARG(4),len;
    int32_t n=(int32_t)ARG(3),capacity=(int32_t)ARG(5);
    if(flags!=0x100 && flags!=0x200) { error(c,6,1004);return; }
    /* Default guest locale=en-US. No Turkish/linguistic casing implied. */
    if(locale!=0 && locale!=0x409 && locale!=0x400 && locale!=0x800 && locale!=0x7f) {
        guest_trap(c,R32(c->esp),"LCMapStringW locale unavailable");return;
    }
    if(!length(src,n<0?-1:n,2,&len) || capacity<0 ||
       (capacity && !range(dst,(uint64_t)capacity*2))) { error(c,6,87);return; }
    if(capacity && src!=dst && overlap(src,(uint64_t)len*2,dst,(uint64_t)capacity*2)) { error(c,6,87);return; }
    for(uint32_t i=0;i<len;i++)if(western_type(R16(src+2*i))<0) {
        guest_trap(c,R32(c->esp),"LCMapStringW input outside CP1252 repertoire unavailable");return;
    }
    if(!capacity) { ret(c,6,len);return; }
    if((uint32_t)capacity<len) { error(c,6,122);return; }
    for(uint32_t i=0;i<len;i++)W16(dst+2*i,western_case(R16(src+2*i),flags==0x200));
    ret(c,6,len);
}
static void acp(CPU *c) { ret(c,0,1252); }
static void valid(CPU *c) { ret(c,1,supported(ARG(0))); }
static void info(CPU *c) {
    uint32_t cp=page(ARG(0)),p=ARG(1);
    if(!supported(cp) || !range(p,20)) { error(c,2,87);return; }
    memset(HP(p),0,20);W32(p,cp==65001?4:1);W8(p+4,'?');ret(c,2,1);
}
/* ANSI and comparison helpers over CP1252 / en-US (LCID 0x409).
 * https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-getstringtypea
 * https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-comparestringw
 * Comparison is case-folded code-point order, not the Windows linguistic sort. */
static int western_locale(uint32_t lcid) { return !lcid || lcid==0x409 || lcid==0x400 || lcid==0x800 || lcid==0x7f || lcid==0x9; }
static void string_type_a(CPU *c) {
    uint32_t type=ARG(1),src=ARG(2),dst=ARG(4),len;int32_t n=(int32_t)ARG(3);
    if(type!=1) { error(c,5,1004);return; }
    if(!length(src,n<0?-1:n,1,&len) || !range(dst,(uint64_t)len*2)) { error(c,5,87);return; }
    for(uint32_t i=0;i<len;i++) { int t=western_type(movies_cp1252_decode(R8(src+i)));W16(dst+2*i,(uint16_t)(t<0?0:t)); }
    ret(c,5,1);
}
static void map_string_a(CPU *c) {
    uint32_t flags=ARG(1),src=ARG(2),dst=ARG(4),len;int32_t n=(int32_t)ARG(3),capacity=(int32_t)ARG(5);
    if(flags!=0x100 && flags!=0x200) { error(c,6,1004);return; }
    if(!length(src,n<0?-1:n,1,&len) || capacity<0 || (capacity && !range(dst,(uint64_t)capacity))) { error(c,6,87);return; }
    if(!capacity) { ret(c,6,len);return; }
    if((uint32_t)capacity<len) { error(c,6,122);return; }
    for(uint32_t i=0;i<len;i++) {
        uint8_t b=R8(src+i);uint16_t m=western_case(movies_cp1252_decode(b),flags==0x200);int e=movies_cp1252_encode(m,0);
        W8(dst+i,e<0?b:(uint8_t)e);
    }
    ret(c,6,len);
}
static uint16_t text_char(uint32_t p,uint32_t i,int wide) { return wide?R16(p+2*i):movies_cp1252_decode(R8(p+i)); }
static uint32_t text_length(uint32_t p,int32_t n,int wide) {
    if(n>=0)return (uint32_t)n;
    uint32_t i=0;while(range(p,(uint64_t)(i+1)*(wide?2:1)) && text_char(p,i,wide))i++;return i;
}
static int compare_text(uint32_t a,uint32_t na,uint32_t b,uint32_t nb,int wide,int fold) {
    for(uint32_t i=0;i<na && i<nb;i++) {
        uint16_t x=text_char(a,i,wide),y=text_char(b,i,wide);
        if(fold) { x=western_case(x,0);y=western_case(y,0); }
        if(x!=y)return x<y?-1:1;
    }
    return na==nb?0:(na<nb?-1:1);
}
static void compare_string(CPU *c,int wide) {
    uint32_t flags=ARG(1),a=ARG(2),b=ARG(4);int32_t na=(int32_t)ARG(3),nb=(int32_t)ARG(5);
    if(!a || !b || na< -1 || nb< -1) { error(c,6,87);return; }
    uint32_t la=text_length(a,na,wide),lb=text_length(b,nb,wide);
    if(!range(a,(uint64_t)la*(wide?2:1)+ (la?0:1)) || !range(b,(uint64_t)lb*(wide?2:1)+(lb?0:1))) { error(c,6,87);return; }
    ret(c,6,(uint32_t)(2+compare_text(a,la,b,lb,wide,flags&1)));
}
static void compare_a(CPU *c) { compare_string(c,0); }static void compare_w(CPU *c) { compare_string(c,1); }
static void lstrcmpi_a(CPU *c) {
    uint32_t a=ARG(0),b=ARG(1);
    if(!a || !b) { ret(c,2,a?1:(b?(uint32_t)-1:0));return; }
    ret(c,2,(uint32_t)compare_text(a,text_length(a,-1,0),b,text_length(b,-1,0),0,1));
}
static void dbcs_lead(CPU *c) { ret(c,2,0); }
static void oem_cp(CPU *c) { ret(c,0,437); }
static void thread_locale(CPU *c) { ret(c,0,0x409); }
static void valid_locale(CPU *c) { ret(c,2,ARG(0)==0x409); }
/* en-US LOCALE_* values. https://learn.microsoft.com/en-us/windows/win32/intl/locale-information-constants */
static const char *locale_value(uint32_t type) {
    static const char *days[]={"Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"};
    static const char *sdays[]={"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    static const char *months[]={"January","February","March","April","May","June","July","August","September","October","November","December"};
    static const char *smonths[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    if(type>=0x2a && type<=0x30)return days[type-0x2a];
    if(type>=0x31 && type<=0x37)return sdays[type-0x31];
    if(type>=0x38 && type<=0x43)return months[type-0x38];
    if(type>=0x44 && type<=0x4f)return smonths[type-0x44];
    switch(type) {
        case 0x1:return "0409";case 0x2:return "English (United States)";case 0x3:return "ENU";case 0x4:return "English";
        case 0x5:return "1";case 0x6:return "United States";case 0x7:return "USA";case 0x8:return "United States";
        case 0x9:return "0409";case 0xa:return "1";case 0xb:return "437";case 0xc:return ",";case 0xd:return "1";
        case 0xe:return ".";case 0xf:return ",";case 0x10:return "3;0";case 0x11:return "2";case 0x12:return "1";
        case 0x13:return "0123456789";case 0x14:return "$";case 0x15:return "USD";case 0x16:return ".";case 0x17:return ",";
        case 0x18:return "3;0";case 0x19:return "2";case 0x1a:return "2";case 0x1b:return "0";case 0x1c:return "0";
        case 0x1d:return "/";case 0x1e:return ":";case 0x1f:return "M/d/yyyy";case 0x20:return "dddd, MMMM dd, yyyy";
        case 0x21:return "0";case 0x22:return "0";case 0x23:return "0";case 0x24:return "1";case 0x25:return "0";
        case 0x26:return "0";case 0x27:return "0";case 0x28:return "AM";case 0x29:return "PM";
        case 0x50:return "";case 0x51:return "-";case 0x52:return "3";case 0x53:return "0";case 0x54:return "1";
        case 0x55:return "0";case 0x56:return "1";case 0x57:return "0";case 0x59:return "en";case 0x5a:return "US";
        case 0x1001:return "English";case 0x1002:return "United States";case 0x1003:return "h:mm:ss tt";
        case 0x1004:return "1252";case 0x1009:return "1";case 0x100a:return "0";case 0x100b:return "0";
        case 0x100c:return "6";case 0x100d:return "0";case 0x1011:return "10000";case 0x1014:return "0";
        case 0x1016:return "MMMM, yyyy";case 0x1017:return "0";case 0x1019:return "1";
    }
    return NULL;
}
static void locale_info(CPU *c,int wide) {
    uint32_t type=ARG(1),out=ARG(2);int32_t capacity=(int32_t)ARG(3);
    int number=(type&0x20000000u)!=0;type&=~(0x80000000u|0x40000000u|0x20000000u);
    if(!western_locale(ARG(0))) { error(c,4,87);return; }
    const char *v=locale_value(type);
    if(!v) { static unsigned logged;if(logged++<16)fprintf(stderr,"[locale] GetLocaleInfo%s type %#x unavailable\n",wide?"W":"A",type);error(c,4,1004);return; }
    if(capacity<0) { error(c,4,87);return; }
    if(number) {
        uint32_t value=(uint32_t)strtoul(v,NULL,(type==1||type==9)?16:10),need=wide?2:4;
        if(!capacity) { ret(c,4,need);return; }
        if((uint32_t)capacity<need || !range(out,4)) { error(c,4,122);return; }
        W32(out,value);ret(c,4,need);return;
    }
    uint32_t n=(uint32_t)strlen(v)+1;
    if(!capacity) { ret(c,4,n);return; }
    if((uint32_t)capacity<n) { error(c,4,122);return; }
    if(!range(out,(uint64_t)n*(wide?2:1))) { error(c,4,87);return; }
    for(uint32_t i=0;i<n;i++) { if(wide)W16(out+2*i,(uint8_t)v[i]);else W8(out+i,(uint8_t)v[i]); }
    ret(c,4,n);
}
static void locale_a(CPU *c) { locale_info(c,0); }static void locale_w(CPU *c) { locale_info(c,1); }
/* One installed locale: the callback receives "00000409". The guest dispatcher
 * is injected so binaries linking this file without the runtime stay linkable. */
static GuestFn (*text_lookup)(uint32_t);
static void (*text_call)(CPU *,GuestFn,int,const uint32_t *);
void movies_text_set_dispatcher(GuestFn (*lookup)(uint32_t),void (*call)(CPU *,GuestFn,int,const uint32_t *)) { text_lookup=lookup;text_call=call; }
static void enum_locales(CPU *c) {
    uint32_t proc=ARG(0);GuestFn fn=proc && text_lookup && text_call?text_lookup(proc):NULL;
    if(!fn) { error(c,2,87);return; }
    if(c->esp<128) { error(c,2,87);return; }
    CPU nested=*c;nested.esp-=96;uint32_t text=nested.esp+80; /* 16 bytes below the caller's frame */
    memcpy(HP(text),"00000409",9);uint32_t args[]={text},before=nested.esp;
    text_call(&nested,fn,1,args);
    if(nested.esp!=before) { guest_trap(c,proc,"EnumSystemLocales callback violated stdcall stack contract");return; }
    ret(c,2,1);
}
/* wsprintf: %[-][0][width][.precision][l|h]{c,C,d,i,u,x,X,s,S,%}; output capped at 1024
 * characters. https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-wsprintfa */
static uint32_t format_a(uint32_t out,uint32_t fmt,uint32_t args) {
    char buffer[1025];uint32_t n=0;
#define PUT(ch) do { if(n<1024)buffer[n++]=(char)(ch); } while(0)
    for(uint32_t i=0;range(fmt+i,1) && R8(fmt+i);i++) {
        char ch=(char)R8(fmt+i);
        if(ch!='%') { PUT(ch);continue; }
        i++;int left=0,zero=0,width=0,precision=-1,wide=0,narrow=0;
        for(;;i++) { ch=(char)R8(fmt+i);if(ch=='-')left=1;else if(ch=='0')zero=1;else if(ch=='#')continue;else break; }
        while(ch>='0'&&ch<='9') { width=width*10+(ch-'0');ch=(char)R8(fmt+ ++i); }
        if(ch=='.') { precision=0;ch=(char)R8(fmt+ ++i);while(ch>='0'&&ch<='9') { precision=precision*10+(ch-'0');ch=(char)R8(fmt+ ++i); } }
        if(ch=='l'||ch=='h') { wide=ch=='l';narrow=ch=='h';ch=(char)R8(fmt+ ++i); }
        if(ch=='I' && R8(fmt+i+1)=='6' && R8(fmt+i+2)=='4') { i+=3;ch=(char)R8(fmt+i); }
        char text[64];const char *body=text;uint32_t blen=0,pointer=0;int is_wide=0;
        switch(ch) {
            case '%':PUT('%');continue;
            case 'c':case 'C':text[0]=(char)R32(args);args+=4;blen=1;break;
            case 'd':case 'i':blen=(uint32_t)snprintf(text,sizeof text,"%d",(int32_t)R32(args));args+=4;break;
            case 'u':blen=(uint32_t)snprintf(text,sizeof text,"%u",R32(args));args+=4;break;
            case 'x':case 'X':blen=(uint32_t)snprintf(text,sizeof text,ch=='x'?"%x":"%X",R32(args));args+=4;break;
            case 'p':blen=(uint32_t)snprintf(text,sizeof text,"%08X",R32(args));args+=4;break;
            case 's':case 'S':pointer=R32(args);args+=4;is_wide=(ch=='S'||wide) && !narrow;
                if(!pointer) { body="(null)";blen=6; }
                else { blen=0;while(range(pointer+blen*(is_wide?2:1),is_wide?2:1) && (is_wide?R16(pointer+2*blen):R8(pointer+blen)) && (precision<0||blen<(uint32_t)precision))blen++; body=NULL; }
                break;
            default:PUT('%');if(ch)PUT(ch);else i--;continue;
        }
        if(precision>=0 && body==text && ch!='c' && ch!='C' && ch!='s' && ch!='S' && blen<(uint32_t)precision) {
            char padded[64];int extra=precision-(int)blen;int neg=text[0]=='-';
            int k=0;if(neg)padded[k++]='-';for(int z=0;z<extra+(neg?1:0)&&k<63;z++)padded[k++]='0';
            memcpy(padded+k,text+neg,blen-neg);k+=(int)blen-neg;memcpy(text,padded,(size_t)k);blen=(uint32_t)k;
        }
        int pad=width>(int)blen?width-(int)blen:0;
        if(!left)while(pad-->0)PUT(zero && body==text?'0':' ');
        for(uint32_t k=0;k<blen;k++) {
            if(body)PUT(body[k]);
            else if(is_wide) { int e=movies_cp1252_encode(R16(pointer+2*k),1);PUT(e<0?'?':e); }
            else PUT(R8(pointer+k));
        }
        if(left)while(pad-->0)PUT(' ');
    }
#undef PUT
    buffer[n]=0;
    if(range(out,n+1))memcpy(HP(out),buffer,n+1);
    return n;
}
static void wsprintf_a(CPU *c) { c->eax=format_a(ARG(0),ARG(1),c->esp+12);c->esp+=4; } /* cdecl */
static void wvsprintf_a(CPU *c) { ret(c,3,format_a(ARG(0),ARG(1),ARG(2))); }
/* GetDateFormatA / GetTimeFormatA pictures (en-US). SYSTEMTIME: year, month,
 * day-of-week, day, hour, minute, second, ms as WORDs; NULL means local now. */
static void read_time(uint32_t p,unsigned v[8]) {
    if(p && range(p,16)) { for(unsigned i=0;i<8;i++)v[i]=R16(p+2*i);return; }
    time_t now=time(NULL);struct tm t;localtime_r(&now,&t);
    v[0]=(unsigned)t.tm_year+1900;v[1]=(unsigned)t.tm_mon+1;v[2]=(unsigned)t.tm_wday;v[3]=(unsigned)t.tm_mday;v[4]=(unsigned)t.tm_hour;v[5]=(unsigned)t.tm_min;v[6]=(unsigned)t.tm_sec;v[7]=0;
}
static unsigned weekday(unsigned y,unsigned m,unsigned d) { static const int t[]={0,3,2,5,0,3,5,1,4,6,2,4};if(m<3)y--;return (y+y/4-y/100+y/400+(unsigned)t[(m-1)%12]+d)%7; }
static void format_picture(char *out,size_t cap,const char *picture,const unsigned v[8],int date) {
    size_t n=0;
    for(size_t i=0;picture[i] && n+16<cap;) {
        char ch=picture[i];size_t run=1;while(picture[i+run]==ch)run++;
        if(ch=='\'') { i++;while(picture[i] && picture[i]!='\'' && n+1<cap)out[n++]=picture[i++];if(picture[i])i++;continue; }
        unsigned value=0;int handled=1;
        if(date && ch=='d') {
            unsigned wd=v[2]<7?v[2]:weekday(v[0],v[1],v[3]);
            if(run>=3)n+=(size_t)snprintf(out+n,cap-n,"%s",locale_value((run>=4?0x2a:0x31)+(wd+6)%7));
            else n+=(size_t)snprintf(out+n,cap-n,run==2?"%02u":"%u",v[3]);
        } else if(date && ch=='M') {
            if(run>=3 && v[1]>=1 && v[1]<=12)n+=(size_t)snprintf(out+n,cap-n,"%s",locale_value((run>=4?0x38:0x44)+v[1]-1));
            else n+=(size_t)snprintf(out+n,cap-n,run==2?"%02u":"%u",v[1]);
        } else if(date && ch=='y') {
            if(run<=2)n+=(size_t)snprintf(out+n,cap-n,run==2?"%02u":"%u",v[0]%100);else n+=(size_t)snprintf(out+n,cap-n,"%u",v[0]);
        } else if(date && ch=='g') n+=(size_t)snprintf(out+n,cap-n,"A.D.");
        else if(!date && (ch=='h'||ch=='H'||ch=='m'||ch=='s')) {
            value=ch=='h'?(v[4]%12?v[4]%12:12):ch=='H'?v[4]:ch=='m'?v[5]:v[6];
            n+=(size_t)snprintf(out+n,cap-n,run>=2?"%02u":"%u",value);
        } else if(!date && ch=='t') { const char *s=v[4]<12?"AM":"PM";n+=(size_t)snprintf(out+n,cap-n,"%.*s",run>=2?2:1,s); }
        else handled=0;
        if(!handled) { for(size_t k=0;k<run && n+1<cap;k++)out[n++]=ch; }
        i+=run;
    }
    out[n]=0;
}
static void date_time_format(CPU *c,int date) {
    uint32_t flags=ARG(1),st=ARG(2),picture=ARG(3),out=ARG(4);int32_t capacity=(int32_t)ARG(5);
    if(!western_locale(ARG(0)) || capacity<0) { error(c,6,87);return; }
    char pic[256];
    if(picture) { uint32_t i=0;for(;i<255 && range(picture+i,1) && R8(picture+i);i++)pic[i]=(char)R8(picture+i);pic[i]=0; }
    else strcpy(pic,date?((flags&2)?locale_value(0x20):locale_value(0x1f)):((flags&2)?"h:mm tt":locale_value(0x1003)));
    if(!date && (flags&8))strcpy(pic,"H:mm:ss");
    unsigned v[8];read_time(st,v);
    char text[512];format_picture(text,sizeof text,pic,v,date);
    uint32_t n=(uint32_t)strlen(text)+1;
    if(!capacity) { ret(c,6,n);return; }
    if((uint32_t)capacity<n) { error(c,6,122);return; }
    if(!range(out,n)) { error(c,6,87);return; }
    memcpy(HP(out),text,n);ret(c,6,n);
}
static void date_format_a(CPU *c) { date_time_format(c,1); }static void time_format_a(CPU *c) { date_time_format(c,0); }
ImportHandler *movies_text_handlers(uint32_t *count) {
    static ImportHandler h[]={
        {"KERNEL32.DLL!MultiByteToWideChar",multi},{"KERNEL32.DLL!WideCharToMultiByte",wide},
        {"KERNEL32.DLL!GetStringTypeW",string_type},
        {"KERNEL32.DLL!LCMapStringW",map_string},
        {"KERNEL32.DLL!GetACP",acp},{"KERNEL32.DLL!IsValidCodePage",valid},
        {"KERNEL32.DLL!GetCPInfo",info},
        {"KERNEL32.DLL!GetStringTypeA",string_type_a},{"KERNEL32.DLL!LCMapStringA",map_string_a},
        {"KERNEL32.DLL!CompareStringA",compare_a},{"KERNEL32.DLL!CompareStringW",compare_w},
        {"KERNEL32.DLL!lstrcmpiA",lstrcmpi_a},{"KERNEL32.DLL!IsDBCSLeadByteEx",dbcs_lead},
        {"KERNEL32.DLL!GetOEMCP",oem_cp},{"KERNEL32.DLL!GetThreadLocale",thread_locale},
        {"KERNEL32.DLL!GetUserDefaultLCID",thread_locale},{"KERNEL32.DLL!IsValidLocale",valid_locale},
        {"KERNEL32.DLL!GetLocaleInfoA",locale_a},{"KERNEL32.DLL!GetLocaleInfoW",locale_w},
        {"KERNEL32.DLL!EnumSystemLocalesA",enum_locales},
        {"USER32.DLL!wsprintfA",wsprintf_a},{"USER32.DLL!wvsprintfA",wvsprintf_a},
        {"KERNEL32.DLL!GetDateFormatA",date_format_a},{"KERNEL32.DLL!GetTimeFormatA",time_format_a}};
    *count=sizeof h/sizeof h[0];return h;
}

uint16_t movies_cp1252_decode(uint8_t byte) {
    return byte>=128 && byte<160?cp1252[byte-128]:byte;
}
int movies_cp1252_encode(uint32_t scalar,int bestfit) {
    int result=encode1252(scalar);
    return result<0 && bestfit?bestfit1252(scalar):result;
}
uint16_t movies_western_upper(uint16_t character) {
    return western_case(character,1);
}
