#include "media-com.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
#include <pthread.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void,hold_reader_provider,(),{
 var names=['moviesMediaReaderAvailable','moviesMediaCreateReader','moviesMediaAppendReaderInput','moviesMediaOpenReader','moviesMediaReadReaderSample','moviesMediaSetReaderRange','moviesMediaReleaseReader'];
 Module.savedReaderFixtureProvider={};for(var n of names){Module.savedReaderFixtureProvider[n]=Module[n];delete Module[n];}
});
EM_JS(void,restore_reader_provider,(),{
 for(var n of Object.keys(Module.savedReaderFixtureProvider)){var f=Module.savedReaderFixtureProvider[n];if(f===undefined)delete Module[n];else Module[n]=f;}delete Module.savedReaderFixtureProvider;
});
#ifdef MOVIES_MEDIA_REAL_SERVICE
EM_JS(void,hold_real_service,(),{
 Module.mediaRealProviderFixture={create:Module.moviesMediaCreateWriter,release:Module.moviesMediaReleaseWriter,configure:Module.moviesMediaConfigureWriter};
 delete Module.moviesMediaCreateWriter;delete Module.moviesMediaReleaseWriter;delete Module.moviesMediaConfigureWriter;
});
EM_JS(void,setup_bridge,(),{
 var saved=Module.mediaRealProviderFixture,create=saved.create,release=saved.release,configure=saved.configure;
 if(typeof create!=="function"||typeof release!=="function"||typeof configure!=="function")throw new Error("Real media service must be ready before fixture main");
 Module.writerTest=new Set();
 Module.moviesMediaCreateWriter=()=>{var token=create();if(token)Module.writerTest.add(token);return token;};
 Module.moviesMediaReleaseWriter=t=>{var result=release(t);if(result===1||result===true)Module.writerTest.delete(t);return result;};
 Module.moviesMediaConfigureWriter=(t,p)=>configure(t,p);
});
#else
EM_JS(void,setup_bridge,(),{Module.writerTest=new Set();Module.moviesMediaCreateWriter=()=>{Module.writerTest.add(7);return 7;};Module.moviesMediaReleaseWriter=t=>Module.writerTest.delete(t);Module.moviesMediaConfigureWriter=(t,p)=>{if(!Module.writerTest.has(t)||p.videoCodec!=="wmv2"||p.audioCodec!=="wmav2"||p.width!==640||p.height!==480||p.fpsNumerator!==25)throw new Error("Bad actual profile bridge");return 1;};});
#endif
EM_JS(int,bridge_empty,(),{return Module.writerTest.size===0;});
#endif
uint8_t*g_mem;static GuestFn methods[58];static jmp_buf trapped;static unsigned allocated,released;static int available=1;
void register_function(uint32_t p,GuestFn fn){assert(p>=MOVIES_MEDIA_TRAMP&&(p-MOVIES_MEDIA_TRAMP)/16<58);methods[(p-MOVIES_MEDIA_TRAMP)/16]=fn;}
void guest_trap(CPU*c,uint32_t p,const char*why){(void)c;(void)p;(void)why;longjmp(trapped,1);}
static int create_backend(uint32_t*out){if(!available)return 0;*out=++allocated;return 1;}
static int release_backend(uint32_t token){assert(token&&token<=allocated);released++;return 1;}
static uint32_t invoke(GuestFn f,unsigned n,const uint32_t*a){CPU c={0};c.esp=0x800;c.ebx=123;W32(c.esp,0x123456);for(unsigned i=0;i<n;i++)W32(c.esp+4+4*i,a[i]);f(&c);assert(c.esp==0x804+4*n&&c.ebx==123);return c.eax;}
static uint32_t api(const char*name,unsigned n,const uint32_t*a){uint32_t count;ImportHandler*h=movies_media_handlers(&count);for(unsigned i=0;i<count;i++)if(!strcmp(strchr(h[i].name,'!')+1,name))return invoke(h[i].fn,n,a);assert(0);return 0;}
#define API(name,...) api(name,sizeof((uint32_t[]){__VA_ARGS__})/4,(uint32_t[]){__VA_ARGS__})
static uint32_t method(uint32_t o,unsigned slot,unsigned n,const uint32_t*a){uint32_t args[8]={o};assert(n<8);for(unsigned i=0;i<n;i++)args[i+1]=a[i];uint32_t p=R32(R32(o)+slot*4);assert(p>=MOVIES_MEDIA_TRAMP&&p<MOVIES_MEDIA_TRAMP+58*16);return invoke(methods[(p-MOVIES_MEDIA_TRAMP)/16],n+1,args);}
#define M(o,s,...) method(o,s,sizeof((uint32_t[]){__VA_ARGS__})/4,(uint32_t[]){__VA_ARGS__})
static void guid(uint32_t p,uint32_t a,uint16_t b,uint16_t c,const uint8_t d[8]){W32(p,a);W16(p+4,b);W16(p+6,c);memcpy(HP(p+8),d,8);}
static void ids(unsigned kind){
 static const uint8_t unk[]={0xc0,0,0,0,0,0,0,0x46},direct[]={0x9f,0x53,0,0x20,0xaf,0x0b,0xa7,0x70},iface[]={0xb0,0x3a,0,0x20,0xaf,0x0b,0xa7,0x70};
 guid(0x1000,0,0,0,unk);
 if(kind==1){guid(0x1100,0xe436ebb3,0x524f,0x11ce,direct);guid(0x1200,0x56a868a9,0x0ad4,0x11ce,iface);}
 if(kind==2){uint8_t cl[]={0xb3,0xf0,0,0xaa,0,0x37,0x61,0xc5},ii[]={0xab,0xfa,0,0xa0,0xc9,0xc6,0xe3,0x8d};guid(0x1100,0xbf87b6e1,0x8c27,0x11d0,cl);guid(0x1200,0x93e5a4e0,0x2d50,0x11d2,ii);}
 if(kind==3){uint8_t cl[]={0x8b,0x16,0,0xc0,0x4f,0xb6,0xbd,0x3d};guid(0x1100,0x7c23220e,0x55bb,0x11d3,cl);guid(0x1200,0x56a86895,0x0ad4,0x11ce,iface);}
 if(kind==12){uint8_t cl[]={0x93,0xbc,0x44,0x45,0x53,0x54,0,0},ii[]={0xa5,0x21,0,0x20,0xaf,0x0b,0xe5,0x60};guid(0x1100,0x47d4d946,0x62e8,0x11cf,cl);guid(0x1200,0x279afa83,0x4981,0x11ce,ii);}
}
static uint32_t make(unsigned kind){ids(kind);assert(!API("CoCreateInstance",0x1100,0,1,0x1200,0x2000));return R32(0x2000);}
static void name(const char*s){unsigned i=0;for(;s[i];i++)W16(0x3000+i*2,s[i]);W16(0x3000+i*2,0);}
static void *other_thread(void*p){(void)p;assert(API("CoInitialize",0)==0x8001010eu);return NULL;}
static int configured,configure_ok=1;
static int configure_backend(uint32_t token,const MoviesMediaProfile*p){assert(token&&token<=allocated);assert(p->width==640&&p->height==480&&p->fps_numerator==25&&p->fps_denominator==1&&p->video_bitrate==1000000&&p->sample_rate==44100&&p->channels==2&&p->audio_bitrate==128000);if(!configure_ok)return 0;configured++;return 1;}
static const char profile_xml[]=
"<profile version='458752' storageformat='1' name='A &amp; B &#x1F3AC;' description='Test'>"
"<streamconfig majortype='{73646976-0000-0010-8000-00AA00389B71}' streamnumber='1' bitrate='1000000'>"
"<wmmediatype subtype='{32564D57-0000-0010-8000-00AA00389B71}' bfixedsizesamples='0' btemporalcompression='1' lsamplesize='0'>"
"<videoinfoheader dwbitrate='1000000' avgtimeperframe='400000' dwbiterrorrate='0'>"
"<rcsource left='0' top='0' right='0' bottom='0'/><rctarget left='0' top='0' right='0' bottom='0'/>"
"<bitmapinfoheader biwidth='640' biheight='480' biplanes='1' bibitcount='24' bicompression='WMV2' bisizeimage='0'/>"
"</videoinfoheader></wmmediatype></streamconfig>"
"<!-- explicit real encoder profile -->"
"<streamconfig majortype='{73647561-0000-0010-8000-00AA00389B71}' streamnumber='2' bitrate='128000'>"
"<wmmediatype subtype='{00000161-0000-0010-8000-00AA00389B71}' bfixedsizesamples='1' btemporalcompression='0' lsamplesize='743'>"
"<waveformatex wFormatTag='353' nChannels='2' nSamplesPerSec='44100' nAvgBytesPerSec='16000' nBlockAlign='743' wBitsPerSample='16' codecdata='00000000010000000000'/>"
"</wmmediatype></streamconfig></profile>";
static void xml(const char*s){for(unsigned i=0;;i++){W16(0x10000+i*2,(unsigned char)s[i]);if(!s[i])return;}}
static uint32_t manager(void){assert(!API("WMCreateProfileManager",0x2000));return R32(0x2000);}
static uint32_t profile(uint32_t manager){xml(profile_xml);assert(!M(manager,5,0x10000,0x2000));return R32(0x2000);}
static uint32_t config_interface(uint32_t writer){const uint8_t suffix[]={0xb5,4,0x82,0x6b,0xb5,0x79,0x2a,0x3b};guid(0x1500,0x45086030,0xf7e4,0x486a,suffix);assert(!M(writer,0,0x1500,0x2000));return R32(0x2000);}
static void profile_tests(void){
 uint32_t manager_object=manager();assert(!M(manager_object,3,0x70000,0x2000));uint32_t empty=R32(0x2000);
 assert(!M(empty,3,0x2000)&&R32(0x2000)==0x70000);assert(!M(empty,8,0x2000)&&R32(0x2000)==0);
 assert(M(manager_object,3,123,0x2000)==0x80070057u&&R32(0x2000)==0);
 uint32_t p=profile(manager_object);assert(!M(p,8,0x2000)&&R32(0x2000)==2);
 W32(0x2100,0);assert(!M(p,4,0,0x2100)&&R32(0x2100)==9); /*UTF16 name includes surrogate pair+NUL*/
 memset(HP(0x2200),0x7b,32);W32(0x2100,2);assert(M(p,4,0x2200,0x2100)==0xc00d07d3u&&R32(0x2100)==9&&R32(0x2200)==0x7b7b7b7b);
 assert(!M(p,4,0x2200,0x2100)&&R16(0x2200)=='A'&&R16(0x2204)=='&'&&R16(0x220c)==0xd83c&&R16(0x220e)==0xdfac);
 uint32_t writer=make(3),config=config_interface(writer);assert(config==writer+4);
 assert(!M(config,0,0x1000,0x2000)&&R32(0x2000)==writer);M(writer,2);
 assert(M(config,8,0x2000)==0x80004005u&&R32(0x2000)==0);
 assert(M(config,7,empty)==0x80004001u);configure_ok=0;assert(M(config,7,p)==0x80004005u);assert(!configured);
 configure_ok=1;assert(!M(config,7,p)&&configured==1);assert(M(config,7,p)==0x8000ffffu);
 W16(0x2500,'X');W16(0x2502,0);assert(!M(p,5,0x2500));
 assert(!M(config,8,0x2000));uint32_t snapshot=R32(0x2000);W32(0x2100,32);assert(!M(snapshot,4,0x2200,0x2100)&&R16(0x2200)=='A');M(snapshot,2);
 M(config,2);M(writer,2);M(empty,2);M(p,2);
 char changed[sizeof profile_xml];memcpy(changed,profile_xml,sizeof changed);char*codec=strstr(changed,"32564D57");assert(codec);codec[1]='3';xml(changed);assert(M(manager_object,5,0x10000,0x2000)==0x80004001u&&R32(0x2000)==0); /*WMV3 cannot silently become WMV2*/
 const char*bad[]={"<profile version='458752' version='458752'/>","<profile version='458752'><streamconfig></profile>","<profile version='458752' name='&external;'/>","<profile version='458752' name='&#xD800;'/>","<profile version='458752'/><profile version='458752'/>","<profile version='458752' name='x'junk='y'/>","<profile version='458752'><!-- bad--comment --></profile>"};
 for(unsigned i=0;i<sizeof bad/sizeof*bad;i++){xml(bad[i]);assert(M(manager_object,5,0x10000,0x2000)==0x80070057u&&R32(0x2000)==0);}
 xml("<!DOCTYPE profile SYSTEM 'file:///secret'><profile version='458752'/>");assert(M(manager_object,5,0x10000,0x2000)==0x80004001u);
 xml("<profile version='458752' unknown='1'/>");assert(M(manager_object,5,0x10000,0x2000)==0x80004001u);
 W16(0x10000,0xd800);W16(0x10002,0);assert(M(manager_object,5,0x10000,0x2000)==0x80070057u);
 assert(M(manager_object,5,MOVIES_MEDIA_END-1,0x2000)==0x80070057u);
 xml("<profile version='458752' name='a\r\nb&#10;c'/>");assert(!M(manager_object,5,0x10000,0x2000));p=R32(0x2000);W32(0x2100,32);assert(!M(p,4,0x2200,0x2100)&&R16(0x2202)==' '&&R16(0x2206)=='\n');M(p,2);
 M(manager_object,2);
}
/* Isolated backend allocator models exact ownership, independent of COM slots. */
static uint32_t allocations[64],next_alloc=0x100000;static unsigned live_allocations;
uint32_t movies_backend_alloc(uint32_t n){for(unsigned i=0;i<64;i++)if(!allocations[i]){uint32_t p=next_alloc;next_alloc+=(n+7)&~7u;assert(next_alloc<0x3000000);allocations[i]=p;live_allocations++;memset(HP(p),0,n);return p;}return 0;}
int movies_backend_free(uint32_t p){for(unsigned i=0;i<64;i++)if(p&&allocations[i]==p){allocations[i]=0;live_allocations--;return 1;}return 0;}
static unsigned stream_refs=1,stream_pos,read_calls,reader_live;static int read_error,source_streams=1,sample_result=1;static uint64_t seek_start,seek_duration;static int seek_end;
static void stream_return(CPU*c,unsigned n,uint32_t r){c->eax=r;c->esp+=4+4*n;}
static void stream_add(CPU*c){assert(R32(c->esp+4)==0x7000);stream_return(c,1,++stream_refs);}
static void stream_release(CPU*c){assert(stream_refs>1);stream_return(c,1,--stream_refs);}
static void stream_seek(CPU*c){assert(!R32(c->esp+8)&&!R32(c->esp+12));unsigned origin=R32(c->esp+16);assert(origin==0||origin==2);stream_pos=origin?13:0;W64(R32(c->esp+20),stream_pos);stream_return(c,5,0);}
static void stream_read(CPU*c){read_calls++;if(read_error){stream_return(c,4,0x80004005u);return;}uint32_t n=R32(c->esp+12);if(n>3)n=3;if(n>13-stream_pos)n=13-stream_pos;for(unsigned i=0;i<n;i++)W8(R32(c->esp+8)+i,stream_pos+i);stream_pos+=n;W32(R32(c->esp+16),n);stream_return(c,4,0);}
GuestFn lookup_function(uint32_t a){switch(a){case 0x9001:return stream_add;case 0x9002:return stream_release;case 0x9003:return stream_read;case 0x9005:return stream_seek;default:return NULL;}}
void guest_call(CPU*c,GuestFn f,int n,const uint32_t*a){for(int i=n-1;i>=0;i--)PUSH32(c,a[i]);PUSH32(c,0x1234);f(c);}
static int reader_available(void){return 1;}
static uint32_t reader_create(uint32_t n){assert(n==13&&!reader_live);reader_live=1;return 17;}
static int reader_append(uint32_t t,uint32_t o,const uint8_t*b,uint32_t n){assert(t==17&&reader_live);for(unsigned i=0;i<n;i++)assert(b[i]==o+i);return 1;}
static int reader_open_backend(uint32_t t,MoviesMediaPCM*p){assert(t==17);*p=(MoviesMediaPCM){44100,2,16,4,8192,7,(uint32_t)source_streams,111220000};return 1;}
static int reader_read_backend(uint32_t t,uint8_t*b,uint32_t cap,uint32_t*n,uint64_t*pts,uint64_t*duration){assert(t==17&&cap==8192);if(sample_result!=1)return sample_result;for(unsigned i=0;i<16;i++)b[i]=(uint8_t)(255-i);*n=16;*pts=10000000;*duration=907;return 1;}
static int reader_range_backend(uint32_t t,uint64_t s,uint64_t d,int end){assert(t==17);seek_start=s;seek_duration=d;seek_end=end;return 1;}
static int reader_release_backend(uint32_t t){assert(t==17&&reader_live);reader_live=0;return 1;}
static void reader_tests(void){
 assert(API("WMCreateSyncReader",0,1,0x2000)==0x80040154u);
 MoviesMediaReaderHost host={reader_available,reader_create,reader_append,reader_open_backend,reader_read_backend,reader_range_backend,reader_release_backend};assert(movies_media_set_reader_host(&host));
 assert(!API("WMCreateSyncReader",0,1,0x2000));uint32_t reader=R32(0x2000);assert(!movies_media_set_reader_host(NULL));
 W32(0x7000,0x7100);for(unsigned i=0;i<6;i++)W32(0x7100+4*i,0x9000+i);
 read_error=1;assert(M(reader,23,0x7000)==0x80004005u&&stream_refs==1&&!reader_live&&!live_allocations);read_error=0;
 source_streams=2;assert(M(reader,23,0x7000)==0x80004001u&&stream_refs==1&&!reader_live&&!live_allocations);source_streams=1;
 assert(!M(reader,23,0x7000)&&stream_refs==2&&reader_live&&read_calls>=11&&!live_allocations);
 assert(!M(reader,14,0x2000)&&R32(0x2000)==1);assert(!M(reader,19,7,0x2000)&&R32(0x2000)==0);assert(!M(reader,20,0,0x2000)&&R16(0x2000)==7);
 assert(M(reader,19,1,0x2000)==0x80070057u);
 assert(!M(reader,18,0,0,0x2000));uint32_t props=R32(0x2000);W32(0x2100,0);assert(!M(props,4,0,0x2100)&&R32(0x2100)==90);W32(0x2100,89);assert(M(props,4,0x2200,0x2100)==0xc00d07d3u);assert(!M(props,4,0x2200,0x2100)&&R32(0x2244)==0x2248&&R16(0x224a)==2&&R32(0x224c)==44100);M(props,2);
 name("Duration");W16(0x2100,0);W16(0x2110,0);assert(!M(reader+4,5,0x2100,0x3000,0x2120,0,0x2110)&&R16(0x2110)==8&&R32(0x2120)==4);assert(!M(reader+4,5,0x2100,0x3000,0x2120,0x2200,0x2110)&&R64(0x2200)==111220000);
 assert(!M(reader,5,10000000,0,0,0)&&seek_start==10000000&&seek_duration==0&&seek_end);assert(!M(reader,5,10000000,0,5000000,0)&&!seek_end&&seek_duration==5000000);
 assert(!M(reader,7,0,0x2000,0x2100,0x2110,0x2120,0x2130,0x2140));uint32_t sample=R32(0x2000);assert(R64(0x2100)==10000000&&R64(0x2110)==907&&R32(0x2120)==1&&R32(0x2130)==0&&R16(0x2140)==7);
 assert(!M(sample,7,0x2000,0x2100)&&R32(0x2100)==16);uint32_t data=R32(0x2000);for(unsigned i=0;i<16;i++)assert(R8(data+i)==255-i);assert(M(sample,4,8193)==0x80070057u);assert(!M(sample,4,8));
 sample_result=0;assert(M(reader,7,0,0x2000,0x2100,0x2110,0x2120,0,0)==0x80004005u&&R32(0x2000)==0);sample_result=2;assert(M(reader,7,7,0x2000,0x2100,0x2110,0x2120,0,0)==0xc00d0bcfu&&R32(0x2000)==0);
 assert(!M(reader,4)&&stream_refs==1&&!reader_live&&live_allocations==1);assert(!M(reader,2));assert(R8(data)==255);assert(!M(sample,2)&&!live_allocations);assert(movies_media_set_reader_host(NULL));
}

#ifdef __EMSCRIPTEN__
EM_JS(void,setup_reader_bridge,(),{
 Module.readerFixture={live:false,offset:0,reads:0};var f=Module.readerFixture;
 Module.moviesMediaReaderAvailable=()=>true;
 Module.moviesMediaCreateReader=size=>{if(size!==13||f.live)throw new Error('reader creation contract');f.live=true;return 29;};
 Module.moviesMediaAppendReaderInput=(t,o,b)=>{if(t!==29||o!==f.offset||!(b instanceof Uint8Array))throw new Error('reader input ABI');for(var i=0;i<b.length;i++)if(b[i]!==o+i)throw new Error('reader input bytes');f.offset+=b.length;return 1;};
 Module.moviesMediaOpenReader=t=>{if(t!==29||f.offset!==13)throw new Error('reader open before upload');return {container:'asf',codec:'wmav2',format:'S16LE',sampleRate:44100,channels:2,bitsPerSample:16,blockAlign:4,maxSampleBytes:8192,audioStreamNumber:7,sourceStreamCount:1,containerDuration100ns:5000000000};};
 Module.moviesMediaReadReaderSample=(t,max)=>{if(t!==29||max!==8192)throw new Error('reader sample ABI');if(f.reads++)return {eos:true};return {eos:false,bytes:new Uint8Array([1,2,3,4]),start100ns:4500000000,duration100ns:227,sampleRate:44100,channels:2,bitsPerSample:16};};
 Module.moviesMediaSetReaderRange=(t,s,d)=>{if(t!==29||s!==4500000000||d!==null)throw new Error('reader 64-bit/range ABI');return 1;};
 Module.moviesMediaReleaseReader=t=>{if(t!==29||!f.live)throw new Error('reader release ABI');f.live=false;return 1;};
});
EM_JS(int,reader_bridge_released,(),{return !Module.readerFixture.live;});
static void reader_bridge_tests(void){
 setup_reader_bridge();assert(!API("WMCreateSyncReader",0,1,0x2000));uint32_t reader=R32(0x2000);
 assert(!M(reader,23,0x7000));assert(!M(reader,5,205032704,1,0,0));
 assert(!M(reader,7,0,0x2000,0x2100,0x2110,0x2120,0,0));uint32_t sample=R32(0x2000);assert(R64(0x2100)==4500000000ull);assert(!M(sample,7,0x2000,0x2100));assert(R32(R32(0x2000))==0x04030201&&R32(0x2100)==4);M(sample,2);
 assert(M(reader,7,0,0x2000,0x2100,0x2110,0x2120,0,0)==0xc00d0bcfu);M(reader,2);assert(reader_bridge_released()&&!live_allocations&&stream_refs==1);
}
#endif

int main(void){
#ifdef __EMSCRIPTEN__
 hold_reader_provider();
#endif
#if defined(__EMSCRIPTEN__) && defined(MOVIES_MEDIA_REAL_SERVICE)
 hold_real_service();
#endif
 uint32_t import_count;ImportHandler*imports=movies_media_handlers(&import_count);unsigned factory_count=0;for(unsigned i=0;i<import_count;i++)if(!strcmp(imports[i].name,"WMVCORE.DLL!WMCreateProfileManager"))factory_count++;assert(factory_count==1);
 g_mem=calloc(1,MOVIES_MEDIA_END);assert(g_mem);assert(movies_media_init());ids(1);
 assert(API("CoCreateInstance",0x1100,0,1,0x1200,0x2000)==0x800401f0u&&R32(0x2000)==0);
 assert(!API("CoInitialize",0));assert(API("CoInitialize",0)==1);
 assert(API("CoInitializeEx",0,0)==0x80010106u);assert(API("CoInitializeEx",1,2)==0x80070057u);
 assert(!movies_media_init());
 assert(API("CoCreateInstance",0x1100,0,1,0x1200,MOVIES_MEDIA_END-2)==0x80004003u);
 uint32_t graph=make(1),capture=make(2);ids(3);
 assert(API("CoCreateInstance",0x1100,0,1,0x1200,0x2000)==0x80040154u&&R32(0x2000)==0); /*no backend, no writer*/
 MoviesMediaHost host={create_backend,release_backend,configure_backend};assert(movies_media_set_host(&host));available=0;
 assert(API("CoCreateInstance",0x1100,0,1,0x1200,0x2000)==0x80040154u);available=1;
 uint32_t writer=make(3),writer2=make(3);assert(allocated==2&&!movies_media_set_host(NULL));
 if(!setjmp(trapped)){uint32_t wrong[]={capture,0,0};invoke(methods[6],3,wrong);assert(0);}
 assert(!M(capture,3,graph));assert(M(capture,3,graph)==0x8000ffffu);
 assert(!M(capture,4,0x2000)&&R32(0x2000)==graph);M(graph,2);
 assert(!M(writer,0,0x1000,0x2000)&&R32(0x2000)==writer);assert(M(writer,2)==1);
 memset(HP(0x1300),0xff,16);assert(M(writer,0,0x1300,0x2000)==0x80004002u&&R32(0x2000)==0);
 name("ASF Enc/Writer");assert(!M(graph,3,writer,0x3000));assert(M(graph,3,writer,0x3000)==0x80004005u);
 assert(M(graph,3,writer2,0x3000)==0x4022du);assert(!M(writer2,12,0x4000)&&R16(0x4000+28)==' ');M(graph,2);
 assert(!M(writer,7,100,0x2000)&&R32(0x2000)==0);assert(!M(writer,4));
 assert(!M(writer,3,0x2100)&&R32(0x2100)==0x7c23220e);
 assert(!M(writer,12,0x4000)&&R32(0x4100)==graph&&R16(0x4000)=='A');M(graph,2);
 assert(!M(graph,6,0x3000,0x2000)&&R32(0x2000)==writer);M(writer,2);
 assert(!M(graph,5,0x2000));uint32_t en=R32(0x2000);
 assert(!M(en,3,1,0x2200,0)&&R32(0x2200)==writer);M(writer,2);
 assert(!M(en,6,0x2000));uint32_t clone=R32(0x2000);
 assert(M(clone,3,2,0x2200,0x2300)==1&&R32(0x2300)==1&&R32(0x2200)==writer2);M(writer2,2);
 assert(!M(graph,4,writer2));assert(M(en,3,1,0x2200,0x2300)==0x80040203u&&R32(0x2300)==0);
 assert(!M(en,5));assert(M(en,4,2)==1);assert(!M(en,5));
 assert(M(en,3,2,0x2200,0)==0x80004003u);assert(M(en,3,0xffffffffu,0x2200,0x2300)==0x80004003u);
 M(en,2);M(clone,2);assert(M(writer2,2)==0&&released==1);
 if(!setjmp(trapped)){M(writer,6,0,0);assert(0);} /*Run cannot fake media*/
 pthread_t thread;assert(!pthread_create(&thread,NULL,other_thread,NULL));assert(!pthread_join(thread,NULL));
 assert(M(writer,2)==1); /*graph owns it*/assert(M(capture,2)==0);assert(M(graph,2)==0);assert(released==2);
 if(!setjmp(trapped)){invoke(methods[2],1,&writer);assert(0);} /*stale object cannot alias new allocation*/
 reader_tests();
#ifdef __EMSCRIPTEN__
 reader_bridge_tests();
#endif
 profile_tests();assert(allocated==released);
 API("CoUninitialize");API("CoUninitialize");assert(API("CoInitializeEx",0,0)==0x80004001u);
 assert(movies_media_init());assert(movies_media_set_host(NULL));
#ifdef __EMSCRIPTEN__
 setup_bridge();assert(!API("CoInitialize",0));writer=make(3);assert(!bridge_empty());uint32_t manager_object=manager(),p=profile(manager_object),cfg=config_interface(writer);assert(!M(cfg,7,p));M(cfg,2);M(p,2);M(manager_object,2);assert(M(writer,2)==0&&bridge_empty());API("CoUninitialize");
#endif
 /* DirectSound probe object used by the OpenAL router: create, Initialize, cooperative level, speaker config. */
 {assert(API("CoInitialize",0)<=1);uint32_t ds=make(12);assert(ds);assert(!M(ds,10,0u));assert(!M(ds,6,0x1234u,2u));W32(0x2100,0);assert(!M(ds,8,0x2100u)&&R32(0x2100)==4);
  W32(0x2200,96);assert(!M(ds,4,0x2200u));W32(0x2300,0xdead);assert(M(ds,3,0x2400u,0x2300u,0u)==0x80004001u&&R32(0x2300)==0);(void)0;}
 #ifdef __EMSCRIPTEN__
 restore_reader_provider();
 #endif
 free(g_mem);puts("media COM graph, profile XML, ownership, encoding restrictions and backend bridge tests passed");return 0;
}
