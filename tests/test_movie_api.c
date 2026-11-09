/* Movie playback COM objects: stdcall contracts per vtable slot, QI graph, pins and facets. */
#include "movie-api.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t *g_mem;
static GuestFn fns[512];static uint32_t addrs[512];static unsigned nfns;
void register_function(uint32_t a,GuestFn f){assert(nfns<512);addrs[nfns]=a;fns[nfns++]=f;}
GuestFn lookup_function(uint32_t a){for(unsigned i=0;i<nfns;i++)if(addrs[i]==a)return fns[i];return NULL;}
void guest_call(CPU *c,GuestFn fn,int n,const uint32_t *args){for(int i=n-1;i>=0;i--){c->esp-=4;W32(c->esp,args[i]);}c->esp-=4;W32(c->esp,0);fn(c);}
void guest_trap(CPU *c,uint32_t a,const char *m){(void)c;fprintf(stderr,"trap %08x %s\n",a,m);abort();}
uint32_t movies_backend_alloc(uint32_t n){static uint32_t next=0x200000;uint32_t a=next;next+=(n+15)&~15u;return a;}
int movies_backend_free(uint32_t a){(void)a;return 1;}
int movies_dsound_stream_open(uint32_t b){(void)b;return -1;}
void movies_dsound_stream_close(int s){(void)s;} void movies_dsound_stream_play(int s,int p){(void)s;(void)p;}
void movies_dsound_stream_reset(int s){(void)s;} void movies_dsound_stream_volume(int s,int32_t v){(void)s;(void)v;}
uint32_t movies_dsound_stream_space(int s){(void)s;return 0;} uint32_t movies_dsound_stream_write(int s,const uint8_t*p,uint32_t b){(void)s;(void)p;(void)b;return 0;}
uint64_t movies_dsound_stream_played(int s){(void)s;return 0;}
/* Call slot `slot` of object `o` with n arguments after `this`; asserts the callee popped exactly them. */
static uint32_t M(uint32_t o,unsigned slot,unsigned n,...){
    GuestFn f=lookup_function(R32(R32(o)+4*slot));assert(f);
    CPU c={0};c.esp=0x9000;W32(c.esp,0xdeadbeef);W32(c.esp+4,o);va_list ap;va_start(ap,n);for(unsigned i=0;i<n;i++)W32(c.esp+8+4*i,va_arg(ap,uint32_t));va_end(ap);
    f(&c);if(c.esp!=0x9000+4*(n+2)){fprintf(stderr,"slot %u popped %d bytes, expected %u\n",slot,(int)(c.esp-0x9000),4*(n+2));abort();}return c.eax;
}
static const uint8_t clsid[16]={0xa0,0x63,0x74,0x18,0xb7,0x5b,0xd3,0x11,0xac,0xbe,0x00,0x80,0xc7,0x5e,0x24,0x6e};
#define IID(x) {x,0x68,0xa8,0x56,0xd4,0x0a,0xce,0x11,0xb0,0x3a,0x00,0x20,0xaf,0x0b,0xa7,0x70}
static const uint8_t iid_control[16]=IID(0xb1),iid_position[16]=IID(0xb2),iid_audio[16]=IID(0xb3),iid_source[16]=IID(0xa6),iid_filter[16]=IID(0x95),iid_pin[16]=IID(0x91);
int main(void){
    g_mem=calloc(1,0x05000000);assert(g_mem);assert(movies_movie_init());
    memcpy(HP(0x1000),clsid,16);uint32_t reader=0;assert(movies_movie_create_reader(0x1000,&reader)==1&&reader);
    memcpy(HP(0x1010),iid_source,16);memcpy(HP(0x1020),iid_filter,16);memcpy(HP(0x1030),iid_pin,16);
    assert(M(reader,0,2,0x1010u,0x1100u)==0);uint32_t source=R32(0x1100);assert(source&&source!=reader);
    assert(M(source,0,2,0x1020u,0x1104u)==0&&R32(0x1104)==reader);
    assert(M(reader,1,0)&&M(reader,2,0));
    assert(M(reader,3,1,0x1110u)==0&&!memcmp(HP(0x1110),clsid,16));               /* GetClassID */
    assert(M(reader,4,0)==0&&M(reader,5,0)==0&&M(reader,6,2,0u,0u)==0);           /* Stop/Pause/Run */
    assert(M(reader,7,2,0u,0x1120u)==0&&R32(0x1120)==0);                           /* GetState */
    assert(M(reader,8,1,0u)==0&&M(reader,9,1,0x1124u)==0);                         /* Set/GetSyncSource */
    assert(M(reader,10,1,0x1128u)==0);uint32_t e=R32(0x1128);assert(e);            /* EnumPins */
    assert(M(e,3,3,1u,0x1130u,0x1134u)==0&&R32(0x1134)==1);uint32_t pin=R32(0x1130);assert(pin);
    assert(M(e,3,3,1u,0x1130u,0x1134u)==1&&R32(0x1134)==0);                         /* only video (not loaded: no audio) */
    assert(M(e,5,0)==0&&M(e,4,1,1u)==0);
    assert(M(reader,11,2,0x1140u,0x1144u)!=0&&M(reader,12,1,0x1200u)==0&&M(reader,13,2,0u,0u)==0);
    assert(M(source,3,2,0x1300u,0u)!=0);                                            /* Load of an empty name fails cleanly */
    assert(M(pin,0,2,0x1030u,0x1150u)==0&&R32(0x1150)==pin);
    assert(M(pin,6,1,0x1154u)==0x80040209u&&R32(0x1154)==0);                        /* ConnectedTo */
    assert(M(pin,7,1,0x1400u)==0x80040209u&&M(pin,8,1,0x1500u)==0&&R32(0x1504)==1); /* ConnectionMediaType, QueryPinInfo */
    assert(M(pin,9,1,0x1158u)==0&&R32(0x1158)==1&&M(pin,10,1,0x115cu)!=0&&M(pin,11,1,0x1400u)==1);
    assert(M(pin,4,2,0u,0u)!=0&&M(pin,5,0)==1&&M(pin,14,0)==0&&M(pin,15,0)==0&&M(pin,16,0)==0&&M(pin,17,6,0u,0u,0u,0u,0u,0u)==0);
    /* Playback graph facets come from a graph that holds a guest filter; none here. */
    memcpy(HP(0x1040),iid_control,16);memcpy(HP(0x1050),iid_position,16);memcpy(HP(0x1060),iid_audio,16);
    assert(movies_movie_query(0x7777,0x1040,0x1160)==0x80004002u);
    puts("movie playback COM stdcall and pin contract tests passed");
}
