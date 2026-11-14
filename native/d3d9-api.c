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
/* Per-buffer GPU upload cache: draws sharing one buffer with different ranges or
 * conversions keep their own GPU copies instead of evicting each other every draw. */
#define UPLOAD_CACHE 4
typedef struct UploadEntry {uint32_t gpu,version,offset,bytes,fvf,mask,bias,key,age;} UploadEntry;
typedef struct Object {
 uint32_t type,address,refs,internal,parent,format,usage,pool,size,mem,gpu,version,uploaded,offset,upload_size,index_bias;
 uint32_t width,height,depth,hwnd,flags,interval,windowed,rt,ds,vb,ib,stride,stream_offset,fvf,scene,locked;
 uint32_t draw_ack_logged,draw_reject_logged,draw104_logged;uint32_t diag_draws,diag_draw_ok,diag_clears,diag_clear_ok,diag_clear_caller,diag_present,diag_parent,diag_logs,diag_table_seen,diag_table_state,diag_lock_attempt,diag_lock_ok,diag_cooperative,diag_cooperative_failed;uint32_t viewport[6],scissor[4],rs[256],tss[8][33],sampler[16][14],texture,container,surface,lockflags,color_mask,lost,target;float matrices[48],material[17];uint32_t material_set,light_hash,specular_logged,stage_textures[7],stage_logged,texmat_set;float texmat[8][16];UploadEntry cache[UPLOAD_CACHE];uint32_t cache_clock;float clip_planes[6][4];LightState lights[16];
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
  const v=Array.from(HEAPU32.subarray(a>>>2,(a>>>2)+64));
  let command,args={},result;const scissor=v[26]?{x:v[27],y:v[28],width:v[29]-v[27],height:v[30]-v[28]}:null;
  if(cmd===1){command='capabilities';}
  else if(cmd===2){command='createDevice';args={width:v[0],height:v[1],hwnd:v[2],depth16:!!v[3],windowed:!!v[4],presentationInterval:v[5],backBufferFormat:v[6]};}
  else if(cmd===3){command='clear';const f=new Float32Array(new Uint32Array([v[1]]).buffer)[0];args={color:v[0],depth:f,scissor,zbuffer:(v[2]&2)!==0,...(v[2]&1?{}:{target:false})};}
  else if(cmd===4){command='present';}
  else if(cmd===5||cmd===6){command=cmd===5?'uploadVertices':'uploadIndices';args={bytes:HEAPU8.slice(data,data+bytes).buffer,format:cmd===5?(v[0]===0xffff?['raw',v[1],v[2],...[v[3],v[4],v[5],v[6]].map(x=>x?x-1:'-')].join('/'):v[0]===0x252?'XYZ_NORMAL_DIFFUSE_UV2':v[0]===0x104?'XYZRHW_UV':v[0]===0x244?'XYZRHW_DIFFUSE_UV2':v[0]===0x242?'XYZ_DIFFUSE_UV2':v[0]===0x144?'XYZRHW_DIFFUSE_UV':v[0]===0x142?'XYZ_DIFFUSE_UV':'XYZ_DIFFUSE'):v[0]===101?'uint16':'uint32'};}
  else if(cmd===7){command='drawIndexed';const f=new Float32Array(new Uint32Array([v[10],v[11]]).buffer);args={vertices:v[0],indices:v[1],firstIndex:v[2],indexCount:v[3],baseVertex:v[4]|0,depthTest:!!v[5],depthCompare:['never','less','equal','less-equal','greater','not-equal','greater-equal','always'][v[23]-1]??'less-equal',...(v[60]&0x100?{colorWriteMask:v[60]&15,blendConstant:v[57],depthBias:new Float32Array(new Uint32Array([v[58]]).buffer)[0],slopeScaleDepthBias:new Float32Array(new Uint32Array([v[59]]).buffer)[0]}:{}),depthWrite:!!v[24],blend:!!v[25],...(v[25]&256?{blendFactors:{source:(v[25]>>>4)&15,destination:v[25]&15,...(v[56]&15?{operation:v[56]&15}:{}),...((v[56]>>>8)&1?{alphaSource:(v[56]>>>12)&15,alphaDestination:(v[56]>>>16)&15,alphaOperation:((v[56]>>>4)&15)||1}:{})}}:{}),blendMode:v[25]===3?'destination-times-source':v[25]===2?'source-color':'source-alpha',...(v[45]?{texture1:v[45],sampler1:{addressU:v[48],addressV:v[49],addressW:v[50],minFilter:v[51],magFilter:v[52],mipFilter:v[53],lodBias:new Float32Array(new Uint32Array([v[54]]).buffer)[0]}}:{}),...(v[31]===4?{stages:[v.slice(33,39),v.slice(39,45)]}:{textureEquation:v[31]===3?'diffuse-rgb-texture-alpha':v[31]===2?'diffuse':v[31]?'factor-rgb-texture-alpha':'modulate'}),textureFactor:v[32],scissor,texture:v[12]||null,alphaTest:{enabled:!!v[20],func:v[21],reference:v[22]},sampler:{addressU:v[13],addressV:v[14],addressW:v[15],minFilter:v[16],magFilter:v[17],mipFilter:v[18],lodBias:new Float32Array(new Uint32Array([v[19]]).buffer)[0]},viewport:{x:v[6],y:v[7],width:v[8],height:v[9],minDepth:f[0],maxDepth:f[1]},world:Array.from(HEAPF32.subarray(m>>>2,(m>>>2)+16)),view:Array.from(HEAPF32.subarray((m>>>2)+16,(m>>>2)+32)),projection:Array.from(HEAPF32.subarray((m>>>2)+32,(m>>>2)+48)),...(v[61]===1?{lighting:HEAPF32.slice((m>>>2)+48,(m>>>2)+240)}:{})};}
  else if(cmd===8){command='release';args={handle:v[0]};}
  else if(cmd===9){command='destroyDevice';}
  else if(cmd===10){command='uploadTextureRGBA';args={width:v[0],height:v[1],bytes:HEAPU8.slice(data,data+bytes).buffer};}
  else if(cmd===11){command='createRenderTarget';args={width:v[0],height:v[1]};}
  else if(cmd===12){command='setRenderTarget';args={texture:v[0]||null};}
  else if(cmd===13){command='readRenderTarget';args={texture:v[0]};}
  else if(cmd===16){command='readPixels';}
  else if(cmd===15){command='copyRenderTarget';args={source:v[0],destination:v[1],...(v[4]?{region:{sourceX:v[5],sourceY:v[6],x:v[7],y:v[8],width:v[9],height:v[10]}}:{})};}
  else if(cmd===14){command='writeRenderTarget';args={texture:v[0],width:v[1],height:v[2],bytes:HEAPU8.slice(data,data+bytes).buffer};}else return 0;
  result=call(command,args);
  if(result&&typeof result.then==='function')throw Error('D3D9 graphics hook must be synchronous');
  if(cmd===1){if(!result||result.backend!=='WebGPU')return 0;const values=[result.width,result.height,result.depthFormat==='depth16unorm'?1:0,result.maxBufferBytes,result.presentationIntervalMask,result.maxTextureDimension,(result.renderTargetFormats?.includes(22)?1:0)|(result.renderTargetFormats?.includes(25)?2:0)];if(values.some(x=>!Number.isInteger(x)||x<0||x>4294967295))return 0;HEAPU32.set(values,out>>>2);}
  else if(cmd===12){if(!result||result.width!==v[1]||result.height!==v[2])return 0;}
  else if(cmd===13||cmd===16){if(!result||result.width!==v[1]||result.height!==v[2]||!(result.rgba instanceof Uint8Array)||result.rgba.length!==bytes)return 0;const s=result.rgba;for(let i=0;i<bytes;i+=4){HEAPU8[data+i]=s[i+2];HEAPU8[data+i+1]=s[i+1];HEAPU8[data+i+2]=s[i];HEAPU8[data+i+3]=s[i+3];}}
  else if(cmd===5||cmd===6||cmd===10||cmd===11){if(!Number.isInteger(result)||result<1||result>2147483647)return 0;HEAPU32[out>>>2]=result;}
  else if(cmd===2&&result!==true)return 0;
  return 1;
 }catch(error){Module.moviesGraphicsLastError=String(error?.stack||error);return 0;}
});
#endif
#ifdef __EMSCRIPTEN__
EM_JS(void,graphics_failure_log,(uint32_t cmd),{const e=Module.moviesGraphicsLastError;Module.moviesGraphicsLastError=undefined;err('[D3D9] graphics bridge command '+cmd+' failed: '+(e?String(e).split('\n').slice(0,3).join(' | '):'rejected result'));});
#endif
static int invoke(MoviesD3D9Request*q,MoviesD3D9Reply*r){memset(r,0,sizeof*r);if(host)return host(q,r,host_context);
#ifdef __EMSCRIPTEN__
 int ok=graphics_bridge(q->command,q->args,q->data,q->bytes,q->matrices,r->values);
 static unsigned failures;if(!ok&&failures++<16)graphics_failure_log(q->command);
 return ok;
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
static int invoke(MoviesD3D9Request*q,MoviesD3D9Reply*r);
static void release_upload_cache(Object*b){for(unsigned i=0;i<UPLOAD_CACHE;i++)if(b->cache[i].gpu){MoviesD3D9Request q={.command=MOVIES_D3D9_RELEASE_RESOURCE,.args={b->cache[i].gpu}};MoviesD3D9Reply r;(void)invoke(&q,&r);b->cache[i].gpu=0;}b->gpu=0;}
static void destroy(Object*o){
 uint32_t parent=o->parent;
 if(o->type==DEVICE){bind_target(o,NULL);uint32_t children[]={o->vb,o->ib,o->rt,o->ds,o->texture,o->stage_textures[0],o->stage_textures[1],o->stage_textures[2],o->stage_textures[3],o->stage_textures[4],o->stage_textures[5],o->stage_textures[6]};for(unsigned i=0;i<12;i++){Object*k=find(children[i]);if(k&&k->internal){k->internal--;if(!k->refs&&!k->internal)destroy(k);}}MoviesD3D9Request q={.command=MOVIES_D3D9_DESTROY};MoviesD3D9Reply r;(void)invoke(&q,&r);live_device=0;}
 if(o->type==TEXTURE&&o->surface){Object*s=find(o->surface);if(s&&s->internal){s->internal--;if(!s->refs&&!s->internal)destroy(s);}}
 if(o->type==VERTEX||o->type==INDEX)release_upload_cache(o);
 else if(o->gpu){MoviesD3D9Request q={.command=MOVIES_D3D9_RELEASE_RESOURCE,.args={o->gpu}};MoviesD3D9Reply r;(void)invoke(&q,&r);}
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
static int texture_format(uint32_t format);
static void check_format(CPU*c){MoviesD3D9Reply r;if(!adapter(c)){diagnostic_ret(c,7,INVALID,2,"check_format");return;}uint32_t usage=ARG(4),type=ARG(5),format=ARG(6);int supported=(type==3&&usage==0&&texture_format(format))||(type==1&&usage==1&&(format==22||(format==25&&caps(&r)&&(r.values[6]&2))))||(type==1&&usage==2&&format==80&&caps(&r)&&r.values[2]);diagnostic_ret(c,7,ARG(2)==1&&target_format(ARG(3))&&supported?0:UNAVAILABLE,2,"check_format");}
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
 /* Flags: TARGET(1), ZBUFFER(2), STENCIL(4); there is no stencil buffer, so the
  * stencil part is a no-op. ZBUFFER without a depth surface is INVALIDCALL. Each
  * rectangle (or the whole viewport when Count is 0) is intersected with the
  * viewport and, when enabled, the scissor rect. */
 uint32_t flags=ARG(3),count=ARG(1),rects=ARG(2);
 if(!flags||(flags&~7u)||((flags&2u)&&!d->depth)||(count&&(!rects||count>4096||!range(rects,(uint64_t)count*16)))||!isfinite(z)||z<0||z>1){ret(c,7,INVALID);return;}
 if(!(flags&3u)){d->diag_clear_ok++;ret(c,7,0);return;} /* stencil only */
 uint32_t tw,th;target_size(d,&tw,&th);int ok=1;
 for(uint32_t i=0;i<(count?count:1u)&&ok;i++){
  int64_t l=d->viewport[0],t=d->viewport[1],r_=l+(int64_t)d->viewport[2],b=t+(int64_t)d->viewport[3];
  if(count){int32_t rl=(int32_t)R32(rects+i*16),rt=(int32_t)R32(rects+i*16+4),rr=(int32_t)R32(rects+i*16+8),rb=(int32_t)R32(rects+i*16+12);if(rl>l)l=rl;if(rt>t)t=rt;if(rr<r_)r_=rr;if(rb<b)b=rb;}
  if(d->rs[174]){l=l>(int32_t)d->scissor[0]?l:(int32_t)d->scissor[0];t=t>(int32_t)d->scissor[1]?t:(int32_t)d->scissor[1];r_=r_<(int32_t)d->scissor[2]?r_:(int32_t)d->scissor[2];b=b<(int32_t)d->scissor[3]?b:(int32_t)d->scissor[3];}
  if(r_>tw)r_=tw;if(b>th)b=th;if(l<0)l=0;if(t<0)t=0;
  if(l>=r_||t>=b)continue; /*empty area: nothing to clear*/
  MoviesD3D9Request q={.command=MOVIES_D3D9_CLEAR,.args={ARG(4),bits,flags&3u}};
  if(l||t||r_!=tw||b!=th){q.args[26]=1;q.args[27]=(uint32_t)l;q.args[28]=(uint32_t)t;q.args[29]=(uint32_t)r_;q.args[30]=(uint32_t)b;}
  MoviesD3D9Reply r;ok=invoke(&q,&r);}
 if(ok)d->diag_clear_ok++;ret(c,7,ok?0:LOST);
}
static void present(CPU*c){Object*d=self(c,DEVICE);if(d->scene||ARG(1)||ARG(2)||ARG(3)||ARG(4)){ret(c,5,INVALID);return;}MoviesD3D9Request q={.command=MOVIES_D3D9_PRESENT};MoviesD3D9Reply r;int ok=invoke(&q,&r);if(ok)movies_wait_profile_present();present_diagnostic(c,d,ok);ret(c,5,ok?0:LOST);}
static int transform_index(uint32_t value){return value==256?0:value==2?16:value==3?32:-1;}
/* D3DTS_TEXTURE0..7 (16..23): per-stage texture-coordinate matrices; unset = identity. */
static const float*texture_matrix(Object*d,unsigned stage){static const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};return d->texmat_set&(1u<<stage)?d->texmat[stage]:identity;}
static void set_transform(CPU*c){Object*d=self(c,DEVICE);
 if(ARG(1)>=16&&ARG(1)<=23){if(!range(ARG(2),64)){ret(c,3,INVALID);return;}float values[16];memcpy(values,HP(ARG(2)),64);for(unsigned i=0;i<16;i++)if(!isfinite(values[i])){ret(c,3,INVALID);return;}memcpy(d->texmat[ARG(1)-16],values,64);d->texmat_set|=1u<<(ARG(1)-16);ret(c,3,0);return;}
 int index=transform_index(ARG(1));if(index<0){unsupported(c,"D3D9 SetTransform unsupported state");return;}if(!range(ARG(2),64)){ret(c,3,INVALID);return;}float values[16];memcpy(values,HP(ARG(2)),64);for(unsigned i=0;i<16;i++)if(!isfinite(values[i])){ret(c,3,INVALID);return;}memcpy(d->matrices+index,values,64);ret(c,3,0);}
static void get_transform(CPU*c){Object*d=self(c,DEVICE);
 if(ARG(1)>=16&&ARG(1)<=23){if(!range(ARG(2),64)){ret(c,3,INVALID);return;}memcpy(HP(ARG(2)),texture_matrix(d,ARG(1)-16),64);ret(c,3,0);return;}
 int index=transform_index(ARG(1));if(index<0||!range(ARG(2),64)){ret(c,3,INVALID);return;}memcpy(HP(ARG(2)),d->matrices+index,64);ret(c,3,0);}
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
 /* Every documented render state is stored; enumerations are range-checked like
  * the runtime (D3DERR_INVALIDCALL), and draw-time code maps what WebGPU supports
  * and logs (once) what it does not (fog, stencil, fill/shade modes). Only states
  * outside the D3DRENDERSTATETYPE set stop the run. */
 if(!known_state(state)){static unsigned reports;if(reports++<32)fprintf(stderr,"[D3D9] SetRenderState caller=%08x state=%u value=%08x\n",R32(c->esp),state,value);unsupported(c,"D3D9 SetRenderState unknown state");return;}
 int ok=1;switch(state){
  case 7:ok=value<=2&&(!value||d->depth);break;
  case 8:case 9:ok=value>=1&&value<=3;break;
  case 22:ok=value>=1&&value<=3;break;
  case 19:case 207:ok=value>=1&&value<=15;break;
  case 20:case 208:ok=value>=1&&value<=15&&value!=12&&value!=13;break;
  case 171:case 209:ok=value>=1&&value<=5;break;
  case 23:case 25:case 56:case 189:ok=value>=1&&value<=8;break;
  case 24:ok=value<=255;break;
  case 145:case 146:case 147:case 148:ok=value<=2;break;}
 if(!ok){ret(c,3,INVALID);return;}d->rs[state]=value;ret(c,3,0);}
static void get_state(CPU*c){Object*d=self(c,DEVICE);if(!known_state(ARG(1))){ret(c,3,INVALID);return;}if(!range(ARG(2),4)){ret(c,3,INVALID);return;}W32(ARG(2),d->rs[ARG(1)]);ret(c,3,0);}
/* Flexible vertex formats: position (XYZ or XYZRHW), optional normal, point size,
 * diffuse, specular and 0..8 texture coordinate sets of 1..4 floats. Blend-weight
 * positions (XYZB*), XYZW and LASTBETA flags are not supported.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dfvf */
static int stage1_texture(Object*d);static void texture_rgba(Object*t,uint8_t*rgba);
typedef struct Fvf{uint32_t stride,rhw,normal,diffuse,specular,tex_count,tex_off[8],tex_size[8];}Fvf; /* offsets are +1, 0 = absent */
static int decode_fvf(uint32_t fvf,Fvf*f){
 memset(f,0,sizeof*f);uint32_t pos=fvf&0x400eu,off;
 if(fvf&~(0x0eu|0x10u|0x20u|0x40u|0x80u|0xf00u|0xffff0000u))return 0;
 if(pos==2)off=12;else if(pos==4){off=16;f->rhw=1;}else return 0;
 if(fvf&0x10u){f->normal=off+1;off+=12;}
 if(fvf&0x20u)off+=4;
 if(fvf&0x40u){f->diffuse=off+1;off+=4;}
 if(fvf&0x80u){f->specular=off+1;off+=4;}
 uint32_t n=(fvf>>8)&15u;if(n>8)return 0;
 for(uint32_t i=0;i<n;i++){uint32_t code=(fvf>>(16+2*i))&3u;f->tex_size[i]=code==0?2:code==1?3:code==2?4:1;f->tex_off[i]=off;off+=4*f->tex_size[i];}
 f->tex_count=n;f->stride=off;return 1;}
static int valid_fvf(uint32_t fvf){Fvf f;return decode_fvf(fvf,&f);}
static void set_fvf(CPU*c){Object*d=self(c,DEVICE);Fvf decoded;if(!decode_fvf(ARG(1),&decoded)){fprintf(stderr,"[D3D9] SetFVF rejected fvf=%08x caller=%08x\n",ARG(1),R32(c->esp));unsupported(c,"D3D9 SetFVF blend-weight/XYZW vertex formats unsupported");return;}d->fvf=ARG(1);ret(c,2,0);}
static void get_fvf(CPU*c){Object*d=self(c,DEVICE);if(!range(ARG(1),4)){ret(c,2,INVALID);return;}W32(ARG(1),d->fvf);ret(c,2,0);}
static void create_buffer(CPU*c,uint32_t type){Object*d=self(c,DEVICE);uint32_t out=ARG(5),size=ARG(1),usage=ARG(2),fmt=ARG(3),pool=ARG(4);MoviesD3D9Reply caps_;
 static uint32_t seen[16][3];static unsigned seen_count;uint32_t key[3]={type,usage,pool};int fresh=1;for(unsigned i=0;i<seen_count;i++)if(!memcmp(seen[i],key,sizeof key))fresh=0;
 if(fresh&&seen_count<16){memcpy(seen[seen_count++],key,sizeof key);fprintf(stderr,"[D3D9] CreateBuffer requested caller=%08x type=%u usage=%08x pool=%u size=%u format=%08x\n",R32(c->esp),type,usage,pool,size,fmt);}

 if(!range(out,4)){ret(c,7,INVALID);return;}W32(out,0);
 if(!size||ARG(6)||(usage&~0x208u)||pool>1||(pool==1&&(usage&0x200))||(type==INDEX&&fmt!=101&&fmt!=102)||(type==VERTEX&&fmt!=0&&!valid_fvf(fmt))){ret(c,7,INVALID);return;}
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
static void stream_source(CPU*c){Object*d=self(c,DEVICE);Object*b=find(ARG(2));if(ARG(1)||(!b&&ARG(2))||(b&&(b->type!=VERTEX||b->parent!=d->address||ARG(3)>b->size||(ARG(4)<4||ARG(4)>512||(ARG(4)&3))))){ret(c,5,INVALID);return;}bind(d,&d->vb,b);d->stream_offset=ARG(3);d->stride=ARG(4);ret(c,5,0);}
static void set_indices(CPU*c){Object*d=self(c,DEVICE);Object*b=find(ARG(1));if((ARG(1)&&!b)||(b&&(b->type!=INDEX||b->parent!=d->address))){ret(c,2,INVALID);return;}bind(d,&d->ib,b);ret(c,2,0);}
/* Immutable upload snapshots cover only the draw's referenced span. Guest
 * indices stay untouched; a normalized copy permits firstIndex/baseVertex0.
 * This is a contiguous span, not a sparse gather: unused holes inside it still
 * pass through backend float validation. No NaN-hole support is claimed. */
/* Fixed-function vertex lighting for XYZ|NORMAL|TEX1 (no vertex colours, so
 * every material source resolves to the material). Computed on the CPU in world
 * space, which equals D3D9's camera-space result for an orthonormal view and no
 * specular term, then stored as the vertex diffuse of an XYZ|DIFFUSE|TEX1 snapshot.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/mathematics-of-lighting */
static uint32_t light_hash(Object*d){
 uint32_t h=2166136261u;
#define MIX(p,n) for(size_t k_=0;k_<(n);k_++){h^=((const uint8_t*)(p))[k_];h*=16777619u;}
 MIX(d->matrices,64);MIX(d->material,68);const unsigned states[]={29,137,139,141,143,145,147,148};for(unsigned i=0;i<8;i++)MIX(&d->rs[states[i]],4);
 for(unsigned i=0;i<16;i++)if(d->lights[i].used&&d->lights[i].enabled){MIX(&d->lights[i].index,4);MIX(d->lights[i].words,104);}
#undef MIX
 return h|1u;}
static float clamp01(float v){return v<0?0:v>1?1:v;}
static float finite_or_zero(float v){return isfinite(v)?v:0.f;}
static void unpack_color(uint32_t c,float o[4]){o[0]=((c>>16)&255)/255.f;o[1]=((c>>8)&255)/255.f;o[2]=(c&255)/255.f;o[3]=(c>>24)/255.f;}
static uint32_t pack_color(const float c[4]){return ((uint32_t)(clamp01(c[3])*255+.5f)<<24)|((uint32_t)(clamp01(c[0])*255+.5f)<<16)|((uint32_t)(clamp01(c[1])*255+.5f)<<8)|(uint32_t)(clamp01(c[2])*255+.5f);}
/* Fixed-function vertex lighting, evaluated in world space (equal to D3D9's
 * camera-space result for an orthonormal view; specular term not modelled).
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/mathematics-of-lighting */
/* Per-draw light setup: every enabled light is decoded once (normalized directions,
 * colours, cone cosines); directional ambient terms fold into the global ambient. */
typedef struct PreLight{uint32_t type;float dir[3],pos[3],diffuse[3],ambient[3],range,att[3],ct,cp,falloff;}PreLight;
typedef struct LightCtx{const float*m;float inv[9],global[3];PreLight lights[16];unsigned n;int normalize;}LightCtx;
static void light_setup(Object*d,LightCtx*L){
 const float*m=d->matrices;L->m=m;float a=m[0],b=m[1],cc=m[2],e=m[4],f=m[5],g=m[6],hh=m[8],ii=m[9],j=m[10];
 float det=a*(f*j-g*ii)-b*(e*j-g*hh)+cc*(e*ii-f*hh);if(fabsf(det)<1e-30f)det=1e-30f;float r=1/det;
 /* Row vectors: n' = n * transpose(inverse(W3)); inv holds that product's columns. */
 L->inv[0]=(f*j-g*ii)*r;L->inv[1]=-(e*j-g*hh)*r;L->inv[2]=(e*ii-f*hh)*r;L->inv[3]=-(b*j-cc*ii)*r;L->inv[4]=(a*j-cc*hh)*r;L->inv[5]=-(a*ii-b*hh)*r;L->inv[6]=(b*g-cc*f)*r;L->inv[7]=-(a*g-cc*e)*r;L->inv[8]=(a*f-b*e)*r;
 uint32_t amb=d->rs[139];L->global[0]=((amb>>16)&255)/255.f;L->global[1]=((amb>>8)&255)/255.f;L->global[2]=(amb&255)/255.f;
 L->n=0;
 for(unsigned i=0;i<16;i++){if(!d->lights[i].used||!d->lights[i].enabled)continue;float w[26];memcpy(w,d->lights[i].words,104);PreLight*pl=&L->lights[L->n];
  pl->type=d->lights[i].words[0];float l=sqrtf(w[16]*w[16]+w[17]*w[17]+w[18]*w[18]);
  for(unsigned k=0;k<3;k++){pl->diffuse[k]=w[1+k];pl->ambient[k]=w[9+k];pl->pos[k]=w[13+k];pl->dir[k]=l>0?w[16+k]/l:0;}
  pl->range=w[19];pl->falloff=w[20];pl->att[0]=w[21];pl->att[1]=w[22];pl->att[2]=w[23];pl->ct=cosf(w[24]*.5f);pl->cp=cosf(w[25]*.5f);
  if(pl->type==3){if(!(l>0))continue;for(unsigned k=0;k<3;k++){pl->dir[k]=-pl->dir[k];L->global[k]+=pl->ambient[k];}}
  L->n++;}
 L->normalize=d->rs[143]!=0;}
static uint32_t light_vertex(const LightCtx*L,const float p[3],const float nn[3],const float md[4],const float ma[4],const float me[4]){
 const float*m=L->m;float wp[3],wn[3];
 for(unsigned k=0;k<3;k++){wp[k]=p[0]*m[k]+p[1]*m[4+k]+p[2]*m[8+k]+m[12+k];wn[k]=nn[0]*L->inv[k*3]+nn[1]*L->inv[k*3+1]+nn[2]*L->inv[k*3+2];}
 if(L->normalize){float l=sqrtf(wn[0]*wn[0]+wn[1]*wn[1]+wn[2]*wn[2]);if(l>0){l=1/l;for(unsigned k=0;k<3;k++)wn[k]*=l;}}
 float ambient[3]={L->global[0],L->global[1],L->global[2]},diffuse[3]={0,0,0};
 for(unsigned li=0;li<L->n;li++){const PreLight*pl=&L->lights[li];
  if(pl->type==3){float ndl=wn[0]*pl->dir[0]+wn[1]*pl->dir[1]+wn[2]*pl->dir[2];if(ndl>0)for(unsigned k=0;k<3;k++)diffuse[k]+=pl->diffuse[k]*ndl;continue;}
  float D[3],dist=0;for(unsigned k=0;k<3;k++){D[k]=pl->pos[k]-wp[k];dist+=D[k]*D[k];}dist=sqrtf(dist);if(dist>pl->range)continue;if(dist>0){float inv=1/dist;for(unsigned k=0;k<3;k++)D[k]*=inv;}
  float den=pl->att[0]+pl->att[1]*dist+pl->att[2]*dist*dist,atten=den>0?1/den:1,spot=1;
  if(pl->type==2){float rho=-(D[0]*pl->dir[0]+D[1]*pl->dir[1]+D[2]*pl->dir[2]);spot=rho>pl->ct?1:rho<=pl->cp?0:pl->ct>pl->cp?powf((rho-pl->cp)/(pl->ct-pl->cp),pl->falloff):1;}
  float ndl=wn[0]*D[0]+wn[1]*D[1]+wn[2]*D[2];if(ndl<0)ndl=0;float s=atten*spot;
  for(unsigned k=0;k<3;k++){ambient[k]+=pl->ambient[k]*s;diffuse[k]+=pl->diffuse[k]*ndl*s;}}
 float rgba[4];for(unsigned k=0;k<3;k++)rgba[k]=me[k]+ma[k]*ambient[k]+md[k]*diffuse[k];rgba[3]=md[3];
 return pack_color(rgba);}
/* D3DRS_*MATERIALSOURCE: MATERIAL(0), COLOR1(1, vertex diffuse), COLOR2(2, vertex
 * specular); a missing vertex colour falls back to the material. */
static const float*material_source(Object*d,uint32_t state,const float*material,const float*c1,const float*c2){
 if(!d->rs[141])return material;uint32_t v=d->rs[state];return v==1&&c1?c1:v==2&&c2?c2:material;}
/* Convert any supported FVF to the GPU's XYZ|DIFFUSE|TEX1 (24B) or XYZRHW|DIFFUSE|TEX1
 * (28B) layout: lit diffuse (or vertex diffuse/white), and the stage-0 coordinate set. */
/* Stage-0 texture coordinates: D3DTSS_TEXCOORDINDEX selects a set or generates
 * camera-space normal/position/reflection/sphere-map input (high word 1..4);
 * D3DTSS_TEXTURETRANSFORMFLAGS COUNTn applies D3DTS_TEXTURE0 (row vector) and
 * PROJECTED divides by the last counted component (per vertex, not per pixel).
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/texture-coordinate-processing */
static int texcoord_processing(Object*d){return (d->tss[0][11]>>16)!=0||(d->tss[0][24]&0x1ffu)!=0||(d->tss[1][11]>>16)!=0||(d->tss[1][24]&0x1ffu)!=0;}
static void texture_coords(Object*d,unsigned stage,const Fvf*f,const uint8_t*s,const float pos[3],uint32_t texset,float uv[2]){
 uint32_t gen=d->tss[stage][11]>>16,flags=d->tss[stage][24];float in[4]={0,0,1,0};
 if(gen&&!f->rhw){const float*w=d->matrices,*v=d->matrices+16;float wp[3],cp[3],n[3]={0,0,0},wn[3],cn[3];
  for(unsigned k=0;k<3;k++)wp[k]=pos[0]*w[k]+pos[1]*w[4+k]+pos[2]*w[8+k]+w[12+k];
  for(unsigned k=0;k<3;k++)cp[k]=wp[0]*v[k]+wp[1]*v[4+k]+wp[2]*v[8+k]+v[12+k];
  if(f->normal){memcpy(n,s+f->normal-1,12);for(unsigned k=0;k<3;k++)n[k]=finite_or_zero(n[k]);}
  for(unsigned k=0;k<3;k++)wn[k]=n[0]*w[k]+n[1]*w[4+k]+n[2]*w[8+k];
  for(unsigned k=0;k<3;k++)cn[k]=wn[0]*v[k]+wn[1]*v[4+k]+wn[2]*v[8+k];
  float l=sqrtf(cn[0]*cn[0]+cn[1]*cn[1]+cn[2]*cn[2]);if(l>0)for(unsigned k=0;k<3;k++)cn[k]/=l;
  if(gen==1){in[0]=cn[0];in[1]=cn[1];in[2]=cn[2];in[3]=1;}
  else if(gen==2){in[0]=cp[0];in[1]=cp[1];in[2]=cp[2];in[3]=1;}
  else{float e[3]={cp[0],cp[1],cp[2]};float el=sqrtf(e[0]*e[0]+e[1]*e[1]+e[2]*e[2]);if(el>0)for(unsigned k=0;k<3;k++)e[k]/=el;
   float dn=e[0]*cn[0]+e[1]*cn[1]+e[2]*cn[2],r[3];for(unsigned k=0;k<3;k++)r[k]=e[k]-2*dn*cn[k];
   if(gen==3){in[0]=r[0];in[1]=r[1];in[2]=r[2];in[3]=1;}
   else{float m=2*sqrtf(r[0]*r[0]+r[1]*r[1]+(r[2]+1)*(r[2]+1));if(m>0){in[0]=r[0]/m+.5f;in[1]=r[1]/m+.5f;}in[2]=1;}}}
 else if(texset<f->tex_count){uint32_t size=f->tex_size[texset];float t[4]={0,0,0,0};memcpy(t,s+f->tex_off[texset],4*size);for(unsigned k=0;k<size;k++)in[k]=finite_or_zero(t[k]);if(size<3)in[2]=1;if(size<4)in[3]=size==3?1:0;}
 uint32_t n=flags&0xffu;float out[4]={in[0],in[1],in[2],in[3]};
 if(n){const float*m=texture_matrix(d,stage);for(unsigned k=0;k<4;k++)out[k]=in[0]*m[k]+in[1]*m[4+k]+in[2]*m[8+k]+in[3]*m[12+k];
  if((flags&256u)&&n>=2&&n<=4){float q=out[n-1];if(fabsf(q)>1e-12f){out[0]/=q;out[1]/=q;}}}
 uv[0]=finite_or_zero(out[0]);uv[1]=finite_or_zero(out[1]);}
static uint32_t stage_set(Object*d,unsigned stage,const Fvf*f){uint32_t set=d->tss[stage][11]&0xffffu;return set<f->tex_count?set:0;}
/* Per-draw work is hoisted out of the vertex loop: material sources depend only on
 * the FVF and render states; plain texture coordinates (no generation, no transform)
 * are a copy; and with only directional lights the emissive + ambient term is one
 * constant and positions need no world transform. Results equal light_vertex /
 * texture_coords (the general paths, still used for everything else). */
typedef struct UvPlan{int simple;uint32_t set;}UvPlan;
static UvPlan uv_plan(Object*d,unsigned stage,const Fvf*f){UvPlan u={0,stage_set(d,stage,f)};u.simple=!(d->tss[stage][11]>>16)&&!(d->tss[stage][24]&0xffu);return u;}
static void convert_vertices(Object*d,const Fvf*f,const uint8_t*src,uint32_t count,uint8_t*dst,uint32_t texset){
 int lit=!f->rhw&&d->rs[137];LightCtx L;if(lit)light_setup(d,&L);
 const uint32_t out=f->rhw?36:32,posn=f->rhw?4u:3u;(void)texset;
 float c1[4]={0},c2[4]={0};const float*c1p=f->diffuse?c1:NULL,*c2p=f->specular?c2:NULL;
 const float*md=NULL,*ma=NULL,*me=NULL;int directional=0;float base[3]={0,0,0};
 if(lit){md=material_source(d,145,d->material,c1p,c2p);ma=material_source(d,147,d->material+4,c1p,c2p);me=material_source(d,148,d->material+12,c1p,c2p);
  directional=1;for(unsigned i=0;i<L.n;i++)if(L.lights[i].type!=3)directional=0;
  /* Constant emissive + ambient when neither comes from the vertex colours. */
  if(directional&&ma!=c1&&ma!=c2&&me!=c1&&me!=c2)for(unsigned k=0;k<3;k++)base[k]=me[k]+ma[k]*L.global[k];else directional=0;}
 UvPlan plan[2]={uv_plan(d,0,f),uv_plan(d,1,f)};
 for(uint32_t v=0;v<count;v++){const uint8_t*s=src+(size_t)v*f->stride;uint8_t*o=dst+(size_t)v*out;float pos[4];memcpy(pos,s,posn*4);
  for(unsigned k=0;k<posn;k++)pos[k]=finite_or_zero(pos[k]);
  uint32_t color=0xffffffffu,raw;
  if(f->diffuse){memcpy(&raw,s+f->diffuse-1,4);color=raw;if(lit)unpack_color(raw,c1);}
  if(f->specular&&lit){memcpy(&raw,s+f->specular-1,4);unpack_color(raw,c2);}
  if(lit){float nn[3]={0,0,0};if(f->normal){memcpy(nn,s+f->normal-1,12);for(unsigned k=0;k<3;k++)nn[k]=finite_or_zero(nn[k]);}
   if(directional){float wn[3];for(unsigned k=0;k<3;k++)wn[k]=nn[0]*L.inv[k*3]+nn[1]*L.inv[k*3+1]+nn[2]*L.inv[k*3+2];
    if(L.normalize){float l=sqrtf(wn[0]*wn[0]+wn[1]*wn[1]+wn[2]*wn[2]);if(l>0){l=1/l;for(unsigned k=0;k<3;k++)wn[k]*=l;}}
    float diffuse[3]={0,0,0};for(unsigned li=0;li<L.n;li++){const PreLight*pl=&L.lights[li];float ndl=wn[0]*pl->dir[0]+wn[1]*pl->dir[1]+wn[2]*pl->dir[2];if(ndl>0)for(unsigned k=0;k<3;k++)diffuse[k]+=pl->diffuse[k]*ndl;}
    float rgba[4];for(unsigned k=0;k<3;k++)rgba[k]=base[k]+md[k]*diffuse[k];rgba[3]=md[3];color=pack_color(rgba);}
   else color=light_vertex(&L,pos,nn,md,ma,me);}
  float uv[4]={0,0,0,0};
  for(unsigned st=0;st<2;st++){
   if(plan[st].simple){if(plan[st].set<f->tex_count){float tc[2]={0,0};uint32_t size=f->tex_size[plan[st].set];memcpy(tc,s+f->tex_off[plan[st].set],4*(size<2?size:2));uv[st*2]=finite_or_zero(tc[0]);uv[st*2+1]=size>1?finite_or_zero(tc[1]):0;}}
   else texture_coords(d,st,f,s,pos,plan[st].set,uv+st*2);}
  memcpy(o,pos,posn*4);memcpy(o+posn*4,&color,4);memcpy(o+posn*4+4,uv,16);}}
/* GPU lighting: lit draws upload position, normal, vertex diffuse and the stage
 * coordinates unchanged (XYZ|NORMAL|DIFFUSE|TEX2, 44 bytes) and send the lights and
 * material per draw; the vertex shader evaluates light_vertex's formula. The upload
 * no longer depends on lights or matrices, so unchanged meshes stay cached across
 * frames (unless texture-coordinate processing makes them depend on matrices).
 * Specular material sources and more than 8 lights keep the CPU path (convert_vertices). */
#define GPU_LIGHTS 8u
static int cpu_lighting_forced=-1;
void movies_d3d9_force_cpu_lighting(int on){cpu_lighting_forced=on!=0;}
static int gpu_lighting(Object*d,const Fvf*f){
 if(cpu_lighting_forced<0){const char*e=getenv("MOVIES_CPU_LIGHTING");cpu_lighting_forced=e&&*e=='1';}
 if(cpu_lighting_forced)return 0;
 if(f->rhw||!d->rs[137])return 0; /* texture-coordinate processing stays on the CPU (convert_lit_vertices) */
 unsigned n=0;for(unsigned i=0;i<16;i++)if(d->lights[i].used&&d->lights[i].enabled)n++;if(n>GPU_LIGHTS)return 0;
 if(d->rs[141]&&f->specular){const unsigned st[]={145,147,148};for(unsigned i=0;i<3;i++)if(d->rs[st[i]]==2)return 0;}
 return 1;}
static void gpu_lighting_block(Object*d,const Fvf*f,float*o){
 LightCtx L;light_setup(d,&L);memset(o,0,192*sizeof*o);
 for(unsigned k=0;k<3;k++){o[k*4]=L.inv[k*3];o[k*4+1]=L.inv[k*3+1];o[k*4+2]=L.inv[k*3+2];}
 o[3]=L.normalize?1.f:0.f;o[11]=(float)L.n;memcpy(o+12,L.global,12);
 memcpy(o+16,d->material,16);memcpy(o+20,d->material+4,16);memcpy(o+24,d->material+12,16);
 const unsigned st[]={145,147,148};for(unsigned i=0;i<3;i++)o[28+i]=(d->rs[141]&&d->rs[st[i]]==1&&f->diffuse)?1.f:0.f;
 for(unsigned i=0;i<L.n&&i<GPU_LIGHTS;i++){const PreLight*pl=&L.lights[i];float*w=o+32+i*20;
  memcpy(w,pl->diffuse,12);w[3]=(float)pl->type;memcpy(w+4,pl->ambient,12);w[7]=isfinite(pl->range)?pl->range:3.4e38f;
  memcpy(w+8,pl->pos,12);w[11]=pl->falloff;memcpy(w+12,pl->dir,12);w[15]=pl->ct;memcpy(w+16,pl->att,12);w[19]=pl->cp;}
 for(unsigned i=0;i<192;i++)o[i]=finite_or_zero(o[i]);}
static void convert_lit_vertices(Object*d,const Fvf*f,const uint8_t*src,uint32_t count,uint8_t*dst){
 UvPlan plan[2]={uv_plan(d,0,f),uv_plan(d,1,f)};
 for(uint32_t v=0;v<count;v++){const uint8_t*s=src+(size_t)v*f->stride;uint8_t*o=dst+(size_t)v*44;float pn[6]={0,0,0,0,0,0};memcpy(pn,s,12);
  if(f->normal)memcpy(pn+3,s+f->normal-1,12);for(unsigned k=0;k<6;k++)pn[k]=finite_or_zero(pn[k]);
  uint32_t color=0xffffffffu;if(f->diffuse)memcpy(&color,s+f->diffuse-1,4);
  float uv[4]={0,0,0,0};
  for(unsigned st=0;st<2;st++){
   if(plan[st].simple){if(plan[st].set<f->tex_count){float tc[2]={0,0};uint32_t size=f->tex_size[plan[st].set];memcpy(tc,s+f->tex_off[plan[st].set],4*(size<2?size:2));uv[st*2]=finite_or_zero(tc[0]);uv[st*2+1]=size>1?finite_or_zero(tc[1]):0;}}
   else texture_coords(d,st,f,s,pn,plan[st].set,uv+st*2);}
  memcpy(o,pn,24);memcpy(o+24,&color,4);memcpy(o+28,uv,16);}}
/* Upload statistics, logged at a few milestones: which path vertex data takes
 * (cache hit, passthrough, GPU-lit packing, CPU conversion) and the busiest
 * CPU-converted formats with the reason they could not pass through. */
typedef struct UploadStat{uint32_t fvf,why;uint64_t vertices,calls;}UploadStat;
static UploadStat upload_stats[16];static uint64_t upload_path[4],upload_vertices_path[4],upload_total;
static void upload_note(unsigned path,uint32_t fvf,uint32_t why,uint32_t vertices){
 upload_path[path]++;upload_vertices_path[path]+=vertices;
 if(path==3){unsigned i=0;while(i<16&&upload_stats[i].calls&&(upload_stats[i].fvf!=fvf||upload_stats[i].why!=why))i++;if(i<16){upload_stats[i].fvf=fvf;upload_stats[i].why=why;upload_stats[i].calls++;upload_stats[i].vertices+=vertices;}}
 if(++upload_total==20000||upload_total==200000||upload_total==1000000){
  fprintf(stderr,"[D3D9 uploads] %llu: hit=%llu pass=%llu gpulit=%llu cpu=%llu calls; vertices pass=%llu gpulit=%llu cpu=%llu\n",(unsigned long long)upload_total,
   (unsigned long long)upload_path[0],(unsigned long long)upload_path[1],(unsigned long long)upload_path[2],(unsigned long long)upload_path[3],(unsigned long long)upload_vertices_path[1],(unsigned long long)upload_vertices_path[2],(unsigned long long)upload_vertices_path[3]);
  for(unsigned i=0;i<16&&upload_stats[i].calls;i++)fprintf(stderr,"[D3D9 uploads]   cpu fvf=%03x why=%x calls=%llu vertices=%llu (why: 1 lit, 2 texset, 4 texproc, 8 stage1, 16 format)\n",upload_stats[i].fvf,upload_stats[i].why,(unsigned long long)upload_stats[i].calls,(unsigned long long)upload_stats[i].vertices);}}
static int upload(Object*b,uint32_t offset,uint32_t bytes,uint32_t fvf,uint32_t mask,uint32_t bias,Object*d){
 Fvf f={0};uint32_t key=0,texset=0,upload_why=0,raw_set0=0,raw_set1=0;int passthrough=0,gpu_lit=0,raw=0;
 if(b->type==VERTEX){
  if(!decode_fvf(fvf,&f)||!d)return 0;
  gpu_lit=gpu_lighting(d,&f);
  int lit=!f.rhw&&d->rs[137]&&!gpu_lit;texset=d->tss[0][11]&0xffffu;if(texset>=f.tex_count)texset=0;
  int texproc=texcoord_processing(d);
  int stage1=stage1_texture(d);
  passthrough=(fvf==0x42||fvf==0x142||fvf==0x144||fvf==0x104)&&!lit&&!gpu_lit&&!texset&&!mask&&!texproc&&!stage1;
  if(gpu_lit)lit=0;
  /* Raw: no CPU work is needed (no CPU lighting, no texture-coordinate processing,
   * both stage coordinate sets have two components), so the game's own bytes are
   * uploaded and the GPU reads each field where the FVF stores it. */
  if(!passthrough&&!lit&&!texproc&&!mask&&f.tex_count>0&&(!gpu_lit||f.normal)){
   raw_set0=stage_set(d,0,&f);raw_set1=stage_set(d,1,&f);raw=f.tex_size[raw_set0]>=2&&f.tex_size[raw_set1]>=2;}
  key=passthrough?1u:raw?(((raw_set0+1)*0x9e3779b1u)^((raw_set1+7)*0x27d4eb2fu)^0x2545f491u)|1u:((lit||texproc?light_hash(d):gpu_lit?0x6c1b0d4bu:2u)^((texset+1)*0x9e3779b1u)^((d->tss[1][11]&0xffffu)+7)*0x27d4eb2fu)|1u;
  upload_why=(lit?1u:0u)|(texset?2u:0u)|(texproc?4u:0u)|(stage1?8u:0u)|((fvf==0x42||fvf==0x142||fvf==0x144||fvf==0x104)?0u:16u);
  if(texproc){uint32_t h=key;const uint8_t*p=(const uint8_t*)texture_matrix(d,0);for(unsigned i=0;i<64;i++){h^=p[i];h*=16777619u;}
   p=(const uint8_t*)texture_matrix(d,1);for(unsigned i=0;i<64;i++){h^=p[i];h*=16777619u;}h^=d->tss[1][11]*0x165667b1u^d->tss[1][24]*0x9e3779b9u;
   p=(const uint8_t*)(d->matrices+16);for(unsigned i=0;i<64;i++){h^=p[i];h*=16777619u;}h^=d->tss[0][11]*0x85ebca6bu^d->tss[0][24]*0xc2b2ae35u;key=h|1u;}}
 for(unsigned i=0;i<UPLOAD_CACHE;i++){UploadEntry*e=&b->cache[i];if(e->gpu&&e->version==b->version&&e->offset==offset&&e->bytes==bytes&&e->fvf==fvf&&e->mask==mask&&e->bias==bias&&e->key==key){e->age=++b->cache_clock;b->gpu=e->gpu;if(b->type==VERTEX)upload_note(0,fvf,0,0);return 1;}}
 if(!bytes||offset>b->size||bytes>b->size-offset)return 0;
 MoviesD3D9Request q={.command=b->type==VERTEX?MOVIES_D3D9_UPLOAD_VERTEX:MOVIES_D3D9_UPLOAD_INDEX,.args={b->type==VERTEX?fvf:b->format},.data=HP(b->mem+offset),.bytes=bytes};
 uint32_t stride=b->type==VERTEX?f.stride:b->format==101?2:4;if(!stride||bytes%stride)return 0;
 uint8_t*converted=NULL;
 if(b->type==VERTEX&&raw){
  q.args[0]=0xffffu;q.args[1]=f.stride;q.args[2]=f.rhw?4u:3u;q.args[3]=f.normal;q.args[4]=f.diffuse; /* +1-encoded, 0 = absent */
  q.args[5]=f.tex_off[raw_set0]+1u;q.args[6]=f.tex_off[raw_set1]+1u;upload_note(1,fvf,0,bytes/stride);
 }else if(b->type==VERTEX&&!passthrough){
  uint32_t count=bytes/stride,out=gpu_lit?44:f.rhw?36:32;upload_note(gpu_lit?2:3,fvf,upload_why,count);uint64_t expanded=(uint64_t)count*out;if(expanded>32u*1024u*1024u)return 0;
  converted=malloc((size_t)expanded);if(!converted)return 0;
  if(gpu_lit)convert_lit_vertices(d,&f,(const uint8_t*)q.data,count,converted);
  else convert_vertices(d,&f,(const uint8_t*)q.data,count,converted,texset);
  if(mask)for(uint32_t i=0;i<count;i++){uint32_t color;memcpy(&color,converted+i*out+(gpu_lit?24:f.rhw?16:12),4);color|=mask;memcpy(converted+i*out+(gpu_lit?24:f.rhw?16:12),&color,4);}
  q.data=converted;q.bytes=(uint32_t)expanded;q.args[0]=gpu_lit?0x252:f.rhw?0x244:0x242;
 }else if(b->type==VERTEX){upload_note(1,fvf,0,stride?bytes/stride:0);
 }else if(b->type==INDEX&&bias){
  converted=malloc(bytes);if(!converted)return 0;memcpy(converted,q.data,bytes);
  for(uint32_t i=0;i<bytes;i+=stride){
   if(stride==2){uint16_t v;memcpy(&v,converted+i,2);if(v<bias){free(converted);return 0;}v=(uint16_t)(v-bias);memcpy(converted+i,&v,2);}
   else{uint32_t v;memcpy(&v,converted+i,4);if(v<bias){free(converted);return 0;}v-=bias;memcpy(converted+i,&v,4);}}
  q.data=converted;
 }
 MoviesD3D9Reply r;int ok=invoke(&q,&r)&&r.values[0];free(converted);if(!ok)return 0;
 /* Slot: empty, else stale (older buffer version), else least recently used. */
 unsigned slot=UPLOAD_CACHE;
 for(unsigned i=0;i<UPLOAD_CACHE&&slot==UPLOAD_CACHE;i++)if(!b->cache[i].gpu)slot=i;
 for(unsigned i=0;i<UPLOAD_CACHE&&slot==UPLOAD_CACHE;i++)if(b->cache[i].version!=b->version)slot=i;
 if(slot==UPLOAD_CACHE){slot=0;for(unsigned i=1;i<UPLOAD_CACHE;i++)if(b->cache[i].age<b->cache[slot].age)slot=i;}
 uint32_t old=b->cache[slot].gpu;
 b->cache[slot]=(UploadEntry){r.values[0],b->version,offset,bytes,fvf,mask,bias,key,++b->cache_clock};b->gpu=r.values[0];
 if(old){MoviesD3D9Request release_={.command=MOVIES_D3D9_RELEASE_RESOURCE,.args={old}};MoviesD3D9Reply ignored;if(!invoke(&release_,&ignored))return 0;}return 1;
}
/* Only the audited mode6 alpha blend equation. State setters allow the
 * original sequential updates; unsupported active combinations fail at draw. */
/* BOTHSRCALPHA/BOTHINVSRCALPHA (12/13) set both factors of the pair. */
static void blend_pair(uint32_t src,uint32_t dst,uint32_t*out_src,uint32_t*out_dst){if(src==12){src=5;dst=6;}else if(src==13){src=6;dst=5;}*out_src=src;*out_dst=dst;}
static uint32_t blend_encoding(Object*d){if(!d->rs[27])return 0;uint32_t src,dst;blend_pair(d->rs[19],d->rs[20],&src,&dst);
 if(d->rs[171]==1&&!d->rs[206]){if(src==5&&dst==6)return 1;if(src==3&&dst==4)return 2;if(src==1&&dst==3)return 3;}return 256|(src<<4)|dst;}
/* args[56]: blend op | alpha op<<4 | separate<<8 | alpha src<<12 | alpha dst<<16. */
static uint32_t blend_extended(Object*d){if(!d->rs[27])return 0;uint32_t v=d->rs[171]&15;
 if(d->rs[206]){uint32_t as,ad;blend_pair(d->rs[207],d->rs[208],&as,&ad);v|=((d->rs[209]&15)<<4)|(1u<<8)|(as<<12)|(ad<<16);}return v;}
static int draw_blend_supported(CPU*c,Object*d){
 static unsigned logged;unsigned why=0;
 if(d->rs[28])why|=1;if(d->rs[52])why|=2;if(d->rs[152])why|=16;if(d->rs[8]!=3)why|=4;if(d->rs[9]==1)why|=8;
 if(why&~logged){logged|=why;fprintf(stderr,"[D3D9] render state approximated flags=%x caller=%08x (1 fog not drawn, 2 stencil ignored, 4 non-solid fill drawn solid, 8 flat shading drawn smooth, 16 user clip planes ignored)\n",why,R32(c->esp));}
 return 1;}

/* General fixed-function texture-stage cascade (stages 0 and 1) is evaluated in
 * the WGSL shader from the raw D3DTSS_COLOROP/ARG1/ARG2/ALPHAOP/ARG1/ARG2 values.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/texture-blending */
static int stage_arg_texture(uint32_t a){return (a&0x0fu)==2;}
static int op_uses_texture(uint32_t op,uint32_t a1,uint32_t a2){
 if(op<=1)return 0;if(op==13||op==15)return 1;if(op==2)return stage_arg_texture(a1);if(op==3)return stage_arg_texture(a2);
 return stage_arg_texture(a1)||stage_arg_texture(a2);}
static int stage1_texture(Object*d){return d->tss[0][1]!=1&&d->tss[1][1]!=1&&(op_uses_texture(d->tss[1][1],d->tss[1][2],d->tss[1][3])||op_uses_texture(d->tss[1][4],d->tss[1][5],d->tss[1][6]));}
static int stage0_texture(Object*d){return d->tss[0][1]!=1&&(op_uses_texture(d->tss[0][1],d->tss[0][2],d->tss[0][3])||op_uses_texture(d->tss[0][4],d->tss[0][5],d->tss[0][6]));}
static void fill_stages(CPU*c,Object*d,MoviesD3D9Request*q){
 q->args[31]=4;
 for(unsigned s=0;s<2;s++)for(unsigned k=0;k<6;k++)q->args[33+s*6+k]=d->tss[s][1+k];
 if(d->tss[0][1]==1)q->args[39]=1; /* a disabled stage ends the cascade */
 static unsigned logged;unsigned why=0;
 if(d->tss[0][1]!=1&&d->tss[1][1]!=1&&d->tss[2][1]!=1)why|=1;
 for(unsigned s=0;s<2;s++){if(d->tss[s][28]==5)why|=2;for(unsigned k=1;k<=4;k+=3)if(d->tss[s][k]>=22)why|=4;}
 if(d->tss[0][11]&0xffffu)why|=8;if(d->tss[0][24])why|=16;
 if(why&~logged){logged|=why;fprintf(stderr,"[D3D9] texture-stage approximation flags=%x caller=%08x (1 stage>=2 enabled, 2 RESULTARG TEMP, 4 bump/dot3/arg0 op, 8 texcoord index, 16 texture transform) stage0=%u,%u,%u/%u,%u,%u stage1=%u,%u,%u/%u,%u,%u\n",why,R32(c->esp),d->tss[0][1],d->tss[0][2],d->tss[0][3],d->tss[0][4],d->tss[0][5],d->tss[0][6],d->tss[1][1],d->tss[1][2],d->tss[1][3],d->tss[1][4],d->tss[1][5],d->tss[1][6]);}
}
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
/* Shared draw plumbing. Culling (D3DCULL_CW/CCW) is drawn as CULL_NONE: depth
 * testing hides back faces of closed meshes. Point and line primitives are not
 * rasterized by the WebGPU triangle path and are skipped (logged once per type). */
static int draw_texture(CPU*c,Object*d,Object**out){
 Object*t=find(d->texture);if(t&&!stage0_texture(d))t=NULL;
 if(t&&d->target&&find(d->target)&&find(d->target)->container==t->address){unsupported(c,"D3D9 draw samples the active render target");return 0;}
 *out=t;return 1;}
static int issue_draw(CPU*c,Object*d,uint32_t vg,uint32_t ig,uint32_t count,Object*t){
 Object*t1=stage1_texture(d)?find(d->stage_textures[0]):NULL;if(t1&&(t1->locked||(d->target&&find(d->target)&&find(d->target)->container==t1->address)))t1=NULL;
 MoviesD3D9Request q={.command=MOVIES_D3D9_DRAW,.args={vg,ig,0,count,0,d->rs[7]}};q.args[12]=t?t->gpu:0;q.args[20]=d->rs[15];q.args[21]=d->rs[25];q.args[22]=d->rs[24];q.args[23]=d->rs[23];q.args[24]=d->rs[14];q.args[25]=blend_encoding(d);q.args[26]=d->rs[174];fill_stages(c,d,&q);q.args[32]=d->rs[60];q.args[56]=blend_extended(d);q.args[57]=d->rs[193];q.args[58]=d->rs[195];q.args[59]=d->rs[175];q.args[60]=(d->rs[168]&15)|0x100u;memcpy(q.args+27,d->scissor,16);const unsigned samplers[]={1,2,3,6,5,7,8};for(unsigned i=0;i<7;i++)q.args[13+i]=d->sampler[0][samplers[i]];if(t1){q.args[45]=t1->gpu;for(unsigned i=0;i<7;i++)q.args[48+i]=d->sampler[1][samplers[i]];}memcpy(q.args+6,d->viewport,24);memcpy(q.matrices,d->matrices,sizeof q.matrices);
 {Fvf f;if(decode_fvf(d->fvf,&f)&&gpu_lighting(d,&f)){q.args[61]=1;gpu_lighting_block(d,&f,q.lighting);}}
 MoviesD3D9Reply r;return invoke(&q,&r);}
static int draw_preamble(CPU*c,Object*d,uint32_t type,unsigned argc,const char*method){
 if(d->rs[22]<1||d->rs[22]>3){unsupported(c,"D3D9 draw needs unsupported culling mode");return 0;}
 if(type>=1&&type<=3){static unsigned logged;if(!(logged&(1u<<type))){logged|=1u<<type;fprintf(stderr,"[D3D9] %s point/line primitive type %u skipped caller=%08x\n",method,type,R32(c->esp));}draw_ret(c,d,argc,0,method);return 0;}
 if(type<4||type>6){draw_ret(c,d,argc,INVALID,method);return 0;}
 Fvf f;if(decode_fvf(d->fvf,&f)&&!f.rhw&&d->rs[137]&&d->rs[29]&&!d->specular_logged){d->specular_logged=1;fprintf(stderr,"[D3D9] specular lighting requested caller=%08x; drawn without the specular term\n",R32(c->esp));}
 return 1;}
static uint32_t source_indices(uint32_t type,uint32_t primitives){return type==4?primitives*3:primitives+2;}
/* Triangle strips/fans become lists (winding preserved; culling is off anyway). */
static uint32_t*triangulate(uint32_t type,const uint32_t*src,uint32_t primitives){
 uint32_t*out=malloc((size_t)primitives*12);if(!out)return NULL;
 for(uint32_t i=0;i<primitives;i++){uint32_t*o=out+i*3;
  if(type==4){o[0]=src[i*3];o[1]=src[i*3+1];o[2]=src[i*3+2];}
  else if(type==5){if(i&1){o[0]=src[i+1];o[1]=src[i];}else{o[0]=src[i];o[1]=src[i+1];}o[2]=src[i+2];}
  else{o[0]=src[0];o[1]=src[i+1];o[2]=src[i+2];}}
 return out;}
/* Draw a host-built triangle list against vertex storage b (a bound buffer or a
 * temporary UP wrapper): upload the referenced vertex span, a temporary index
 * buffer rebased to it, draw, release the temporary indices. */
static int draw_generated(CPU*c,Object*d,Object*b,uint32_t stream_offset,uint32_t stride,int64_t base,const uint32_t*tri,uint32_t count,unsigned argc,const char*method){
 if(stream_offset>b->size)return INVALID;uint64_t available=(b->size-stream_offset)/stride;int64_t mn=INT64_MAX,mx=-1;
 for(uint32_t i=0;i<count;i++){int64_t a=(int64_t)tri[i]+base;if(a<0||(uint64_t)a>=available)return INVALID;if(a<mn)mn=a;if(a>mx)mx=a;}
 Object*t;if(!draw_texture(c,d,&t))return -1;if(t&&t->locked)return INVALID;
 uint64_t offset=stream_offset+(uint64_t)mn*stride,bytes=(uint64_t)(mx-mn+1)*stride;
 if(offset>UINT32_MAX||bytes>UINT32_MAX||!upload(b,(uint32_t)offset,(uint32_t)bytes,d->fvf,0,0,d))return LOST;
 uint32_t*rebased=malloc((size_t)count*4);if(!rebased)return OOM;
 for(uint32_t i=0;i<count;i++)rebased[i]=(uint32_t)((int64_t)tri[i]+base-mn);
 MoviesD3D9Request q={.command=MOVIES_D3D9_UPLOAD_INDEX,.args={102},.data=rebased,.bytes=count*4};MoviesD3D9Reply r;
 int ok=invoke(&q,&r)&&r.values[0];free(rebased);if(!ok)return LOST;uint32_t ig=r.values[0];
 ok=issue_draw(c,d,b->gpu,ig,count,t);
 q=(MoviesD3D9Request){.command=MOVIES_D3D9_RELEASE_RESOURCE,.args={ig}};if(!invoke(&q,&r))ok=0;
 (void)argc;(void)method;return ok?0:LOST;}
static void draw_indexed(CPU*c){Object*d=self(c,DEVICE);d->diag_draws++;if(!draw_blend_supported(c,d))return;Object*vb=find(d->vb),*ib=find(d->ib);uint32_t type=ARG(1),first=ARG(5),primitives=ARG(6),minimum=ARG(3),vertices=ARG(4);int32_t base=(int32_t)ARG(2);
 Fvf f;int fvf_ok=decode_fvf(d->fvf,&f);
 const char*reason=!d->scene?"outside scene":(type<1||type>6)?"unsupported topology":!vb||!ib?"missing bound buffer":vb->locked||ib->locked?"locked bound buffer":!fvf_ok?"unsupported bound FVF":d->stride!=f.stride?"FVF stride mismatch":(!primitives||primitives>0x3fffffffu||!vertices)?"empty/overflow draw count":NULL;
 if(reason){indexed_reject(c,d,vb,ib,reason,UINT32_MAX,0);return;}
 if(!draw_preamble(c,d,type,7,"DrawIndexedPrimitive"))return;
 uint32_t count=source_indices(type,primitives),index_stride=ib->format==101?2:4;
 if((uint64_t)first+count>ib->size/index_stride||(uint64_t)minimum+vertices>0x100000000ull){indexed_reject(c,d,vb,ib,"index/declared vertex range",UINT32_MAX,0);return;}
 uint32_t min_index=UINT32_MAX,max_index=0;
 for(uint32_t i=0;i<count;i++){uint32_t p=ib->mem+(first+i)*index_stride,idx=index_stride==2?R16(p):R32(p);int64_t actual=(int64_t)idx+base;
 if(idx<minimum||(uint64_t)idx>=(uint64_t)minimum+vertices||actual<0||(uint64_t)actual>=(vb->size-d->stream_offset)/d->stride){indexed_reject(c,d,vb,ib,idx<minimum||(uint64_t)idx>=(uint64_t)minimum+vertices?"index outside declared vertex range":"index/base outside vertex buffer",i,idx);return;}if(idx<min_index)min_index=idx;if(idx>max_index)max_index=idx;}
 if(type!=4){
  uint32_t*src=malloc((size_t)count*4);if(!src){draw_ret(c,d,7,OOM,"DrawIndexedPrimitive");return;}
  for(uint32_t i=0;i<count;i++){uint32_t p=ib->mem+(first+i)*index_stride;src[i]=index_stride==2?R16(p):R32(p);}
  uint32_t*tri=triangulate(type,src,primitives);free(src);if(!tri){draw_ret(c,d,7,OOM,"DrawIndexedPrimitive");return;}
  int hr=draw_generated(c,d,vb,d->stream_offset,d->stride,base,tri,primitives*3,7,"DrawIndexedPrimitive");free(tri);if(hr>=0||hr<-1)draw_ret(c,d,7,(uint32_t)hr,"DrawIndexedPrimitive");return;}
 Object*t;if(!draw_texture(c,d,&t))return;
 if(t&&t->locked){indexed_reject(c,d,vb,ib,"locked texture",UINT32_MAX,0);return;}
 uint64_t vertex_offset=(uint64_t)d->stream_offset+(uint64_t)((int64_t)min_index+base)*d->stride,vertex_bytes=((uint64_t)max_index-min_index+1)*d->stride;
 if(vertex_offset>UINT32_MAX||vertex_bytes>UINT32_MAX||!upload(vb,(uint32_t)vertex_offset,(uint32_t)vertex_bytes,d->fvf,0,0,d)||!upload(ib,first*index_stride,count*index_stride,0,0,min_index,NULL)){draw_ret(c,d,7,LOST,"DrawIndexedPrimitive");return;}
 draw_ret(c,d,7,issue_draw(c,d,vb->gpu,ib->gpu,count,t)?0:LOST,"DrawIndexedPrimitive");
}
/* DrawPrimitive(type,startVertex,primitiveCount) on the bound stream 0. */
static void draw_primitive(CPU*c){Object*d=self(c,DEVICE);d->diag_draws++;if(!draw_blend_supported(c,d))return;Object*vb=find(d->vb);uint32_t type=ARG(1),start=ARG(2),primitives=ARG(3);
 Fvf f;if(!d->scene||!vb||vb->locked||!decode_fvf(d->fvf,&f)||d->stride!=f.stride||!primitives||primitives>0x3fffffffu){draw_ret(c,d,4,INVALID,"DrawPrimitive");return;}
 if(!draw_preamble(c,d,type,4,"DrawPrimitive"))return;
 uint32_t n=source_indices(type,primitives);uint32_t*src=malloc((size_t)n*4);if(!src){draw_ret(c,d,4,OOM,"DrawPrimitive");return;}
 for(uint32_t i=0;i<n;i++)src[i]=start+i;uint32_t*tri=triangulate(type,src,primitives);free(src);if(!tri){draw_ret(c,d,4,OOM,"DrawPrimitive");return;}
 int hr=draw_generated(c,d,vb,d->stream_offset,d->stride,0,tri,primitives*3,4,"DrawPrimitive");free(tri);if(hr!=-1)draw_ret(c,d,4,(uint32_t)hr,"DrawPrimitive");}
/* DrawPrimitiveUP(type,primitiveCount,data,stride): caller memory; invalidates stream 0. */
static void draw_primitive_up(CPU*c){Object*d=self(c,DEVICE);d->diag_draws++;if(!draw_blend_supported(c,d))return;uint32_t type=ARG(1),primitives=ARG(2),data=ARG(3),stride=ARG(4);
 bind(d,&d->vb,NULL);d->stride=d->stream_offset=0;Fvf f;
 if(!d->scene||!decode_fvf(d->fvf,&f)||stride!=f.stride||!primitives||primitives>0x3fffffffu||!data){draw_ret(c,d,5,INVALID,"DrawPrimitiveUP");return;}
 if(!draw_preamble(c,d,type,5,"DrawPrimitiveUP"))return;
 uint32_t n=source_indices(type,primitives);uint64_t bytes=(uint64_t)n*stride;if(bytes>32u*1024u*1024u||!range(data,bytes)){draw_ret(c,d,5,INVALID,"DrawPrimitiveUP");return;}
 uint32_t*src=malloc((size_t)n*4);if(!src){draw_ret(c,d,5,OOM,"DrawPrimitiveUP");return;}for(uint32_t i=0;i<n;i++)src[i]=i;
 uint32_t*tri=triangulate(type,src,primitives);free(src);if(!tri){draw_ret(c,d,5,OOM,"DrawPrimitiveUP");return;}
 Object tmp={0};tmp.type=VERTEX;tmp.mem=data;tmp.size=(uint32_t)bytes;tmp.version=1;
 int hr=draw_generated(c,d,&tmp,0,stride,0,tri,primitives*3,5,"DrawPrimitiveUP");free(tri);
 release_upload_cache(&tmp);
 if(hr!=-1)draw_ret(c,d,5,(uint32_t)hr,"DrawPrimitiveUP");}
/* DrawIndexedPrimitiveUP for formats/topologies beyond the audited 144/28B list path. */
static void draw_indexed_up_general(CPU*c,Object*d){uint32_t type=ARG(1),minimum=ARG(2),vertices=ARG(3),primitives=ARG(4),indices=ARG(5),format=ARG(6),data=ARG(7),stride=ARG(8);
 bind(d,&d->vb,NULL);bind(d,&d->ib,NULL);d->stride=d->stream_offset=0;Fvf f;
 if(!d->scene||!decode_fvf(d->fvf,&f)||stride!=f.stride||!primitives||primitives>0x3fffffffu||!vertices||!data||(format!=101&&format!=102)){draw_ret(c,d,9,INVALID,"DrawIndexedPrimitiveUP");return;}
 if(!draw_preamble(c,d,type,9,"DrawIndexedPrimitiveUP"))return;
 uint32_t n=source_indices(type,primitives),is=format==101?2:4;uint64_t vbytes=((uint64_t)minimum+vertices)*stride;
 if(vbytes>32u*1024u*1024u||!range(data,vbytes)||!range(indices,(uint64_t)n*is)){draw_ret(c,d,9,INVALID,"DrawIndexedPrimitiveUP");return;}
 uint32_t*src=malloc((size_t)n*4);if(!src){draw_ret(c,d,9,OOM,"DrawIndexedPrimitiveUP");return;}
 for(uint32_t i=0;i<n;i++){src[i]=is==2?R16(indices+i*2):R32(indices+i*4);if(src[i]<minimum||src[i]>=minimum+vertices){free(src);draw_ret(c,d,9,INVALID,"DrawIndexedPrimitiveUP");return;}}
 uint32_t*tri=triangulate(type,src,primitives);free(src);if(!tri){draw_ret(c,d,9,OOM,"DrawIndexedPrimitiveUP");return;}
 Object tmp={0};tmp.type=VERTEX;tmp.mem=data;tmp.size=(uint32_t)vbytes;tmp.version=1;
 int hr=draw_generated(c,d,&tmp,0,stride,0,tri,primitives*3,9,"DrawIndexedPrimitiveUP");free(tri);
 release_upload_cache(&tmp);
 if(hr!=-1)draw_ret(c,d,9,(uint32_t)hr,"DrawIndexedPrimitiveUP");}
/* Observed9EB1A3: XYZRHW|DIFFUSE|TEX1, stride28, INDEX16 triangles.
 * Caller buffers are copied synchronously. UP invalidates stream0 and indices.
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-drawindexedprimitiveup */
static void draw_indexed_up(CPU*c){
 Object*d=self(c,DEVICE);d->diag_draws++;if(!draw_blend_supported(c,d))return;
 if(!(d->fvf==0x144&&ARG(8)==28&&ARG(1)==4&&ARG(6)==101)||stage1_texture(d)||texcoord_processing(d)){draw_indexed_up_general(c,d);return;}
 uint32_t minimum=ARG(2),vertices=ARG(3),primitives=ARG(4),indices=ARG(5),data=ARG(7);
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
 Object*t=find(d->texture);if(t&&!stage0_texture(d))t=NULL;
 if(t&&d->target&&find(d->target)&&find(d->target)->container==t->address){unsupported(c,"D3D9 draw samples the active render target");return;}if(t&&t->locked){draw_ret(c,d,9,INVALID,"DrawIndexedPrimitiveUP");return;}
 uint32_t mask=0;
 uint8_t*vb=malloc((size_t)vertex_bytes);uint16_t*ib=malloc((size_t)index_bytes);if(!vb||!ib){free(vb);free(ib);draw_ret(c,d,9,OOM,"DrawIndexedPrimitiveUP");return;}
 memcpy(vb,HP((uint32_t)vertex_start),(size_t)vertex_bytes);
 if(mask)for(uint32_t i=0;i<vertices;i++){uint32_t color;memcpy(&color,vb+i*28+16,4);color|=mask;memcpy(vb+i*28+16,&color,4);}
 for(uint32_t i=0;i<count;i++){uint16_t v;memcpy(&v,HP(indices+i*2),2);ib[i]=(uint16_t)(v-minimum);}
 MoviesD3D9Request q={.command=MOVIES_D3D9_UPLOAD_VERTEX,.args={0x144},.data=vb,.bytes=(uint32_t)vertex_bytes};MoviesD3D9Reply r;uint32_t vg=0,ig=0;int ok=invoke(&q,&r)&&r.values[0];if(ok)vg=r.values[0];
 if(ok){q=(MoviesD3D9Request){.command=MOVIES_D3D9_UPLOAD_INDEX,.args={101},.data=ib,.bytes=(uint32_t)index_bytes};ok=invoke(&q,&r)&&r.values[0];if(ok)ig=r.values[0];}free(vb);free(ib);
 if(ok){q=(MoviesD3D9Request){.command=MOVIES_D3D9_DRAW,.args={vg,ig,0,count,0,d->rs[7]}};q.args[12]=t?t->gpu:0;q.args[20]=d->rs[15];q.args[21]=d->rs[25];q.args[22]=d->rs[24];q.args[23]=d->rs[23];q.args[24]=d->rs[14];q.args[25]=blend_encoding(d);q.args[26]=d->rs[174];fill_stages(c,d,&q);q.args[32]=d->rs[60];memcpy(q.args+27,d->scissor,16);const unsigned samplers[]={1,2,3,6,5,7,8};for(unsigned i=0;i<7;i++)q.args[13+i]=d->sampler[0][samplers[i]];memcpy(q.args+6,d->viewport,24);memcpy(q.matrices,d->matrices,sizeof q.matrices);ok=invoke(&q,&r);}
 uint32_t handles[]={vg,ig};for(unsigned i=0;i<2;i++)if(handles[i]){q=(MoviesD3D9Request){.command=MOVIES_D3D9_RELEASE_RESOURCE,.args={handles[i]}};if(!invoke(&q,&r))ok=0;}
 draw_ret(c,d,9,ok?0:LOST,"DrawIndexedPrimitiveUP");
}
static void get_stream_source(CPU*c){Object*d=self(c,DEVICE);uint32_t out=ARG(2),offset=ARG(3),stride=ARG(4);if(ARG(1)||!range(out,4)||!range(offset,4)||!range(stride,4)){ret(c,5,INVALID);return;}Object*b=find(d->vb);if(b)add(b);W32(out,d->vb);W32(offset,d->stream_offset);W32(stride,d->stride);ret(c,5,0);}
static void get_indices(CPU*c){Object*d=self(c,DEVICE);uint32_t out=ARG(1);if(!range(out,4)){ret(c,2,INVALID);return;}Object*b=find(d->ib);if(b)add(b);W32(out,d->ib);ret(c,2,0);}
/* Original video path:99B26D CreateTexture(levels1,usage0,A8R8G8B8,poolMANAGED),
 * 99A5FB LockRect(level0,NULL,NOSYSLOCK),99A62C UnlockRect(level0).
 * Guest BGRA byte order is preserved; only GPU upload changes it to RGBA. */
#define DXT1 0x31545844u
#define DXT2 0x32545844u
#define DXT3 0x33545844u
#define DXT4 0x34545844u
#define DXT5 0x35545844u
/* Sampled texture formats decoded to RGBA8 on upload: A8R8G8B8, X8R8G8B8, R8G8B8,
 * R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, A8R3G3B2, A8, L8, A8L8 and DXT1..5.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dformat */
static int block_format(uint32_t format){return format==DXT1||format==DXT2||format==DXT3||format==DXT4||format==DXT5;}
static uint32_t block_bytes(uint32_t format){return format==DXT1?8:16;}
static uint32_t pixel_bytes(uint32_t format){switch(format){case 21:case 22:return 4;case 20:return 3;case 23:case 24:case 25:case 26:case 29:case 51:return 2;case 28:case 50:return 1;default:return 0;}}
static int texture_format(uint32_t format){return block_format(format)||pixel_bytes(format)!=0;}
static uint32_t texture_pitch(uint32_t width,uint32_t format){return block_format(format)?((width+3)/4)*block_bytes(format):width*pixel_bytes(format);}
/* DXT3/BC2: explicit four-bit alpha, always four RGB565 palette colors,
 * independent of endpoint ordering. Unpremultiplied, no gamma conversion.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/opaque-and-1-bit-alpha-textures
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/textures-with-alpha-channels */
static void texture_rgba(Object*t,uint8_t*rgba){
 const uint8_t*src=HP(t->mem);
 if(block_format(t->format)){
  uint32_t pitch=texture_pitch(t->width,t->format);
  for(uint32_t by=0;by<t->height;by+=4)for(uint32_t bx=0;bx<t->width;bx+=4){
   const uint8_t*b=src+(by/4)*pitch+(bx/4)*block_bytes(t->format);const uint8_t*rgb=b+(t->format==DXT1?0:8);uint8_t color[4][3];uint32_t ends[2];
   uint8_t alpha8[8]={0};if(t->format==DXT4||t->format==DXT5){alpha8[0]=b[0];alpha8[1]=b[1];for(unsigned k=2;k<8;k++)alpha8[k]=b[0]>b[1]?(uint8_t)(((8-k)*b[0]+(k-1)*b[1])/7):k<6?(uint8_t)(((6-k)*b[0]+(k-1)*b[1])/5):k==6?0:255;}
   uint64_t abits=0;for(unsigned k=0;k<6;k++)abits|=(uint64_t)b[2+k]<<(8*k);
   for(unsigned e=0;e<2;e++){uint32_t v=rgb[e*2]|((uint32_t)rgb[1+e*2]<<8);ends[e]=v;uint32_t r=v>>11,g=(v>>5)&63,bl=v&31;color[e][0]=(r<<3)|(r>>2);color[e][1]=(g<<2)|(g>>4);color[e][2]=(bl<<3)|(bl>>2);}
   int transparent=t->format==DXT1&&ends[0]<=ends[1];
   for(unsigned c=0;c<3;c++){color[2][c]=transparent?(color[0][c]+color[1][c])/2:(2*color[0][c]+color[1][c])/3;color[3][c]=transparent?0:(color[0][c]+2*color[1][c])/3;}
   for(unsigned y=0;y<4&&by+y<t->height;y++)for(unsigned x=0;x<4&&bx+x<t->width;x++){unsigned i=y*4+x,index=(rgb[4+y]>>(x*2))&3;uint8_t*out=rgba+((by+y)*t->width+bx+x)*4;memcpy(out,color[index],3);out[3]=t->format==DXT1?(transparent&&index==3?0:255):(t->format==DXT4||t->format==DXT5)?alpha8[(abits>>(3*i))&7]:((b[i/2]>>((i&1)*4))&15)*17;}
  }
 }else for(uint32_t i=0;i<t->width*t->height;i++){
  uint8_t*o=rgba+i*4;uint32_t v=pixel_bytes(t->format)==2?(uint32_t)(src[i*2]|(src[i*2+1]<<8)):0;
  switch(t->format){
   case 26:o[0]=((v>>8)&15)*17;o[1]=((v>>4)&15)*17;o[2]=(v&15)*17;o[3]=(v>>12)*17;break;
   case 23:o[0]=(uint8_t)(((v>>11)&31)*255/31);o[1]=(uint8_t)(((v>>5)&63)*255/63);o[2]=(uint8_t)((v&31)*255/31);o[3]=255;break;
   case 24:case 25:o[0]=(uint8_t)(((v>>10)&31)*255/31);o[1]=(uint8_t)(((v>>5)&31)*255/31);o[2]=(uint8_t)((v&31)*255/31);o[3]=t->format==25&&!(v&0x8000)?0:255;break;
   case 29:o[0]=(uint8_t)(((v>>5)&7)*255/7);o[1]=(uint8_t)(((v>>2)&7)*255/7);o[2]=(uint8_t)((v&3)*255/3);o[3]=(uint8_t)(v>>8);break;
   case 51:o[0]=o[1]=o[2]=(uint8_t)v;o[3]=(uint8_t)(v>>8);break;
   case 28:o[0]=o[1]=o[2]=0;o[3]=src[i];break;
   case 50:o[0]=o[1]=o[2]=src[i];o[3]=255;break;
   case 20:o[0]=src[i*3+2];o[1]=src[i*3+1];o[2]=src[i*3];o[3]=255;break;
   case 22:o[0]=src[i*4+2];o[1]=src[i*4+1];o[2]=src[i*4];o[3]=255;break;
   default:o[0]=src[i*4+2];o[1]=src[i*4+1];o[2]=src[i*4];o[3]=src[i*4+3];break;}
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
 if(ARG(3)!=1||!texture_format(ARG(5))||(!systemmem&&(ARG(4)||ARG(6)!=1))||(systemmem&&(ARG(4)&~0x200u))){texture_create_failure(c,UNAVAILABLE);return;}
 uint64_t decoded=(uint64_t)w*h*4;
 if(block_format(ARG(5))&&((w&3)||(h&3))){texture_create_failure(c,INVALID);return;}
 uint64_t size=block_format(ARG(5))?(uint64_t)(w/4)*(h/4)*block_bytes(ARG(5)):(uint64_t)w*h*pixel_bytes(ARG(5));
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
 uint32_t pitch=texture_pitch(t->width,t->format),offset=block_format(t->format)?(top/4)*pitch+(left/4)*block_bytes(t->format):top*pitch+left*pixel_bytes(t->format);
 t->locked=1;t->lockflags=flags;W32(locked,pitch);W32(locked+4,t->mem+offset);return 1;
}
static uint32_t texture_unlock(Object*t){if(!t->locked)return INVALID;t->locked=0;if(t->lockflags&0x10)return 0;t->version++;return t->pool==2?0:upload_texture(t)?0:LOST;}
static void lock_texture(CPU*c){Object*t=self(c,TEXTURE);ret(c,5,!ARG(1)&&texture_lock(t,ARG(2),ARG(3),ARG(4))?0:INVALID);}
static void unlock_texture(CPU*c){Object*t=self(c,TEXTURE);ret(c,2,ARG(1)?INVALID:texture_unlock(t));}
/* Standalone render-target surfaces (CreateRenderTarget) own a GPU target; the
 * lockable form also owns guest storage. Lock reads the GPU target back; a
 * non-READONLY unlock uploads it. D3DX fallback at B0E5F1 uses Lockable=TRUE. */
static int rt_read(Object*o,uint32_t mem){MoviesD3D9Request q={.command=MOVIES_D3D9_READ_TARGET,.args={o->gpu,o->width,o->height},.data=HP(mem),.bytes=o->width*o->height*4};MoviesD3D9Reply r;return invoke(&q,&r);}
static int rt_write(Object*o,uint32_t gpu){uint32_t bytes=o->width*o->height*4;uint8_t*rgba=malloc(bytes);if(!rgba)return 0;texture_rgba(o,rgba);
 MoviesD3D9Request q={.command=MOVIES_D3D9_WRITE_TARGET,.args={gpu,o->width,o->height},.data=rgba,.bytes=bytes};MoviesD3D9Reply r;int ok=invoke(&q,&r);free(rgba);return ok;}
/* The implicit backbuffer (PresentationFlags=0, so not lockable on Windows). The
 * statically linked D3DX lock helper B0E42C locks it first (caller B0E839) and only
 * then falls back to copies; we serve the lock as a readback of the last rendered
 * image in the backbuffer's own format (22 X8R8G8B8 or 25 A1R5G5B5, already
 * quantized to 16 bits). Guest writes are not uploaded back. */
static void lock_backbuffer(CPU*c,Object*d,Object*s){uint32_t locked=ARG(1),rect=ARG(2),flags=ARG(3);
 if(s->locked||!range(locked,8)||(flags&~0x2810u)||(flags&0x2000u)||(s->format!=22&&s->format!=25)){ret(c,4,INVALID);return;}
 uint32_t left=0,top=0,right=s->width,bottom=s->height;
 if(rect){if(!range(rect,16)){ret(c,4,INVALID);return;}left=R32(rect);top=R32(rect+4);right=R32(rect+8);bottom=R32(rect+12);}
 if(left>=right||top>=bottom||right>s->width||bottom>s->height){ret(c,4,INVALID);return;}
 if(!s->mem){s->size=s->width*s->height*4;s->mem=movies_backend_alloc(s->size);if(!s->mem){allocation_failure(SURFACE,s->size,"backbuffer lock storage");s->size=0;ret(c,4,OOM);return;}}
 MoviesD3D9Request q={.command=MOVIES_D3D9_READ_BACKBUFFER,.args={0,s->width,s->height},.data=HP(s->mem),.bytes=s->width*s->height*4};MoviesD3D9Reply r;
 if(!invoke(&q,&r)){ret(c,4,LOST);return;}
 uint32_t bpp=s->format==25?2:4,pitch=s->width*bpp;
 if(bpp==2){uint8_t*m=HP(s->mem);for(uint32_t i=0,n=s->width*s->height;i<n;i++){const uint8_t*p=m+i*4; /* B,G,R,A */
  uint16_t v=(uint16_t)((p[3]>=128?0x8000u:0)|((p[2]>>3)<<10)|((p[1]>>3)<<5)|(p[0]>>3));m[i*2]=(uint8_t)v;m[i*2+1]=(uint8_t)(v>>8);}}
 static unsigned logs;if(logs++<4)fprintf(stderr,"[D3D9] backbuffer LockRect caller=%08x %ux%u format=%u flags=%x\n",R32(c->esp),s->width,s->height,s->format,flags);
 s->locked=1;s->lockflags=flags;W32(locked,pitch);W32(locked+4,s->mem+top*pitch+left*bpp);ret(c,4,0);(void)d;}
static void lock_rt_surface(CPU*c,Object*s){uint32_t locked=ARG(1),rect=ARG(2),flags=ARG(3);
 if(s->usage&2u){ret(c,4,INVALID);return;} /* D3DFMT_D16 depth is not lockable */
 Object*d=find(s->parent);if(!s->gpu&&d&&d->rt==s->address){lock_backbuffer(c,d,s);return;}
 if(!(s->usage&1u)||!s->gpu){unsupported(c,"D3D9 back/depth surface LockRect unsupported");return;}
 if(!s->mem||s->locked||!range(locked,8)||(flags&~0x2810u)||(flags&0x2000u)){ret(c,4,INVALID);return;}
 uint32_t left=0,top=0,right=s->width,bottom=s->height;
 if(rect){if(!range(rect,16)){ret(c,4,INVALID);return;}left=R32(rect);top=R32(rect+4);right=R32(rect+8);bottom=R32(rect+12);}
 if(left>=right||top>=bottom||right>s->width||bottom>s->height){ret(c,4,INVALID);return;}
 if(!rt_read(s,s->mem)){ret(c,4,LOST);return;}
 s->locked=1;s->lockflags=flags;W32(locked,s->width*4);W32(locked+4,s->mem+top*s->width*4+left*4);ret(c,4,0);}
static void lock_surface(CPU*c){Object*s=self(c,SURFACE);Object*t=find(s->container);if(!t){lock_rt_surface(c,s);return;}ret(c,4,texture_lock(t,ARG(1),ARG(2),ARG(3))?0:INVALID);}
static void unlock_surface(CPU*c){Object*s=self(c,SURFACE);Object*t=find(s->container);
 if(!t&&!s->gpu&&(s->usage&1u)){Object*d=find(s->parent);if(d&&d->rt==s->address){if(!s->locked){ret(c,1,INVALID);return;}s->locked=0;
  static unsigned dropped;if(!(s->lockflags&0x10u)&&dropped++<4)fprintf(stderr,"[D3D9] backbuffer UnlockRect without READONLY caller=%08x: guest writes are not uploaded\n",R32(c->esp));ret(c,1,0);return;}}
 if(!t){if(!(s->usage&1u)||!s->gpu){unsupported(c,"D3D9 back/depth surface UnlockRect unsupported");return;}if(!s->locked){ret(c,1,INVALID);return;}
  s->locked=0;ret(c,1,(s->lockflags&0x10u)||rt_write(s,s->gpu)?0:LOST);return;}ret(c,1,texture_unlock(t));}
static void surface_container(CPU*c){Object*s=self(c,SURFACE);uint32_t out=ARG(2);if(!range(out,4)){ret(c,3,INVALID);return;}W32(out,0);Object*t=find(s->container);if(!t||(!iid(ARG(1),ids[0])&&!iid(ARG(1),ids[TEXTURE])&&!iid(ARG(1),base_texture_id)&&!iid(ARG(1),resource_id))){ret(c,3,0x80004002);return;}add(t);W32(out,t->address);ret(c,3,0);}
/* Stages 0 and 1 are sampled. Textures on stages 2..7 are stored and owned
 * (refcounted) but not sampled; the shader cascade evaluates stages 0..1 only. */
static void set_texture(CPU*c){Object*d=self(c,DEVICE);Object*t=find(ARG(2));uint32_t stage=ARG(1);if(stage>=8||((ARG(2)&&!t)||(t&&(t->type!=TEXTURE||t->parent!=d->address||t->pool==2)))){ret(c,3,INVALID);return;}
 if(stage){if(t&&!(d->stage_logged&1)){d->stage_logged|=1;fprintf(stderr,"[D3D9] texture bound to stage %u caller=%08x; stages 0 and 1 are sampled\n",stage,R32(c->esp));}bind(d,&d->stage_textures[stage-1],t);ret(c,3,0);return;}
 bind(d,&d->texture,t);ret(c,3,0);}
static void get_texture(CPU*c){Object*d=self(c,DEVICE);uint32_t out=ARG(2);if(ARG(1)>=8||!range(out,4)){ret(c,3,INVALID);return;}Object*t=find(ARG(1)?d->stage_textures[ARG(1)-1]:d->texture);if(t)add(t);W32(out,t?t->address:0);ret(c,3,0);}
/* Valid query state domains are independent of our supported drawing subset.
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dtexturestagestatetype
 * https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dsamplerstatetype */
static int stage_state(uint32_t state){return (state>=1&&state<=11)||(state>=22&&state<=24)||(state>=26&&state<=28)||state==32;}
static void state_failure(CPU*c,const char*name){static unsigned reports;if(reports++<32)fprintf(stderr,"[D3D9] %s caller=%08x stage=%u state=%u valueOrOut=%08x\n",name,R32(c->esp),ARG(1),ARG(2),ARG(3));unsupported(c,name);}
static void set_texture_stage(CPU*c){Object*d=self(c,DEVICE);uint32_t stage=ARG(1),state=ARG(2),value=ARG(3);
 if(stage>=8||!stage_state(state)){state_failure(c,"D3D9 SetTextureStageState invalid stage/state");return;}
 if(d->tss[stage][state]==value){ret(c,4,0);return;} /* exact no-op preserves state, including disabled stages */
 /* D3DTEXTUREOP 1..26; D3DTA selectors 0..6 with COMPLEMENT(0x10)/ALPHAREPLICATE(0x20);
  * RESULTARG CURRENT/TEMP; remaining states are stored (bump-env, texcoord index,
  * transform flags, constant) and diagnosed at draw time if they matter. */
 int arg=(value&~0x30u)<=6;
 int ok=(state==1||state==4)?(value>=1&&value<=26):(state==2||state==3||state==5||state==6||state==26||state==27)?arg:state==28?(value==1||value==5):1;
 if(!ok){fprintf(stderr,"[D3D9] SetTextureStageState rejected stage=%u state=%u value=%u caller=%08x\n",stage,state,value,R32(c->esp));state_failure(c,"D3D9 SetTextureStageState unsupported state/value");return;}d->tss[stage][state]=value;ret(c,4,0);}
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
/* No multisampling, no shared handle; A8R8G8B8/X8R8G8B8. Observed D3DX B0E5F1:
 * (w,h,srcFormat,NONE,0,Lockable=TRUE,&surface,NULL).
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-createrendertarget */
static void create_render_target(CPU*c){Object*d=self(c,DEVICE);uint32_t w=ARG(1),h=ARG(2),format=ARG(3),out=ARG(7);MoviesD3D9Reply cap;
 if(!range(out,4)){ret(c,9,INVALID);return;}W32(out,0);
 if(!w||!h||ARG(8)||ARG(6)>1){ret(c,9,INVALID);return;}
 if(ARG(4)||ARG(5)||(format!=21&&format!=22)||!caps(&cap)||w>cap.values[5]||h>cap.values[5]){ret(c,9,UNAVAILABLE);return;}
 Object*o=allocate(SURFACE);if(!o){ret(c,9,OOM);return;}
 o->parent=d->address;o->width=w;o->height=h;o->format=format;o->usage=1;o->pool=0;o->size=w*h*4;
 if(ARG(6)){o->mem=movies_backend_alloc(o->size);if(!o->mem){W32(o->address,0);memset(o,0,sizeof*o);ret(c,9,OOM);return;}}
 MoviesD3D9Request q={.command=MOVIES_D3D9_CREATE_TARGET,.args={w,h}};MoviesD3D9Reply r;
 if(!invoke(&q,&r)||!r.values[0]){destroy(o);ret(c,9,OOM);return;}o->gpu=r.values[0];
 static unsigned logs;if(logs++<8)fprintf(stderr,"[D3D9] CreateRenderTarget caller=%08x %ux%u format=%u lockable=%u\n",R32(c->esp),w,h,format,ARG(6));
 add(o);W32(out,o->address);ret(c,9,0);}
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
static uint32_t surface_gpu(Object*s){if(!s||s->type!=SURFACE)return 0;Object*t=find(s->container);if(t)return (t->usage&1u)?t->gpu:0;return (s->usage&1u)?s->gpu:0;}
/* Unscaled copies between render-target surfaces (1:1, so the filter has no
 * effect): whole same-size surfaces, or equal-size rectangles. Observed D3DX
 * B0E60B (NONE), A2B3B4 (LINEAR) and AFE246 (D3DXLoadSurfaceFromSurface fast path,
 * NONE, only when both rectangles have the same size). Scaling is not claimed.
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-stretchrect */
static void stretch_rect(CPU*c){Object*d=self(c,DEVICE);Object*src=find(ARG(1)),*dst=find(ARG(3));uint32_t filter=ARG(5);
 if(!src||!dst||src->type!=SURFACE||dst->type!=SURFACE||src->parent!=d->address||dst->parent!=d->address||src==dst||filter>3){ret(c,6,INVALID);return;}
 uint32_t sg=surface_gpu(src),dg=surface_gpu(dst),sr[4]={0,0,src->width,src->height},dr[4]={0,0,dst->width,dst->height};
 if(ARG(2)){if(!range(ARG(2),16)){ret(c,6,INVALID);return;}for(int i=0;i<4;i++)sr[i]=R32(ARG(2)+4*i);}
 if(ARG(4)){if(!range(ARG(4),16)){ret(c,6,INVALID);return;}for(int i=0;i<4;i++)dr[i]=R32(ARG(4)+4*i);}
 if(sr[0]>=sr[2]||sr[1]>=sr[3]||sr[2]>src->width||sr[3]>src->height||dr[0]>=dr[2]||dr[1]>=dr[3]||dr[2]>dst->width||dr[3]>dst->height){ret(c,6,INVALID);return;}
 int whole=!ARG(2)&&!ARG(4)&&src->width==dst->width&&src->height==dst->height;
 if(!sg||!dg||(!whole&&(sr[2]-sr[0]!=dr[2]-dr[0]||sr[3]-sr[1]!=dr[3]-dr[1]))){static unsigned reports;if(reports++<8)fprintf(stderr,"[D3D9] StretchRect unsupported caller=%08x src=%ux%u gpu=%u rect=%08x dst=%ux%u gpu=%u rect=%08x filter=%u\n",R32(c->esp),src->width,src->height,sg,ARG(2),dst->width,dst->height,dg,ARG(4),filter);
  unsupported(c,"D3D9 StretchRect supports unscaled render-target copies only");return;}
 if(src->locked||dst->locked){ret(c,6,INVALID);return;}
 MoviesD3D9Request q={.command=MOVIES_D3D9_COPY_TARGET,.args={sg,dg,src->width,src->height,!whole,sr[0],sr[1],dr[0],dr[1],sr[2]-sr[0],sr[3]-sr[1]}};MoviesD3D9Reply r;ret(c,6,invoke(&q,&r)?0:LOST);}
static void unsupported_1_35(CPU*c){unsupported(c,"IDirect3DDevice9::ColorFill unsupported");}
static void unsupported_1_36(CPU*c){unsupported(c,"IDirect3DDevice9::CreateOffscreenPlainSurface unsupported");}
/* One simultaneous target (NumSimultaneousRTs=1). Index0 rejects NULL. Accepts the
 * implicit backbuffer or level0 of this device's RENDERTARGET texture. Setting a
 * target resets viewport and scissor to the full target (as Wine's wined3d does).
 * https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-setrendertarget */
static uint32_t surface_gpu(Object*s);
static void set_render_target(CPU*c){Object*d=self(c,DEVICE);uint32_t index=ARG(1);Object*s=find(ARG(2));
 if(index||!s||s->type!=SURFACE||s->parent!=d->address){ret(c,3,INVALID);return;}
 Object*t=find(s->container);int backbuffer=s->address==d->rt;uint32_t gpu=backbuffer?0:surface_gpu(s);
 if(!backbuffer&&!gpu){ret(c,3,INVALID);return;}
 if(t&&t->address==d->texture){unsupported(c,"D3D9 SetRenderTarget on the bound texture");return;}
 MoviesD3D9Request q={.command=MOVIES_D3D9_SET_TARGET,.args={gpu,s->width,s->height}};MoviesD3D9Reply r;
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
/* User clip planes are stored and queryable; drawing ignores them (logged once
 * when D3DRS_CLIPPLANEENABLE is set at draw time). */
static void set_clip_plane(CPU*c){Object*d=self(c,DEVICE);uint32_t i=ARG(1),p=ARG(2);if(i>=6||!range(p,16)){ret(c,3,INVALID);return;}memcpy(d->clip_planes[i],HP(p),16);ret(c,3,0);}
static void get_clip_plane(CPU*c){Object*d=self(c,DEVICE);uint32_t i=ARG(1),p=ARG(2);if(i>=6||!range(p,16)){ret(c,3,INVALID);return;}memcpy(HP(p),d->clip_planes[i],16);ret(c,3,0);}
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
static GuestFn table_1[]={query,addref,release,cooperative,available_texture_memory,unsupported_1_5,get_direct3d,get_caps,device_mode,creation_parameters,unsupported_1_10,unsupported_1_11,unsupported_1_12,unsupported_1_13,unsupported_1_14,unsupported_1_15,unsupported_1_16,present,get_backbuffer,unsupported_1_19,unsupported_1_20,unsupported_1_21,unsupported_1_22,create_texture,unsupported_1_24,unsupported_1_25,create_vb,create_ib,create_render_target,unsupported_1_29,update_surface,unsupported_1_31,render_target_data,unsupported_1_33,stretch_rect,unsupported_1_35,unsupported_1_36,set_render_target,get_rt,unsupported_1_39,get_ds,begin_scene,end_scene,clear_target,set_transform,get_transform,unsupported_1_46,set_viewport,get_viewport,set_material,get_material,set_light,get_light,light_enable,get_light_enable,set_clip_plane,get_clip_plane,set_state,get_state,unsupported_1_59,unsupported_1_60,unsupported_1_61,unsupported_1_62,unsupported_1_63,get_texture,set_texture,get_texture_stage,set_texture_stage,get_sampler,set_sampler,unsupported_1_70,unsupported_1_71,unsupported_1_72,unsupported_1_73,unsupported_1_74,set_scissor,get_scissor,unsupported_1_77,unsupported_1_78,unsupported_1_79,unsupported_1_80,draw_primitive,draw_indexed,draw_primitive_up,draw_indexed_up,unsupported_1_85,unsupported_1_86,unsupported_1_87,unsupported_1_88,set_fvf,get_fvf,unsupported_1_91,set_vertex_shader,get_vertex_shader,unsupported_1_94,unsupported_1_95,unsupported_1_96,unsupported_1_97,unsupported_1_98,unsupported_1_99,stream_source,get_stream_source,unsupported_1_102,unsupported_1_103,set_indices,get_indices,unsupported_1_106,set_pixel_shader,get_pixel_shader,unsupported_1_109,unsupported_1_110,unsupported_1_111,unsupported_1_112,unsupported_1_113,unsupported_1_114,unsupported_1_115,unsupported_1_116,unsupported_1_117,unsupported_1_118};
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
