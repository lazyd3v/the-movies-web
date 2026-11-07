/* Browser-only acceptance fixture. Supply real compressed ASF bytes in
 * Module.moviesReaderFixtureBytes and the seven real reader service callbacks.
 * No decoder mocks are installed here. This exercises the actual C COM ABI. */
#include "media-com.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <emscripten.h>
uint8_t *g_mem;
static GuestFn methods[48];
static uint32_t source_size,position,refs=1,next_alloc=0x100000,allocations[64],live;
static unsigned stream_reads;
EM_JS(uint32_t,copy_source,(uint8_t * destination,uint32_t capacity),{
 var b=Module.moviesReaderFixtureBytes;
 if(!(b instanceof Uint8Array)||!b.length||b.length>capacity)throw new Error('Real reader fixture requires bounded compressed source bytes');
 HEAPU8.set(b,destination);return b.length;
});
EM_JS(void,publish_result,(uint32_t bytes,uint32_t hash,uint32_t range_bytes,uint32_t reads),{
 var result={ok:true,pcmBytes:bytes,pcmFNV1a:hash>>>0,rangeBytes:range_bytes,originalIStreamReadCalls:reads};
 console.log('MOVIES_REAL_READER_RESULT '+JSON.stringify(result));
 if(typeof Module.moviesReaderFixtureResult==='function')Module.moviesReaderFixtureResult(result);
});
void register_function(uint32_t a,GuestFn f){assert(a>=MOVIES_MEDIA_TRAMP&&(a-MOVIES_MEDIA_TRAMP)/16<48);methods[(a-MOVIES_MEDIA_TRAMP)/16]=f;}
void guest_trap(CPU*c,uint32_t a,const char*s){(void)c;fprintf(stderr,"reader fixture trap %08x: %s\n",a,s);abort();}
uint32_t movies_backend_alloc(uint32_t n){for(unsigned i=0;i<64;i++)if(!allocations[i]){uint32_t p=next_alloc;next_alloc+=(n+7)&~7u;assert(next_alloc<0x3000000);allocations[i]=p;live++;memset(HP(p),0,n);return p;}return 0;}
int movies_backend_free(uint32_t p){for(unsigned i=0;i<64;i++)if(p&&allocations[i]==p){allocations[i]=0;live--;return 1;}return 0;}
static void done(CPU*c,unsigned n,uint32_t r){c->eax=r;c->esp+=4+4*n;}
static void add(CPU*c){done(c,1,++refs);}static void release_stream(CPU*c){assert(refs>1);done(c,1,--refs);}
static void seek(CPU*c){assert(!R32(c->esp+8)&&!R32(c->esp+12));uint32_t origin=R32(c->esp+16);assert(origin==0||origin==2);position=origin?source_size:0;W64(R32(c->esp+20),position);done(c,5,0);}
static void read_stream(CPU*c){uint32_t n=R32(c->esp+12);if(n>source_size-position)n=source_size-position;memcpy(HP(R32(c->esp+8)),HP(0x3000000+position),n);position+=n;W32(R32(c->esp+16),n);stream_reads++;done(c,4,0);}
GuestFn lookup_function(uint32_t a){switch(a){case 0x9001:return add;case 0x9002:return release_stream;case 0x9003:return read_stream;case 0x9005:return seek;default:return NULL;}}
void guest_call(CPU*c,GuestFn f,int n,const uint32_t*a){for(int i=n-1;i>=0;i--)PUSH32(c,a[i]);PUSH32(c,0x1234);f(c);}
static uint32_t invoke(GuestFn f,unsigned n,const uint32_t*a){CPU c={0};c.esp=0x800;W32(c.esp,0x1234);for(unsigned i=0;i<n;i++)W32(c.esp+4+4*i,a[i]);f(&c);assert(c.esp==0x804+4*n);return c.eax;}
static uint32_t api(const char*name,unsigned n,const uint32_t*a){uint32_t count;ImportHandler*h=movies_media_handlers(&count);for(unsigned i=0;i<count;i++)if(!strcmp(strchr(h[i].name,'!')+1,name))return invoke(h[i].fn,n,a);abort();}
#define API(name,...) api(name,sizeof((uint32_t[]){__VA_ARGS__})/4,(uint32_t[]){__VA_ARGS__})
static uint32_t method(uint32_t o,unsigned slot,unsigned n,const uint32_t*a){uint32_t args[8]={o};assert(n<8);for(unsigned i=0;i<n;i++)args[i+1]=a[i];uint32_t p=R32(R32(o)+4*slot);assert(p>=MOVIES_MEDIA_TRAMP&&p<MOVIES_MEDIA_TRAMP+48*16);return invoke(methods[(p-MOVIES_MEDIA_TRAMP)/16],n+1,args);}
#define M(o,s,...) method(o,s,sizeof((uint32_t[]){__VA_ARGS__})/4,(uint32_t[]){__VA_ARGS__})
static uint32_t consume(uint32_t reader,uint32_t*hash){uint32_t total=0;uint64_t previous=0;for(unsigned iterations=0;iterations<1000;iterations++){
 uint32_t hr=M(reader,7,0,0x2000,0x2100,0x2110,0x2120,0x2130,0x2140);if(hr==0xc00d0bcfu)return total;assert(!hr&&R32(0x2130)==0&&R16(0x2140)==1);uint32_t sample=R32(0x2000);uint64_t pts=R64(0x2100);assert(pts>=previous&&R64(0x2110)>0);previous=pts;
 assert(!M(sample,7,0x2000,0x2100));uint32_t bytes=R32(0x2100),data=R32(0x2000);assert(bytes&&bytes<=8192&&!(bytes%4));for(unsigned i=0;i<bytes;i++)*hash=(*hash^R8(data+i))*16777619u;total+=bytes;assert(!M(sample,2));
 }abort();}
int main(void){
 g_mem=calloc(1,MOVIES_MEDIA_END);assert(g_mem);source_size=copy_source(HP(0x3000000),0x100000);assert(source_size==97317);assert(movies_media_init());assert(!API("CoInitialize",0));
 W32(0x7000,0x7100);for(unsigned i=0;i<6;i++)W32(0x7100+4*i,0x9000+i);
 assert(!API("WMCreateSyncReader",0,1,0x2000));uint32_t reader=R32(0x2000);assert(!M(reader,23,0x7000)&&refs==2);
 assert(!M(reader,18,0,0,0x2000));uint32_t props=R32(0x2000);W32(0x2100,90);assert(!M(props,4,0x2200,0x2100)&&R16(0x224a)==2&&R32(0x224c)==44100);assert(!M(props,2));
 uint32_t hash=2166136261u,total=consume(reader,&hash);assert(total==1900544&&hash==0x9959043au);assert(!M(reader,5,10000000,0,5000000,0));uint32_t range_hash=2166136261u,range_bytes=consume(reader,&range_hash);assert(range_bytes==88200);
 assert(!M(reader,4)&&refs==1&&!live);assert(!M(reader,2));API("CoUninitialize");free(g_mem);publish_result(total,hash,range_bytes,stream_reads);return 0;
}
