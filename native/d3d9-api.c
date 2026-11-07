/* Bounded original-game D3D9 frontend, not the upstream D3D8 shim.
 * ABI: Wine wine-11.0 d3d9.h/d3d9types.h/d3d9caps.h (definitions inspected,
 * implementation original). Contracts: Microsoft Learn /windows/win32/api/d3d9/
 * nf-d3d9-idirect3d9-createdevice and /windows/win32/direct3d9/d3dpresent-parameters.
 * Only implemented caps/states are returned. Unsupported methods trap by name.
 */
#include "d3d9-api.h"
#include "heap-api.h"
#include "wait-profile.h"
#include "window-api.h"
#include <pthread.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifndef MOVIES_D3D9_SPACE
#define MOVIES_D3D9_SPACE GUEST_SPACE
#endif
__attribute__((weak)) void movies_wait_profile_present(void){}
#define BASE 0x05000000u
#define TRAMP 0x0e020000u
#define INVALID 0x8876086cu
#define UNAVAILABLE 0x8876086au
#define LOST 0x88760868u
#define OOM 0x8007000eu
#define ARG(n) R32(c->esp+4+4*(n))
enum { FACTORY=1,DEVICE,SURFACE,VERTEX,INDEX,TEXTURE };
typedef struct LightState {uint32_t index,used,enabled;uint32_t words[26];} LightState;
typedef struct Object {
 uint32_t type,address,refs,internal,parent,format,usage,pool,size,mem,gpu,version,uploaded,offset,upload_size,index_bias;
 uint32_t width,height,depth,hwnd,flags,interval,windowed,rt,ds,vb,ib,stride,stream_offset,fvf,scene,locked;
 uint32_t draw_ack_logged,draw_reject_logged,draw104_logged;uint32_t diag_draws,diag_draw_ok,diag_clears,diag_clear_ok,diag_clear_caller,diag_present,diag_parent,diag_logs,diag_table_seen,diag_table_state,diag_lock_attempt,diag_lock_ok,diag_cooperative,diag_cooperative_failed;uint32_t viewport[6],scissor[4],rs[256],tss[8][33],sampler[16][14],texture,container,surface,lockflags,color_mask,lost,target;float matrices[48],material[17];uint32_t material_set;LightState lights[16];
} Object;
#define OBJECT_CAPACITY 4096u
static Object *objects[OBJECT_CAPACITY];static uint32_t live_device;
static pthread_t owner;static int initialized;
static MoviesD3D9Host host;static void *host_context;
static int range(uint32_t p,uint64_t n){return p&&(uint64_t)p+n<=MOVIES_D3D9_SPACE;}
static void ret(CPU*c,unsigned n,uint32_t r){c->eax=r;c->esp+=4*(n+1);}
/* Bounded startup diagnostics: no per-frame logging. Capture actual requests/results. */
static void diagnostic_ret(CPU*c,unsigned n,uint32_t result,unsigned id,const char*name){
 static unsigned counts[8];if(counts[id]++<16){fprintf(stderr,"[D3D9] %s caller=%08x result=%08x args=",name,R32(c->esp),result);for(unsigned i=0;i<n;i++)fprintf(stderr,"%s%08x",i?",":"",ARG(i));
 if(id==7&&range(ARG(5),56)){fprintf(stderr," pp=");for(unsigned i=0;i<14;i++)fprintf(stderr,"%s%08x",i?",":"",R32(ARG(5)+i*4));}fprintf(stderr,"\n");}ret(c,n,result);
}
static int on_owner(void){return initialized&&pthread_equal(owner,pthread_self());}
static void check(CPU*c){if(!on_owner())guest_trap(c,R32(c->esp),"D3D9 called outside initializing UI thread");}
static Object *find(uint32_t address){if(address<BASE+0x2000||(address-(BASE+0x2000))%16)return NULL;uint32_t i=(address-(BASE+0x2000))/16;if(i>=OBJECT_CAPACITY)return NULL;Object*o=objects[i];return o&&o->type?o:NULL;}
static Object *self(CPU*c,uint32_t type){check(c);Object*o=find(ARG(0));if(!o||(type&&o->type!=type))guest_trap(c,R32(c->esp),"invalid D3D9 COM object");return o;}
static void unsupported(CPU*c,const char*name){self(c,0);static unsigned reports;if(reports++<64)fprintf(stderr,"[D3D9] unsupported %s caller=%08x stack=%08x,%08x,%08x\n",name,R32(c->esp),ARG(0),ARG(1),ARG(2));guest_trap(c,R32(c->esp),name);}
static const uint32_t ids[7][4]={
 {0,0,0x000000c0,0x46000000}, /* IUnknown */
 {0x81bdcbca,0x426d64d4,0x01ad8dae,0x5c27f447},
 {0xd0223b96,0x43fdbf7a,0x3ba4bd92,0xebb9820d},
 {0x0cfbaf3a,0x429a9ff6,0x79a2b399,0x9bb8f86a},
 {0xb64bb1b5,0x4df6fd70,0xd01991bf,0xe35524a1},
 {0x7c9dd65e,0x4529d3f7,0x5878eeac,0x35deac30},
 {0x85c31227,0x4f003de5,0x1af13a9b,0xb5188cc3}};
static const uint32_t base_texture_id[4]={0x580ca87e,0x4d541d3c,0x3db71d99,0xce98c2e3};
static const uint32_t resource_id[4]={0x05eec05d,0x43628f7d,0xbad199b9,0x04c757f3};
static int iid(uint32_t p,const uint32_t *id){return range(p,16)&&R32(p)==id[0]&&R32(p+4)==id[1]&&R32(p+8)==id[2]&&R32(p+12)==id[3];}
#ifdef __EMSCRIPTEN__
EM_JS(int,graphics_bridge,(uint32_t cmd, const uint32_t *a, const void *data, uint32_t bytes, const float *m, uint32_t *out),{
 try {
  const call=Module.moviesGraphicsCall;
  if(typeof call!=='function')return 0;
  const v=Array.from(HEAPU32.subarray(a>>>2,(a>>>2)+33));
  let command,args={},result;const scissor=v[26]?{x:v[27],y:v[28],width:v[29]-v[27],height:v[30]-v[28]}:null;
  if(cmd===1){command='capabilities';}
  else if(cmd===2){command='createDevice';args={width:v[0],height:v[1],hwnd:v[2],depth16:!!v[3],windowed:!!v[4],presentationInterval:v[5],backBufferFormat:v[6]};}
  else if(cmd===3){command='clear';const f=new Float32Array(new Uint32Array([v[1]]).buffer)[0];args={color:v[0],depth:f,scissor,zbuffer:(v[2]&2)!==0};}
  else if(cmd===4){command='present';}
  else if(cmd===5||cmd===6){command=cmd===5?'uploadVertices':'uploadIndices';args={bytes:HEAPU8.slice(data,data+bytes).buffer,format:cmd===5?(v[0]===0x144?'XYZRHW_DIFFUSE_UV':v[0]===0x142?'XYZ_DIFFUSE_UV':'XYZ_DIFFUSE'):v[0]===101?'uint16':'uint32'};}
  else if(cmd===7){command='drawIndexed';const f=new Float32Array(new Uint32Array([v[10],v[11]]).buffer);args={vertices:v[0],indices:v[1],firstIndex:v[2],indexCount:v[3],baseVertex:v[4]|0,depthTest:!!v[5],depthCompare:v[23]===8?'always':'less-equal',depthWrite:!!v[24],blend:!!v[25],...(v[25]&256?{blendFactors:{source:(v[25]>>>4)&15,destination:v[25]&15}}:{}),blendMode:v[25]===3?'destination-times-source':v[25]===2?'source-color':'source-alpha',textureEquation:v[31]===3?'diffuse-rgb-texture-alpha':v[31]===2?'diffuse':v[31]?'factor-rgb-texture-alpha':'modulate',textureFactor:v[32],scissor,texture:v[12]||null,alphaTest:{enabled:!!v[20],func:v[21],reference:v[22]},sampler:{addressU:v[13],addressV:v[14],addressW:v[15],minFilter:v[16],magFilter:v[17],mipFilter:v[18],lodBias:new Float32Array(new Uint32Array([v[19]]).buffer)[0]},viewport:{x:v[6],y:v[7],width:v[8],height:v[9],minDepth:f[0],maxDepth:f[1]},world:Array.from(HEAPF32.subarray(m>>>2,(m>>>2)+16)),view:Array.from(HEAPF32.subarray((m>>>2)+16,(m>>>2)+32)),projection:Array.from(HEAPF32.subarray((m>>>2)+32,(m>>>2)+48))};}
  else if(cmd===8){command='release';args={handle:v[0]};}
  else if(cmd===9){command='destroyDevice';}
  else if(cmd===10){command='uploadTextureRGBA';args={width:v[0],height:v[1],bytes:HEAPU8.slice(data,data+bytes).buffer};}
  else if(cmd===11){command='createRenderTarget';args={width:v[0],height:v[1]};}
  else if(cmd===12){command='setRenderTarget';args={texture:v[0]||null};}
  else if(cmd===13){command='readRenderTarget';args={texture:v[0]};}
  else if(cmd===14){command='writeRenderTarget';args={texture:v[0],width:v[1],height:v[2],bytes:HEAPU8.slice(data,data+bytes).buffer};}else return 0;
  result=call(command,args);
  if(result&&typeof result.then==='function')throw Error('D3D9 graphics hook must be synchronous');
  if(cmd===1){if(!result||result.backend!=='WebGPU')return 0;const values=[result.width,result.height,result.depthFormat==='depth16unorm'?1:0,result.maxBufferBytes,result.presentationIntervalMask,result.maxTextureDimension,(result.renderTargetFormats?.includes(22)?1:0)|(result.renderTargetFormats?.includes(25)?2:0)];if(values.some(x=>!Number.isInteger(x)||x<0||x>4294967295))return 0;HEAPU32.set(values,out>>>2);}
  else if(cmd===12){if(!result||result.width!==v[1]||result.height!==v[2])return 0;}
  else if(cmd===13){if(!result||result.width!==v[1]||result.height!==v[2]||!(result.rgba instanceof Uint8Array)||result.rgba.length!==bytes)return 0;const s=result.rgba;for(let i=0;i<bytes;i+=4){HEAPU8[data+i]=s[i+2];HEAPU8[data+i+1]=s[i+1];HEAPU8[data+i+2]=s[i];HEAPU8[data+i+3]=s[i+3];}}
  else if(cmd===5||cmd===6||cmd===10||cmd===11){if(!Number.isInteger(result)||result<1||result>2147483647)return 0;HEAPU32[out>>>2]=result;}
  else if(cmd===2&&result!==true)return 0;
  return 1;
 }catch(error){Module.moviesGraphicsLastError=String(error?.stack||error);return 0;}
});
#endif
static int invoke(MoviesD3D9Request*q,MoviesD3D9Reply*r){memset(r,0,sizeof*r);if(host)return host(q,r,host_context);
#ifdef __EMSCRIPTEN__
 return graphics_bridge(q->command,q->args,q->data,q->bytes,q->matrices,r->values);
#else
 return 0;
#endif
}
static int caps(MoviesD3D9Reply*r){MoviesD3D9Request q={.command=MOVIES_D3D9_CAPS};if(!invoke(&q,r))return 0;return r->values[0]&&r->values[0]<=4096&&r->values[1]&&r->values[1]<=4096&&r->values[2]<=1&&r->values[3]&&r->values[3]<=(32u<<20)&&!(r->values[4]&~0x80000001u)&&r->values[5]&&r->values[5]<=4096&&(r->values[6]&1)&&!(r->values[6]&~3u);}
static void allocation_failure(uint32_t type,uint32_t bytes,const char*reason){
 static unsigned reports;if(reports++>=16)return;unsigned counts[7]={0};for(unsigned i=0;i<OBJECT_CAPACITY;i++)if(objects[i]&&objects[i]->type<=TEXTURE)counts[objects[i]->type]++;
 MoviesHeapBudget budget;movies_heap_budget(&budget);
 fprintf(stderr,"[D3D9 allocation] failed type=%u bytes=%u reason=%s capacity=%u live(factory,device,surface,vb,ib,texture)=%u,%u,%u,%u,%u,%u guestHeapFree=%u unreserved=%u\n",type,bytes,reason,OBJECT_CAPACITY,counts[1],counts[2],counts[3],counts[4],counts[5],counts[6],budget.free_bytes,budget.unreserved);
}
static Object *allocate(uint32_t type){for(unsigned i=0;i<OBJECT_CAPACITY;i++)if(!objects[i]||!objects[i]->type){
 Object*o=objects[i];if(!o){o=calloc(1,sizeof*o);if(!o){allocation_failure(type,sizeof*o,"host metadata allocation");return NULL;}objects[i]=o;}
 memset(o,0,sizeof*o);o->type=type;o->address=BASE+0x2000+i*16;W32(o->address,BASE+(type-1)*0x400);return o;
 }
 allocation_failure(type,0,"COM object table exhausted");return NULL;}
static void add(Object*o){if(o->type>=SURFACE&&!o->refs&&o->parent){Object*p=find(o->container?o->container:o->parent);if(p)add(p);}o->refs++;}
static void drop(Object*o);static void bind_target(Object*d,Object*s);
static void destroy(Object*o){
 uint32_t parent=o->parent;
 if(o->type==DEVICE){bind_target(o,NULL);uint32_t children[]={o->vb,o->ib,o->rt,o->ds,o->texture};for(unsigned i=0;i<5;i++){Object*k=find(children[i]);if(k&&k->internal){k->internal--;if(!k->refs&&!k->internal)destroy(k);}}MoviesD3D9Request q={.command=MOVIES_D3D9_DESTROY};MoviesD3D9Reply r;(void)invoke(&q,&r);live_device=0;}
 if(o->type==TEXTURE&&o->surface){Object*s=find(o->surface);if(s&&s->internal){s->internal--;if(!s->refs&&!s->internal)destroy(s);}}
 if(o->gpu){MoviesD3D9Request q={.command=MOVIES_D3D9_RELEASE_RESOURCE,.args={o->gpu}};MoviesD3D9Reply r;(void)invoke(&q,&r);}
 if(o->mem&&!movies_backend_free(o->mem))abort();
 uint32_t type=o->type;W32(o->address,0);memset(o,0,sizeof*o);
 if(type==DEVICE){Object*p=find(parent);if(p)drop(p);}
}
static void drop(Object*o){if(!o->refs)abort();o->refs--;uint32_t parent=(o->type>=SURFACE&&!o->refs)?(o->container?o->container:o->parent):0;if(!o->refs&&!o->internal)destroy(o);Object*p=find(parent);if(p)drop(p);}
static void bind(Object*d,uint32_t*slot,Object*o){Object*old=find(*slot);if(old==o)return;if(o)o->internal++;*slot=o?o->address:0;if(old&&old->internal){old->internal--;if(!old->refs&&!old->internal)destroy(old);} (void)d;}
/* The device references its current render-target surface and, through it, the
 * container texture (D3D9 keeps both alive while bound). NULL selects the
 * implicit backbuffer, which the device already owns. */
static void release_internal(Object*o){if(o&&o->internal){o->internal--;if(!o->refs&&!o->internal)destroy(o);}}
static void bind_target(Object*d,Object*s){Object*old=find(d->target);if(old==s)return;if(s){s->internal++;Object*t=find(s->container);if(t)t->internal++;}d->target=s?s->address:0;
 if(old){Object*t=find(old->container);release_internal(old);release_internal(t);}}
static void target_size(Object*d,uint32_t*w,uint32_t*h){Object*s=find(d->target);*w=s?s->width:d->width;*h=s?s->height:d->height;}
static void query(CPU*c){Object*o=self(c,0);uint32_t out=ARG(2);if(!range(out,4)){ret(c,3,0x80070057);return;}W32(out,0);if(!iid(ARG(1),ids[0])&&!iid(ARG(1),ids[o->type])&&!(o->type>=SURFACE&&iid(ARG(1),resource_id))&&!(o->type==TEXTURE&&iid(ARG(1),base_texture_id))){ret(c,3,0x80004002);return;}add(o);W32(out,o->address);ret(c,3,0);}
static void addref(CPU*c){Object*o=self(c,0);add(o);ret(c,1,o->refs+o->internal);}
static void release(CPU*c){Object*o=self(c,0);if(!o->refs){unsupported(c,"D3D9 Release without external reference");return;}uint32_t refs=o->refs-1+o->internal;drop(o);ret(c,1,refs);}
static void factory(CPU*c){check(c);MoviesD3D9Reply r;if(ARG(0)!=32||!caps(&r)){diagnostic_ret(c,1,0,0,"factory");return;}Object*o=allocate(FACTORY);if(o)o->refs=1;diagnostic_ret(c,1,o?o->address:0,0,"factory");}
static int adapter(CPU*c){self(c,FACTORY);return ARG(1)==0;}
static void adapter_count(CPU*c){self(c,FACTORY);MoviesD3D9Reply r;ret(c,1,caps(&r)?1:0);}
static void identifier(CPU*c){uint32_t out=ARG(3);if(!adapter(c)||ARG(2)&~2u||!range(out,1100)){ret(c,4,INVALID);return;}memset(HP(out),0,1100);memcpy(HP(out),"TheMoviesWebGPU",15);memcpy(HP(out+512),"The Movies bounded WebGPU bridge",32);memcpy(HP(out+1024),"\\\\.\\DISPLAY1",13);W32(out+1056,1);ret(c,4,0);}
static int target_format(uint32_t format){MoviesD3D9Reply r;return caps(&r)&&(format==22||(format==25&&(r.values[6]&2)));}
static void mode_count(CPU*c){if(!adapter(c)){ret(c,3,0);return;}ret(c,3,target_format(ARG(2))?1:0);}
static void mode_out(uint32_t p,MoviesD3D9Reply*r){W32(p,r->values[0]);W32(p+4,r->values[1]);W32(p+8,0);W32(p+12,22);}
static void enum_mode(CPU*c){MoviesD3D9Reply r;if(!adapter(c)||!target_format(ARG(2))||ARG(3)||!range(ARG(4),16)){ret(c,5,INVALID);return;}if(!caps(&r)){ret(c,5,UNAVAILABLE);return;}mode_out(ARG(4),&r);W32(ARG(4)+12,ARG(2));ret(c,5,0);}
static void display_mode(CPU*c){MoviesD3D9Reply r;if(!adapter(c)||!range(ARG(2),16)){ret(c,3,INVALID);return;}if(!caps(&r)){ret(c,3,UNAVAILABLE);return;}mode_out(ARG(2),&r);ret(c,3,0);}
static void device_type(CPU*c){MoviesD3D9Reply r;if(!adapter(c)){diagnostic_ret(c,6,INVALID,1,"device_type");return;}diagnostic_ret(c,6,ARG(2)==1&&target_format(ARG(3))&&(ARG(4)==22||(ARG(4)==25&&caps(&r)&&(r.values[6]&2))||(ARG(5)&&ARG(4)==0))&&ARG(5)<=1&&caps(&r)?0:UNAVAILABLE,1,"device_type");}
static void check_format(CPU*c){MoviesD3D9Reply r;if(!adapter(c)){diagnostic_ret(c,7,INVALID,2,"check_format");return;}uint32_t usage=ARG(4),type=ARG(5),format=ARG(6);int supported=(type==3&&usage==0&&(format==21||format==26||format==0x33545844u||format==0x31545844u))||(type==1&&usage==1&&(format==22||(format==25&&caps(&r)&&(r.values[6]&2))))||(type==1&&usage==2&&format==80&&caps(&r)&&r.values[2]);diagnostic_ret(c,7,ARG(2)==1&&target_format(ARG(3))&&supported?0:UNAVAILABLE,2,"check_format");}
static void depth_match(CPU*c){MoviesD3D9Reply r;diagnostic_ret(c,6,adapter(c)&&ARG(2)==1&&target_format(ARG(3))&&(ARG(4)==22||(ARG(4)==25&&caps(&r)&&(r.values[6]&2)))&&ARG(5)==80&&caps(&r)&&r.values[2]?0:UNAVAILABLE,3,"depth_match");}
static void multisample(CPU*c){MoviesD3D9Reply r;uint32_t p=ARG(6);if(!adapter(c)||(p&&!range(p,4))){diagnostic_ret(c,7,INVALID,4,"multisample");return;}int ok=ARG(2)==1&&(ARG(3)==22||(ARG(3)==25&&caps(&r)&&(r.values[6]&2)))&&ARG(4)<=1&&ARG(5)==0&&caps(&r);if(ok&&p)W32(p,1);diagnostic_ret(c,7,ok?0:UNAVAILABLE,4,"multisample");}
static int fill_caps(uint32_t p){MoviesD3D9Reply r;if(!caps(&r))return 0;memset(HP(p),0,304);W32(p,1);W32(p+20,r.values[4]);W32(p+28,0x80000);W32(p+32,0x10);W32(p+36,0x01000000|(r.values[2]?0x10:0));W32(p+40,r.values[2]?0x88:0);W32(p+44,0x7ff);W32(p+48,0x7ff);W32(p+52,255);W32(p+56,8);W32(p+60,5);W32(p+64,0x03030300);W32(p+76,7);W32(p+88,r.values[5]);W32(p+92,r.values[5]);W32(p+104,r.values[5]);W32(p+108,1);W32(p+144,10);W32(p+148,1);W32(p+152,1);W32(p+180,r.values[3]/48);W32(p+184,r.values[3]/16-1);W32(p+188,1);W32(p+192,28);W32(p+232,1);W32(p+240,1);return 1;}
static void adapter_caps(CPU*c){if(!adapter(c)||ARG(2)!=1||!range(ARG(3),304)){diagnostic_ret(c,4,INVALID,5,"adapter_caps");return;}diagnostic_ret(c,4,fill_caps(ARG(3))?0:UNAVAILABLE,5,"adapter_caps");}
static void get_caps(CPU*c){self(c,DEVICE);if(!range(ARG(1),304)){diagnostic_ret(c,2,INVALID,6,"get_caps");return;}diagnostic_ret(c,2,fill_caps(ARG(1))?0:LOST,6,"get_caps");}
static void create_device(CPU*c){
 Object*f=self(c,FACTORY);uint32_t pp=ARG(5),out=ARG(6);MoviesD3D9Reply cr;
 if(!range(out,4)){diagnostic_ret(c,7,INVALID,7,"create_device");return;}W32(out,0);
 if(ARG(1)||ARG(2)!=1||!range(pp,56)){diagnostic_ret(c,7,INVALID,7,"create_device");return;}
 if(live_device||!caps(&cr)){diagnostic_ret(c,7,UNAVAILABLE,7,"create_device");return;}
 /* HW T&L capability is deliberately absent; the original retries SW VP. */
 if((ARG(4)&~4u)!=0x20){diagnostic_ret(c,7,UNAVAILABLE,7,"create_device");return;}
 uint32_t hwnd=R32(pp+28)?R32(pp+28):ARG(3);MoviesWindowEvent window;
 if(!hwnd||!movies_window_get(hwnd,&window)||window.client_width<=0||window.client_height<=0){diagnostic_ret(c,7,INVALID,7,"create_device");return;}
 uint32_t w=R32(pp),h=R32(pp+4),fmt=R32(pp+8),count=R32(pp+12),windowed=R32(pp+32),depth=R32(pp+36),interval=R32(pp+52);
 if(!w&&windowed)w=(uint32_t)window.client_width;if(!h&&windowed)h=(uint32_t)window.client_height;if(!fmt&&windowed)fmt=22;if(!count)count=1;
 uint32_t requested_interval=interval?interval:1;
 if(w!=(uint32_t)window.client_width||h!=(uint32_t)window.client_height||w!=cr.values[0]||h!=cr.values[1]||(fmt!=22&&fmt!=25)||(fmt==25&&!(cr.values[6]&2))||count!=1||R32(pp+16)||R32(pp+20)||R32(pp+24)!=1||windowed>1||depth>1||(depth&&(!cr.values[2]||R32(pp+40)!=80))||R32(pp+44)||R32(pp+48)||!(cr.values[4]&requested_interval)||(interval!=0&&interval!=1&&interval!=0x80000000u)){diagnostic_ret(c,7,UNAVAILABLE,7,"create_device");return;}
 Object*d=allocate(DEVICE),*rt=NULL,*ds=NULL;
 if(d)rt=allocate(SURFACE);if(rt&&depth)ds=allocate(SURFACE);
 if(!d||!rt||(depth&&!ds)){if(d){W32(d->address,0);memset(d,0,sizeof*d);}if(rt){W32(rt->address,0);memset(rt,0,sizeof*rt);}diagnostic_ret(c,7,OOM,7,"create_device");return;}
 MoviesD3D9Request q={.command=MOVIES_D3D9_CREATE,.args={w,h,hwnd,depth,windowed,interval,fmt}};MoviesD3D9Reply r;
 if(!invoke(&q,&r)){W32(d->address,0);memset(d,0,sizeof*d);W32(rt->address,0);memset(rt,0,sizeof*rt);if(ds){W32(ds->address,0);memset(ds,0,sizeof*ds);}diagnostic_ret(c,7,UNAVAILABLE,7,"create_device");return;}
 d->refs=1;d->parent=f->address;add(f);d->width=w;d->height=h;d->format=fmt;d->depth=depth;d->hwnd=hwnd;d->flags=ARG(4);d->interval=interval;d->windowed=windowed;d->rt=rt->address;d->ds=ds?ds->address:0;
 rt->internal=1;rt->parent=d->address;rt->format=fmt;rt->width=w;rt->height=h;rt->usage=1;
 if(ds){ds->internal=1;ds->parent=d->address;ds->format=80;ds->width=w;ds->height=h;ds->usage=2;}
 for(unsigned i=0;i<48;i++)d->matrices[i]=(i%16)%5==0?1.f:0.f;
 d->scissor[2]=w;d->scissor[3]=h;d->viewport[2]=w;d->viewport[3]=h;d->viewport[5]=0x3f800000;
 /* Factual D3D9 defaults from Microsoft D3DRENDERSTATETYPE; unsupported
 * features remain disabled and setters still trap on changed unimplemented values.
 * POINTSIZE_MAX stays0, matching this triangle-only frontend's MaxPointSize cap. */
 d->rs[8]=0x3u;d->rs[9]=0x2u;d->rs[14]=0x1u;d->rs[16]=0x1u;d->rs[19]=0x2u;d->rs[20]=0x1u;d->rs[22]=0x3u;d->rs[23]=0x4u;d->rs[25]=0x8u;d->rs[37]=0x3f800000u;d->rs[38]=0x3f800000u;d->rs[53]=0x1u;d->rs[54]=0x1u;d->rs[55]=0x1u;d->rs[56]=0x8u;d->rs[58]=0xffffffffu;d->rs[59]=0xffffffffu;d->rs[60]=0xffffffffu;d->rs[136]=0x1u;d->rs[137]=0x1u;d->rs[141]=0x1u;d->rs[142]=0x1u;d->rs[145]=0x1u;d->rs[146]=0x2u;d->rs[154]=0x3f800000u;d->rs[155]=0x3f800000u;d->rs[158]=0x3f800000u;d->rs[161]=0x1u;d->rs[162]=0xffffffffu;d->rs[168]=0xfu;d->rs[171]=0x1u;d->rs[172]=0x3u;d->rs[173]=0x1u;d->rs[178]=0x3f800000u;d->rs[179]=0x3f800000u;d->rs[182]=0x3f800000u;d->rs[186]=0x1u;d->rs[187]=0x1u;d->rs[188]=0x1u;d->rs[189]=0x8u;d->rs[190]=0xfu;d->rs[191]=0xfu;d->rs[192]=0xfu;d->rs[193]=0xffffffffu;d->rs[207]=0x2u;d->rs[208]=0x1u;d->rs[209]=0x1u;
 d->rs[7]=depth;d->rs[8]=3;d->rs[9]=2;d->rs[14]=1;d->rs[22]=3;d->rs[23]=4;d->rs[137]=1;d->rs[168]=15;/* D3D9 documented state defaults; storage does not advertise feature support. */
 for(unsigned stage=0;stage<8;stage++){uint32_t*t=d->tss[stage];t[1]=stage?1:4;t[4]=stage?1:2;t[2]=t[5]=2;t[3]=t[6]=t[26]=t[27]=t[28]=1;t[11]=stage;}
 for(unsigned sampler=0;sampler<16;sampler++){uint32_t*t=d->sampler[sampler];t[1]=t[2]=t[3]=t[5]=t[6]=t[10]=1;}

 W32(pp,w);W32(pp+4,h);W32(pp+8,fmt);W32(pp+12,count);W32(out,d->address);live_device=d->address;diagnostic_ret(c,7,0,7,"create_device");
}
static void cooperative(CPU*c){Object*d=self(c,DEVICE);MoviesD3D9Reply r;int ok=caps(&r);d->diag_cooperative++;if(!ok)d->diag_cooperative_failed++;ret(c,1,ok?0:LOST);}
static void available_texture_memory(CPU*c){self(c,DEVICE);MoviesD3D9Reply r;ret(c,1,caps(&r)?r.values[3]:0); /* Conservative virtual allocation budget, not physical VRAM. */}
static void get_direct3d(CPU*c){Object*d=self(c,DEVICE);if(!range(ARG(1),4)){ret(c,2,INVALID);return;}Object*f=find(d->parent);add(f);W32(ARG(1),f->address);ret(c,2,0);}
static void creation_parameters(CPU*c){Object*d=self(c,DEVICE);uint32_t p=ARG(1);if(!range(p,16)){ret(c,2,INVALID);return;}W32(p,0);W32(p+4,1);W32(p+8,d->hwnd);W32(p+12,d->flags);ret(c,2,0);}
static void device_mode(CPU*c){Object*d=self(c,DEVICE);if(ARG(1)||!range(ARG(2),16)){ret(c,3,INVALID);return;}MoviesD3D9Reply r={.values={d->width,d->height}};mode_out(ARG(2),&r);ret(c,3,0);}
static void return_surface(CPU*c,Object*d,uint32_t surface,uint32_t out,unsigned argc){if(!range(out,4)){ret(c,argc,INVALID);return;}W32(out,0);Object*s=find(surface);if(!s){ret(c,argc,0x88760866u);return;}add(s);W32(out,s->address);ret(c,argc,0);(void)d;}
static void get_rt(CPU*c){Object*d=self(c,DEVICE);if(ARG(1)){ret(c,3,INVALID);return;}return_surface(c,d,d->target?d->target:d->rt,ARG(2),3);}
static void get_ds(CPU*c){Object*d=self(c,DEVICE);return_surface(c,d,d->ds,ARG(1),2);}
static void get_backbuffer(CPU*c){Object*d=self(c,DEVICE);if(ARG(1)||ARG(2)||ARG(3)){ret(c,5,INVALID);return;}return_surface(c,d,d->rt,ARG(4),5);}
static void surface_desc(CPU*c){Object*s=self(c,SURFACE);uint32_t p=ARG(1);if(!range(p,32)){ret(c,2,INVALID);return;}W32(p,s->format);W32(p+4,1);W32(p+8,s->usage);W32(p+12,s->pool);W32(p+16,0);W32(p+20,0);W32(p+24,s->width);W32(p+28,s->height);ret(c,2,0);}
static void resource_device(CPU*c){Object*o=self(c,0);if(o->type<SURFACE||!range(ARG(1),4)){ret(c,2,INVALID);return;}Object*d=find(o->parent);if(!d){ret(c,2,LOST);return;}add(d);W32(ARG(1),d->address);ret(c,2,0);}
static void resource_type(CPU*c){Object*o=self(c,0);ret(c,1,o->type==SURFACE?1:o->type==VERTEX?6:o->type==INDEX?7:3);}
static void begin_scene(CPU*c){Object*d=self(c,DEVICE);if(d->scene){ret(c,1,INVALID);return;}d->scene=1;ret(c,1,0);}
static void end_scene(CPU*c){Object*d=self(c,DEVICE);if(!d->scene){ret(c,1,INVALID);return;}d->scene=0;ret(c,1,0);}
#ifdef __EMSCRIPTEN__
EM_JS(int,present_diagnostic_enabled,(void),{return Module.moviesProfileWaits===true?1:0;});
#else
static int present_diagnostic_enabled(void){const char*p=getenv("PORT_D3D_PRESENT_DIAGNOSTICS");return p&&!strcmp(p,"1");}
#endif
static uint32_t diagnostic_word(uint32_t p){return range(p,4)?R32(p):0;}
static uint32_t diagnostic_texture(uint32_t p){uint32_t wrapper=diagnostic_word(p);return wrapper&&range(wrapper,40)?R32(wrapper+36):0;}
static void present_diagnostic(CPU*c,Object*d,int ok){
 static int enabled=-1;if(enabled<0)enabled=present_diagnostic_enabled();
 if(enabled){uint32_t n=++d->diag_present,parent=diagnostic_word(c->esp+24);
  if(d->diag_logs<48&&(n<=8||!(n&(n-1))||parent!=d->diag_parent)){
   d->diag_logs++;fprintf(stderr,"[D3D9 present] n=%u ok=%d caller=%08x parent=%08x clear=%u/%u clearCaller=%08x draw=%u/%u rtTexture=%08x memTexture=%08x surfaces=%08x,%08x,%08x state=%08x queue=%08x count=%u\n",n,ok,diagnostic_word(c->esp),parent,d->diag_clear_ok,d->diag_clears,d->diag_clear_caller,d->diag_draw_ok,d->diag_draws,diagnostic_texture(0x105ca58),diagnostic_texture(0x105ca5c),diagnostic_word(0x105ca60),diagnostic_word(0x105ca64),diagnostic_word(0x105ca68),diagnostic_word(0x105be9c),diagnostic_word(0x105b578),diagnostic_word(0x105b580));
   fprintf(stderr,"[D3D9 present] bufferLocks=%u/%u cooperative=%u failed=%u\n",d->diag_lock_ok,d->diag_lock_attempt,d->diag_cooperative,d->diag_cooperative_failed);
   uint32_t manager=diagnostic_word(0x104dafc),widget=manager&&range(manager,0x6e8)?R32(manager+0x6e0):0;
   uint32_t player=widget&&range(widget,0x350)?R32(widget+0x344):0;
   fprintf(stderr,"[D3D9 present] frontend manager=%08x ready=%08x widget=%08x flags=%u,%u,%u callback=%08x player=%08x loaded=%u\n",manager,manager&&range(manager,0x6e8)?R8(manager+0x6e4):0,widget,widget&&range(widget,0x350)?R8(widget+0x348):0,widget&&range(widget,0x350)?R8(widget+0x349):0,widget&&range(widget,0x350)?R8(widget+0x34a):0,widget&&range(widget,0x350)?R32(widget+0x34c):0,player,player&&range(player,0x15)?R8(player+0x14):0);
   /* Audited447C30→423670→43E9A0→9A6360 preserves ESI, the0xCC
    * state object. Interpret no other Present call chain as this structure. */
   if(parent==0x447cb1){
    uint32_t object=c->esi,begin=0,end=0;int valid=range(object,0xcc);
    if(valid){begin=R32(object+0xb0);end=R32(object+0xb4);}
    int entry=valid&&begin&&end>begin&&(uint64_t)end-begin>=12&&((end-begin)%12)==0&&!(begin&3)&&range(begin,(uint64_t)end-begin);
    fprintf(stderr,"[D3D9 present] loop object=%08x globalObject=%08x valid=%d flags88=%02x begin=%08x end=%08x entryValid=%d stateID=%08x update=%08x render=%08x\n",object,diagnostic_word(0xf87b04),valid,valid?R8(object+0x88):0,begin,end,entry,entry?R32(end-12):0,entry?R32(end-8):0,entry?R32(end-4):0);
    uint32_t ui=diagnostic_word(0x104df40),ui2=diagnostic_word(0x105cc64),vb=diagnostic_word(0x10bb230);
    int uiValid=range(ui,0x374),vbValid=range(vb,0x14);uint32_t vt=uiValid?R32(ui):0,vt2=ui2&&range(ui2,4)?R32(ui2):0;
    fprintf(stderr,"[D3D9 present] renderGates wholePass=%u entryFilter=%u sprite=%08x ui=%08x valid=%d flags218=%02x gate1ec=%08x flags324=%02x,%02x field340=%08x uiMethods=%08x,%08x,%08x,%08x,%08x secondary=%08x method4=%08x dynVB=%08x valid=%d buffer=%08x capacity=%u flags=%08x\n",range(0x105beb4,1)?R8(0x105beb4):0,range(0x105bec4,1)?R8(0x105bec4):0,diagnostic_word(0x105cd94),ui,uiValid,uiValid?R8(ui+0x218):0,uiValid?R32(ui+0x1ec):0,uiValid?R8(ui+0x324):0,uiValid?R8(ui+0x325):0,uiValid?R32(ui+0x340):0,range(vt,0xec)?R32(vt+0x2c):0,range(vt,0xec)?R32(vt+0xdc):0,range(vt,0xec)?R32(vt+0xe0):0,range(vt,0xec)?R32(vt+0xe4):0,range(vt,0xec)?R32(vt+0xe8):0,ui2,range(vt2,8)?R32(vt2+4):0,vb,vbValid,vbValid?R32(vb+4):0,vbValid?R32(vb+8):0,vbValid?R32(vb+0x10):0);
    uint32_t gate=uiValid?R32(ui+0x1ec):0,gateVT=range(gate,4)?R32(gate):0,node=uiValid?R32(ui+0x130):0;
    int nodeValid=node&&uiValid&&node!=ui+0x120&&range(node,12);uint32_t child=nodeValid?R32(node+8):0;int childValid=range(child,0x219);uint32_t childVT=childValid?R32(child):0;
    fprintf(stderr,"[D3D9 present] uiChildren firstNode=%08x sentinel=%08x nodeValid=%d next=%08x child=%08x childValid=%d childVT=%08x render=%08x flags218=%02x gate1ec=%08x rootGateMethod14=%08x\n",node,uiValid?ui+0x120:0,nodeValid,nodeValid?R32(node):0,child,childValid,childVT,range(childVT,0x30)?R32(childVT+0x2c):0,childValid?R8(child+0x218):0,childValid?R32(child+0x1ec):0,range(gateVT,0x18)?R32(gateVT+0x14):0);
    uint32_t stateID=entry?R32(end-12):0;
    if((!d->diag_table_seen||(stateID==2&&d->diag_table_state!=2))&&range(0x105c618,21*16)){
     fprintf(stderr,"[D3D9 present] callbackTable stateID=%u records(function,index,arg,flags)=",stateID);
     for(unsigned i=0;i<21;i++){uint32_t p=0x105c618+i*16;fprintf(stderr,"%s%08x,%u,%08x,%08x",i?";":"",R32(p),R32(p+4),R32(p+8),R32(p+12));}fprintf(stderr,"\n");d->diag_table_seen=1;
    }d->diag_table_state=stateID;

   }
  }d->diag_parent=parent;
 }d->diag_draws=d->diag_draw_ok=d->diag_clears=d->diag_clear_ok=0;d->diag_lock_attempt=d->diag_lock_ok=d->diag_cooperative=d->diag_cooperative_failed=0;
}
static void clear_target(CPU*c){Object*d=self(c,DEVICE);d->diag_clears++;d->diag_clear_caller=R32(c->esp);float z;uint32_t bits=ARG(5);memcpy(&z,&bits,4);
 /* TARGET, or TARGET|ZBUFFER with a depth surface (observed 9A56D7 wrapper 9A56B0
  * clears TARGET alone when the pass disables Z). Clear covers the viewport,
  * intersected with the scissor rect when scissor test is enabled.
  * https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-clear */
 uint32_t flags=ARG(3);
 if(ARG(1)||ARG(2)||(flags!=1u&&!(flags==3u&&d->depth))||!isfinite(z)||z<0||z>1||ARG(6)){unsupported(c,"D3D9 Clear unsupported rectangle/flags/depth/stencil combination");return;}
 uint32_t tw,th;target_size(d,&tw,&th);int64_t l=d->viewport[0],t=d->viewport[1],r_=l+(int64_t)d->viewport[2],b=t+(int64_t)d->viewport[3];
 if(d->rs[174]){l=l>(int32_t)d->scissor[0]?l:(int32_t)d->scissor[0];t=t>(int32_t)d->scissor[1]?t:(int32_t)d->scissor[1];r_=r_<(int32_t)d->scissor[2]?r_:(int32_t)d->scissor[2];b=b<(int32_t)d->scissor[3]?b:(int32_t)d->scissor[3];}
 if(r_>tw)r_=tw;if(b>th)b=th;if(l<0)l=0;if(t<0)t=0;
 if(l>=r_||t>=b){d->diag_clear_ok++;ret(c,7,0);return;} /*empty area: nothing to clear*/
 MoviesD3D9Request q={.command=MOVIES_D3D9_CLEAR,.args={ARG(4),bits,flags}};
 if(l||t||r_!=tw||b!=th){q.args[26]=1;q.args[27]=(uint32_t)l;q.args[28]=(uint32_t)t;q.args[29]=(uint32_t)r_;q.args[30]=(uint32_t)b;}
 MoviesD3D9Reply r;int ok=invoke(&q,&r);if(ok)d->diag_clear_ok++;ret(c,7,ok?0:LOST);
}
static void present(CPU*c){Object*d=self(c,DEVICE);if(d->scene||ARG(1)||ARG(2)||ARG(3)||ARG(4)){ret(c,5,INVALID);return;}MoviesD3D9Request q={.command=MOVIES_D3D9_PRESENT};MoviesD3D9Reply r;int ok=invoke(&q,&r);if(ok)movies_wait_profile_present();present_diagnostic(c,d,ok);ret(c,5,ok?0:LOST);}
static int transform_index(uint32_t value){return value==256?0:value==2?16:value==3?32:-1;}
static void set_transform(CPU*c){Object*d=self(c,DEVICE);int index=transform_index(ARG(1));if(index<0){unsupported(c,"D3D9 SetTransform unsupported state");return;}if(!range(ARG(2),64)){ret(c,3,INVALID);return;}float values[16];memcpy(values,HP(ARG(2)),64);for(unsigned i=0;i<16;i++)if(!isfinite(values[i])){ret(c,3,INVALID);return;}memcpy(d->matrices+index,values,64);ret(c,3,0);}
static void get_transform(CPU*c){Object*d=self(c,DEVICE);int index=transform_index(ARG(1));if(index<0||!range(ARG(2),64)){ret(c,3,INVALID);return;}memcpy(HP(ARG(2)),d->matrices+index,64);ret(c,3,0);}
static void set_viewport(CPU*c){Object*d=self(c,DEVICE);uint32_t p=ARG(1);if(!range(p,24)){ret(c,2,INVALID);return;}uint32_t v[6];memcpy(v,HP(p),24);float min,max;memcpy(&min,v+4,4);memcpy(&max,v+5,4);uint32_t tw,th;target_size(d,&tw,&th);if(!v[2]||!v[3]||(uint64_t)v[0]+v[2]>tw||(uint64_t)v[1]+v[3]>th||!isfinite(min)||!isfinite(max)||min<0||max>1||min>max){ret(c,2,INVALID);return;}memcpy(d->viewport,v,24);ret(c,2,0);}
static void get_viewport(CPU*c){Object*d=self(c,DEVICE);if(!range(ARG(1),24)){ret(c,2,INVALID);return;}memcpy(HP(ARG(1)),d->viewport,24);ret(c,2,0);}
static int known_state(uint32_t state){switch(state){case 7:case 8:case 9:case 14:case 15:case 16:case 19:case 20:case 22:case 23:case 24:case 25:case 26:case 27:case 28:case 29:case 34:case 35:case 36:case 37:case 38:case 48:case 52:case 53:case 54:case 55:case 56:case 57:case 58:case 59:case 60:case 128:case 129:case 130:case 131:case 132:case 133:case 134:case 135:case 136:case 137:case 139:case 140:case 141:case 142:case 143:case 145:case 146:case 147:case 148:case 151:case 152:case 154:case 155:case 156:case 157:case 158:case 159:case 160:case 161:case 162:case 163:case 166:case 167:case 168:case 170:case 171:case 172:case 173:case 174:case 175:case 176:case 178:case 179:case 180:case 181:case 182:case 183:case 184:case 185:case 186:case 187:case 188:case 189:case 190:case 191:case 192:case 193:case 194:case 195:case 198:case 199:case 200:case 201:case 202:case 203:case 204:case 205:case 206:case 207:case 208:case 209:return 1;default:return 0;}}
/* At88804A: return/device/state/value/savedESI/parent, so parent isESP+20. */
static void set_state(CPU*c){Object*d=self(c,DEVICE);uint32_t state=ARG(1),value=ARG(2);/* D3DBLEND1..15 are classicD3D9;12/13 are source-only legacy factors.
 * 16/17 require9Ex; FORCE_DWORD is not a factor. Storage is not draw support.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dblend */
 if(state==19||state==20){
  if(value<1||value>15||(state==20&&(value==12||value==13))){ret(c,3,INVALID);return;}
  static uint32_t seen[2];unsigned slot=state-19;uint32_t bit=1u<<value;
  if(!(seen[slot]&bit)){seen[slot]|=bit;fprintf(stderr,"[D3D9] blend state stored caller=%08x wrapperParent=%08x state=%u value=%u\n",R32(c->esp),R32(c->esp)==0x88804a&&range(c->esp,24)?R32(c->esp+20):0,state,value);}
 }
 if(known_state(state)&&d->rs[state]==value){ret(c,3,0);return;}int ok=0;switch(state){case 7:ok=value<=1&&(!value||d->depth);break;case 8:ok=value==3;break;case 9:ok=value==2;break;case 14:ok=value<=1;break;case 22:ok=value==1;break;case 23:ok=value==4||value==8;break;case 174:ok=value<=1;break;case 168:ok=value==15;break;case 15:ok=value<=1;break;case 24:ok=value<=255;break;case 25:ok=value>=1&&value<=8;break;case 137:ok=value<=1;break;case 19:case 20:ok=1;break;case 27:ok=value<=1;break;case 60:ok=1;break;case 28:case 29:case 52:ok=value==0;break;}
 if(!ok){static unsigned reports;if(reports++<32)fprintf(stderr,"[D3D9] SetRenderState caller=%08x state=%u value=%08x\n",R32(c->esp),state,value);unsupported(c,"D3D9 SetRenderState unsupported state/value");return;}d->rs[state]=value;ret(c,3,0);}
static void get_state(CPU*c){Object*d=self(c,DEVICE);if(!known_state(ARG(1))){ret(c,3,INVALID);return;}if(!range(ARG(2),4)){ret(c,3,INVALID);return;}W32(ARG(2),d->rs[ARG(1)]);ret(c,3,0);}
static void set_fvf(CPU*c){Object*d=self(c,DEVICE);if(ARG(1)!=0x42&&ARG(1)!=0x142&&ARG(1)!=0x144&&ARG(1)!=0x104){unsupported(c,"D3D9 SetFVF only XYZ/XYZRHW diffuse single-UV formats implemented");return;}d->fvf=ARG(1);ret(c,2,0);}
static void get_fvf(CPU*c){Object*d=self(c,DEVICE);if(!range(ARG(1),4)){ret(c,2,INVALID);return;}W32(ARG(1),d->fvf);ret(c,2,0);}
static void create_buffer(CPU*c,uint32_t type){Object*d=self(c,DEVICE);uint32_t out=ARG(5),size=ARG(1),usage=ARG(2),fmt=ARG(3),pool=ARG(4);MoviesD3D9Reply caps_;
 static uint32_t seen[16][3];static unsigned seen_count;uint32_t key[3]={type,usage,pool};int fresh=1;for(unsigned i=0;i<seen_count;i++)if(!memcmp(seen[i],key,sizeof key))fresh=0;
 if(fresh&&seen_count<16){memcpy(seen[seen_count++],key,sizeof key);fprintf(stderr,"[D3D9] CreateBuffer requested caller=%08x type=%u usage=%08x pool=%u size=%u format=%08x\n",R32(c->esp),type,usage,pool,size,fmt);}

 if(!range(out,4)){ret(c,7,INVALID);return;}W32(out,0);
 if(!size||ARG(6)||(usage&~0x208u)||pool>1||(pool==1&&(usage&0x200))||(type==INDEX&&fmt!=101&&fmt!=102)||(type==VERTEX&&fmt!=0&&fmt!=0x42&&fmt!=0x142&&fmt!=0x144&&fmt!=0x104)){ret(c,7,INVALID);return;}
 if(!caps(&caps_)||size>caps_.values[3]){ret(c,7,UNAVAILABLE);return;}
 Object*b=allocate(type);if(!b){ret(c,7,OOM);return;}
 b->mem=movies_backend_alloc(size);if(!b->mem){allocation_failure(type,size,"guest buffer storage allocation");W32(b->address,0);memset(b,0,sizeof*b);ret(c,7,OOM);return;}
 b->parent=d->address;b->size=size;b->usage=usage;b->format=fmt;b->pool=pool;b->version=1;add(b);W32(out,b->address);ret(c,7,0);
}
static void create_vb(CPU*c){create_buffer(c,VERTEX);}static void create_ib(CPU*c){create_buffer(c,INDEX);}
static void buffer_lock_failure(CPU*c,Object*b){static unsigned reports;if(reports++<16)fprintf(stderr,"[D3D9] BufferLock rejected caller=%08x type=%u usage=%08x pool=%u size=%u offset=%u bytes=%u flags=%08x locked=%u out=%08x\n",R32(c->esp),b->type,b->usage,b->pool,b->size,ARG(1),ARG(2),ARG(4),b->locked,ARG(3));ret(c,5,INVALID);}
/* NOSYSLOCK is supported: this backend never takes the legacy display-wide
 * critical section. DISCARD/NOOVERWRITE still require DYNAMIC usage.
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d9helper/nf-d3d9helper-idirect3dvertexbuffer9-lock */
static void buffer_lock(CPU*c){Object*b=self(c,0);uint32_t offset=ARG(1),bytes=ARG(2),out=ARG(3),flags=ARG(4);Object*d=find(b->parent);if(d&&d->type==DEVICE)d->diag_lock_attempt++;
 if((b->type!=VERTEX&&b->type!=INDEX)||b->locked||!range(out,4)||offset>b->size||bytes>b->size-offset||(flags&~(0x10u|0x800u|0x1000u|0x2000u))||((flags&0x10)&&(b->usage&8))||((flags&(0x1000u|0x2000u))&&!(b->usage&0x200u))||((flags&0x1000u)&&(flags&0x2000u))){buffer_lock_failure(c,b);return;}
 if(!bytes)bytes=b->size-offset;if(!bytes){buffer_lock_failure(c,b);return;}if(d&&d->type==DEVICE)d->diag_lock_ok++;b->locked=1;W32(out,b->mem+offset);ret(c,5,0);
}
static void buffer_unlock(CPU*c){Object*b=self(c,0);if((b->type!=VERTEX&&b->type!=INDEX)||!b->locked){ret(c,1,INVALID);return;}b->locked=0;b->version++;ret(c,1,0);}
static void buffer_desc(CPU*c){Object*b=self(c,0);uint32_t out=ARG(1);if((b->type!=VERTEX&&b->type!=INDEX)||!range(out,b->type==VERTEX?24:20)){ret(c,2,INVALID);return;}W32(out,b->type==VERTEX?100:b->format);W32(out+4,b->type==VERTEX?6:7);W32(out+8,b->usage);W32(out+12,b->pool);W32(out+16,b->size);if(b->type==VERTEX)W32(out+20,b->format);ret(c,2,0);}
static void stream_source(CPU*c){Object*d=self(c,DEVICE);Object*b=find(ARG(2));if(ARG(1)||(!b&&ARG(2))||(b&&(b->type!=VERTEX||b->parent!=d->address||ARG(3)>b->size||(ARG(4)!=16&&ARG(4)!=24&&ARG(4)!=28)))){ret(c,5,INVALID);return;}bind(d,&d->vb,b);d->stream_offset=ARG(3);d->stride=ARG(4);ret(c,5,0);}
static void set_indices(CPU*c){Object*d=self(c,DEVICE);Object*b=find(ARG(1));if((ARG(1)&&!b)||(b&&(b->type!=INDEX||b->parent!=d->address))){ret(c,2,INVALID);return;}bind(d,&d->ib,b);ret(c,2,0);}
/* Immutable upload snapshots cover only the draw's referenced span. Guest
 * indices stay untouched; a normalized copy permits firstIndex/baseVertex0.
 * This is a contiguous span, not a sparse gather: unused holes inside it still
 * pass through backend float validation. No NaN-hole support is claimed. */
static int upload(Object*b,uint32_t offset,uint32_t bytes,uint32_t fvf,uint32_t mask,uint32_t bias){
 if(b->gpu&&b->uploaded==b->version&&b->offset==offset&&b->upload_size==bytes&&b->fvf==fvf&&b->color_mask==mask&&b->index_bias==bias)return 1;
 if(!bytes||offset>b->size||bytes>b->size-offset)return 0;
 MoviesD3D9Request q={.command=b->type==VERTEX?MOVIES_D3D9_UPLOAD_VERTEX:MOVIES_D3D9_UPLOAD_INDEX,.args={b->type==VERTEX?fvf:b->format},.data=HP(b->mem+offset),.bytes=bytes};
 uint32_t stride=b->type==VERTEX?(fvf==0x144?28:(fvf==0x142||fvf==0x104)?24:16):b->format==101?2:4;if(bytes%stride)return 0;
 uint8_t*converted=NULL;
 /* Missing diffuse defaults to opaque white (D3DTA_DIFFUSE), while guest104
  * XYZRHW+UV keeps24-byte storage. Only the host snapshot expands to144/28B.
  * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dta */
 if(b->type==VERTEX&&fvf==0x104){
  uint64_t expanded=(uint64_t)(bytes/24)*28;if(expanded>32u*1024u*1024u)return 0;
  converted=malloc((size_t)expanded);if(!converted)return 0;
  for(uint32_t i=0;i<bytes/24;i++){memcpy(converted+i*28,(const uint8_t*)q.data+i*24,16);uint32_t white=0xffffffff;memcpy(converted+i*28+16,&white,4);memcpy(converted+i*28+20,(const uint8_t*)q.data+i*24+16,8);}
  q.data=converted;q.bytes=(uint32_t)expanded;q.args[0]=0x144;
 }else if(mask||(b->type==INDEX&&bias)){
  converted=malloc(bytes);if(!converted)return 0;memcpy(converted,q.data,bytes);
  for(uint32_t i=0;i<bytes;i+=stride){if(b->type==VERTEX){uint32_t color;memcpy(&color,converted+i+(fvf==0x144?16:12),4);color|=mask;memcpy(converted+i+(fvf==0x144?16:12),&color,4);}
   else if(stride==2){uint16_t v;memcpy(&v,converted+i,2);if(v<bias){free(converted);return 0;}v=(uint16_t)(v-bias);memcpy(converted+i,&v,2);}
   else{uint32_t v;memcpy(&v,converted+i,4);if(v<bias){free(converted);return 0;}v-=bias;memcpy(converted+i,&v,4);}}
  q.data=converted;
 }
 MoviesD3D9Reply r;int ok=invoke(&q,&r)&&r.values[0];free(converted);if(!ok)return 0;
 uint32_t old=b->gpu;b->gpu=r.values[0];b->uploaded=b->version;b->offset=offset;b->upload_size=bytes;b->index_bias=bias;b->fvf=fvf;b->color_mask=mask;
 if(old){MoviesD3D9Request release_={.command=MOVIES_D3D9_RELEASE_RESOURCE,.args={old}};MoviesD3D9Reply ignored;if(!invoke(&release_,&ignored))return 0;}return 1;
}
/* Only the audited mode6 alpha blend equation. State setters allow the
 * original sequential updates; unsupported active combinations fail at draw. */
/* Original material20: unused ARG2 values do not affect SELECTARG1. */
static int diffuse_equation(Object*d){return !d->texture&&d->tss[0][1]==2&&d->tss[0][2]==0&&d->tss[0][4]==2&&d->tss[0][5]==0&&d->tss[1][1]==1&&d->tss[1][4]==1;}
static int diffuse_texture_equation(Object*d){return d->texture&&d->tss[0][1]==2&&d->tss[0][2]==0&&d->tss[0][4]==2&&d->tss[0][5]==2&&d->tss[1][1]==1&&d->tss[1][4]==1;}
static uint32_t blend_encoding(Object*d){if(!d->rs[27])return 0;if(d->rs[19]==5&&d->rs[20]==6)return 1;if(d->rs[19]==3&&d->rs[20]==4)return 2;if(d->rs[19]==1&&d->rs[20]==3)return 3;return 256|(d->rs[19]<<4)|d->rs[20];}
static int draw_blend_supported(CPU*c,Object*d){if(d->rs[27]&&(d->rs[19]<1||d->rs[19]>11||d->rs[20]<1||d->rs[20]>11||d->rs[171]!=1||d->rs[206])){
 static unsigned reports;if(reports++<8)fprintf(stderr,"[D3D9] active blend rejected caller=%08x fvf=%08x stride=%u texture=%08x blend=%u,%u,%u,%u separate=%u color=%u,%u,%u alpha=%u,%u,%u factor=%08x\n",R32(c->esp),d->fvf,d->stride,d->texture,d->rs[27],d->rs[19],d->rs[20],d->rs[171],d->rs[206],d->tss[0][1],d->tss[0][2],d->tss[0][3],d->tss[0][4],d->tss[0][5],d->tss[0][6],d->rs[60]);
 unsupported(c,"D3D9 active blend requires classic factors1..11 ADD and separate-alpha disabled");return 0;}return 1;}
/* Audited material35: select TFACTOR RGB, multiply its alpha by texture alpha.
 * D3DTOP_SELECTARG2 and MODULATE: Microsoft Learn /direct3d9/d3dtextureop. */
static int factor_texture_equation(Object*d){return d->tss[0][1]==3&&d->tss[0][3]==3&&d->tss[0][4]==4&&d->tss[0][5]==3&&d->tss[0][6]==2&&d->tss[1][1]==1&&d->tss[1][4]==1;}
static int texture_mask(CPU*c,Object*d,uint32_t*mask){*mask=0;if(factor_texture_equation(d)||diffuse_texture_equation(d))return 1;for(unsigned channel=0;channel<2;channel++){unsigned op=channel?4:1,a=channel?5:2,b=channel?6:3;uint32_t first=d->tss[0][a],second=d->tss[0][b];
 if(d->tss[0][op]==2){if(first==2)*mask|=channel?0xff000000u:0x00ffffffu;else{unsupported(c,"D3D9 SELECTARG1 requires texture in implemented pipeline");return 0;}}
 else if(d->tss[0][op]!=4||!((first==2&&second<=1)||(second==2&&first<=1))){unsupported(c,"D3D9 MODULATE requires one texture and one diffuse argument");return 0;}}
 return 1;}
/* One acknowledgement and one returned rejection per device, not per frame.
 * Acknowledgement means submitted backend command, not presentation/scanout. */
static void draw_ret(CPU*c,Object*d,unsigned argc,uint32_t hr,const char*method){
 if(d->fvf==0x104){uint32_t bit=hr?2u:1u;if(!(d->draw104_logged&bit)){d->draw104_logged|=bit;fprintf(stderr,"[D3D9] FVF104 draw caller=%08x result=%08x method=%s stride=%u texture=%08x color=%u,%u,%u alpha=%u,%u,%u blend=%u,%u,%u,%u lighting=%u\n",R32(c->esp),hr,method,d->stride,d->texture,d->tss[0][1],d->tss[0][2],d->tss[0][3],d->tss[0][4],d->tss[0][5],d->tss[0][6],d->rs[27],d->rs[19],d->rs[20],d->rs[171],d->rs[137]);}}
 if(!hr)d->diag_draw_ok++;
 if(!hr&&!d->draw_ack_logged){d->draw_ack_logged=1;fprintf(stderr,"[D3D9] first backend-acknowledged draw device=%08x method=%s caller=%08x primitives=%u\n",d->address,method,R32(c->esp),ARG(argc==9?4:6));}
 if(hr&&!d->draw_reject_logged){d->draw_reject_logged=1;fprintf(stderr,"[D3D9] first rejected draw device=%08x method=%s caller=%08x result=%08x primitives=%u\n",d->address,method,R32(c->esp),hr,ARG(argc==9?4:6));}
 ret(c,argc,hr);
}
static void indexed_reject(CPU*c,Object*d,Object*vb,Object*ib,const char*reason,uint32_t at,uint32_t index){
 static unsigned reports;if(reports++<8)fprintf(stderr,"[D3D9] DrawIndexedPrimitive rejected reason=%s caller=%08x type=%u base=%d minimum=%u vertices=%u start=%u primitives=%u scene=%u fvf=%08x stride=%u streamOffset=%u vb=%08x size=%u locked=%u usage=%08x ib=%08x size=%u locked=%u format=%u indexAt=%u index=%u texture=%08x\n",reason,R32(c->esp),ARG(1),(int32_t)ARG(2),ARG(3),ARG(4),ARG(5),ARG(6),d->scene,d->fvf,d->stride,d->stream_offset,d->vb,vb?vb->size:0,vb?vb->locked:0,vb?vb->usage:0,d->ib,ib?ib->size:0,ib?ib->locked:0,ib?ib->format:0,at,index,d->texture);
 draw_ret(c,d,7,INVALID,"DrawIndexedPrimitive");
}
static void draw_indexed(CPU*c){Object*d=self(c,DEVICE);d->diag_draws++;if(!draw_blend_supported(c,d))return;Object*vb=find(d->vb),*ib=find(d->ib);uint32_t first=ARG(5),primitives=ARG(6),minimum=ARG(3),vertices=ARG(4);int32_t base=(int32_t)ARG(2);
 const char*reason=!d->scene?"outside scene":ARG(1)!=4?"unsupported topology":!vb||!ib?"missing bound buffer":vb->locked||ib->locked?"locked bound buffer":(d->fvf!=0x42&&d->fvf!=0x142&&d->fvf!=0x144&&d->fvf!=0x104)?"unsupported bound FVF":d->stride!=(d->fvf==0x144?28u:(d->fvf==0x142||d->fvf==0x104)?24u:16u)?"FVF stride mismatch":(!primitives||primitives>0xffffffffu/3||!vertices)?"empty/overflow draw count":NULL;
 if(reason){indexed_reject(c,d,vb,ib,reason,UINT32_MAX,0);return;}
 if((d->rs[137]&&d->fvf!=0x104&&d->fvf!=0x144)||d->rs[22]!=1){unsupported(c,"D3D9 draw needs unsupported default lighting/culling");return;}
 uint32_t count=primitives*3,index_stride=ib->format==101?2:4;
 if((uint64_t)first+count>ib->size/index_stride||(uint64_t)minimum+vertices>0x100000000ull){indexed_reject(c,d,vb,ib,"index/declared vertex range",UINT32_MAX,0);return;}
 uint32_t min_index=UINT32_MAX,max_index=0;
 for(uint32_t i=0;i<count;i++){uint32_t p=ib->mem+(first+i)*index_stride,idx=index_stride==2?R16(p):R32(p);int64_t actual=(int64_t)idx+base;
 if(idx<minimum||(uint64_t)idx>=(uint64_t)minimum+vertices||actual<0||(uint64_t)actual>=(vb->size-d->stream_offset)/d->stride){indexed_reject(c,d,vb,ib,idx<minimum||(uint64_t)idx>=(uint64_t)minimum+vertices?"index outside declared vertex range":"index/base outside vertex buffer",i,idx);return;}if(idx<min_index)min_index=idx;if(idx>max_index)max_index=idx;}
 Object*t=find(d->texture);uint32_t mask=0;
 if(t&&d->target&&find(d->target)&&find(d->target)->container==t->address){unsupported(c,"D3D9 draw samples the active render target");return;}
 if(!diffuse_equation(d)&&(d->fvf==0x142||d->fvf==0x144||d->fvf==0x104)!=!!t){unsupported(c,"D3D9 textured draw requires UV format and bound texture");return;}
 if(t){if(t->locked){indexed_reject(c,d,vb,ib,"locked texture",UINT32_MAX,0);return;}if(!texture_mask(c,d,&mask))return;}
 uint64_t vertex_offset=(uint64_t)d->stream_offset+(uint64_t)((int64_t)min_index+base)*d->stride,vertex_bytes=((uint64_t)max_index-min_index+1)*d->stride;
 if(vertex_offset>UINT32_MAX||vertex_bytes>UINT32_MAX||!upload(vb,(uint32_t)vertex_offset,(uint32_t)vertex_bytes,d->fvf,mask,0)||!upload(ib,first*index_stride,count*index_stride,0,0,min_index)){draw_ret(c,d,7,LOST,"DrawIndexedPrimitive");return;}
 MoviesD3D9Request q={.command=MOVIES_D3D9_DRAW,.args={vb->gpu,ib->gpu,0,count,0,d->rs[7]}};q.args[12]=t?t->gpu:0;q.args[20]=d->rs[15];q.args[21]=d->rs[25];q.args[22]=d->rs[24];q.args[23]=d->rs[23];q.args[24]=d->rs[14];q.args[25]=blend_encoding(d);q.args[26]=d->rs[174];q.args[31]=diffuse_texture_equation(d)?3:diffuse_equation(d)?2:factor_texture_equation(d);q.args[32]=d->rs[60];memcpy(q.args+27,d->scissor,16);const unsigned samplers[]={1,2,3,6,5,7,8};for(unsigned i=0;i<7;i++)q.args[13+i]=d->sampler[0][samplers[i]];memcpy(q.args+6,d->viewport,24);memcpy(q.matrices,d->matrices,sizeof q.matrices);MoviesD3D9Reply r;draw_ret(c,d,7,invoke(&q,&r)?0:LOST,"DrawIndexedPrimitive");
}
/* Observed9EB1A3: XYZRHW|DIFFUSE|TEX1, stride28, INDEX16 triangles.
 * Caller buffers are copied synchronously. UP invalidates stream0 and indices.
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-drawindexedprimitiveup */
static void draw_indexed_up(CPU*c){
 Object*d=self(c,DEVICE);d->diag_draws++;if(!draw_blend_supported(c,d))return;uint32_t minimum=ARG(2),vertices=ARG(3),primitives=ARG(4),indices=ARG(5),data=ARG(7);
 bind(d,&d->vb,NULL);bind(d,&d->ib,NULL);d->stride=d->stream_offset=0;
 MoviesD3D9Reply limits;if(!caps(&limits)){draw_ret(c,d,9,LOST,"DrawIndexedPrimitiveUP");return;}
 uint64_t vertex_start=(uint64_t)data+(uint64_t)minimum*28,vertex_bytes=(uint64_t)vertices*28,index_bytes=(uint64_t)primitives*6;
 if(!d->scene||ARG(1)!=4||d->fvf!=0x144||ARG(6)!=101||ARG(8)!=28||!vertices||!primitives||primitives>limits.values[3]/48||
    (uint64_t)minimum+vertices>65536||vertex_bytes>limits.values[3]||index_bytes>limits.values[3]||!data||vertex_start>UINT32_MAX||
    !range((uint32_t)vertex_start,vertex_bytes)||!range(indices,index_bytes)){draw_ret(c,d,9,INVALID,"DrawIndexedPrimitiveUP");return;}
 uint32_t count=primitives*3;
 for(uint32_t i=0;i<count;i++){uint16_t v;memcpy(&v,HP(indices+i*2),2);if(v<minimum||(uint64_t)v>=(uint64_t)minimum+vertices){draw_ret(c,d,9,INVALID,"DrawIndexedPrimitiveUP");return;}}
 for(uint32_t i=0;i<vertices;i++){float v[7];memcpy(v,HP((uint32_t)vertex_start+i*28),28);
  if(!isfinite(v[0])||!isfinite(v[1])||!isfinite(v[2])||!isfinite(v[3])||!isfinite(v[5])||!isfinite(v[6])||v[2]<0||v[2]>1||v[3]<=0||!isfinite(1.f/v[3])){draw_ret(c,d,9,INVALID,"DrawIndexedPrimitiveUP");return;}}
 /* XYZRHW bypasses lighting, but unrelated unsupported culling remains explicit. */
 if(d->rs[22]!=1){unsupported(c,"D3D9 UP draw requires implemented no-culling mode");return;}
 Object*t=find(d->texture);if(!t&&!diffuse_equation(d)){unsupported(c,"D3D9 XYZRHW textured draw requires bound texture");return;}
 if(t&&d->target&&find(d->target)&&find(d->target)->container==t->address){unsupported(c,"D3D9 draw samples the active render target");return;}if(t&&t->locked){draw_ret(c,d,9,INVALID,"DrawIndexedPrimitiveUP");return;}
 uint32_t mask=0;if(t&&!texture_mask(c,d,&mask))return;
 uint8_t*vb=malloc((size_t)vertex_bytes);uint16_t*ib=malloc((size_t)index_bytes);if(!vb||!ib){free(vb);free(ib);draw_ret(c,d,9,OOM,"DrawIndexedPrimitiveUP");return;}
 memcpy(vb,HP((uint32_t)vertex_start),(size_t)vertex_bytes);
 if(mask)for(uint32_t i=0;i<vertices;i++){uint32_t color;memcpy(&color,vb+i*28+16,4);color|=mask;memcpy(vb+i*28+16,&color,4);}
 for(uint32_t i=0;i<count;i++){uint16_t v;memcpy(&v,HP(indices+i*2),2);ib[i]=(uint16_t)(v-minimum);}
 MoviesD3D9Request q={.command=MOVIES_D3D9_UPLOAD_VERTEX,.args={0x144},.data=vb,.bytes=(uint32_t)vertex_bytes};MoviesD3D9Reply r;uint32_t vg=0,ig=0;int ok=invoke(&q,&r)&&r.values[0];if(ok)vg=r.values[0];
 if(ok){q=(MoviesD3D9Request){.command=MOVIES_D3D9_UPLOAD_INDEX,.args={101},.data=ib,.bytes=(uint32_t)index_bytes};ok=invoke(&q,&r)&&r.values[0];if(ok)ig=r.values[0];}free(vb);free(ib);
 if(ok){q=(MoviesD3D9Request){.command=MOVIES_D3D9_DRAW,.args={vg,ig,0,count,0,d->rs[7]}};q.args[12]=t?t->gpu:0;q.args[20]=d->rs[15];q.args[21]=d->rs[25];q.args[22]=d->rs[24];q.args[23]=d->rs[23];q.args[24]=d->rs[14];q.args[25]=blend_encoding(d);q.args[26]=d->rs[174];q.args[31]=diffuse_texture_equation(d)?3:diffuse_equation(d)?2:factor_texture_equation(d);q.args[32]=d->rs[60];memcpy(q.args+27,d->scissor,16);const unsigned samplers[]={1,2,3,6,5,7,8};for(unsigned i=0;i<7;i++)q.args[13+i]=d->sampler[0][samplers[i]];memcpy(q.args+6,d->viewport,24);memcpy(q.matrices,d->matrices,sizeof q.matrices);ok=invoke(&q,&r);}
 uint32_t handles[]={vg,ig};for(unsigned i=0;i<2;i++)if(handles[i]){q=(MoviesD3D9Request){.command=MOVIES_D3D9_RELEASE_RESOURCE,.args={handles[i]}};if(!invoke(&q,&r))ok=0;}
 draw_ret(c,d,9,ok?0:LOST,"DrawIndexedPrimitiveUP");
}
static void get_stream_source(CPU*c){Object*d=self(c,DEVICE);uint32_t out=ARG(2),offset=ARG(3),stride=ARG(4);if(ARG(1)||!range(out,4)||!range(offset,4)||!range(stride,4)){ret(c,5,INVALID);return;}Object*b=find(d->vb);if(b)add(b);W32(out,d->vb);W32(offset,d->stream_offset);W32(stride,d->stride);ret(c,5,0);}
static void get_indices(CPU*c){Object*d=self(c,DEVICE);uint32_t out=ARG(1);if(!range(out,4)){ret(c,2,INVALID);return;}Object*b=find(d->ib);if(b)add(b);W32(out,d->ib);ret(c,2,0);}
/* Original video path:99B26D CreateTexture(levels1,usage0,A8R8G8B8,poolMANAGED),
 * 99A5FB LockRect(level0,NULL,NOSYSLOCK),99A62C UnlockRect(level0).
 * Guest BGRA byte order is preserved; only GPU upload changes it to RGBA. */
#define DXT1 0x31545844u
#define DXT3 0x33545844u
static int block_format(uint32_t format){return format==DXT1||format==DXT3;}
static uint32_t texture_pitch(uint32_t width,uint32_t format){return block_format(format)?((width+3)/4)*(format==DXT1?8:16):width*(format==26?2:4);}
/* DXT3/BC2: explicit four-bit alpha, always four RGB565 palette colors,
 * independent of endpoint ordering. Unpremultiplied, no gamma conversion.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/opaque-and-1-bit-alpha-textures
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/textures-with-alpha-channels */
static void texture_rgba(Object*t,uint8_t*rgba){
 const uint8_t*src=HP(t->mem);
 if(block_format(t->format)){
  uint32_t pitch=texture_pitch(t->width,t->format);
  for(uint32_t by=0;by<t->height;by+=4)for(uint32_t bx=0;bx<t->width;bx+=4){
   const uint8_t*b=src+(by/4)*pitch+(bx/4)*(t->format==DXT1?8:16);const uint8_t*rgb=b+(t->format==DXT1?0:8);uint8_t color[4][3];uint32_t ends[2];
   for(unsigned e=0;e<2;e++){uint32_t v=rgb[e*2]|((uint32_t)rgb[1+e*2]<<8);ends[e]=v;uint32_t r=v>>11,g=(v>>5)&63,bl=v&31;color[e][0]=(r<<3)|(r>>2);color[e][1]=(g<<2)|(g>>4);color[e][2]=(bl<<3)|(bl>>2);}
   int transparent=t->format==DXT1&&ends[0]<=ends[1];
   for(unsigned c=0;c<3;c++){color[2][c]=transparent?(color[0][c]+color[1][c])/2:(2*color[0][c]+color[1][c])/3;color[3][c]=transparent?0:(color[0][c]+2*color[1][c])/3;}
   for(unsigned y=0;y<4&&by+y<t->height;y++)for(unsigned x=0;x<4&&bx+x<t->width;x++){unsigned i=y*4+x,index=(rgb[4+y]>>(x*2))&3;uint8_t*out=rgba+((by+y)*t->width+bx+x)*4;memcpy(out,color[index],3);out[3]=t->format==DXT1?(transparent&&index==3?0:255):((b[i/2]>>((i&1)*4))&15)*17;}
  }
 }else for(uint32_t i=0;i<t->width*t->height;i++){
  if(t->format==26){uint32_t v=src[i*2]|((uint32_t)src[i*2+1]<<8);rgba[i*4]=((v>>8)&15)*17;rgba[i*4+1]=((v>>4)&15)*17;rgba[i*4+2]=(v&15)*17;rgba[i*4+3]=(v>>12)*17;}
  else{rgba[i*4]=src[i*4+2];rgba[i*4+1]=src[i*4+1];rgba[i*4+2]=src[i*4];rgba[i*4+3]=src[i*4+3];}
 }
}
static int upload_texture(Object*t){
 uint32_t bytes=t->width*t->height*4;uint8_t*rgba=malloc(bytes);if(!rgba)return 0;
 texture_rgba(t,rgba);
 MoviesD3D9Request q={.command=MOVIES_D3D9_UPLOAD_TEXTURE,.args={t->width,t->height},.data=rgba,.bytes=bytes};MoviesD3D9Reply r;
 int ok=invoke(&q,&r)&&r.values[0];free(rgba);if(!ok)return 0;
 uint32_t old=t->gpu;t->gpu=r.values[0];t->uploaded=t->version;
 if(old){MoviesD3D9Request release_={.command=MOVIES_D3D9_RELEASE_RESOURCE,.args={old}};MoviesD3D9Reply ignored;if(!invoke(&release_,&ignored))return 0;}return 1;
}
/* SYSTEMMEM dynamic resources are CPU-backed only; D3DPOOL permits this
 * combination. DISCARD contents are undefined (retaining old bytes is legal).
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dpool */
static void texture_create_failure(CPU*c,uint32_t hr){static uint32_t seen[16][4];static unsigned count;uint32_t key[4]={ARG(3),ARG(4),ARG(5),ARG(6)};int fresh=1;for(unsigned i=0;i<count;i++)if(!memcmp(seen[i],key,sizeof key))fresh=0;if(fresh&&count<16){memcpy(seen[count++],key,sizeof key);fprintf(stderr,"[D3D9] CreateTexture rejected caller=%08x result=%08x args=",R32(c->esp),hr);for(unsigned i=1;i<=8;i++)fprintf(stderr,"%s%08x",i==1?"":",",ARG(i));fprintf(stderr,"\n");}ret(c,9,hr);}
static void create_texture(CPU*c){Object*d=self(c,DEVICE);uint32_t w=ARG(1),h=ARG(2),out=ARG(7);MoviesD3D9Reply cap;
 if(!range(out,4)){ret(c,9,INVALID);return;}W32(out,0);
 if(!w||!h||ARG(8)){ret(c,9,INVALID);return;}
 /* Observed 99B26D: 512x512, one level, D3DUSAGE_RENDERTARGET, A8R8G8B8, DEFAULT.
  * GPU-only and not lockable (no D3DUSAGE_DYNAMIC); no guest copy exists.
  * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dusage */
 if(ARG(4)&1u){
  if(ARG(4)!=1||ARG(6)!=0||ARG(3)!=1||(ARG(5)!=21&&ARG(5)!=22)){texture_create_failure(c,UNAVAILABLE);return;}
  if(!caps(&cap)||w>cap.values[5]||h>cap.values[5]){texture_create_failure(c,UNAVAILABLE);return;}
  Object*t=allocate(TEXTURE);if(!t){ret(c,9,OOM);return;}
  t->parent=d->address;t->width=w;t->height=h;t->format=ARG(5);t->pool=0;t->usage=1;t->version=1;
  MoviesD3D9Request q={.command=MOVIES_D3D9_CREATE_TARGET,.args={w,h}};MoviesD3D9Reply r;
  if(!invoke(&q,&r)||!r.values[0]){W32(t->address,0);memset(t,0,sizeof*t);texture_create_failure(c,OOM);return;}
  t->gpu=r.values[0];t->uploaded=t->version;
  static unsigned created;if(created++<8)fprintf(stderr,"[D3D9] render-target texture created caller=%08x size=%ux%u format=%u texture=%08x\n",R32(c->esp),w,h,ARG(5),t->address);
  add(t);W32(out,t->address);ret(c,9,0);return;
 }
 int systemmem=ARG(6)==2;
 if(ARG(3)!=1||(ARG(5)!=21&&(systemmem||(ARG(5)!=26&&!block_format(ARG(5)))))||(!systemmem&&(ARG(4)||ARG(6)!=1))||(systemmem&&(ARG(4)&~0x200u))){texture_create_failure(c,UNAVAILABLE);return;}
 uint64_t decoded=(uint64_t)w*h*4;
 if(block_format(ARG(5))&&((w&3)||(h&3))){texture_create_failure(c,INVALID);return;}
 uint64_t size=block_format(ARG(5))?(uint64_t)(w/4)*(h/4)*(ARG(5)==DXT1?8:16):(uint64_t)w*h*(ARG(5)==26?2:4);
 if(!caps(&cap)||w>cap.values[5]||h>cap.values[5]||decoded>cap.values[3]||size>cap.values[3]){ret(c,9,UNAVAILABLE);return;}
 Object*t=allocate(TEXTURE);if(!t){ret(c,9,OOM);return;}
 t->parent=d->address;t->width=w;t->height=h;t->format=ARG(5);t->pool=ARG(6);t->usage=ARG(4);t->size=(uint32_t)size;t->version=1;t->mem=movies_backend_alloc(t->size);
 if(!t->mem){allocation_failure(TEXTURE,t->size,"guest texture storage allocation");W32(t->address,0);memset(t,0,sizeof*t);ret(c,9,OOM);return;}
 if(!systemmem&&!upload_texture(t)){destroy(t);ret(c,9,LOST);return;}
 add(t);W32(out,t->address);ret(c,9,0);
}
static void texture_levels(CPU*c){self(c,TEXTURE);ret(c,1,1);}
static void texture_set_lod(CPU*c){self(c,TEXTURE);ret(c,2,0); /* one managed mip: clamps requestedLOD to0 */}
static void texture_get_lod(CPU*c){self(c,TEXTURE);ret(c,1,0);}
static void texture_desc(CPU*c){Object*t=self(c,TEXTURE);uint32_t p=ARG(2);if(ARG(1)||!range(p,32)){ret(c,3,INVALID);return;}W32(p,t->format);W32(p+4,1);W32(p+8,t->usage);W32(p+12,t->pool);W32(p+16,0);W32(p+20,0);W32(p+24,t->width);W32(p+28,t->height);ret(c,3,0);}
static void texture_surface(CPU*c){Object*t=self(c,TEXTURE);uint32_t p=ARG(2);if(!range(p,4)){ret(c,3,INVALID);return;}W32(p,0);if(ARG(1)){ret(c,3,INVALID);return;}Object*s=find(t->surface);
 if(!s){s=allocate(SURFACE);if(!s){ret(c,3,OOM);return;}s->parent=t->parent;s->container=t->address;s->internal=1;s->format=t->format;s->pool=t->pool;s->usage=t->usage;s->width=t->width;s->height=t->height;t->surface=s->address;}
 add(s);W32(p,s->address);ret(c,3,0);
}
static int texture_lock(Object*t,uint32_t locked,uint32_t rect,uint32_t flags){
 if(!t->mem||t->locked||!range(locked,8)||(flags&~0x2810u)||((flags&0x2000u)&&(!(t->usage&0x200u)||rect||(flags&0x10u))))return 0;
 uint32_t left=0,top=0,right=t->width,bottom=t->height;
 if(rect){if(!range(rect,16))return 0;left=R32(rect);top=R32(rect+4);right=R32(rect+8);bottom=R32(rect+12);}
 if(left>=right||top>=bottom||right>t->width||bottom>t->height)return 0;
 if(block_format(t->format)&&((left|top|right|bottom)&3))return 0;
 uint32_t pitch=texture_pitch(t->width,t->format),offset=block_format(t->format)?(top/4)*pitch+(left/4)*(t->format==DXT1?8:16):top*pitch+left*(t->format==26?2:4);
 t->locked=1;t->lockflags=flags;W32(locked,pitch);W32(locked+4,t->mem+offset);return 1;
}
static uint32_t texture_unlock(Object*t){if(!t->locked)return INVALID;t->locked=0;if(t->lockflags&0x10)return 0;t->version++;return t->pool==2?0:upload_texture(t)?0:LOST;}
static void lock_texture(CPU*c){Object*t=self(c,TEXTURE);ret(c,5,!ARG(1)&&texture_lock(t,ARG(2),ARG(3),ARG(4))?0:INVALID);}
static void unlock_texture(CPU*c){Object*t=self(c,TEXTURE);ret(c,2,ARG(1)?INVALID:texture_unlock(t));}
static void lock_surface(CPU*c){Object*s=self(c,SURFACE);Object*t=find(s->container);if(!t){unsupported(c,"D3D9 back/depth surface LockRect unsupported");return;}ret(c,4,texture_lock(t,ARG(1),ARG(2),ARG(3))?0:INVALID);}
static void unlock_surface(CPU*c){Object*s=self(c,SURFACE);Object*t=find(s->container);if(!t){unsupported(c,"D3D9 back/depth surface UnlockRect unsupported");return;}ret(c,1,texture_unlock(t));}
static void surface_container(CPU*c){Object*s=self(c,SURFACE);uint32_t out=ARG(2);if(!range(out,4)){ret(c,3,INVALID);return;}W32(out,0);Object*t=find(s->container);if(!t||(!iid(ARG(1),ids[0])&&!iid(ARG(1),ids[TEXTURE])&&!iid(ARG(1),base_texture_id)&&!iid(ARG(1),resource_id))){ret(c,3,0x80004002);return;}add(t);W32(out,t->address);ret(c,3,0);}
static void set_texture(CPU*c){Object*d=self(c,DEVICE);Object*t=find(ARG(2));if(ARG(1)||((ARG(2)&&!t)||(t&&(t->type!=TEXTURE||t->parent!=d->address||t->pool==2)))){ret(c,3,INVALID);return;}bind(d,&d->texture,t);ret(c,3,0);}
static void get_texture(CPU*c){Object*d=self(c,DEVICE);uint32_t out=ARG(2);if(ARG(1)||!range(out,4)){ret(c,3,INVALID);return;}Object*t=find(d->texture);if(t)add(t);W32(out,t?t->address:0);ret(c,3,0);}
/* Valid query state domains are independent of our supported drawing subset.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dtexturestagestatetype
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dsamplerstatetype */
static int stage_state(uint32_t state){return (state>=1&&state<=11)||(state>=22&&state<=24)||(state>=26&&state<=28)||state==32;}
static void state_failure(CPU*c,const char*name){static unsigned reports;if(reports++<32)fprintf(stderr,"[D3D9] %s caller=%08x stage=%u state=%u valueOrOut=%08x\n",name,R32(c->esp),ARG(1),ARG(2),ARG(3));unsupported(c,name);}
static void set_texture_stage(CPU*c){Object*d=self(c,DEVICE);uint32_t stage=ARG(1),state=ARG(2),value=ARG(3);
 if(stage>=8||!stage_state(state)){state_failure(c,"D3D9 SetTextureStageState invalid stage/state");return;}
 if(d->tss[stage][state]==value){ret(c,4,0);return;} /* exact no-op preserves state, including disabled stages */
 int ok=!stage&&((state==1||state==4)?(value==2||value==3||value==4):(state==2||state==5||state==3||state==6)?value<=3:(state==11||state==24)?value==0:0);
 if(!ok){state_failure(c,"D3D9 SetTextureStageState unsupported state/value");return;}d->tss[stage][state]=value;ret(c,4,0);}
static void get_texture_stage(CPU*c){Object*d=self(c,DEVICE);uint32_t stage=ARG(1),state=ARG(2);if(!range(ARG(3),4)){ret(c,4,INVALID);return;}if(stage>=8||!stage_state(state)){state_failure(c,"D3D9 GetTextureStageState invalid stage/state");return;}W32(ARG(3),d->tss[stage][state]);ret(c,4,0);}
static int sampler_state(uint32_t state){return state>=1&&state<=13;}
static void set_sampler(CPU*c){Object*d=self(c,DEVICE);uint32_t stage=ARG(1),state=ARG(2),value=ARG(3);
 if(stage>=16||!sampler_state(state)){state_failure(c,"D3D9 SetSamplerState invalid sampler/state");return;}
 if(d->sampler[stage][state]==value){ret(c,4,0);return;}
 float bias;memcpy(&bias,&value,4);int ok=(state<=3)?(value>=1&&value<=3):(state==5||state==6)?(value==1||value==2):state==7?value<=2:state==8?(isfinite(bias)&&bias>=-16.f&&bias<=15.99f):state==10?value==1:value==0;
 if(!ok){state_failure(c,"D3D9 SetSamplerState unsupported address/filter/mip/bias");return;}d->sampler[stage][state]=value;ret(c,4,0);}
static void get_sampler(CPU*c){Object*d=self(c,DEVICE);uint32_t stage=ARG(1),state=ARG(2);if(!range(ARG(3),4)){ret(c,4,INVALID);return;}if(stage>=16||!sampler_state(state)){state_failure(c,"D3D9 GetSamplerState invalid sampler/state");return;}W32(ARG(3),d->sampler[stage][state]);ret(c,4,0);}
static void unsupported_0_3(CPU*c){unsupported(c,"IDirect3D9::RegisterSoftwareDevice unsupported");}
static void unsupported_0_13(CPU*c){unsupported(c,"IDirect3D9::CheckDeviceFormatConversion unsupported");}
static void unsupported_0_15(CPU*c){unsupported(c,"IDirect3D9::GetAdapterMonitor unsupported");}
static GuestFn table_0[]={query,addref,release,unsupported_0_3,adapter_count,identifier,mode_count,enum_mode,display_mode,device_type,check_format,multisample,depth_match,unsupported_0_13,adapter_caps,unsupported_0_15,create_device};
static void unsupported_1_5(CPU*c){unsupported(c,"IDirect3DDevice9::EvictManagedResources unsupported");}
static void unsupported_1_10(CPU*c){unsupported(c,"IDirect3DDevice9::SetCursorProperties unsupported");}
static void unsupported_1_11(CPU*c){unsupported(c,"IDirect3DDevice9::SetCursorPosition unsupported");}
static void unsupported_1_12(CPU*c){unsupported(c,"IDirect3DDevice9::ShowCursor unsupported");}
static void unsupported_1_13(CPU*c){unsupported(c,"IDirect3DDevice9::CreateAdditionalSwapChain unsupported");}
static void unsupported_1_14(CPU*c){unsupported(c,"IDirect3DDevice9::GetSwapChain unsupported");}
static void unsupported_1_15(CPU*c){unsupported(c,"IDirect3DDevice9::GetNumberOfSwapChains unsupported");}
static void unsupported_1_16(CPU*c){unsupported(c,"IDirect3DDevice9::Reset unsupported");}
static void unsupported_1_19(CPU*c){unsupported(c,"IDirect3DDevice9::GetRasterStatus unsupported");}
static void unsupported_1_20(CPU*c){unsupported(c,"IDirect3DDevice9::SetDialogBoxMode unsupported");}
static void unsupported_1_21(CPU*c){unsupported(c,"IDirect3DDevice9::SetGammaRamp unsupported");}
static void unsupported_1_22(CPU*c){unsupported(c,"IDirect3DDevice9::GetGammaRamp unsupported");}
static void unsupported_1_24(CPU*c){unsupported(c,"IDirect3DDevice9::CreateVolumeTexture unsupported");}
static void unsupported_1_25(CPU*c){unsupported(c,"IDirect3DDevice9::CreateCubeTexture unsupported");}
static void unsupported_1_28(CPU*c){unsupported(c,"IDirect3DDevice9::CreateRenderTarget unsupported");}
static void unsupported_1_29(CPU*c){unsupported(c,"IDirect3DDevice9::CreateDepthStencilSurface unsupported");}
static Object*surface_texture(Object*s){return s&&s->type==SURFACE?find(s->container):NULL;}
/* Observed 9A5335: UpdateSurface(SYSTEMMEM A8R8G8B8 level0 0x105ca64, NULL, RT level0
 * 0x105ca60, NULL) after CPU edits. Whole-surface copies into render-target textures only.
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-updatesurface */
static void update_surface(CPU*c){Object*d=self(c,DEVICE);Object*src=find(ARG(1)),*dst=find(ARG(3)),*st=surface_texture(src),*dt=surface_texture(dst);
 if(!src||!dst||src->type!=SURFACE||dst->type!=SURFACE||src->parent!=d->address||dst->parent!=d->address){ret(c,5,INVALID);return;}
 if(ARG(2)||ARG(4)){unsupported(c,"D3D9 UpdateSurface sub-rectangles unsupported");return;}
 if(!st||st->pool!=2||!dt||dt->pool!=0||st->locked){ret(c,5,INVALID);return;}
 if(!(dt->usage&1u)||!dt->gpu){unsupported(c,"D3D9 UpdateSurface destination must be a render-target texture");return;}
 if(st->width!=dt->width||st->height!=dt->height||st->format!=21||(dt->format!=21&&dt->format!=22)){ret(c,5,INVALID);return;}
 uint32_t bytes=st->width*st->height*4;uint8_t*rgba=malloc(bytes);if(!rgba){ret(c,5,OOM);return;}texture_rgba(st,rgba);
 MoviesD3D9Request q={.command=MOVIES_D3D9_WRITE_TARGET,.args={dt->gpu,dt->width,dt->height},.data=rgba,.bytes=bytes};MoviesD3D9Reply r;int ok=invoke(&q,&r);free(rgba);ret(c,5,ok?0:LOST);}
static void unsupported_1_31(CPU*c){unsupported(c,"IDirect3DDevice9::UpdateTexture unsupported");}
/* Observed 9A52D9: GetRenderTargetData(RT level0 -> same-size SYSTEMMEM A8R8G8B8), then
 * LockRect. Backbuffer sources are not claimed.
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-getrendertargetdata */
static void render_target_data(CPU*c){Object*d=self(c,DEVICE);Object*src=find(ARG(1)),*dst=find(ARG(2)),*st=surface_texture(src),*dt=surface_texture(dst);
 if(!src||!dst||src->type!=SURFACE||dst->type!=SURFACE||src->parent!=d->address||dst->parent!=d->address){ret(c,3,INVALID);return;}
 if(!st||!(st->usage&1u)||!st->gpu){unsupported(c,"D3D9 GetRenderTargetData source must be a render-target texture");return;}
 if(!dt||dt->pool!=2||dt->locked||dt->width!=st->width||dt->height!=st->height||dt->format!=21){ret(c,3,INVALID);return;}
 MoviesD3D9Request q={.command=MOVIES_D3D9_READ_TARGET,.args={st->gpu,st->width,st->height},.data=HP(dt->mem),.bytes=dt->size};MoviesD3D9Reply r;
 if(!invoke(&q,&r)){ret(c,3,LOST);return;}dt->version++;
 static unsigned logs;if(logs++<4)fprintf(stderr,"[D3D9] GetRenderTargetData caller=%08x %ux%u\n",R32(c->esp),st->width,st->height);
 ret(c,3,0);}
static void unsupported_1_33(CPU*c){unsupported(c,"IDirect3DDevice9::GetFrontBufferData unsupported");}
static void unsupported_1_34(CPU*c){unsupported(c,"IDirect3DDevice9::StretchRect unsupported");}
static void unsupported_1_35(CPU*c){unsupported(c,"IDirect3DDevice9::ColorFill unsupported");}
static void unsupported_1_36(CPU*c){unsupported(c,"IDirect3DDevice9::CreateOffscreenPlainSurface unsupported");}
/* One simultaneous target (NumSimultaneousRTs=1). Index0 rejects NULL. Accepts the
 * implicit backbuffer or level0 of this device's RENDERTARGET texture. Setting a
 * target resets viewport and scissor to the full target (as Wine's wined3d does).
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-setrendertarget */
static void set_render_target(CPU*c){Object*d=self(c,DEVICE);uint32_t index=ARG(1);Object*s=find(ARG(2));
 if(index||!s||s->type!=SURFACE||s->parent!=d->address){ret(c,3,INVALID);return;}
 Object*t=find(s->container);int backbuffer=s->address==d->rt;
 if(!backbuffer&&(!t||!(t->usage&1u)||!t->gpu)){ret(c,3,INVALID);return;}
 if(t&&t->address==d->texture){unsupported(c,"D3D9 SetRenderTarget on the bound texture");return;}
 MoviesD3D9Request q={.command=MOVIES_D3D9_SET_TARGET,.args={backbuffer?0:t->gpu,s->width,s->height}};MoviesD3D9Reply r;
 if(!invoke(&q,&r)){ret(c,3,LOST);return;}
 bind_target(d,backbuffer?NULL:s);
 uint32_t one=0x3f800000u,vp[6]={0,0,s->width,s->height,0,one};memcpy(d->viewport,vp,24);
 int32_t rect[4]={0,0,(int32_t)s->width,(int32_t)s->height};memcpy(d->scissor,rect,16);
 static unsigned logs;if(logs++<16)fprintf(stderr,"[D3D9] SetRenderTarget caller=%08x surface=%08x target=%s %ux%u\n",R32(c->esp),s->address,backbuffer?"backbuffer":"texture",s->width,s->height);
 ret(c,3,0);}
static void unsupported_1_39(CPU*c){unsupported(c,"IDirect3DDevice9::SetDepthStencilSurface unsupported");}
static void unsupported_1_46(CPU*c){unsupported(c,"IDirect3DDevice9::MultiplyTransform unsupported");}
static void set_material(CPU*c){Object*d=self(c,DEVICE);uint32_t p=ARG(1);if(!range(p,68)){ret(c,2,INVALID);return;}float values[17];memcpy(values,HP(p),68);for(unsigned i=0;i<17;i++)if(!isfinite(values[i])){ret(c,2,INVALID);return;}if(values[16]<0){ret(c,2,INVALID);return;}memcpy(d->material,values,68);d->material_set=1;ret(c,2,0);}

static void get_material(CPU*c){Object*d=self(c,DEVICE);if(!range(ARG(1),68)){ret(c,2,INVALID);return;}if(!d->material_set){unsupported(c,"D3D9 GetMaterial before explicitly initialized material");return;}memcpy(HP(ARG(1)),d->material,68);ret(c,2,0);}

/* Defined light state does not advertise a lighting renderer. Every lit draw
 * still traps until a supported normal-bearing FVF and lighting path exist. */
static LightState*light_state(Object*d,uint32_t index,int create){LightState*empty=NULL;for(unsigned i=0;i<16;i++){LightState*l=&d->lights[i];if(l->used&&l->index==index)return l;if(!l->used&&!empty)empty=l;}if(!create||!empty)return NULL;empty->used=1;empty->index=index;empty->words[0]=3;empty->words[1]=empty->words[2]=empty->words[3]=empty->words[18]=0x3f800000u;return empty;}
static void set_light(CPU*c){Object*d=self(c,DEVICE);uint32_t index=ARG(1),p=ARG(2);if(!range(p,104)){ret(c,3,INVALID);return;}uint32_t words[26];float values[26];memcpy(words,HP(p),104);memcpy(values,words,104);uint32_t type=words[0];if(type<1||type>3){ret(c,3,INVALID);return;}for(unsigned i=1;i<26;i++)if(!isfinite(values[i])){ret(c,3,INVALID);return;}
 if(type!=3&&(values[19]<0||values[19]>1.8446743e19f||values[21]<0||values[22]<0||values[23]<0)){ret(c,3,INVALID);return;}if(type==2&&(values[20]<0||values[24]<0||values[24]>values[25]||values[25]>3.1415927f)){ret(c,3,INVALID);return;}
 LightState*l=light_state(d,index,1);if(!l){ret(c,3,OOM);return;}memcpy(l->words,words,104);static unsigned reports;if(reports++<16){fprintf(stderr,"[D3D9] SetLight caller=%08x index=%u type=%u words=",R32(c->esp),index,type);for(unsigned i=0;i<26;i++)fprintf(stderr,"%s%08x",i?",":"",words[i]);fprintf(stderr,"\n");}ret(c,3,0);}
static void get_light(CPU*c){Object*d=self(c,DEVICE);LightState*l=light_state(d,ARG(1),0);if(!l||!range(ARG(2),104)){ret(c,3,INVALID);return;}memcpy(HP(ARG(2)),l->words,104);ret(c,3,0);}
static void light_enable(CPU*c){Object*d=self(c,DEVICE);LightState*l=light_state(d,ARG(1),1);if(!l){ret(c,3,OOM);return;}l->enabled=ARG(2)!=0;ret(c,3,0);}
static void get_light_enable(CPU*c){Object*d=self(c,DEVICE);LightState*l=light_state(d,ARG(1),0);if(!l||!range(ARG(2),4)){ret(c,3,INVALID);return;}W32(ARG(2),l->enabled);ret(c,3,0);}
static void unsupported_1_55(CPU*c){unsupported(c,"IDirect3DDevice9::SetClipPlane unsupported");}
static void unsupported_1_56(CPU*c){unsupported(c,"IDirect3DDevice9::GetClipPlane unsupported");}
static void unsupported_1_59(CPU*c){unsupported(c,"IDirect3DDevice9::CreateStateBlock unsupported");}
static void unsupported_1_60(CPU*c){unsupported(c,"IDirect3DDevice9::BeginStateBlock unsupported");}
static void unsupported_1_61(CPU*c){unsupported(c,"IDirect3DDevice9::EndStateBlock unsupported");}
static void unsupported_1_62(CPU*c){unsupported(c,"IDirect3DDevice9::SetClipStatus unsupported");}
static void unsupported_1_63(CPU*c){unsupported(c,"IDirect3DDevice9::GetClipStatus unsupported");}
static void unsupported_1_70(CPU*c){unsupported(c,"IDirect3DDevice9::ValidateDevice unsupported");}
static void unsupported_1_71(CPU*c){unsupported(c,"IDirect3DDevice9::SetPaletteEntries unsupported");}
static void unsupported_1_72(CPU*c){unsupported(c,"IDirect3DDevice9::GetPaletteEntries unsupported");}
static void unsupported_1_73(CPU*c){unsupported(c,"IDirect3DDevice9::SetCurrentTexturePalette unsupported");}
static void unsupported_1_74(CPU*c){unsupported(c,"IDirect3DDevice9::GetCurrentTexturePalette unsupported");}
/* RECT is target-relative and right/bottom exclusive, independent of viewport.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/scissor-test */
static void set_scissor(CPU*c){Object*d=self(c,DEVICE);uint32_t p=ARG(1);int32_t rect[4];if(!range(p,16)){ret(c,2,INVALID);return;}memcpy(rect,HP(p),16);uint32_t tw,th;target_size(d,&tw,&th);
 if(rect[0]<0||rect[1]<0||rect[2]<rect[0]||rect[3]<rect[1]||(uint32_t)rect[2]>tw||(uint32_t)rect[3]>th){ret(c,2,INVALID);return;}memcpy(d->scissor,rect,16);ret(c,2,0);}
static void get_scissor(CPU*c){Object*d=self(c,DEVICE);if(!range(ARG(1),16)){ret(c,2,INVALID);return;}memcpy(HP(ARG(1)),d->scissor,16);ret(c,2,0);}
static void unsupported_1_77(CPU*c){unsupported(c,"IDirect3DDevice9::SetSoftwareVertexProcessing unsupported");}
static void unsupported_1_78(CPU*c){unsupported(c,"IDirect3DDevice9::GetSoftwareVertexProcessing unsupported");}
static void unsupported_1_79(CPU*c){unsupported(c,"IDirect3DDevice9::SetNPatchMode unsupported");}
static void unsupported_1_80(CPU*c){unsupported(c,"IDirect3DDevice9::GetNPatchMode unsupported");}
static void unsupported_1_81(CPU*c){unsupported(c,"IDirect3DDevice9::DrawPrimitive unsupported");}
static void unsupported_1_83(CPU*c){unsupported(c,"IDirect3DDevice9::DrawPrimitiveUP unsupported");}
static void unsupported_1_85(CPU*c){unsupported(c,"IDirect3DDevice9::ProcessVertices unsupported");}
static void unsupported_1_86(CPU*c){unsupported(c,"IDirect3DDevice9::CreateVertexDeclaration unsupported");}
static void unsupported_1_87(CPU*c){unsupported(c,"IDirect3DDevice9::SetVertexDeclaration unsupported");}
static void unsupported_1_88(CPU*c){unsupported(c,"IDirect3DDevice9::GetVertexDeclaration unsupported");}
static void unsupported_1_91(CPU*c){unsupported(c,"IDirect3DDevice9::CreateVertexShader unsupported");}
/* Fixed-function pipeline has no guest programmable shader object. These
 * bindings leave FVF and texture-stage state unchanged. Non-null shaders still
 * trap; no programmable shader capability is advertised.
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-setvertexshader
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-getpixelshader */
static void set_vertex_shader(CPU*c){self(c,DEVICE);if(ARG(1)){unsupported(c,"D3D9 non-null vertex shader execution unsupported");return;}ret(c,2,0);}
static void get_vertex_shader(CPU*c){self(c,DEVICE);uint32_t out=ARG(1),shader=0;if(!range(out,4)){ret(c,2,INVALID);return;}memcpy(HP(out),&shader,4);ret(c,2,0);}
static void set_pixel_shader(CPU*c){self(c,DEVICE);if(ARG(1)){unsupported(c,"D3D9 non-null pixel shader execution unsupported");return;}ret(c,2,0);}
static void get_pixel_shader(CPU*c){Object*d=self(c,DEVICE);uint32_t out=ARG(1),shader=0;if((d->flags&0x10)||!range(out,4)){ret(c,2,INVALID);return;}memcpy(HP(out),&shader,4);ret(c,2,0);}
static void unsupported_1_94(CPU*c){unsupported(c,"IDirect3DDevice9::SetVertexShaderConstantF unsupported");}
static void unsupported_1_95(CPU*c){unsupported(c,"IDirect3DDevice9::GetVertexShaderConstantF unsupported");}
static void unsupported_1_96(CPU*c){unsupported(c,"IDirect3DDevice9::SetVertexShaderConstantI unsupported");}
static void unsupported_1_97(CPU*c){unsupported(c,"IDirect3DDevice9::GetVertexShaderConstantI unsupported");}
static void unsupported_1_98(CPU*c){unsupported(c,"IDirect3DDevice9::SetVertexShaderConstantB unsupported");}
static void unsupported_1_99(CPU*c){unsupported(c,"IDirect3DDevice9::GetVertexShaderConstantB unsupported");}
static void unsupported_1_102(CPU*c){unsupported(c,"IDirect3DDevice9::SetStreamSourceFreq unsupported");}
static void unsupported_1_103(CPU*c){unsupported(c,"IDirect3DDevice9::GetStreamSourceFreq unsupported");}
static void unsupported_1_106(CPU*c){unsupported(c,"IDirect3DDevice9::CreatePixelShader unsupported");}
static void unsupported_1_109(CPU*c){unsupported(c,"IDirect3DDevice9::SetPixelShaderConstantF unsupported");}
static void unsupported_1_110(CPU*c){unsupported(c,"IDirect3DDevice9::GetPixelShaderConstantF unsupported");}
static void unsupported_1_111(CPU*c){unsupported(c,"IDirect3DDevice9::SetPixelShaderConstantI unsupported");}
static void unsupported_1_112(CPU*c){unsupported(c,"IDirect3DDevice9::GetPixelShaderConstantI unsupported");}
static void unsupported_1_113(CPU*c){unsupported(c,"IDirect3DDevice9::SetPixelShaderConstantB unsupported");}
static void unsupported_1_114(CPU*c){unsupported(c,"IDirect3DDevice9::GetPixelShaderConstantB unsupported");}
static void unsupported_1_115(CPU*c){unsupported(c,"IDirect3DDevice9::DrawRectPatch unsupported");}
static void unsupported_1_116(CPU*c){unsupported(c,"IDirect3DDevice9::DrawTriPatch unsupported");}
static void unsupported_1_117(CPU*c){unsupported(c,"IDirect3DDevice9::DeletePatch unsupported");}
static void unsupported_1_118(CPU*c){unsupported(c,"IDirect3DDevice9::CreateQuery unsupported");}
static GuestFn table_1[]={query,addref,release,cooperative,available_texture_memory,unsupported_1_5,get_direct3d,get_caps,device_mode,creation_parameters,unsupported_1_10,unsupported_1_11,unsupported_1_12,unsupported_1_13,unsupported_1_14,unsupported_1_15,unsupported_1_16,present,get_backbuffer,unsupported_1_19,unsupported_1_20,unsupported_1_21,unsupported_1_22,create_texture,unsupported_1_24,unsupported_1_25,create_vb,create_ib,unsupported_1_28,unsupported_1_29,update_surface,unsupported_1_31,render_target_data,unsupported_1_33,unsupported_1_34,unsupported_1_35,unsupported_1_36,set_render_target,get_rt,unsupported_1_39,get_ds,begin_scene,end_scene,clear_target,set_transform,get_transform,unsupported_1_46,set_viewport,get_viewport,set_material,get_material,set_light,get_light,light_enable,get_light_enable,unsupported_1_55,unsupported_1_56,set_state,get_state,unsupported_1_59,unsupported_1_60,unsupported_1_61,unsupported_1_62,unsupported_1_63,get_texture,set_texture,get_texture_stage,set_texture_stage,get_sampler,set_sampler,unsupported_1_70,unsupported_1_71,unsupported_1_72,unsupported_1_73,unsupported_1_74,set_scissor,get_scissor,unsupported_1_77,unsupported_1_78,unsupported_1_79,unsupported_1_80,unsupported_1_81,draw_indexed,unsupported_1_83,draw_indexed_up,unsupported_1_85,unsupported_1_86,unsupported_1_87,unsupported_1_88,set_fvf,get_fvf,unsupported_1_91,set_vertex_shader,get_vertex_shader,unsupported_1_94,unsupported_1_95,unsupported_1_96,unsupported_1_97,unsupported_1_98,unsupported_1_99,stream_source,get_stream_source,unsupported_1_102,unsupported_1_103,set_indices,get_indices,unsupported_1_106,set_pixel_shader,get_pixel_shader,unsupported_1_109,unsupported_1_110,unsupported_1_111,unsupported_1_112,unsupported_1_113,unsupported_1_114,unsupported_1_115,unsupported_1_116,unsupported_1_117,unsupported_1_118};
static void unsupported_2_4(CPU*c){unsupported(c,"IDirect3DSurface9::SetPrivateData unsupported");}
static void unsupported_2_5(CPU*c){unsupported(c,"IDirect3DSurface9::GetPrivateData unsupported");}
static void unsupported_2_6(CPU*c){unsupported(c,"IDirect3DSurface9::FreePrivateData unsupported");}
static void unsupported_2_7(CPU*c){unsupported(c,"IDirect3DSurface9::SetPriority unsupported");}
static void unsupported_2_8(CPU*c){unsupported(c,"IDirect3DSurface9::GetPriority unsupported");}
static void unsupported_2_9(CPU*c){unsupported(c,"IDirect3DSurface9::PreLoad unsupported");}
static void unsupported_2_15(CPU*c){unsupported(c,"IDirect3DSurface9::GetDC unsupported");}
static void unsupported_2_16(CPU*c){unsupported(c,"IDirect3DSurface9::ReleaseDC unsupported");}
static GuestFn table_2[]={query,addref,release,resource_device,unsupported_2_4,unsupported_2_5,unsupported_2_6,unsupported_2_7,unsupported_2_8,unsupported_2_9,resource_type,surface_container,surface_desc,lock_surface,unlock_surface,unsupported_2_15,unsupported_2_16};
static void unsupported_3_4(CPU*c){unsupported(c,"IDirect3DVertexBuffer9::SetPrivateData unsupported");}
static void unsupported_3_5(CPU*c){unsupported(c,"IDirect3DVertexBuffer9::GetPrivateData unsupported");}
static void unsupported_3_6(CPU*c){unsupported(c,"IDirect3DVertexBuffer9::FreePrivateData unsupported");}
static void unsupported_3_7(CPU*c){unsupported(c,"IDirect3DVertexBuffer9::SetPriority unsupported");}
static void unsupported_3_8(CPU*c){unsupported(c,"IDirect3DVertexBuffer9::GetPriority unsupported");}
static void unsupported_3_9(CPU*c){unsupported(c,"IDirect3DVertexBuffer9::PreLoad unsupported");}
static GuestFn table_3[]={query,addref,release,resource_device,unsupported_3_4,unsupported_3_5,unsupported_3_6,unsupported_3_7,unsupported_3_8,unsupported_3_9,resource_type,buffer_lock,buffer_unlock,buffer_desc};
static void unsupported_4_4(CPU*c){unsupported(c,"IDirect3DIndexBuffer9::SetPrivateData unsupported");}
static void unsupported_4_5(CPU*c){unsupported(c,"IDirect3DIndexBuffer9::GetPrivateData unsupported");}
static void unsupported_4_6(CPU*c){unsupported(c,"IDirect3DIndexBuffer9::FreePrivateData unsupported");}
static void unsupported_4_7(CPU*c){unsupported(c,"IDirect3DIndexBuffer9::SetPriority unsupported");}
static void unsupported_4_8(CPU*c){unsupported(c,"IDirect3DIndexBuffer9::GetPriority unsupported");}
static void unsupported_4_9(CPU*c){unsupported(c,"IDirect3DIndexBuffer9::PreLoad unsupported");}
static GuestFn table_4[]={query,addref,release,resource_device,unsupported_4_4,unsupported_4_5,unsupported_4_6,unsupported_4_7,unsupported_4_8,unsupported_4_9,resource_type,buffer_lock,buffer_unlock,buffer_desc};
static void unsupported_5_4(CPU*c){unsupported(c,"IDirect3DTexture9::SetPrivateData unsupported");}
static void unsupported_5_5(CPU*c){unsupported(c,"IDirect3DTexture9::GetPrivateData unsupported");}
static void unsupported_5_6(CPU*c){unsupported(c,"IDirect3DTexture9::FreePrivateData unsupported");}
static void unsupported_5_7(CPU*c){unsupported(c,"IDirect3DTexture9::SetPriority unsupported");}
static void unsupported_5_8(CPU*c){unsupported(c,"IDirect3DTexture9::GetPriority unsupported");}
static void unsupported_5_9(CPU*c){unsupported(c,"IDirect3DTexture9::PreLoad unsupported");}
static void unsupported_5_14(CPU*c){unsupported(c,"IDirect3DTexture9::SetAutoGenFilterType unsupported");}
static void unsupported_5_15(CPU*c){unsupported(c,"IDirect3DTexture9::GetAutoGenFilterType unsupported");}
static void unsupported_5_16(CPU*c){unsupported(c,"IDirect3DTexture9::GenerateMipSubLevels unsupported");}
static void unsupported_5_21(CPU*c){unsupported(c,"IDirect3DTexture9::AddDirtyRect unsupported");}
static GuestFn table_5[]={query,addref,release,resource_device,unsupported_5_4,unsupported_5_5,unsupported_5_6,unsupported_5_7,unsupported_5_8,unsupported_5_9,resource_type,texture_set_lod,texture_get_lod,texture_levels,unsupported_5_14,unsupported_5_15,unsupported_5_16,texture_desc,texture_surface,lock_texture,unlock_texture,unsupported_5_21};
int movies_d3d9_init(void){
 if(initialized&&!on_owner())return 0;
 for(unsigned i=0;i<OBJECT_CAPACITY;i++)if(objects[i]&&objects[i]->type)return 0;
 for(unsigned i=0;i<OBJECT_CAPACITY;i++){free(objects[i]);objects[i]=NULL;}
 if((uint64_t)BASE+0x40000>MOVIES_D3D9_SPACE)return 0;
 owner=pthread_self();memset(HP(BASE),0,0x40000);live_device=0;
 GuestFn *tables[]={table_0,table_1,table_2,table_3,table_4,table_5};
 const unsigned counts[]={17,119,17,14,14,22};
 for(unsigned t=0;t<6;t++)for(unsigned i=0;i<counts[t];i++){uint32_t target=TRAMP+t*0x1000+i*16;register_function(target,tables[t][i]);W32(BASE+t*0x400+i*4,target);}
 initialized=1;return 1;
}
void movies_d3d9_set_host(MoviesD3D9Host callback,void*context){if(initialized&&!on_owner())abort();host=callback;host_context=context;}
ImportHandler *movies_d3d9_handlers(uint32_t*count){static ImportHandler handlers[]={{"D3D9.DLL!Direct3DCreate9",factory}};*count=1;return handlers;}
