/* Contracts: learn.microsoft.com/en-us/windows/win32/api/stringapiset/
 * nf-stringapiset-multibytetowidechar and nf-stringapiset-widechartomultibyte.
 * Supports exact CP1252 conversion and UTF8, Microsoft CP1252 best-fit mappings; no normalization.
 * XP MB UTF8 permissive mode drops malformed input and sets last-error1113.
 * Malformed UTF16 without strict flag traps (XP/Vista behavior differs).
 */
#include "text-api.h"
#include "startup-api.h"
#include <limits.h>
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
ImportHandler *movies_text_handlers(uint32_t *count) {
    static ImportHandler h[]={
        {"KERNEL32.DLL!MultiByteToWideChar",multi},{"KERNEL32.DLL!WideCharToMultiByte",wide},
        {"KERNEL32.DLL!GetStringTypeW",string_type},
        {"KERNEL32.DLL!LCMapStringW",map_string},
        {"KERNEL32.DLL!GetACP",acp},{"KERNEL32.DLL!IsValidCodePage",valid},
        {"KERNEL32.DLL!GetCPInfo",info}};
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
