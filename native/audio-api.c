#include "audio-api.h"
#include <stdio.h>
#include <pthread.h>
#define LIMIT 256
#define EVENTS 512
#define DONE 1u
#define PREPARED 2u
#define INQUEUE 16u
#ifndef MOVIES_AUDIO_SPACE
#define MOVIES_AUDIO_SPACE GUEST_SPACE
#endif
static MoviesAudioHost host;
static pthread_t owner;
static int owned,pumping;
static uint32_t handle,serial,token,channels,callback,instance;
typedef struct Header {uint32_t address,data,bytes;int queued;} Header;
static Header headers[LIMIT];
typedef struct Event {uint32_t callback,handle,message,instance,header;} Event;
static Event events[EVENTS];static unsigned event_head,event_count;
static int range(uint32_t p,uint32_t n){return p&&(uint64_t)p+n<=MOVIES_AUDIO_SPACE;}
static void check(CPU*c,unsigned n){if(!owned||!pthread_equal(owner,pthread_self()))guest_trap(c,0,"waveOut called outside owner guest thread");if(!range(c->esp,4*(n+1)))guest_trap(c,0,"invalid waveOut stack");}
#define ARG(n) R32(c->esp+4+4*(n))
static void ret(CPU*c,unsigned n,uint32_t value){c->eax=value;c->esp+=4*(n+1);}
static unsigned queued(void){unsigned n=0;for(unsigned i=0;i<LIMIT;i++)n+=headers[i].queued!=0;return n;}
static Header *find(uint32_t p){for(unsigned i=0;i<LIMIT;i++)if(headers[i].address==p&&p)return &headers[i];return NULL;}
static void event(CPU*c,uint32_t message,uint32_t header){if(!callback)return;if(event_count==EVENTS)guest_trap(c,0,"waveOut callback queue overflow");events[(event_head+event_count++)%EVENTS]=(Event){callback,handle,message,instance,header};}
static void finish(CPU*c,Header*h){if(!range(h->address,32))guest_trap(c,0,"waveOut header invalid on completion");h->queued=0;W32(h->address+16,(R32(h->address+16)&~INQUEUE)|DONE|PREPARED);event(c,0x3bd,h->address);}
static void collect(CPU*c){if(!handle)return;uint32_t ids[LIMIT],n=0;if(!host.poll(token,ids,LIMIT,&n)||n>LIMIT)guest_trap(c,0,"waveOut backend polling failed");
 /* Validate entire completion batch before touching guest state. */
 for(unsigned i=0;i<n;i++){Header*h=find(ids[i]);if(!h||!h->queued)guest_trap(c,0,"waveOut backend returned unknown completion");for(unsigned j=0;j<i;j++)if(ids[j]==ids[i])guest_trap(c,0,"duplicate waveOut backend completion");}
 for(unsigned i=0;i<n;i++)finish(c,find(ids[i]));}
int movies_audio_set_host(const MoviesAudioHost*p){if(handle||event_count||(owned&&!pthread_equal(owner,pthread_self())))return 0;if(p&&(!p->available||!p->open||!p->write||!p->poll||!p->reset||!p->close))return 0;host=p?*p:(MoviesAudioHost){0};owner=pthread_self();owned=1;return 1;}
void movies_audio_pump(CPU*c){check(c,0);if(pumping)guest_trap(c,0,"recursive waveOut callback pump");pumping=1;collect(c);unsigned budget=event_count;
 while(budget--&&event_count){Event e=events[event_head];event_head=(event_head+1)%EVENTS;--event_count;GuestFn fn=lookup_function(e.callback);if(!fn)guest_trap(c,e.callback,"untranslated waveOut callback");if(c->esp<256||!range(c->esp-256,256))guest_trap(c,e.callback,"waveOut callback stack invalid");CPU nested=*c;nested.esp-=128;uint32_t before=nested.esp,args[]={e.handle,e.message,e.instance,e.header,0};guest_call(&nested,fn,5,args);if(nested.esp!=before)guest_trap(c,e.callback,"waveOut callback violated stdcall");}
 pumping=0;}
/* PlaySoundA(sound, module, flags): editor previews; logged, reported as played. */
static void play_sound(CPU*c){check(c,3);static unsigned logs;if(logs++<8)fprintf(stderr,"[audio] PlaySoundA flags=%08x caller=%08x: not played in the browser\n",ARG(2),R32(c->esp));ret(c,3,1);}
static void num_devs(CPU*c){check(c,0);uint32_t n=host.available&&host.available()?1:0;{static unsigned logs;if(logs++<4)fprintf(stderr,"[audio] waveOutGetNumDevs=%u caller=%08x\n",n,R32(c->esp));}ret(c,0,n);}
static void open_device(CPU*c){check(c,6);uint32_t out=ARG(0),device=ARG(1),format=ARG(2),cb=ARG(3),inst=ARG(4),flags=ARG(5);int query=flags&1;
 {static unsigned logs;if(logs++<8&&range(format,16))fprintf(stderr,"[audio] waveOutOpen device=%d tag=%u channels=%u rate=%u bits=%u flags=%08x caller=%08x\n",(int)device,R16(format),R16(format+2),R32(format+4),R16(format+14),flags,R32(c->esp));}
 if(device!=0&&device!=0xffffffffu){ret(c,6,2);return;}
 if((flags&~0x30009u)||((flags&0x30000u)!=0&&(flags&0x30000u)!=0x30000u)||(!query&&!range(out,4))||!range(format,16)||(!(flags&0x30000u)&&cb)){ret(c,6,11);return;}
 uint32_t ch=R16(format+2);if(R16(format)!=1||(ch!=1&&ch!=2)||R32(format+4)!=44100||R32(format+8)!=44100*ch*2||R16(format+12)!=ch*2||R16(format+14)!=16){ret(c,6,32);return;}
 if((flags&0x30000u)&&(!cb||!lookup_function(cb))){ret(c,6,11);return;}
 if(!host.available||!host.available()){ret(c,6,6);return;}if(query){ret(c,6,0);return;}if(handle){ret(c,6,4);return;}
 uint32_t next=host.open(ch);if(!next){fprintf(stderr,"[audio] host audio sink unavailable\n");ret(c,6,6);return;}fprintf(stderr,"[audio] waveOut device opened channels=%u\n",ch);token=next;handle=0x0c000000u|(++serial&0xffffffu);channels=ch;callback=cb;instance=inst;memset(headers,0,sizeof headers);W32(out,handle);event(c,0x3bb,0);ret(c,6,0);}
static int valid(CPU*c,unsigned argc){check(c,argc);if(!handle||ARG(0)!=handle){ret(c,argc,5);return 0;}return 1;}
static void prepare(CPU*c){if(!valid(c,3))return;uint32_t p=ARG(1);if(ARG(2)!=32||!range(p,32)){ret(c,3,11);return;}Header*h=find(p);if(h){ret(c,3,0);return;}if(!range(R32(p),R32(p+4))||!R32(p+4)||R32(p+4)>262144||R32(p+4)%(channels*2)){ret(c,3,11);return;}if(R32(p+16)&(INQUEUE|4|8)){ret(c,3,8);return;}
 for(unsigned i=0;i<LIMIT;i++)if(!headers[i].address){headers[i]=(Header){p,R32(p),R32(p+4),0};W32(p+16,R32(p+16)|PREPARED);ret(c,3,0);return;}ret(c,3,7);}
static void write_buffer(CPU*c){if(!valid(c,3))return;uint32_t p=ARG(1);if(ARG(2)!=32||!range(p,32)){ret(c,3,11);return;}Header*h=find(p);if(!h||!(R32(p+16)&PREPARED)){ret(c,3,34);return;}if(h->queued){ret(c,3,33);return;}if(R32(p)!=h->data||R32(p+4)!=h->bytes||(R32(p+16)&(4|8))){ret(c,3,8);return;}if(event_count+queued()+2>=EVENTS){ret(c,3,7);return;}if(!host.write(token,p,HP(h->data),h->bytes)){ret(c,3,7);return;}h->queued=1;W32(p+16,(R32(p+16)&~DONE)|PREPARED|INQUEUE);ret(c,3,0);}
static void unprepare(CPU*c){if(!valid(c,3))return;uint32_t p=ARG(1);if(ARG(2)!=32||!range(p,32)){ret(c,3,11);return;}Header*h=find(p);if(h&&h->queued){ret(c,3,33);return;}if(h)memset(h,0,sizeof *h);W32(p+16,R32(p+16)&~PREPARED);ret(c,3,0);}
static void reset_device(CPU*c){if(!valid(c,1))return;if(!host.reset(token)){guest_trap(c,0,"waveOut reset not acknowledged by renderer");return;}for(unsigned i=0;i<LIMIT;i++)if(headers[i].queued)finish(c,&headers[i]);ret(c,1,0);}
static void close_device(CPU*c){if(!valid(c,1))return;if(queued()){ret(c,1,33);return;}if(!host.close(token)){guest_trap(c,0,"waveOut backend release failed");return;}event(c,0x3bc,0);handle=token=0;memset(headers,0,sizeof headers);ret(c,1,0);}
static void get_caps(CPU*c){check(c,3);uint32_t dev=ARG(0),out=ARG(1),size=ARG(2);{static unsigned logs;if(logs++<4)fprintf(stderr,"[audio] waveOutGetDevCaps device=%d size=%u available=%d caller=%08x\n",(int)dev,size,host.available?host.available():-1,R32(c->esp));}if(dev!=0&&dev!=0xffffffffu&&(!handle||dev!=handle)){ret(c,3,2);return;}if(!host.available||!host.available()){ret(c,3,6);return;}if(size&&!range(out,size)){ret(c,3,11);return;}uint8_t caps[52]={0};memcpy(caps+8,"Movies Web PCM",14);uint32_t formats=0xc00;memcpy(caps+40,&formats,4);caps[44]=2;if(size)memcpy(HP(out),caps,size<52?size:52);ret(c,3,0);}
/* Mapper namespace contains exactly the live browser default-output sink.
 * No physical device enumeration or invented OS preference is exposed. */
static void message(CPU*c){check(c,4);
 if(ARG(0)!=0xffffffffu||ARG(1)!=0x2015u){ret(c,4,8);return;}
 if(!range(ARG(2),4)||!range(ARG(3),4)||ARG(2)==ARG(3)){ret(c,4,11);return;}
 W32(ARG(2),host.available&&host.available()?0:0xffffffffu);
 /* PREFERREDONLY: this implementation cannot fall back to another sink. */
 W32(ARG(3),1);ret(c,4,0);
}
/* Recording (voice-over) and the mixer: the browser build exposes no capture
 * device and no mixer, as on a PC without a microphone. The game probes
 * waveInGetDevCapsA(WAVE_MAPPER) at A347A5 and treats failure as "cannot record".
 * Device-ID calls fail with MMSYSERR_BADDEVICEID (2); handle calls with
 * MMSYSERR_INVALHANDLE (5), since no handle was ever issued.
 * https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveingetdevcaps */
static void capture_log(CPU*c,const char*name){static unsigned logs;if(logs++<8)fprintf(stderr,"[audio] %s caller=%08x: no recording device or mixer in the browser\n",name,R32(c->esp));}
static void wave_in_caps(CPU*c){check(c,3);capture_log(c,"waveInGetDevCapsA");ret(c,3,2);}
static void wave_in_open(CPU*c){check(c,6);capture_log(c,"waveInOpen");if(!(ARG(5)&1)&&range(ARG(0),4))W32(ARG(0),0);ret(c,6,2);}
static void wave_in_handle1(CPU*c){check(c,1);ret(c,1,5);}
static void wave_in_handle3(CPU*c){check(c,3);ret(c,3,5);}
static void mixer_count(CPU*c){check(c,0);capture_log(c,"mixerGetNumDevs");ret(c,0,0);}
static void mixer_caps(CPU*c){check(c,3);capture_log(c,"mixerGetDevCapsA");ret(c,3,ARG(0)>=0x10000u?5:2);}
static void mixer_open(CPU*c){check(c,5);capture_log(c,"mixerOpen");if(range(ARG(0),4))W32(ARG(0),0);ret(c,5,2);}
static void mixer_close(CPU*c){check(c,1);ret(c,1,5);}
/* hmxobj is a handle when MIXER_OBJECTF_HANDLE (0x80000000) is set, else a device ID. */
static void mixer_object(CPU*c){check(c,3);capture_log(c,"mixer line/control query");ret(c,3,(ARG(2)&0x80000000u)?5:2);}
ImportHandler *movies_audio_handlers(uint32_t*count){static ImportHandler handlers[]={
 {"WINMM.DLL!waveOutGetNumDevs",num_devs},{"WINMM.DLL!waveOutOpen",open_device},{"WINMM.DLL!waveOutPrepareHeader",prepare},{"WINMM.DLL!waveOutWrite",write_buffer},{"WINMM.DLL!waveOutUnprepareHeader",unprepare},{"WINMM.DLL!waveOutReset",reset_device},{"WINMM.DLL!waveOutClose",close_device},{"WINMM.DLL!waveOutGetDevCapsA",get_caps},{"WINMM.DLL!waveOutMessage",message},{"WINMM.DLL!PlaySoundA",play_sound},
 {"WINMM.DLL!waveInGetDevCapsA",wave_in_caps},{"WINMM.DLL!waveInOpen",wave_in_open},{"WINMM.DLL!waveInClose",wave_in_handle1},{"WINMM.DLL!waveInStart",wave_in_handle1},{"WINMM.DLL!waveInStop",wave_in_handle1},{"WINMM.DLL!waveInReset",wave_in_handle1},
 {"WINMM.DLL!waveInPrepareHeader",wave_in_handle3},{"WINMM.DLL!waveInUnprepareHeader",wave_in_handle3},{"WINMM.DLL!waveInAddBuffer",wave_in_handle3},
 {"WINMM.DLL!mixerGetNumDevs",mixer_count},{"WINMM.DLL!mixerGetDevCapsA",mixer_caps},{"WINMM.DLL!mixerOpen",mixer_open},{"WINMM.DLL!mixerClose",mixer_close},
 {"WINMM.DLL!mixerGetLineInfoA",mixer_object},{"WINMM.DLL!mixerGetLineControlsA",mixer_object},{"WINMM.DLL!mixerGetControlDetailsA",mixer_object},{"WINMM.DLL!mixerSetControlDetails",mixer_object}};*count=sizeof handlers/sizeof handlers[0];return handlers;}

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(int,audio_available_js,(),{return typeof Module.moviesAudioAvailable==='function'?Module.moviesAudioAvailable():0;});
EM_JS(uint32_t,audio_open_js,(uint32_t channels),{return Module.moviesAudioOpen(channels);});
EM_JS(int,audio_write_js,(uint32_t token,uint32_t header,const uint8_t *bytes,uint32_t length),{return Module.moviesAudioWrite(token,header,HEAPU8.subarray(bytes,bytes+length));});
EM_JS(int,audio_poll_js,(uint32_t token,uint32_t *ids,uint32_t capacity,uint32_t *count),{const result=Module.moviesAudioPoll(token);if(!Array.isArray(result)||result.length>capacity||result.some(id=>!Number.isInteger(id)||id<=0||id>0xffffffff))return 0;for(let i=0;i<result.length;i++)HEAPU32[(ids>>>2)+i]=result[i];HEAPU32[count>>>2]=result.length;return 1;});
EM_JS(int,audio_reset_js,(uint32_t token),{return Module.moviesAudioReset(token)===1?1:0;});
EM_JS(int,audio_close_js,(uint32_t token),{return Module.moviesAudioClose(token)===1?1:0;});
int movies_audio_init_browser(void){MoviesAudioHost provider={audio_available_js,audio_open_js,audio_write_js,audio_poll_js,audio_reset_js,audio_close_js};return movies_audio_set_host(&provider);}
#else
int movies_audio_init_browser(void){return movies_audio_set_host(NULL);}
#endif
