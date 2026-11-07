#include "audio-api.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
uint8_t *g_mem;static jmp_buf trap;static uint32_t handle,ready=1,host_open,host_queued[256],host_count,done_count,callback_count,messages[512],callback_headers[512],bad_poll;
static GuestFn imports[9];
void guest_trap(CPU*c,uint32_t address,const char*why){(void)c;(void)address;assert(why);longjmp(trap,1);}
static void callback_fn(CPU*c){assert(R32(c->esp+4)==handle);assert(R32(c->esp+12)==0x1234);assert(!R32(c->esp+20));messages[callback_count]=R32(c->esp+8);callback_headers[callback_count++]=R32(c->esp+16);c->esp+=24;c->eax=0xdead;}
GuestFn lookup_function(uint32_t p){return p==0xabcdef?callback_fn:NULL;}
void guest_call(CPU*c,GuestFn fn,int n,const uint32_t*args){for(int i=n-1;i>=0;i--){c->esp-=4;W32(c->esp,args[i]);}c->esp-=4;W32(c->esp,0);fn(c);}
static int available(void){return ready;}
static uint32_t open_host(uint32_t ch){assert(ch==2);host_open++;return 17;}
static int write_host(uint32_t token,uint32_t header,const uint8_t*data,uint32_t bytes){assert(token==17&&bytes==512&&data==HP(0x800));host_queued[host_count++]=header;return 1;}
static int poll_host(uint32_t token,uint32_t*ids,uint32_t cap,uint32_t*n){assert(token==17&&cap==256);if(bad_poll){ids[0]=0xbad;*n=1;return 1;}*n=done_count;memcpy(ids,host_queued,4*done_count);memmove(host_queued,host_queued+done_count,4*(host_count-done_count));host_count-=done_count;done_count=0;return 1;}
static int reset_host(uint32_t token){assert(token==17);host_count=done_count=0;return 1;}
static int close_host(uint32_t token){assert(token==17&&!host_count);host_open--;return 1;}
static uint32_t call(unsigned index,unsigned n,const uint32_t*args){CPU c={0};c.esp=0x1000;c.ebx=11;c.esi=22;c.edi=33;W32(c.esp,0x111);for(unsigned i=0;i<n;i++)W32(c.esp+4+4*i,args[i]);CPU expected=c;imports[index](&c);expected.esp+=4*(n+1);expected.eax=c.eax;assert(!memcmp(&c,&expected,sizeof c));return c.eax;}
#define CALL(i,...) call(i,sizeof((uint32_t[]){__VA_ARGS__})/4,(uint32_t[]){__VA_ARGS__})
static void pump(void){CPU c={0};c.esp=0x1000;c.eax=77;c.ebx=88;CPU old=c;movies_audio_pump(&c);assert(!memcmp(&c,&old,sizeof c));}
int main(void){g_mem=calloc(1,0x10000);assert(g_mem);uint32_t n;ImportHandler*h=movies_audio_handlers(&n);assert(n==9);for(unsigned i=0;i<n;i++)imports[i]=h[i].fn;
 MoviesAudioHost provider={available,open_host,write_host,poll_host,reset_host,close_host};assert(movies_audio_set_host(NULL));assert(call(0,0,NULL)==0);assert(movies_audio_set_host(&provider));assert(call(0,0,NULL)==1);
 W16(0x200,1);W16(0x202,2);W32(0x204,44100);W32(0x208,176400);W16(0x20c,4);W16(0x20e,16);
 assert(CALL(1,0,0xffffffff,0x200,0,0,9)==0&&!host_open);assert(CALL(1,0,0xffffffff,0x200,0,0,0)==11);
 W16(0x20e,8);assert(CALL(1,0,0xffffffff,0x200,0,0,9)==32);W16(0x20e,16);
 assert(CALL(1,0x300,0xffffffff,0x200,0xabcdef,0x1234,0x30000)==0);handle=R32(0x300);assert(handle&&host_open==1&&!callback_count);assert(!movies_audio_set_host(NULL));pump();assert(callback_count==1&&messages[0]==0x3bb);
 assert(CALL(7,handle,0x400,52)==0);assert(R32(0x428)==0xc00&&R16(0x42c)==2);assert(CALL(8,0xffffffff,0x2015,0x500,0x504)==0&&R32(0x500)==0&&R32(0x504)==1);
 ready=0;assert(CALL(8,0xffffffff,0x2015,0x500,0x504)==0&&R32(0x500)==0xffffffff);ready=1;
 W32(0x500,123);assert(CALL(8,0xffffffff,0x2015,0x500,0xffff)==11&&R32(0x500)==123);
 assert(CALL(8,handle,0x2015,0x500,0x504)==8);assert(CALL(8,0xffffffff,0x2016,0x500,0x504)==8);
 for(unsigned p=0x600;p<=0x620;p+=32){W32(p,0x800);W32(p+4,512);assert(CALL(3,handle,p,32)==34);assert(CALL(2,handle,p,32)==0);assert(R32(p+16)==2);assert(CALL(3,handle,p,32)==0);assert(R32(p+16)==18);assert(CALL(3,handle,p,32)==33);assert(CALL(4,handle,p,32)==33);}
 assert(host_count==2&&callback_count==1);assert(CALL(6,handle)==33);pump();assert(callback_count==1);done_count=1;pump();assert(callback_count==2&&messages[1]==0x3bd&&callback_headers[1]==0x600);assert(R32(0x610)==3&&R32(0x630)==18);
 assert(CALL(4,handle,0x600,32)==0&&R32(0x610)==1);assert(CALL(5,handle)==0);assert(R32(0x630)==3&&host_count==0);assert(callback_count==2);pump();assert(callback_count==3&&callback_headers[2]==0x620);
 assert(CALL(4,handle,0x620,32)==0);assert(CALL(6,handle)==0&&!host_open);assert(!movies_audio_set_host(NULL));pump();assert(callback_count==4&&messages[3]==0x3bc);assert(CALL(6,handle)==5);assert(movies_audio_set_host(NULL));
 assert(movies_audio_set_host(&provider));assert(CALL(1,0x300,0,0x200,0,0,0)==0);handle=R32(0x300);bad_poll=1;if(!setjmp(trap)){pump();assert(0);}assert(!callback_count||callback_count==4);
 free(g_mem);puts("audio ABI, actual-completion ownership, reset and callback tests passed");return 0;}
