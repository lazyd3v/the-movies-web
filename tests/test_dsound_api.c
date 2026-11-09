/* DirectSound secondary buffer ring, cursor fallback, Lock wrap, class factory. */
#define _POSIX_C_SOURCE 200809L
#include "dsound-api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
uint8_t *g_mem;
static GuestFn fns[64];static uint32_t addrs[64];static unsigned nfns;
void register_function(uint32_t a,GuestFn f){addrs[nfns]=a;fns[nfns++]=f;}
void guest_trap(CPU *c,uint32_t a,const char *m){(void)c;(void)a;fprintf(stderr,"trap %s\n",m);abort();}
static uint32_t next=0x04018000;static unsigned frees;
static uint32_t alloc(uint32_t n){uint32_t a=next;next+=(n+15)&~15u;return a;}
static int release(uint32_t a){(void)a;frees++;return 1;}
static uint32_t method(uint32_t self,unsigned slot,unsigned n,...){
    uint32_t target=R32(R32(self)+4*slot);GuestFn f=NULL;for(unsigned i=0;i<nfns;i++)if(addrs[i]==target)f=fns[i];assert(f);
    CPU c={0};c.esp=0x1000;W32(c.esp,0xdeadbeef);W32(c.esp+4,self);
    __builtin_va_list ap;__builtin_va_start(ap,n);for(unsigned i=0;i<n;i++)W32(c.esp+8+4*i,__builtin_va_arg(ap,uint32_t));__builtin_va_end(ap);
    f(&c);assert(c.esp==0x1000+4*(n+2));return c.eax;
}
static const uint8_t iid_factory[16]={1,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
static const uint8_t iid_ks[16]={0x30,0xac,0xef,0x31,0x5c,0x51,0xd0,0x11,0xa9,0xaa,0x00,0xaa,0x00,0x61,0xbe,0x93};
int main(void){
    g_mem=calloc(1,0x04020000);assert(g_mem);
    uint32_t desc=0x2000,wfx=0x2100,out=0x2200;
    W32(desc,20);W32(desc+4,0x18000);W32(desc+8,17920);W32(desc+12,0);W32(desc+16,wfx);
    W16(wfx,1);W16(wfx+2,2);W32(wfx+4,44100);W32(wfx+8,176400);W16(wfx+12,4);W16(wfx+14,16);W16(wfx+16,0);
    W32(out,0x1234);assert(movies_dsound_create_buffer(desc,out)==0x80004001u&&R32(out)==0); /* not initialized */
    assert(movies_dsound_init(alloc,release));
    W32(desc+4,0x18010);assert(movies_dsound_create_buffer(desc,out)==0x8878001eu);W32(desc+4,0x18000);
    W32(wfx+4,22050);assert(movies_dsound_create_buffer(desc,out)==0x88780064u);W32(wfx+4,44100);
    assert(movies_dsound_create_buffer(desc,out)==0);uint32_t buf=R32(out);assert(buf);
    volatile int32_t *ctl=movies_dsound_control_block();
    assert(ctl[2]==17920&&ctl[3]==2&&ctl[6]==10000&&ctl[0]==0);
    /* Lock entire buffer, then a wrapped block. */
    uint32_t p1=0x2300,n1=0x2304,p2=0x2308,n2=0x230c;
    assert(method(buf,11,7,0u,0u,p1,n1,p2,n2,2u)==0&&R32(n1)==17920&&R32(p2)==0&&R32(n2)==0);
    uint32_t ring=R32(p1);assert(ring>=0x04018000);
    assert(method(buf,11,7,17920u-256,512u,p1,n1,p2,n2,0u)==0&&R32(p1)==ring+17920-256&&R32(n1)==256&&R32(p2)==ring&&R32(n2)==256);
    assert(method(buf,19,4,R32(p1),256u,R32(p2),256u)==0);
    assert(method(buf,11,7,17920u,4u,p1,n1,p2,n2,0u)==0x80070057u);
    /* Play: no renderer heartbeat, so the cursor follows the wall clock. */
    assert(method(buf,12,3,0u,0u,1u)==0&&ctl[0]==1);
    assert(method(buf,9,1,0x2400u)==0&&R32(0x2400)==5);
    struct timespec ts={0,150*1000000};nanosleep(&ts,NULL);
    assert(method(buf,4,2,0x2410u,0x2414u)==0);uint32_t play=R32(0x2410);assert(play>0&&play%4==0&&play<17920);
    /* With a heartbeat the renderer's cursor is authoritative. */
    ctl[5]++;ctl[4]=1000;assert(method(buf,4,2,0x2410u,0u)==0&&R32(0x2410)==4000);
    assert(method(buf,15,1,(uint32_t)-2000)==0&&ctl[6]==1000);
    assert(method(buf,18,0)==0&&ctl[0]==0);
    assert(method(buf,1,0)==2&&method(buf,2,0)==1&&method(buf,2,0)==0&&ctl[1]==0&&frees==1);
    /* dsound.dll class object -> IKsPropertySet with E_NOTIMPL Get. */
    uint32_t n;ImportHandler *h=movies_dsound_handlers(&n);assert(n==1);
    memcpy(HP(0x2500),iid_factory,16);memcpy(HP(0x2510),iid_ks,16);
    CPU c={0};c.esp=0x1000;W32(c.esp,0);W32(c.esp+4,0x2520);W32(c.esp+8,0x2500);W32(c.esp+12,0x2530);h[0].fn(&c);assert(c.eax==0&&c.esp==0x1010);
    uint32_t cf=R32(0x2530);assert(cf);
    assert(method(cf,3,3,0u,0x2510u,0x2534u)==0);uint32_t ks=R32(0x2534);assert(ks);
    W32(0x2540,0xffff);assert(method(ks,3,7,0x2510u,7u,0u,0u,0x2550u,8u,0x2540u)==0x80004001u&&R32(0x2540)==0);
    assert(method(ks,2,0)>=1&&method(cf,2,0)>=1);
    /* Host stream: bounded writes ahead of the cursor, progress, underrun resync. */
    int st=movies_dsound_stream_open(4096);assert(st>=0);
    assert(movies_dsound_stream_space(st)==4096);
    uint8_t pcm[8192];for(unsigned i=0;i<sizeof pcm;i++)pcm[i]=(uint8_t)(i|1);
    assert(movies_dsound_stream_write(st,pcm,8192)==4096&&movies_dsound_stream_space(st)==0);
    movies_dsound_stream_play(st,1);ctl[st*8+5]++;ctl[st*8+4]=256;
    assert(movies_dsound_stream_played(st)==256&&movies_dsound_stream_space(st)==1024);
    assert(movies_dsound_stream_write(st,pcm,2000)==1024);
    ctl[st*8+5]++;ctl[st*8+4]=0; /* cursor wrapped a full lap: played 1024 > written 1024? equal */
    assert(movies_dsound_stream_played(st)==1024);
    movies_dsound_stream_volume(st,-10000);assert(ctl[st*8+6]==0);movies_dsound_stream_volume(st,0);assert(ctl[st*8+6]==10000);
    movies_dsound_stream_reset(st);assert(movies_dsound_stream_played(st)==0&&movies_dsound_stream_space(st)==4096&&ctl[st*8]==0);
    movies_dsound_stream_close(st);assert(ctl[st*8+1]==0);
    free(g_mem);puts("DirectSound ring, cursor, lock and class factory tests passed");
}
