#include "audio-api.h"
#include <emscripten.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
uint8_t *g_mem;static GuestFn imports[9];static uint32_t handle,done,opened,closed;
void guest_trap(CPU*c,uint32_t at,const char*why){(void)c;fprintf(stderr,"audio trap %x: %s\n",at,why);abort();}
static void callback(CPU*c){assert(R32(c->esp+4)==handle);assert(R32(c->esp+12)==0x1234);uint32_t msg=R32(c->esp+8);if(msg==0x3bd){assert(R32(R32(c->esp+16)+16)&1);done++;}else if(msg==0x3bb)opened++;else if(msg==0x3bc)closed++;else abort();c->esp+=24;}
GuestFn lookup_function(uint32_t p){return p==0xabcdef?callback:NULL;}
void guest_call(CPU*c,GuestFn fn,int n,const uint32_t*args){for(int i=n-1;i>=0;i--){c->esp-=4;W32(c->esp,args[i]);}c->esp-=4;W32(c->esp,0);fn(c);}
EM_JS(int,available,(),{return Module.audioSink?.context.state==='running'?1:0;});
EM_JS(uint32_t,open_host,(uint32_t channels),{return channels===2&&Module.audioSink?1:0;});
EM_JS(int,write_host,(uint32_t token,uint32_t header,const uint8_t *pcm,uint32_t bytes),{try{Module.audioSink.producer.write(header,new Int16Array(HEAPU8.buffer,pcm,bytes/2));return 1;}catch(e){Module.audioError=String(e);console.error(Module.audioError);return 0;}});
EM_JS(int,poll_host,(uint32_t token,uint32_t *ids,uint32_t capacity,uint32_t *count),{try{const state=Module.audioSink.producer.poll();if(state.buffers.length>capacity||state.buffers.some(b=>b.status!=='played')){Module.audioError='Unexpected completion batch: '+JSON.stringify(state);console.error(Module.audioError);return 0;}for(let i=0;i<state.buffers.length;i++)HEAPU32[(ids>>>2)+i]=state.buffers[i].id;HEAPU32[count>>>2]=state.buffers.length;Module.lastAudioStats=state;return 1;}catch(e){Module.audioError=String(e);console.error(Module.audioError);return 0;}});
/* Fixture's page performs the real async reset first. Production must do this
 * handshake synchronously on a guest worker, never wait on the page thread. */
EM_JS(int,reset_host,(uint32_t token),{if(!Module.audioResetAcknowledged)return 0;Module.audioResetAcknowledged=false;return 1;});
EM_JS(int,close_host,(uint32_t token),{if(Module.audioSink.producer.poll().queuedBuffers!==0)return 0;return Module.audioSink.producer.close().length===0?1:0;});
static uint32_t call(unsigned index,unsigned n,const uint32_t*args){CPU c={0};c.esp=0x1000;W32(c.esp,0);for(unsigned i=0;i<n;i++)W32(c.esp+4+i*4,args[i]);imports[index](&c);assert(c.esp==0x1000+4*(n+1));return c.eax;}
#define CALL(i,...) call(i,sizeof((uint32_t[]){__VA_ARGS__})/4,(uint32_t[]){__VA_ARGS__})
EMSCRIPTEN_KEEPALIVE unsigned audio_poll(void){CPU c={0};c.esp=0x1000;movies_audio_pump(&c);return done;}
EMSCRIPTEN_KEEPALIVE void audio_start(void){g_mem=calloc(1,0x10000);assert(g_mem);uint32_t n;ImportHandler*h=movies_audio_handlers(&n);assert(n==9);for(unsigned i=0;i<n;i++)imports[i]=h[i].fn;
#ifdef MOVIES_AUDIO_PRODUCTION
 assert(movies_audio_init_browser());
#else
 MoviesAudioHost host={available,open_host,write_host,poll_host,reset_host,close_host};assert(movies_audio_set_host(&host));
#endif
assert(call(0,0,NULL)==1);assert(CALL(8,0xffffffff,0x2015,0x500,0x504)==0&&R32(0x500)==0&&R32(0x504)==1);
 W16(0x200,1);W16(0x202,2);W32(0x204,44100);W32(0x208,176400);W16(0x20c,4);W16(0x20e,16);assert(CALL(1,0,0xffffffff,0x200,0,0,9)==0);assert(CALL(1,0x300,0xffffffff,0x200,0xabcdef,0x1234,0x30000)==0);handle=R32(0x300);audio_poll();assert(opened==1);
 for(unsigned frame=0;frame<128;frame++){int16_t x=(int16_t)(8192*sin(2*3.141592653589793*440*frame/44100));W16(0x800+frame*4,x);W16(0x802+frame*4,x);}
 for(unsigned i=0;i<64;i++){uint32_t p=0x2000+i*32;W32(p,0x800);W32(p+4,512);assert(CALL(2,handle,p,32)==0);assert(CALL(3,handle,p,32)==0);}assert(done==0);assert(CALL(6,handle)==33);
}
EMSCRIPTEN_KEEPALIVE void audio_queue_reset(void){assert(done==64);for(unsigned i=0;i<8;i++)assert(CALL(3,handle,0x2000+i*32,32)==0);}
EMSCRIPTEN_KEEPALIVE void audio_finish_reset(void){assert(CALL(5,handle)==0);assert(done==64);audio_poll();assert(done==72);for(unsigned i=0;i<64;i++)assert(CALL(4,handle,0x2000+i*32,32)==0);assert(CALL(6,handle)==0);audio_poll();assert(closed==1);assert(movies_audio_set_host(NULL));free(g_mem);g_mem=NULL;}
