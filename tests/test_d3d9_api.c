#include "d3d9-api.h"
#include "heap-api.h"
#include "window-api.h"
#include <assert.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void,install_js_host,(void),{
 Module.fixtureCalls=[];Module.fixtureHandle=1;
 Module.moviesGraphicsCall=(command,args)=>{Module.fixtureCalls.push(command);
 if(command==='capabilities')return {backend:'WebGPU',width:640,height:480,depthFormat:'depth16unorm',maxBufferBytes:4194304,presentationIntervalMask:1,maxTextureDimension:4096,renderTargetFormats:[22,25]};
 if(command==='createDevice')return args.width===640&&args.height===480&&args.hwnd===42;
 if(command==='uploadVertices'||command==='uploadIndices'||command==='uploadTextureRGBA')return Module.fixtureHandle++;
 return null;};
});
EM_JS(int,js_called,(void),{return Module.fixtureCalls.includes('createDevice')&&Module.fixtureCalls.includes('clear')&&Module.fixtureCalls.includes('present');});
#endif
uint8_t*g_mem;static GuestFn table[6][119];static _Thread_local jmp_buf trap;
static int check_finite_vertices;static unsigned last_vertex_bytes,last_index_bytes,last_vertex_fvf;static uint8_t last_vertices[256],last_indices[128];
static uint8_t uploaded[128];static unsigned uploaded_bytes;
static uint32_t last_vertex_args[8];static float last_lighting[192];static uint32_t last_lit;static unsigned calls[17],next_handle=1;static uint32_t last_copy[11];static uint32_t last_target[3],clearargs[64];static int ready=1;static uint32_t drawargs[64];
void movies_set_last_error(uint32_t e){(void)e;}
void register_function(uint32_t a,GuestFn f){assert(a>=0x0e020000&&a<0x0e026000);unsigned t=(a-0x0e020000)/0x1000,slot=((a-0x0e020000)%0x1000)/16;assert(slot<119);table[t][slot]=f;}
void guest_trap(CPU*c,uint32_t p,const char*s){(void)c;(void)p;(void)s;longjmp(trap,1);}
int movies_window_get(uint32_t hwnd,MoviesWindowEvent*out){if(hwnd!=42)return 0;memset(out,0,sizeof*out);out->hwnd=42;out->client_width=640;out->client_height=480;return 1;}
static int backend(const MoviesD3D9Request*q,MoviesD3D9Reply*r,void*ctx){(void)ctx;if(!ready)return 0;assert(q->command>=1&&q->command<=16);calls[q->command]++;
 if(q->command==MOVIES_D3D9_CAPS){r->values[0]=640;r->values[1]=480;r->values[2]=1;r->values[3]=4<<20;r->values[4]=1;r->values[5]=4096;r->values[6]=3;}
 if(q->command==MOVIES_D3D9_CREATE)assert(q->args[0]==640&&q->args[1]==480&&q->args[2]==42&&q->args[3]==1);
 if(q->command==MOVIES_D3D9_UPLOAD_VERTEX||q->command==MOVIES_D3D9_UPLOAD_INDEX||q->command==MOVIES_D3D9_UPLOAD_TEXTURE){assert(q->data&&q->bytes);r->values[0]=next_handle++;uploaded_bytes=q->bytes;memcpy(uploaded,q->data,q->bytes<128?q->bytes:128);}
 if(q->command==MOVIES_D3D9_UPLOAD_VERTEX){memcpy(last_vertex_args,q->args,sizeof last_vertex_args);last_vertex_bytes=q->bytes;last_vertex_fvf=q->args[0];memcpy(last_vertices,q->data,q->bytes<256?q->bytes:256);if(check_finite_vertices){unsigned stride=q->args[0]==0x142?24:16;for(unsigned at=0;at<q->bytes;at+=stride)for(unsigned k=0;k<3;k++){float v;memcpy(&v,(const uint8_t*)q->data+at+k*4,4);if(!isfinite(v))return 0;}}}
 if(q->command==MOVIES_D3D9_UPLOAD_INDEX){last_index_bytes=q->bytes;memcpy(last_indices,q->data,q->bytes<128?q->bytes:128);}
 if(q->command==MOVIES_D3D9_CREATE_TARGET){assert(q->args[0]&&q->args[1]&&!q->data);r->values[0]=next_handle++;}
 if(q->command==MOVIES_D3D9_CLEAR)memcpy(clearargs,q->args,sizeof clearargs);
 if(q->command==MOVIES_D3D9_COPY_TARGET){assert(q->args[0]&&q->args[1]&&q->args[0]!=q->args[1]);memcpy(last_copy,q->args,sizeof last_copy);}
 if(q->command==MOVIES_D3D9_SET_TARGET)memcpy(last_target,q->args,sizeof last_target);
 if(q->command==MOVIES_D3D9_READ_BACKBUFFER)assert(q->args[1]==640&&q->args[2]==480);
 if(q->command==MOVIES_D3D9_READ_TARGET||q->command==MOVIES_D3D9_READ_BACKBUFFER){assert(q->bytes==q->args[1]*q->args[2]*4);uint8_t*out=(uint8_t*)q->data;for(uint32_t i=0;i<q->bytes;i+=4){out[i]=0x11;out[i+1]=0x22;out[i+2]=0x33;out[i+3]=0x44;}}
 if(q->command==MOVIES_D3D9_WRITE_TARGET){assert(q->bytes==q->args[1]*q->args[2]*4);uploaded_bytes=q->bytes;memcpy(uploaded,q->data,q->bytes<128?q->bytes:128);}
 if(q->command==MOVIES_D3D9_DRAW){memcpy(drawargs,q->args,sizeof drawargs);last_lit=q->args[61];memcpy(last_lighting,q->lighting,sizeof last_lighting);assert(q->matrices[0]==1&&q->matrices[16]==1&&q->matrices[32]==1);}
 return 1;
}
static uint32_t invoke(GuestFn f,unsigned argc,const uint32_t*args){CPU c={0};c.fpu_cw=0x0b7f;c.fpu_sw=0x4500;c.top=3;c.st[3]=1.25;c.esp=0x10000;W32(c.esp,0x12345678);for(unsigned i=0;i<argc;i++)W32(c.esp+4+i*4,args[i]);f(&c);assert(c.fpu_cw==0x0b7f&&c.fpu_sw==0x4500&&c.top==3&&c.st[3]==1.25);assert(c.esp==0x10000+4*(argc+1));return c.eax;}
static uint32_t method(uint32_t object,unsigned slot,unsigned argc,...){uint32_t args[12]={object};assert(argc<=12);va_list ap;va_start(ap,argc);for(unsigned i=1;i<argc;i++)args[i]=va_arg(ap,uint32_t);va_end(ap);uint32_t addr=R32(R32(object)+slot*4);return invoke(table[(addr-0x0e020000)/0x1000][((addr-0x0e020000)%0x1000)/16],argc,args);}
static uint32_t factory(unsigned version){uint32_t n;ImportHandler*h=movies_d3d9_handlers(&n);assert(n==1&&!strcmp(h[0].name,"D3D9.DLL!Direct3DCreate9"));return invoke(h[0].fn,1,&version);}
static void present_params(void){memset(HP(0x1000),0,56);W32(0x1000,640);W32(0x1004,480);W32(0x1008,22);W32(0x100c,1);W32(0x1018,1);W32(0x101c,42);W32(0x1020,1);W32(0x1024,1);W32(0x1028,80);}
#ifndef __EMSCRIPTEN__
static void*foreign(void*unused){(void)unused;if(!setjmp(trap)){factory(32);assert(0);}assert(!movies_d3d9_init());return NULL;}
#endif
int main(void){g_mem=calloc(1,0x05500000);assert(g_mem);assert(movies_d3d9_init());assert(!factory(32));movies_d3d9_set_host(backend,NULL);assert(!factory(31));
#ifndef __EMSCRIPTEN__
 pthread_t other;assert(!pthread_create(&other,NULL,foreign,NULL));assert(!pthread_join(other,NULL));
#endif
 uint32_t f=factory(32);assert(f);assert(method(f,4,1)==1);assert(method(f,6,3,0u,22u)==1);assert(method(f,6,3,0u,23u)==0);assert(method(f,6,3,0u,25u)==1);assert(!method(f,7,5,0u,25u,0u,0x2000u)&&R32(0x200c)==25);assert(!method(f,8,3,0u,0x2000u));assert(R32(0x2000)==640&&R32(0x2004)==480&&R32(0x200c)==22);
 memset(HP(0x3000),0xa5,1200);assert(!method(f,5,4,0u,0u,0x3000u));assert(!strcmp((char*)HP(0x3000+512),"The Movies bounded WebGPU bridge"));assert(R8(0x3000+1100)==0xa5);
 assert(!method(f,14,4,0u,1u,0x4000u));assert(R32(0x4000+36)&0x01000000);assert(R32(0x4000+40)==0x88&&R32(0x4000+44)==0x7ff&&R32(0x4000+48)==0x7ff);assert(R32(0x4000+196)==0&&R32(0x4000+204)==0&&R32(0x4000+160)==0&&R32(0x4000+188)==1);assert(!(R32(0x4000+28)&0x10000));
 assert(!method(f,10,7,0u,1u,22u,0u,3u,21u));assert(!method(f,10,7,0u,1u,22u,2u,1u,80u));
 present_params();assert(method(f,16,7,0u,1u,42u,0x40u,0x1000u,0x2000u)==0x8876086a&&R32(0x2000)==0);
 W32(0x1000,800);assert(method(f,16,7,0u,1u,42u,0x20u,0x1000u,0x2000u)==0x8876086a);present_params();
 W32(0x1034,0x80000000);assert(method(f,16,7,0u,1u,42u,0x20u,0x1000u,0x2000u)==0x8876086a);present_params();
 assert(!method(f,16,7,0u,1u,42u,0x20u,0x1000u,0x2000u));uint32_t d=R32(0x2000);assert(d);assert(!method(d,38,3,0u,0x2004u));uint32_t rt=R32(0x2004);assert(!method(d,40,2,0x2008u));uint32_t ds=R32(0x2008);
 assert(!method(rt,12,2,0x4000u));assert(R32(0x4000)==22&&R32(0x4004)==1&&R32(0x4018)==640&&R32(0x401c)==480);assert(!method(ds,12,2,0x4000u)&&R32(0x4000)==80&&R32(0x4008)==2);
 /* Null bindings select the actual existing fixed-function pipeline. */
 for(unsigned i=0;i<2;i++){unsigned set=i?107:92,get=i?108:93;
  W32(0x4100,0xdeadbeef);assert(!method(d,get,2,0x4100u)&&!R32(0x4100));
  assert(!method(d,set,2,0u));assert(!method(d,set,2,0u));
  memset(HP(0x4100),0xa5,8);assert(!method(d,get,2,0x4101u));assert(R8(0x4100)==0xa5&&R8(0x4105)==0xa5);uint32_t shader;memcpy(&shader,HP(0x4101),4);assert(!shader);
  assert(method(d,get,2,0u)==0x8876086c);W8(0x054fffff,0xa5);assert(method(d,get,2,0x054fffffu)==0x8876086c&&R8(0x054fffff)==0xa5);
  if(!setjmp(trap)){method(d,set,2,d);assert(0);} /* no fake non-null execution */
  uint32_t args[]={f,0x4100u};if(!setjmp(trap)){invoke(table[1][get],2,args);assert(0);} /* wrong COM type */
  W32(0x4100,0xdeadbeef);assert(!method(d,get,2,0x4100u)&&!R32(0x4100));
 }
 /* Scissor state starts full-target, disabled, and bounds include all16 bytes. */
 assert(!method(d,76,2,0x4601u));int32_t rect[4];memcpy(rect,HP(0x4601),16);assert(!rect[0]&&!rect[1]&&rect[2]==640&&rect[3]==480);
 const int32_t wanted[4]={10,20,100,200};memcpy(HP(0x4601),wanted,16);assert(!method(d,75,2,0x4601u));memset(HP(0x4620),0xa5,20);assert(!method(d,76,2,0x4621u)&&!memcmp(HP(0x4621),wanted,16)&&R8(0x4620)==0xa5&&R8(0x4631)==0xa5);
 assert(method(d,75,2,0u)==0x8876086c&&method(d,76,2,0u)==0x8876086c);assert(method(d,75,2,0x054ffff8u)==0x8876086c&&method(d,76,2,0x054ffff8u)==0x8876086c);
 const int32_t invalid[][4]={{-1,0,10,10},{0,-1,10,10},{10,0,9,10},{0,10,10,9},{0,0,641,10},{0,0,10,481}};
 for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++){memcpy(HP(0x4601),invalid[i],16);assert(method(d,75,2,0x4601u)==0x8876086c);assert(!method(d,76,2,0x4621u)&&!memcmp(HP(0x4621),wanted,16));}
 const int32_t empty[4]={10,20,10,20};memcpy(HP(0x4601),empty,16);assert(!method(d,75,2,0x4601u));memcpy(HP(0x4601),wanted,16);assert(!method(d,75,2,0x4601u));
 /* Documented render-default regression fixture, reserved enum holes fail. */
 const uint32_t render_defaults[][2]={{7,0x1u},{8,0x3u},{9,0x2u},{14,0x1u},{15,0x0u},{16,0x1u},{19,0x2u},{20,0x1u},{22,0x3u},{23,0x4u},{24,0x0u},{25,0x8u},{26,0x0u},{27,0x0u},{28,0x0u},{29,0x0u},{34,0x0u},{35,0x0u},{36,0x0u},{37,0x3f800000u},{38,0x3f800000u},{48,0x0u},{52,0x0u},{53,0x1u},{54,0x1u},{55,0x1u},{56,0x8u},{57,0x0u},{58,0xffffffffu},{59,0xffffffffu},{60,0xffffffffu},{128,0x0u},{129,0x0u},{130,0x0u},{131,0x0u},{132,0x0u},{133,0x0u},{134,0x0u},{135,0x0u},{136,0x1u},{137,0x1u},{139,0x0u},{140,0x0u},{141,0x1u},{142,0x1u},{143,0x0u},{145,0x1u},{146,0x2u},{147,0x0u},{148,0x0u},{151,0x0u},{152,0x0u},{154,0x3f800000u},{155,0x3f800000u},{156,0x0u},{157,0x0u},{158,0x3f800000u},{159,0x0u},{160,0x0u},{161,0x1u},{162,0xffffffffu},{163,0x0u},{166,0x0u},{167,0x0u},{168,0xfu},{170,0x0u},{171,0x1u},{172,0x3u},{173,0x1u},{174,0x0u},{175,0x0u},{176,0x0u},{178,0x3f800000u},{179,0x3f800000u},{180,0x0u},{181,0x0u},{182,0x3f800000u},{183,0x0u},{184,0x0u},{185,0x0u},{186,0x1u},{187,0x1u},{188,0x1u},{189,0x8u},{190,0xfu},{191,0xfu},{192,0xfu},{193,0xffffffffu},{194,0x0u},{195,0x0u},{198,0x0u},{199,0x0u},{200,0x0u},{201,0x0u},{202,0x0u},{203,0x0u},{204,0x0u},{205,0x0u},{206,0x0u},{207,0x2u},{208,0x1u},{209,0x1u}};
 for(unsigned i=0;i<sizeof(render_defaults)/sizeof(render_defaults[0]);i++){assert(!method(d,58,3,render_defaults[i][0],0x4100u));assert(R32(0x4100)==render_defaults[i][1]);}
 for(unsigned i=0;i<210;i++){int valid=0;for(unsigned j=0;j<sizeof(render_defaults)/sizeof(render_defaults[0]);j++)if(render_defaults[j][0]==i)valid=1;if(!valid)assert(method(d,58,3,i,0x4100u)==0x8876086c);}
 assert(!method(d,57,3,206u,1u)&&!method(d,57,3,206u,0u)); /* separate alpha is stored and forwarded */
 {const float plane[4]={0,1,0,-2};memcpy(HP(0x4700),plane,16);assert(!method(d,55,3,1u,0x4700u));memset(HP(0x4710),0,16);assert(!method(d,56,3,1u,0x4710u)&&!memcmp(HP(0x4700),HP(0x4710),16));assert(method(d,55,3,6u,0x4700u)==0x8876086c);}
 assert(method(d,57,3,171u,6u)==0x8876086c&&method(d,57,3,20u,12u)==0x8876086c&&method(d,57,3,23u,9u)==0x8876086c); /* invalid enums: INVALIDCALL */
 if(!setjmp(trap)){method(d,57,3,250u,1u);assert(0);} /* unknown state stops */
 assert(!method(d,57,3,25u,5u));assert(!method(d,57,3,24u,100u));assert(!method(d,58,3,25u,0x4100u)&&R32(0x4100)==5);assert(!method(d,57,3,15u,1u));
 memset(HP(0x4200),0,68);for(unsigned i=0;i<8;i++)WF32(0x4200+i*4,1);assert(!method(d,49,2,0x4200u));assert(!method(d,50,2,0x4300u)&&!memcmp(HP(0x4200),HP(0x4300),68));W32(0x4200,0x7fc00000);assert(method(d,49,2,0x4200u)==0x8876086c);assert(!method(d,50,2,0x4300u)&&RF32(0x4300)==1);
 /* Light state is real queryable storage; lighting-enabled draws still fail. */
 memset(HP(0x4400),0,104);W32(0x4400,3);WF32(0x4404,1);WF32(0x4448,1);assert(!method(d,51,3,1u,0x4400u));assert(!method(d,52,3,1u,0x4500u)&&!memcmp(HP(0x4400),HP(0x4500),104));assert(!method(d,54,3,1u,0x4100u)&&R32(0x4100)==0);assert(!method(d,53,3,1u,1u));assert(!method(d,54,3,1u,0x4100u)&&R32(0x4100)==1);
 assert(!method(d,53,3,0xffffffffu,0u));assert(!method(d,52,3,0xffffffffu,0x4500u));assert(R32(0x4500)==3&&RF32(0x4504)==1&&RF32(0x450c)==1&&RF32(0x4510)==0&&RF32(0x4548)==1);assert(method(d,52,3,99u,0x4500u)==0x8876086c);W32(0x4400,99);assert(method(d,51,3,1u,0x4400u)==0x8876086c);assert(!method(d,52,3,1u,0x4500u)&&R32(0x4500)==3);
 /* Original994F10 cache query: every valid TSS and sampler state, including disabled stages. */
 const unsigned states[]={1,2,3,4,5,6,7,8,9,10,11,22,23,24,26,27,28,32};
 for(unsigned stage=0;stage<8;stage++)for(unsigned j=0;j<sizeof(states)/sizeof(states[0]);j++){unsigned state=states[j];uint32_t expected=state==1?(stage?1:4):state==4?(stage?1:2):(state==2||state==5)?2:(state==3||state==6||state==26||state==27||state==28)?1:state==11?stage:0;assert(!method(d,66,4,stage,state,0x4100u)&&R32(0x4100)==expected);assert(!method(d,67,4,stage,state,expected));}
 for(unsigned stage=0;stage<16;stage++)for(unsigned state=1;state<=13;state++){uint32_t expected=state<=3||state==5||state==6||state==10;assert(!method(d,68,4,stage,state,0x4100u)&&R32(0x4100)==expected);assert(!method(d,69,4,stage,state,expected));}
 assert(!method(d,67,4,1u,1u,4u)&&!method(d,67,4,1u,1u,1u)); /* stage 1 is evaluated by the shader combiner */
 if(!setjmp(trap)){method(d,67,4,0u,1u,27u);assert(0);} /* D3DTEXTUREOP ends at 26 */
 if(!setjmp(trap)){method(d,67,4,0u,2u,7u);assert(0);} /* D3DTA selector 7 does not exist */
 if(!setjmp(trap)){method(d,66,4,0u,31u,0x4100u);assert(0);} /* sparse enum hole */
 assert(method(d,66,4,0u,1u,0x054fffffu)==0x8876086c);
 assert(!method(d,69,4,0u,1u,3u));assert(!method(d,68,4,0u,1u,0x4100u)&&R32(0x4100)==3);assert(!method(d,68,4,1u,1u,0x4100u)&&R32(0x4100)==1);
 /* Actual995123..9951C3 initializer for all4 samplers. */
 for(unsigned stage=0;stage<4;stage++){assert(!method(d,69,4,stage,7u,1u));assert(!method(d,69,4,stage,6u,2u));assert(!method(d,69,4,stage,5u,2u));assert(!method(d,69,4,stage,8u,0xbf800000u));assert(!method(d,68,4,stage,8u,0x4100u)&&R32(0x4100)==0xbf800000u);}
 assert(!method(d,43,7,0u,0u,3u,0xff112233u,0x3f800000u,0u));assert(calls[MOVIES_D3D9_CLEAR]==1);
 assert(!method(d,26,7,48u,0u,0x42u,1u,0x2010u,0u));uint32_t vb=R32(0x2010);assert(!method(d,27,7,6u,0u,101u,1u,0x2014u,0u));uint32_t ib=R32(0x2014);
 assert(!method(vb,11,5,0u,0u,0x2020u,0u));uint32_t vp=R32(0x2020);for(unsigned i=0;i<3;i++){WF32(vp+i*16,0);WF32(vp+i*16+4,0);WF32(vp+i*16+8,.5f);W32(vp+i*16+12,0xffff0000);}assert(method(vb,11,5,0u,0u,0x2020u,0u)==0x8876086c);assert(!method(vb,12,1));assert(method(vb,11,5,47u,2u,0x2020u,0u)==0x8876086c);
 assert(!method(ib,11,5,0u,0u,0x2020u,0u));uint32_t ip=R32(0x2020);W16(ip,0);W16(ip+2,1);W16(ip+4,2);assert(!method(ib,12,1));
 assert(!method(d,89,2,0x42u));assert(!method(d,92,2,0u)&&!method(d,107,2,0u));assert(!method(d,90,2,0x4100u)&&R32(0x4100)==0x42);assert(!method(d,100,5,0u,vb,0u,16u));assert(!method(d,104,2,ib));assert(!method(d,57,3,137u,0u));assert(!method(d,57,3,22u,1u));assert(!method(d,41,1));
 movies_d3d9_force_cpu_lighting(1); /* CPU lighting reference; the GPU path is checked below */
 /* Lighting on an XYZ|DIFFUSE format converts to a lit XYZ|DIFFUSE|TEX1 snapshot. */
 assert(!method(d,57,3,137u,1u));assert(!method(d,82,7,4u,0u,0u,3u,0u,1u)&&last_vertex_fvf==0x242&&last_vertex_bytes==96);assert(!method(d,57,3,137u,0u));
 assert(!method(d,82,7,4u,0u,0u,3u,0u,1u));assert(calls[MOVIES_D3D9_DRAW]==2&&drawargs[3]==3&&drawargs[5]==1);unsigned uploads=calls[MOVIES_D3D9_UPLOAD_VERTEX];assert(!method(d,82,7,4u,0u,0u,3u,0u,1u)&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==uploads);
 assert(method(d,82,7,4u,0xffffffffu,0u,3u,0u,1u)==0x8876086c);assert(!method(d,42,1));assert(!method(d,17,5,0u,0u,0u,0u));assert(calls[MOVIES_D3D9_PRESENT]==1);
 /* D3D ignores unused vertices: NaNs outside the referenced span must not
  * reach the host validator. Nonzero stream/start, negative base, INDEX32. */
 assert(!method(d,26,7,256u,0x208u,0x42u,0u,0x2030u,0u));uint32_t slice_vb=R32(0x2030);
 assert(!method(d,27,7,32u,0x208u,102u,0u,0x2030u,0u));uint32_t slice_ib=R32(0x2030);
 assert(!method(slice_vb,11,5,0u,0u,0x2020u,0x2800u));uint32_t slice_vp=R32(0x2020);for(unsigned i=0;i<256;i+=4)W32(slice_vp+i,0x7fc00000);
 for(unsigned i=4;i<8;i++){WF32(slice_vp+i*16,(float)i);WF32(slice_vp+i*16+4,0);WF32(slice_vp+i*16+8,.5f);W32(slice_vp+i*16+12,0xffabcdef);}assert(!method(slice_vb,12,1));
 assert(!method(slice_ib,11,5,0u,0u,0x2020u,0x2800u));uint32_t slice_ip=R32(0x2020);for(unsigned i=0;i<8;i++)W32(slice_ip+i*4,0xffffffff);W32(slice_ip+8,5);W32(slice_ip+12,7);W32(slice_ip+16,6);W32(slice_ip+20,5);assert(!method(slice_ib,12,1));
 assert(!method(d,100,5,0u,slice_vb,16u,16u));assert(!method(d,104,2,slice_ib));assert(!method(d,41,1));check_finite_vertices=1;
 assert(!method(d,82,7,4u,0xfffffffeu,5u,3u,2u,1u));assert(last_vertex_bytes==48&&last_index_bytes==12&&drawargs[2]==0&&drawargs[4]==0);uint32_t normalized[3];memcpy(normalized,last_indices,12);assert(normalized[0]==0&&normalized[1]==2&&normalized[2]==1);assert(!memcmp(last_vertices,HP(slice_vp+64),48)&&R32(slice_ip+8)==5&&R32(slice_vp)==0x7fc00000);
 unsigned cached_v=calls[MOVIES_D3D9_UPLOAD_VERTEX],cached_i=calls[MOVIES_D3D9_UPLOAD_INDEX];
 assert(!method(d,82,7,4u,0xfffffffeu,5u,3u,2u,1u)&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==cached_v&&calls[MOVIES_D3D9_UPLOAD_INDEX]==cached_i);
 assert(!method(d,82,7,4u,0xfffffffeu,5u,3u,3u,1u)&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==cached_v&&calls[MOVIES_D3D9_UPLOAD_INDEX]==cached_i+1);memcpy(normalized,last_indices,12);assert(normalized[0]==2&&normalized[1]==1&&normalized[2]==0);
 assert(!method(d,100,5,0u,slice_vb,32u,16u));assert(!method(d,82,7,4u,0xfffffffeu,5u,3u,3u,1u)&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==cached_v+1);assert(!memcmp(last_vertices,HP(slice_vp+80),48));
 assert(!method(d,42,1));assert(!method(slice_ib,11,5,8u,12u,0x2020u,0x1800u));W32(slice_ip+8,5);W32(slice_ip+12,7);W32(slice_ip+16,5);assert(!method(slice_ib,12,1));assert(!method(d,41,1));assert(!method(d,82,7,4u,0xfffffffeu,5u,3u,2u,1u));memcpy(normalized,last_indices,12);assert(normalized[0]==0&&normalized[1]==2&&normalized[2]==0);
 assert(!method(d,42,1));assert(!method(slice_ib,11,5,8u,12u,0x2020u,0x1800u));W32(slice_ip+8,0x80000003);W32(slice_ip+12,0x80000005);W32(slice_ip+16,0x80000004);assert(!method(slice_ib,12,1));assert(!method(d,41,1));assert(!method(d,82,7,4u,0x80000000u,0x80000003u,3u,2u,1u));memcpy(normalized,last_indices,12);assert(normalized[0]==0&&normalized[1]==2&&normalized[2]==1&&drawargs[4]==0);
 assert(!method(d,42,1));assert(!method(slice_ib,11,5,8u,12u,0x2020u,0x1800u));W32(slice_ip+8,0);W32(slice_ip+12,2);W32(slice_ip+16,1);assert(!method(slice_ib,12,1));
 assert(!method(slice_vb,11,5,80u,16u,0x2020u,0x1800u));WF32(slice_vp+80,42.f);assert(!method(slice_vb,12,1));assert(!method(d,41,1));cached_v=calls[MOVIES_D3D9_UPLOAD_VERTEX];assert(!method(d,82,7,4u,3u,0u,3u,2u,1u)&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==cached_v+1);float changed;memcpy(&changed,last_vertices,4);assert(changed==42.f);
 assert(!method(d,42,1));assert(!method(d,100,5,0u,vb,0u,16u));assert(!method(d,104,2,ib));assert(!method(slice_vb,2,1)&&!method(slice_ib,2,1));check_finite_vertices=0;
 /* Valid blend state storage is separate from supported drawing equations. */
 assert(!method(d,57,3,19u,3u)&&!method(d,58,3,19u,0x4100u)&&R32(0x4100)==3);
 assert(!method(d,57,3,20u,4u)&&!method(d,58,3,20u,0x4100u)&&R32(0x4100)==4);assert(!method(d,57,3,27u,1u));
 assert(!method(d,57,3,19u,12u));assert(!method(d,41,1));unsigned blend_draws=calls[MOVIES_D3D9_DRAW];assert(!method(d,82,7,4u,0u,0u,3u,0u,1u));assert(calls[MOVIES_D3D9_DRAW]==blend_draws+1&&drawargs[25]==1); /* BOTHSRCALPHA = SRCALPHA/INVSRCALPHA */
 assert(!method(d,57,3,171u,3u)&&!method(d,57,3,206u,1u)&&!method(d,57,3,207u,2u)&&!method(d,57,3,208u,1u)&&!method(d,57,3,209u,5u));assert(!method(d,82,7,4u,0u,0u,3u,0u,1u)&&drawargs[25]==(256u|0x56u)&&drawargs[56]==(3u|(5u<<4)|(1u<<8)|(2u<<12)|(1u<<16)));
 assert(!method(d,57,3,171u,1u)&&!method(d,57,3,206u,0u));
 assert(!method(d,57,3,195u,0x3a83126fu)&&!method(d,57,3,168u,7u));assert(!method(d,82,7,4u,0u,0u,3u,0u,1u)&&drawargs[58]==0x3a83126fu&&drawargs[60]==(0x100u|7u));assert(!method(d,57,3,195u,0u)&&!method(d,57,3,168u,15u));assert(!method(d,42,1));
 assert(!method(d,57,3,27u,0u)&&!method(d,57,3,19u,2u)&&!method(d,57,3,20u,1u));
 /* NOSYSLOCK is orthogonal to the required DYNAMIC usage on VB and IB. */
 assert(!method(vb,11,5,0u,16u,0x2020u,0x800u));assert(!method(vb,12,1));
 assert(method(vb,11,5,0u,16u,0x2020u,0x1800u)==0x8876086c);assert(method(vb,11,5,0u,16u,0x2020u,0x2800u)==0x8876086c);
 for(unsigned kind=0;kind<2;kind++){
  assert(!method(d,kind?27:26,7,48u,0x208u,kind?101u:0x142u,0u,0x2030u,0u));uint32_t dynamic=R32(0x2030);
  assert(!method(dynamic,11,5,0u,0u,0x2020u,0x2800u));uint32_t dynamicData=R32(0x2020);W32(dynamicData,0x12345678);assert(!method(dynamic,12,1));
  assert(!method(dynamic,11,5,4u,8u,0x2020u,0x1800u)&&R32(0x2020)==dynamicData+4);W32(R32(0x2020),0x87654321);assert(!method(dynamic,12,1)&&R32(dynamicData)==0x12345678);
  assert(method(dynamic,11,5,0u,4u,0x2020u,0x3800u)==0x8876086c);assert(method(dynamic,11,5,0u,4u,0x2020u,0x810u)==0x8876086c);assert(method(dynamic,11,5,47u,2u,0x2020u,0x1800u)==0x8876086c);assert(!method(dynamic,2,1));
 }
 /* Asset startup must exceed the former128-slot table without losing the
  * subsequent system-memory texture, its surface, or dynamic UI buffers. */
 uint32_t many[4096];unsigned many_count=0;
 for(;many_count<200;many_count++){assert(!method(d,23,9,1u,1u,1u,0u,21u,1u,0x2030u,0u));many[many_count]=R32(0x2030);}
 assert(!method(d,23,9,512u,512u,1u,0x200u,21u,2u,0x2030u,0u));uint32_t late=R32(0x2030);
 assert(!method(late,18,3,0u,0x2060u));uint32_t late_surface=R32(0x2060);assert(method(late_surface,2,1)==1);assert(!method(late,2,1));
 assert(!method(d,26,7,96u,0x208u,0x142u,0u,0x2030u,0u));assert(!method(R32(0x2030),2,1));
 assert(!method(d,27,7,12u,0x208u,101u,0u,0x2030u,0u));assert(!method(R32(0x2030),2,1));
 for(;many_count<4096;many_count++){uint32_t hr=method(d,23,9,1u,1u,1u,0u,21u,2u,0x2030u,0u);if(hr){assert(hr==0x8007000e&&!R32(0x2030));break;}many[many_count]=R32(0x2030);}
 assert(many_count>4000&&many_count<4096);uint32_t recycled=many[--many_count];assert(!method(recycled,2,1));
 assert(!method(d,23,9,1u,1u,1u,0u,21u,2u,0x2030u,0u)&&R32(0x2030)==recycled);assert(!method(recycled,2,1));
 while(many_count)assert(!method(many[--many_count],2,1));
 /* A4R4G4B4 guest storage stays packed and expands each nibble exactly. */
 assert(!method(d,23,9,3u,2u,1u,0u,26u,1u,0x2030u,0u));uint32_t packed=R32(0x2030);
 assert(!method(packed,19,5,0u,0x2040u,0u,0u)&&R32(0x2040)==6);uint32_t packedbits=R32(0x2044);W16(packedbits,0x8123);W16(packedbits+2,0xffff);assert(!method(packed,20,2,0u));
 assert(uploaded_bytes==24&&uploaded[0]==17&&uploaded[1]==34&&uploaded[2]==51&&uploaded[3]==136&&uploaded[4]==255&&R16(packedbits)==0x8123);assert(!method(packed,2,1));
 /* Two DXT3 blocks, explicit alpha0..15, reversed endpoints still four colors. */
 assert(!method(d,23,9,8u,4u,1u,0u,0x33545844u,1u,0x2030u,0u));uint32_t bc=R32(0x2030);
 assert(!method(bc,19,5,0u,0x2040u,0u,0u)&&R32(0x2040)==32);uint32_t block=R32(0x2044);
 for(unsigned i=0;i<8;i++)W8(block+i,(i*2)|((i*2+1)<<4));W16(block+8,0x001f);W16(block+10,0xf800);W32(block+12,0xe4e4e4e4);
 memset(HP(block+16),255,8);W16(block+24,0x07e0);W16(block+26,0);W32(block+28,0);
 assert(!method(bc,20,2,0u)&&uploaded_bytes==128);
 for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){unsigned p=(y*8+x)*4;const uint8_t colors[4][3]={{0,0,255},{255,0,0},{85,0,170},{170,0,85}};assert(!memcmp(uploaded+p,colors[x],3)&&uploaded[p+3]==(y*4+x)*17);assert(uploaded[(y*8+x+4)*4+1]==255);}
 W32(0x2050,4);W32(0x2054,0);W32(0x2058,8);W32(0x205c,4);assert(!method(bc,19,5,0u,0x2040u,0x2050u,0x10u)&&R32(0x2044)==block+16&&R32(0x2040)==32);assert(!method(bc,20,2,0u));W32(0x2050,1);assert(method(bc,19,5,0u,0x2040u,0x2050u,0u)==0x8876086c);assert(!method(bc,2,1));
 assert(method(d,23,9,3u,4u,1u,0u,0x33545844u,1u,0x2030u,0u)==0x8876086c);
 /* DXT1 selects four opaque colors only when endpoint0 > endpoint1. */
 assert(!method(d,23,9,8u,4u,1u,0u,0x31545844u,1u,0x2030u,0u));uint32_t bc1=R32(0x2030);
 assert(!method(bc1,19,5,0u,0x2040u,0u,0u)&&R32(0x2040)==16);uint32_t bc1bits=R32(0x2044);
 W16(bc1bits,0xf800);W16(bc1bits+2,0x001f);W32(bc1bits+4,0xe4e4e4e4);W16(bc1bits+8,0x001f);W16(bc1bits+10,0xf800);W32(bc1bits+12,0xe4e4e4e4);assert(!method(bc1,20,2,0u));
 for(unsigned y=0;y<4;y++)for(unsigned x=0;x<8;x++){const uint8_t colors[8][4]={{255,0,0,255},{0,0,255,255},{170,0,85,255},{85,0,170,255},{0,0,255,255},{255,0,0,255},{127,0,127,255},{0,0,0,0}};assert(!memcmp(uploaded+(y*8+x)*4,colors[x],4));}
 W32(0x2050,4);W32(0x2054,0);W32(0x2058,8);W32(0x205c,4);assert(!method(bc1,19,5,0u,0x2040u,0x2050u,0u)&&R32(0x2044)==bc1bits+8);W16(bc1bits+8,0x07e0);W16(bc1bits+10,0x07e0);assert(!method(bc1,20,2,0u));assert(uploaded[6*4+1]==255&&uploaded[7*4+3]==0);assert(!method(bc1,2,1));
 /* Observed RenderTargetMemory: dynamic SYSTEMMEM is CPU-only, no GPU upload. */
 unsigned sysuploads=calls[MOVIES_D3D9_UPLOAD_TEXTURE];
 assert(!method(d,23,9,3u,2u,1u,0x200u,21u,2u,0x2030u,0u));uint32_t sys=R32(0x2030);
 assert(calls[MOVIES_D3D9_UPLOAD_TEXTURE]==sysuploads);
 assert(!method(sys,17,3,0u,0x4000u)&&R32(0x4008)==0x200&&R32(0x400c)==2);
 assert(!method(sys,18,3,0u,0x2060u));uint32_t syssurf=R32(0x2060);
 assert(!method(syssurf,12,2,0x4000u)&&R32(0x4008)==0x200&&R32(0x400c)==2);
 assert(!method(sys,19,5,0u,0x2040u,0u,0x2000u));uint32_t syspixels=R32(0x2044);assert(R32(0x2040)==12);W32(syspixels,0x12345678);
 assert(method(syssurf,13,4,0x2040u,0u,0u)==0x8876086c);assert(!method(sys,20,2,0u));
 assert(!method(syssurf,13,4,0x2040u,0u,0x10u)&&R32(R32(0x2044))==0x12345678);assert(!method(syssurf,14,1));
 assert(calls[MOVIES_D3D9_UPLOAD_TEXTURE]==sysuploads);
 assert(method(d,65,3,0u,sys)==0x8876086c);
 assert(method(sys,19,5,0u,0x2040u,0u,0x2010u)==0x8876086c);
 assert(method(sys,19,5,0u,0x2040u,0x2050u,0x2000u)==0x8876086c);
 assert(method(syssurf,2,1)==1);assert(!method(sys,2,1));
 assert(method(d,23,9,3u,2u,1u,1u,21u,2u,0x2030u,0u)==0x8876086a&&!R32(0x2030));
 /* Additional sampled formats decode to RGBA8 on upload: R5G6B5, L8, A8L8 and DXT5. */
 {assert(!method(f,10,7,0u,1u,22u,0u,3u,23u));assert(!method(f,10,7,0u,1u,22u,0u,3u,0x35545844u));
  assert(!method(d,23,9,2u,1u,1u,0u,23u,1u,0x2030u,0u));uint32_t t565=R32(0x2030);assert(!method(t565,19,5,0u,0x2040u,0u,0x800u));assert(R32(0x2040)==4);
  W16(R32(0x2044),0xf800);W16(R32(0x2044)+2,0x07e0);assert(!method(t565,20,2,0u));
  assert(uploaded_bytes==8&&uploaded[0]==255&&uploaded[1]==0&&uploaded[2]==0&&uploaded[3]==255&&uploaded[4]==0&&uploaded[5]==255&&uploaded[6]==0);assert(!method(t565,2,1));
  assert(!method(d,23,9,2u,1u,1u,0u,50u,1u,0x2030u,0u));uint32_t tl8=R32(0x2030);assert(!method(tl8,19,5,0u,0x2040u,0u,0x800u));assert(R32(0x2040)==2);
  W8(R32(0x2044),0x40);W8(R32(0x2044)+1,0xc0);assert(!method(tl8,20,2,0u));assert(uploaded[0]==0x40&&uploaded[2]==0x40&&uploaded[3]==255&&uploaded[4]==0xc0);assert(!method(tl8,2,1));
  assert(!method(d,23,9,1u,1u,1u,0u,51u,1u,0x2030u,0u));uint32_t tal=R32(0x2030);assert(!method(tal,19,5,0u,0x2040u,0u,0x800u));W16(R32(0x2044),0x8020);assert(!method(tal,20,2,0u));
  assert(uploaded[0]==0x20&&uploaded[1]==0x20&&uploaded[3]==0x80);assert(!method(tal,2,1));
  assert(!method(d,23,9,4u,4u,1u,0u,0x35545844u,1u,0x2030u,0u));uint32_t t5=R32(0x2030);assert(!method(t5,19,5,0u,0x2040u,0u,0x800u));assert(R32(0x2040)==16);
  uint32_t b=R32(0x2044);W8(b,200);W8(b+1,100);for(unsigned k=2;k<8;k++)W8(b+k,0);W16(b+8,0xffff);W16(b+10,0);for(unsigned k=12;k<16;k++)W8(b+k,0);assert(!method(t5,20,2,0u));
  assert(uploaded_bytes==64&&uploaded[0]==255&&uploaded[1]==255&&uploaded[2]==255&&uploaded[3]==200);assert(!method(t5,2,1));}
 /* Exact video texture: one managed A8R8G8B8 level, tight BGRA pitch. */
 assert(method(d,23,9,3u,2u,0u,0u,21u,1u,0x2030u,0u)==0x8876086a);
 assert(!method(d,23,9,3u,2u,1u,0u,21u,1u,0x2030u,0u));uint32_t tx=R32(0x2030);assert(uploaded_bytes==24);
 assert(method(tx,13,1)==1);assert(!method(tx,17,3,0u,0x4000u));assert(R32(0x4000)==21&&R32(0x400c)==1&&R32(0x4018)==3&&R32(0x401c)==2);
 assert(!method(tx,19,5,0u,0x2040u,0u,0x800u));uint32_t pixels=R32(0x2044);assert(R32(0x2040)==12);W32(pixels,0x7f112233);assert(method(tx,19,5,0u,0x2040u,0u,0u)==0x8876086c);assert(!method(tx,20,2,0u));assert(uploaded[0]==0x11&&uploaded[1]==0x22&&uploaded[2]==0x33&&uploaded[3]==0x7f&&R32(pixels)==0x7f112233);
 unsigned texuploads=calls[MOVIES_D3D9_UPLOAD_TEXTURE];assert(!method(tx,19,5,0u,0x2040u,0u,0x10u));assert(!method(tx,20,2,0u));assert(calls[MOVIES_D3D9_UPLOAD_TEXTURE]==texuploads);
 W32(0x2050,1);W32(0x2054,1);W32(0x2058,3);W32(0x205c,2);assert(!method(tx,19,5,0u,0x2040u,0x2050u,0x10u));assert(R32(0x2040)==12&&R32(0x2044)==pixels+16);assert(!method(tx,20,2,0u));W32(0x2050,0xffffffff);assert(method(tx,19,5,0u,0x2040u,0x2050u,0u)==0x8876086c);
 assert(!method(tx,18,3,0u,0x2060u));uint32_t ts=R32(0x2060);assert(!method(ts,13,4,0x2040u,0u,0u));assert(method(tx,19,5,0u,0x2040u,0u,0u)==0x8876086c);assert(!method(ts,14,1));assert(method(ts,2,1)==1);
 assert(!method(d,26,7,72u,0u,0x142u,1u,0x2070u,0u));uint32_t uvb=R32(0x2070);assert(!method(uvb,11,5,0u,0u,0x2020u,0u));uint32_t uvp=R32(0x2020);for(unsigned i=0;i<3;i++){memcpy(HP(uvp+i*24),HP(vp+i*16),16);WF32(uvp+i*24+16,.5f);WF32(uvp+i*24+20,.5f);W32(uvp+i*24+12,0x7f123456);}assert(!method(uvb,12,1));
 assert(!method(d,65,3,0u,tx));assert(!method(d,89,2,0x142u));assert(!method(d,100,5,0u,uvb,0u,24u));assert(!method(d,69,4,0u,1u,3u));assert(!method(d,69,4,0u,2u,3u));assert(!method(d,67,4,0u,1u,2u));assert(!method(d,41,1));assert(!method(d,82,7,4u,0u,0u,3u,0u,1u));assert(drawargs[20]==1&&drawargs[21]==5&&drawargs[22]==100);assert(drawargs[12]&&drawargs[16]==2&&drawargs[17]==2&&drawargs[18]==1&&drawargs[19]==0xbf800000u);assert(uploaded_bytes==72&&uploaded[12]==0x56&&uploaded[13]==0x34&&uploaded[14]==0x12&&uploaded[15]==0x7f&&R32(uvp+12)==0x7f123456);assert(drawargs[31]==4&&drawargs[33]==2&&drawargs[34]==2);assert(!method(d,42,1));
 /* Original 104 has no diffuse: uploaded unchanged (24B, XYZRHW_UV layout), the
  * shader uses opaque white. Pretransformed input bypasses lighting. */
 assert(!method(d,26,7,120u,0x208u,0x104u,0u,0x2030u,0u));uint32_t rhwvb=R32(0x2030);assert(!method(rhwvb,13,2,0x4000u)&&R32(0x4014)==0x104);
 assert(!method(rhwvb,11,5,0u,0u,0x2020u,0x2800u));uint32_t rhwdata=R32(0x2020);memset(HP(rhwdata),255,120);
 for(unsigned i=2;i<5;i++){uint32_t p=rhwdata+i*24;WF32(p,(float)i*10);WF32(p+4,30);WF32(p+8,.5f);WF32(p+12,.25f);WF32(p+16,.125f);WF32(p+20,.875f);}assert(!method(rhwvb,12,1));
 assert(!method(d,89,2,0x104u)&&!method(d,90,2,0x4100u)&&R32(0x4100)==0x104);assert(!method(d,100,5,0u,rhwvb,24u,24u));assert(!method(d,57,3,137u,1u));assert(!method(d,41,1));
 assert(!method(d,82,7,4u,1u,0u,3u,0u,1u));assert(last_vertex_fvf==0x104&&last_vertex_bytes==72); /* uploaded as stored; the GPU supplies opaque white */
 assert(!memcmp(last_vertices,HP(rhwdata+2*24),72));
 assert(R32(rhwdata)==0xffffffff);unsigned rhwuploads=calls[MOVIES_D3D9_UPLOAD_VERTEX];assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==rhwuploads);
 /* Any stage setup draws; the raw stage values reach the shader combiner. */
 assert(!method(d,67,4,0u,1u,3u));assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&drawargs[31]==4&&drawargs[33]==3);
 assert(!method(d,67,4,0u,3u,3u));assert(!method(d,67,4,0u,4u,4u));assert(!method(d,67,4,0u,5u,3u));assert(!method(d,67,4,0u,6u,2u));assert(!method(d,57,3,60u,0x80402010u));assert(!method(d,58,3,60u,0x4100u)&&R32(0x4100)==0x80402010);
 assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&drawargs[31]==4&&drawargs[33]==3&&drawargs[35]==3&&drawargs[36]==4&&drawargs[37]==3&&drawargs[38]==2&&drawargs[32]==0x80402010);unsigned factoruploads=calls[MOVIES_D3D9_UPLOAD_VERTEX];assert(!method(d,57,3,60u,0x10203040u));assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&drawargs[32]==0x10203040&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==factoruploads);
 assert(!method(d,67,4,0u,1u,2u));assert(!method(d,67,4,0u,3u,1u));assert(!method(d,67,4,0u,4u,2u));assert(!method(d,67,4,0u,5u,2u));assert(!method(d,67,4,0u,6u,1u));assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&drawargs[31]==4&&drawargs[33]==2&&drawargs[39]==1);
 assert(!method(d,100,5,0u,rhwvb,24u,16u));assert(method(d,82,7,4u,1u,0u,3u,0u,1u)==0x8876086c);assert(!method(d,42,1));assert(!method(d,100,5,0u,uvb,0u,24u));assert(!method(rhwvb,2,1));
 /* Original9976BE/9976E3 alternate sprite branch binds144/28B, then
  * shares997ABB with142. Nonzero offset/base tests the referenced snapshot. */
 assert(!method(d,26,7,140u,0x208u,0x144u,0u,0x2030u,0u));uint32_t boundrhw=R32(0x2030);
 assert(!method(boundrhw,11,5,0u,0u,0x2020u,0x2800u));uint32_t bounddata=R32(0x2020);memset(HP(bounddata),255,140);
 for(unsigned i=2;i<5;i++){uint32_t p=bounddata+i*28;WF32(p,20.f+i);WF32(p+4,30);WF32(p+8,.5f);WF32(p+12,.25f);W32(p+16,0x7f123456);WF32(p+20,.125f);WF32(p+24,.875f);}assert(!method(boundrhw,12,1));
 assert(!method(d,89,2,0x144u)&&!method(d,100,5,0u,boundrhw,28u,28u));assert(!method(d,41,1));
 assert(!method(d,82,7,4u,1u,0u,3u,0u,1u));assert(last_vertex_fvf==0x144&&last_vertex_bytes==84);
 for(unsigned i=0;i<3;i++){assert(!memcmp(last_vertices+i*28,HP(bounddata+(i+2)*28),16));uint32_t color;memcpy(&color,last_vertices+i*28+16,4);assert(color==0x7f123456);assert(!memcmp(last_vertices+i*28+20,HP(bounddata+(i+2)*28+20),8));}assert(R32(bounddata+2*28+16)==0x7f123456);
 unsigned bounduploads=calls[MOVIES_D3D9_UPLOAD_VERTEX];assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==bounduploads);
 /* XYZ|NORMAL|TEX1 (32B) is lit on the CPU into an XYZ|DIFFUSE|TEX1 snapshot:
  * emissive + matAmbient*(globalAmbient+lightAmbient) + matDiffuse*lightDiffuse*max(N.L,0). */
 {uint32_t saved[7];for(unsigned k=1;k<7;k++){assert(!method(d,66,4,0u,k,0x4200u));saved[k]=R32(0x4200);}
  assert(!method(d,67,4,0u,1u,4u)&&!method(d,67,4,0u,2u,2u)&&!method(d,67,4,0u,3u,0u)&&!method(d,67,4,0u,4u,4u)&&!method(d,67,4,0u,5u,2u)&&!method(d,67,4,0u,6u,0u));
  assert(!method(d,26,7,160u,0x208u,0x112u,0u,0x2030u,0u));uint32_t litvb=R32(0x2030);assert(!method(litvb,11,5,0u,0u,0x2020u,0x2800u));uint32_t litdata=R32(0x2020);memset(HP(litdata),0,160);
  for(unsigned i=1;i<4;i++){uint32_t p=litdata+i*32;WF32(p,(float)i);WF32(p+4,2);WF32(p+8,.5f);WF32(p+20,i==2?1.f:-1.f);WF32(p+24,.25f*i);WF32(p+28,.75f);}assert(!method(litvb,12,1));
  const float material[17]={1,.5f,.25f,.75f, 1,1,1,1, 0,0,0,0, 0,0,0,0, 0};memcpy(HP(0x4300),material,68);assert(!method(d,49,2,0x4300u));
  float light[26]={0};light[1]=light[2]=light[3]=light[4]=1;light[18]=1;memcpy(HP(0x4400),light,104);W32(0x4400,3);assert(!method(d,51,3,0u,0x4400u));assert(!method(d,53,3,0u,1u));
  assert(!method(d,57,3,137u,1u)&&!method(d,57,3,139u,0x00202020u));
  assert(!method(d,89,2,0x112u)&&!method(d,100,5,0u,litvb,0u,32u));
  unsigned lituploads=calls[MOVIES_D3D9_UPLOAD_VERTEX];assert(!method(d,82,7,4u,1u,0u,3u,0u,1u));assert(last_vertex_fvf==0x242&&last_vertex_bytes==96&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==lituploads+1);
  for(unsigned i=0;i<3;i++){assert(!memcmp(last_vertices+i*32,HP(litdata+(i+1)*32),12));assert(!memcmp(last_vertices+i*32+16,HP(litdata+(i+1)*32+24),8));
   uint32_t color;memcpy(&color,last_vertices+i*32+12,4);unsigned a=color>>24,r=(color>>16)&255,g=(color>>8)&255,b=color&255;assert(a==191);
   if(i==1){assert(r==32&&g==32&&b==32);}else{assert(r==255&&g>=159&&g<=161&&b>=95&&b<=97);}}
  assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==lituploads+1);
  assert(!method(d,53,3,0u,0u));assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==lituploads+2);
  {uint32_t color;memcpy(&color,last_vertices+12,4);assert((color&0xffff)==0x2020);}
  /* GPU lighting: raw XYZ|NORMAL|DIFFUSE|TEX2 (44B) plus a per-draw lighting block;
   * the upload is independent of lights, so changing a light re-uses it. */
  {assert(!method(d,53,3,0u,1u));movies_d3d9_force_cpu_lighting(0);unsigned gu=calls[MOVIES_D3D9_UPLOAD_VERTEX];
   assert(!method(d,82,7,4u,1u,0u,3u,0u,1u));assert(last_vertex_fvf==0xffff&&last_vertex_bytes==96&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==gu+1&&last_lit==1);
   /* Raw: the guest's XYZ|NORMAL|TEX1 bytes as stored, described as stride 32, XYZ, normal@12, no diffuse, uv@24 (+1 encoded). */
   assert(last_vertex_args[1]==32&&last_vertex_args[2]==3&&last_vertex_args[3]==13&&last_vertex_args[4]==0&&last_vertex_args[5]==25&&last_vertex_args[6]==25);
   assert(!memcmp(last_vertices,HP(litdata+32),96));
   assert(last_lighting[11]>=1&&last_lighting[35]==3&&last_lighting[46]==-1&&last_lighting[32]==1); /* one directional light, direction negated */
   assert(last_lighting[12]>.12f&&last_lighting[12]<.13f&&last_lighting[16]==1&&last_lighting[17]==.5f&&last_lighting[19]==.75f&&last_lighting[20]==1);
   assert(last_lighting[0]==1&&last_lighting[5]==1&&last_lighting[10]==1&&!last_lighting[28]); /* identity normal matrix, material diffuse source */
   float dim[26]={0};dim[1]=dim[2]=dim[3]=.5f;dim[4]=1;dim[18]=1;memcpy(HP(0x4400),dim,104);W32(0x4400,3);assert(!method(d,51,3,0u,0x4400u));
   assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&calls[MOVIES_D3D9_UPLOAD_VERTEX]==gu+1); /* cached vertices */
   {int dimmed=0;for(unsigned l=0;l<(unsigned)last_lighting[11];l++)if(last_lighting[32+l*20]==.5f&&last_lighting[35+l*20]==3)dimmed=1;assert(dimmed);} /* new light reaches the draw */
   movies_d3d9_force_cpu_lighting(1);assert(!method(d,53,3,0u,0u));}
  assert(!method(d,57,3,137u,0u));assert(!method(d,82,7,4u,1u,0u,3u,0u,1u));assert(last_vertex_fvf==0xffff&&!last_vertex_args[4]&&!last_lit); /* unlit raw, no diffuse: the GPU uses opaque white */
  assert(!method(d,57,3,22u,3u));assert(!method(d,82,7,4u,1u,0u,3u,0u,1u));assert(!method(d,57,3,22u,1u));
  /* General FVF 0x212 (XYZ|NORMAL|TEX2, 40B) through DrawPrimitive as a 2-triangle strip:
   * converted to 24B XYZ|DIFFUSE|TEX1 (stage-0 coordinate set), strip rebuilt as a list. */
  {assert(!method(d,26,7,200u,0x208u,0x212u,0u,0x2030u,0u));uint32_t gvb=R32(0x2030);assert(!method(gvb,11,5,0u,0u,0x2020u,0x2800u));uint32_t gdata=R32(0x2020);memset(HP(gdata),0,200);
   for(unsigned i=0;i<5;i++){uint32_t p=gdata+i*40;WF32(p,(float)i);WF32(p+4,1);WF32(p+8,.5f);WF32(p+20,-1);WF32(p+24,.1f*i);WF32(p+28,.2f*i);WF32(p+32,9);WF32(p+36,9);}assert(!method(gvb,12,1));
   assert(!method(d,89,2,0x212u)&&!method(d,100,5,0u,gvb,0u,40u));
   unsigned draws=calls[MOVIES_D3D9_DRAW];assert(!method(d,81,4,5u,1u,2u));
   assert(calls[MOVIES_D3D9_DRAW]==draws+1&&drawargs[3]==6&&last_vertex_fvf==0xffff&&last_vertex_bytes==160&&last_index_bytes==24);
   assert(last_vertex_args[1]==40&&last_vertex_args[3]==13&&last_vertex_args[5]==25&&last_vertex_args[6]==33); /* raw: stage 0 set 0 @24, stage 1 set 1 @32 */
   float u;memcpy(&u,last_vertices+24,4);assert(u>.09f&&u<.11f); /* vertex 1, coordinate set 0, as stored */
   assert(!method(d,81,4,2u,0u,1u)&&calls[MOVIES_D3D9_DRAW]==draws+1); /* line list skipped, not drawn */
   /* D3DTS_TEXTURE0 with a u translation in row 3 and COUNT2: (u,v,1,0) * M. */
   {float m[16]={1,0,0,0, 0,1,0,0, .5f,0,1,0, 0,0,0,1};memcpy(HP(0x4600),m,64);assert(!method(d,44,3,16u,0x4600u));assert(!method(d,45,3,16u,0x4640u)&&!memcmp(HP(0x4600),HP(0x4640),64));
    assert(!method(d,67,4,0u,24u,2u));assert(!method(d,81,4,5u,1u,2u));float tu;memcpy(&tu,last_vertices+16,4);assert(tu>.59f&&tu<.61f);
    assert(!method(d,67,4,0u,24u,0u));}
   /* DrawPrimitiveUP fan of 3 triangles from caller memory; stream 0 is unbound afterwards. */
   assert(!method(d,83,5,6u,3u,gdata,40u));assert(calls[MOVIES_D3D9_DRAW]==draws+3&&drawargs[3]==9&&last_vertex_bytes==200);
   assert(!method(d,101,5,0u,0x4100u,0x4104u,0x4108u)&&!R32(0x4100));
   /* Stage 1 (lightmap pattern): MODULATE2X(TEXTURE,CURRENT) with coordinate set 1
    * reaches the draw as texture1 and the second UV of the 32B two-set layout. */
   {assert(!method(d,64,3,0u,0x4700u));uint32_t t0=R32(0x4700);assert(t0);if(t0){assert(!method(d,100,5,0u,gvb,0u,40u));assert(!method(d,65,3,1u,t0));
     assert(!method(d,67,4,1u,1u,5u)&&!method(d,67,4,1u,2u,2u)&&!method(d,67,4,1u,3u,1u)&&!method(d,67,4,1u,11u,1u));
     assert(!method(d,81,4,5u,1u,2u)&&drawargs[45]&&drawargs[39]==5);assert(last_vertex_args[6]==33); /* stage 1 reads coordinate set 1 (where 9.0 is stored) */
     assert(!method(d,67,4,1u,1u,1u));assert(!method(d,65,3,1u,0u));assert(method(t0,2,1)>=1);assert(!method(d,100,5,0u,0u,0u,0u));}}
   /* Upload cache: alternating ranges of one buffer upload once each, then hit. */
   {assert(!method(d,100,5,0u,gvb,0u,40u));unsigned vu=calls[MOVIES_D3D9_UPLOAD_VERTEX];
    assert(!method(d,81,4,4u,0u,1u)&&!method(d,81,4,4u,2u,1u));assert(calls[MOVIES_D3D9_UPLOAD_VERTEX]==vu+2);
    assert(!method(d,81,4,4u,0u,1u)&&!method(d,81,4,4u,2u,1u));assert(calls[MOVIES_D3D9_UPLOAD_VERTEX]==vu+2);
    assert(!method(gvb,11,5,0u,0u,0x2020u,0u)&&!method(gvb,12,1));assert(!method(d,81,4,4u,0u,1u));assert(calls[MOVIES_D3D9_UPLOAD_VERTEX]==vu+3); /* new version re-uploads */
    assert(!method(d,100,5,0u,0u,0u,0u));}
   assert(!method(d,89,2,0x144u)&&!method(gvb,2,1));
   if(!setjmp(trap)){method(d,89,2,0x1006u);assert(0);}} /* XYZB blend weights stay unsupported */
  assert(!method(d,57,3,137u,1u));for(unsigned k=1;k<7;k++)assert(!method(d,67,4,0u,k,saved[k]));
  assert(!method(d,89,2,0x144u)&&!method(d,100,5,0u,boundrhw,28u,28u));assert(!method(litvb,2,1));}
 assert(!method(d,65,3,0u,0u));assert(!method(d,67,4,0u,1u,2u)&&!method(d,67,4,0u,2u,0u)&&!method(d,67,4,0u,4u,2u)&&!method(d,67,4,0u,5u,0u));
 assert(!method(d,57,3,27u,1u)&&!method(d,57,3,19u,3u)&&!method(d,57,3,20u,4u));
 assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&drawargs[25]==2&&drawargs[31]==4&&!drawargs[12]);/* cached upload reused: no colour mask in the combiner path */
 for(unsigned src=1;src<=11;src++)for(unsigned dst=1;dst<=11;dst++){assert(!method(d,57,3,19u,src)&&!method(d,57,3,20u,dst));assert(!method(d,82,7,4u,1u,0u,3u,0u,1u));uint32_t encoded=src==5&&dst==6?1:src==3&&dst==4?2:src==1&&dst==3?3:256|(src<<4)|dst;assert(drawargs[25]==encoded&&drawargs[31]==4);}
 assert(!method(d,57,3,19u,3u)&&!method(d,57,3,20u,4u));
 assert(!method(d,67,4,0u,5u,2u));unsigned fallbackDraws=calls[MOVIES_D3D9_DRAW];assert(!method(d,82,7,4u,1u,0u,3u,0u,1u));assert(calls[MOVIES_D3D9_DRAW]==fallbackDraws+1&&drawargs[31]==4&&!drawargs[12]); /*NULL-texture alpha arg falls back to DIFFUSE (D3DTSS docs)*/
 assert(!method(d,65,3,0u,tx)&&!method(d,67,4,0u,2u,2u)&&!method(d,57,3,27u,0u)&&!method(d,57,3,19u,2u)&&!method(d,57,3,20u,1u));
 assert(!method(d,67,4,0u,2u,0u)&&!method(d,57,3,27u,1u)&&!method(d,57,3,19u,1u)&&!method(d,57,3,20u,3u));
 assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&drawargs[25]==3&&drawargs[31]==4&&drawargs[12]);/* cached upload reused: no colour mask in the combiner path */
 assert(!method(d,67,4,0u,5u,0u));assert(!method(d,82,7,4u,1u,0u,3u,0u,1u)&&drawargs[37]==0);assert(!method(d,67,4,0u,5u,2u)&&!method(d,67,4,0u,2u,2u)&&!method(d,57,3,27u,0u)&&!method(d,57,3,19u,2u)&&!method(d,57,3,20u,1u));
 assert(!method(d,100,5,0u,boundrhw,28u,24u));assert(method(d,82,7,4u,1u,0u,3u,0u,1u)==0x8876086c);
 assert(!method(d,42,1)&&!method(d,100,5,0u,uvb,0u,24u)&&!method(boundrhw,2,1));
 /* UP copies only [minimum, minimum+vertices), rebases indices, releases
  * temporary uploads, clears both bound buffers even on invalid returned calls. */
 assert(!method(d,89,2,0x144u));assert(!method(d,41,1));
 for(unsigned i=0;i<4;i++){uint32_t p=0x7000+i*28;WF32(p,20.f+i);WF32(p+4,30.f);WF32(p+8,.5f);WF32(p+12,.5f);W32(p+16,0x7f123456);WF32(p+20,.25f);WF32(p+24,.75f);}
 W16(0x7100,1);W16(0x7102,2);W16(0x7104,3);
 assert(!method(d,57,3,23u,8u)&&!method(d,58,3,23u,0x4100u)&&R32(0x4100)==8);assert(!method(d,57,3,14u,0u)&&!method(d,58,3,14u,0x4100u)&&!R32(0x4100));
 unsigned before_draw=calls[MOVIES_D3D9_DRAW],before_release=calls[MOVIES_D3D9_RELEASE_RESOURCE];
 assert(!method(d,57,3,174u,1u));assert(!method(d,58,3,174u,0x4100u)&&R32(0x4100)==1);assert(!method(d,57,3,27u,1u)&&!method(d,57,3,19u,12u));assert(!method(d,84,9,4u,1u,3u,1u,0x7100u,101u,0x7000u,28u)&&drawargs[25]==1);before_draw=calls[MOVIES_D3D9_DRAW];before_release=calls[MOVIES_D3D9_RELEASE_RESOURCE]; /* BOTHSRCALPHA draws as SRCALPHA/INVSRCALPHA */
 assert(!method(d,57,3,19u,5u)&&!method(d,57,3,20u,6u));assert(!method(d,58,3,19u,0x4100u)&&R32(0x4100)==5);assert(!method(d,58,3,20u,0x4100u)&&R32(0x4100)==6);
 assert(!method(d,67,4,0u,1u,4u)&&!method(d,67,4,0u,2u,2u)&&!method(d,67,4,0u,3u,0u));
 assert(!method(d,67,4,0u,4u,4u)&&!method(d,67,4,0u,5u,0u)&&!method(d,67,4,0u,6u,2u));
 assert(!method(d,57,3,137u,1u)); /* transformed/lit data bypasses lighting */
 assert(!method(d,84,9,4u,1u,3u,1u,0x7100u,101u,0x7000u,28u));
 assert(calls[MOVIES_D3D9_DRAW]==before_draw+1&&calls[MOVIES_D3D9_RELEASE_RESOURCE]==before_release+2&&drawargs[3]==3&&drawargs[4]==0&&drawargs[23]==8&&drawargs[24]==0&&drawargs[25]==1&&drawargs[26]==1&&drawargs[27]==10&&drawargs[28]==20&&drawargs[29]==100&&drawargs[30]==200);
 assert(uploaded_bytes==6&&uploaded[0]==0&&uploaded[2]==1&&uploaded[4]==2);assert(R32(0x7000+28+16)==0x7f123456);
 /* NULL texture: colour DIFFUSE + alpha TEXTURE falls back to diffuse alpha (D3DTSS docs). */
 assert(!method(d,64,3,0u,0x4120u));uint32_t saved_tex=R32(0x4120);assert(saved_tex);assert(!method(d,65,3,0u,0u));
 assert(!method(d,67,4,0u,1u,2u)&&!method(d,67,4,0u,2u,0u)&&!method(d,67,4,0u,4u,2u)&&!method(d,67,4,0u,5u,2u));
 assert(!method(d,84,9,4u,1u,3u,1u,0x7100u,101u,0x7000u,28u)&&drawargs[31]==4&&!drawargs[12]);
 assert(!method(d,67,4,0u,1u,4u)&&!method(d,67,4,0u,2u,2u)); /*colour MODULATE(TEXTURE,DIFFUSE): cascade stops -> diffuse*/
 assert(!method(d,84,9,4u,1u,3u,1u,0x7100u,101u,0x7000u,28u)&&drawargs[31]==4&&!drawargs[12]);
 assert(!method(d,67,4,0u,1u,3u)&&!method(d,67,4,0u,3u,3u)&&!method(d,67,4,0u,4u,4u)&&!method(d,67,4,0u,5u,2u)&&!method(d,67,4,0u,6u,3u));
 {unsigned n=calls[MOVIES_D3D9_DRAW];assert(!method(d,84,9,4u,1u,3u,1u,0x7100u,101u,0x7000u,28u));assert(calls[MOVIES_D3D9_DRAW]==n+1&&drawargs[36]==4&&drawargs[38]==3);} /*TFACTOR*diffuse alpha: shader combiner*/
 assert(!method(d,67,4,0u,1u,4u)&&!method(d,67,4,0u,2u,2u)&&!method(d,67,4,0u,3u,0u)&&!method(d,67,4,0u,4u,4u)&&!method(d,67,4,0u,5u,0u)&&!method(d,67,4,0u,6u,2u));
 assert(!method(d,65,3,0u,saved_tex));assert(method(saved_tex,2,1)>=1);
 assert(!method(d,101,5,0u,0x4100u,0x4104u,0x4108u)&&!R32(0x4100)&&!R32(0x4104)&&!R32(0x4108));assert(!method(d,105,2,0x4100u)&&!R32(0x4100));
 before_draw=calls[MOVIES_D3D9_DRAW];
 W16(0x7100,0);assert(method(d,84,9,4u,1u,3u,1u,0x7100u,101u,0x7000u,28u)==0x8876086c);W16(0x7100,1);
 assert(method(d,84,9,4u,1u,3u,1u,0x054fffffu,101u,0x7000u,28u)==0x8876086c);
 assert(method(d,84,9,4u,1u,3u,1u,0x7100u,101u,0x054ffff0u,28u)==0x8876086c);
 assert(method(d,84,9,4u,1u,3u,1u,0x7100u,101u,0x7000u,24u)==0x8876086c);
 assert(method(d,84,9,4u,1u,3u,0xffffffffu,0x7100u,101u,0x7000u,28u)==0x8876086c);
 assert(method(d,84,9,4u,0xffffffffu,3u,1u,0x7100u,101u,0x7000u,28u)==0x8876086c);
 WF32(0x7000+28+12,0);assert(method(d,84,9,4u,1u,3u,1u,0x7100u,101u,0x7000u,28u)==0x8876086c);WF32(0x7000+28+12,.5f);
 assert(calls[MOVIES_D3D9_DRAW]==before_draw);assert(!method(d,42,1));assert(!method(d,57,3,27u,0u));assert(!method(d,57,3,174u,0u));assert(!method(d,57,3,23u,4u)&&!method(d,57,3,14u,1u));
 assert(!method(d,57,3,171u,2u)&&!method(d,57,3,171u,1u));assert(method(d,57,3,19u,0u)==0x8876086c);assert(!method(d,57,3,23u,2u)&&!method(d,57,3,23u,4u));assert(!method(d,57,3,14u,1u));
 assert(!method(d,100,5,0u,uvb,0u,24u));assert(!method(d,104,2,ib));
 if(!setjmp(trap)){method(d,69,4,0u,5u,3u);assert(0);} /* anisotropic filtering not advertised */
 assert(method(tx,2,1)==1);assert(method(uvb,2,1)==1);

 /* Blend storage accepts classicD3D9 enums without claiming a new renderer. */
 for(unsigned state=19;state<=20;state++)for(unsigned value=1;value<=15;value++){
  if(state==20&&(value==12||value==13)){assert(method(d,57,3,state,value)==0x8876086c);continue;}
  assert(!method(d,57,3,state,value));assert(!method(d,58,3,state,0x4100u)&&R32(0x4100)==value);
 }
 const uint32_t invalidFactors[]={0,16,17,18,0x7fffffff,0xffffffff};
 for(unsigned state=19;state<=20;state++)for(unsigned i=0;i<sizeof invalidFactors/sizeof invalidFactors[0];i++){assert(method(d,57,3,state,invalidFactors[i])==0x8876086c);assert(!method(d,58,3,state,0x4100u)&&R32(0x4100)==15);}
 assert(!method(d,57,3,19u,1u)&&!method(d,57,3,20u,2u)&&!method(d,57,3,27u,1u));assert(!method(d,57,3,19u,12u));unsigned blendRejected=calls[MOVIES_D3D9_DRAW];assert(!method(d,41,1));uint32_t blend_hr=method(d,82,7,4u,0u,0u,3u,0u,1u);assert(!method(d,42,1));assert(!blend_hr||blend_hr==0x8876086c);assert(calls[MOVIES_D3D9_DRAW]<=blendRejected+1);assert(!method(d,57,3,27u,0u));
 /* Render-target textures (observed 99B26D): GPU-only, one level, DEFAULT pool. */
 assert(!method(d,65,3,0u,0u));unsigned targets=calls[MOVIES_D3D9_CREATE_TARGET];
 assert(method(d,23,9,512u,256u,0u,1u,21u,0u,0x2030u,0u)==0x8876086a&&!R32(0x2030)); /*autogen chains not claimed*/
 assert(method(d,23,9,512u,256u,1u,1u,21u,1u,0x2030u,0u)==0x8876086a&&method(d,23,9,512u,256u,1u,0x201u,21u,0u,0x2030u,0u)==0x8876086a);
 assert(calls[MOVIES_D3D9_CREATE_TARGET]==targets);
 assert(!method(d,23,9,512u,256u,1u,1u,21u,0u,0x2030u,0u));uint32_t rtt=R32(0x2030);assert(rtt&&calls[MOVIES_D3D9_CREATE_TARGET]==targets+1);uint32_t rtgpu=next_handle-1;
 assert(method(rtt,19,5,0u,0x2040u,0u,0u)==0x8876086c); /*not lockable*/
 assert(!method(rtt,17,3,0u,0x4000u)&&R32(0x4000)==21&&R32(0x4008)==1&&R32(0x400c)==0&&R32(0x4018)==512&&R32(0x401c)==256);
 assert(!method(rtt,18,3,0u,0x2044u));uint32_t rts=R32(0x2044);assert(rts&&rts!=rt);
 assert(method(d,37,3,1u,rts)==0x8876086c&&method(d,37,3,0u,0u)==0x8876086c&&method(d,37,3,0u,ds)==0x8876086c);
 assert(!method(d,65,3,0u,rtt));unsigned sets=calls[MOVIES_D3D9_SET_TARGET];if(!setjmp(trap)){method(d,37,3,0u,rts);assert(0);}assert(calls[MOVIES_D3D9_SET_TARGET]==sets);assert(!method(d,65,3,0u,0u));
 assert(!method(d,37,3,0u,rts));assert(calls[MOVIES_D3D9_SET_TARGET]==sets+1&&last_target[0]==rtgpu&&last_target[1]==512&&last_target[2]==256);
 assert(!method(d,48,2,0x4100u)&&R32(0x4100)==0&&R32(0x4104)==0&&R32(0x4108)==512&&R32(0x410c)==256&&R32(0x4114)==0x3f800000u);
 assert(!method(d,76,2,0x4700u)&&R32(0x4700)==0&&R32(0x4708)==512&&R32(0x470c)==256);
 W32(0x4108,640);W32(0x410c,480);assert(method(d,47,2,0x4100u)==0x8876086c);W32(0x4108,512);W32(0x410c,256);assert(!method(d,47,2,0x4100u));
 const int32_t full[4]={0,0,512,256},over[4]={0,0,640,480};memcpy(HP(0x4700),over,16);assert(method(d,75,2,0x4700u)==0x8876086c);memcpy(HP(0x4700),full,16);assert(!method(d,75,2,0x4700u));
 /* Clear: TARGET alone keeps depth; area is viewport (∩ scissor when enabled). */
 unsigned clears=calls[MOVIES_D3D9_CLEAR];assert(!method(d,43,7,0u,0u,1u,0u,0x3f800000u,0u));assert(calls[MOVIES_D3D9_CLEAR]==clears+1&&clearargs[2]==1&&!clearargs[26]);
 assert(!method(d,43,7,0u,0u,3u,0u,0x3f800000u,0u)&&clearargs[2]==3);assert(!method(d,43,7,0u,0u,2u,0u,0x3f800000u,0u)&&clearargs[2]==2); /* depth-only clear */
 assert(!method(d,43,7,0u,0u,4u,0u,0x3f800000u,7u)); /* stencil-only: no stencil buffer, no-op */
 assert(!method(d,43,7,0u,0u,7u,0u,0x3f800000u,0u)&&clearargs[2]==3); /* stencil bit dropped */
 {int32_t rect[8]={10,20,30,40, 0,0,5,5};memcpy(HP(0x4800),rect,32);unsigned clears=calls[MOVIES_D3D9_CLEAR];assert(!method(d,43,7,2u,0x4800u,1u,0x11223344u,0x3f800000u,0u));
  assert(calls[MOVIES_D3D9_CLEAR]==clears+2&&clearargs[26]==1&&clearargs[27]==0&&clearargs[29]==5);}
 assert(method(d,43,7,0u,0u,8u,0u,0x3f800000u,0u)==0x8876086c&&method(d,43,7,1u,0u,1u,0u,0x3f800000u,0u)==0x8876086c);
 W32(0x4100,10);W32(0x4104,20);W32(0x4108,100);W32(0x410c,50);assert(!method(d,47,2,0x4100u));
 assert(!method(d,43,7,0u,0u,1u,0u,0x3f800000u,0u)&&clearargs[26]==1&&clearargs[27]==10&&clearargs[28]==20&&clearargs[29]==110&&clearargs[30]==70);
 const int32_t cut[4]={50,0,512,30};memcpy(HP(0x4700),cut,16);assert(!method(d,75,2,0x4700u)&&!method(d,57,3,174u,1u));
 assert(!method(d,43,7,0u,0u,1u,0u,0x3f800000u,0u)&&clearargs[27]==50&&clearargs[28]==20&&clearargs[29]==110&&clearargs[30]==30);
 const int32_t miss[4]={200,100,300,200};memcpy(HP(0x4700),miss,16);assert(!method(d,75,2,0x4700u));clears=calls[MOVIES_D3D9_CLEAR];
 assert(!method(d,43,7,0u,0u,1u,0u,0x3f800000u,0u)&&calls[MOVIES_D3D9_CLEAR]==clears); /*empty area: no backend clear*/
 assert(!method(d,57,3,174u,0u));memcpy(HP(0x4700),full,16);assert(!method(d,75,2,0x4700u));W32(0x4100,0);W32(0x4104,0);W32(0x4108,512);W32(0x410c,256);assert(!method(d,47,2,0x4100u));
 /* GetRenderTargetData -> SYSTEMMEM edit -> UpdateSurface (observed 9A52C0/9A5310). */
 assert(!method(d,23,9,512u,256u,1u,0x200u,21u,2u,0x2034u,0u));uint32_t memt=R32(0x2034);assert(!method(memt,18,3,0u,0x2038u));uint32_t mems=R32(0x2038);
 assert(!method(d,32,3,rts,mems));assert(calls[MOVIES_D3D9_READ_TARGET]==1);
 assert(!method(mems,13,4,0x2050u,0u,0u));assert(R32(0x2050)==2048);uint32_t bits=R32(0x2054);assert(R8(bits)==0x11&&R8(bits+3)==0x44);W8(bits,0x01);W8(bits+1,0x02);W8(bits+2,0x03);W8(bits+3,0x04);
 assert(method(d,30,5,mems,0u,rts,0u)==0x8876086c); /*locked source rejected*/
 assert(!method(mems,14,1));assert(!method(d,30,5,mems,0u,rts,0u));assert(calls[MOVIES_D3D9_WRITE_TARGET]==1&&uploaded_bytes==512u*256u*4u&&uploaded[0]==0x03&&uploaded[1]==0x02&&uploaded[2]==0x01&&uploaded[3]==0x04);
 if(!setjmp(trap)){method(d,32,3,mems,mems);assert(0);} /*non-RT source unclaimed*/
 if(!setjmp(trap)){method(d,30,5,mems,0x4800u,rts,0u);assert(0);} /*sub-rect unclaimed*/
 /* D3DX fallback (B0E5F1): lockable CreateRenderTarget, StretchRect, LockRect read. */
 assert(method(d,28,9,512u,256u,21u,1u,0u,1u,0x2060u,0u)==0x8876086a&&!R32(0x2060)); /*multisampling not claimed*/
 assert(method(d,28,9,512u,256u,21u,0u,0u,1u,0x2060u,0x4000u)==0x8876086c);
 unsigned writes=calls[MOVIES_D3D9_WRITE_TARGET],reads=calls[MOVIES_D3D9_READ_TARGET];
 assert(!method(d,28,9,512u,256u,21u,0u,0u,1u,0x2060u,0u));uint32_t lrt=R32(0x2060);assert(lrt);uint32_t lgpu=next_handle-1;
 assert(!method(lrt,12,2,0x4000u)&&R32(0x4000)==21&&R32(0x4008)==1&&R32(0x400c)==0&&R32(0x4018)==512&&R32(0x401c)==256);
 assert(method(d,34,6,rts,0u,lrt,0u,0u)==0&&calls[MOVIES_D3D9_COPY_TARGET]==1);
 assert(method(d,34,6,rts,0u,rts,0u,0u)==0x8876086c);
 if(!setjmp(trap)){method(d,34,6,rts,0x4800u,lrt,0u,2u);assert(0);} /*scaled rects (20x20 -> whole 512x256) unclaimed*/
 /* D3DXLoadSurfaceFromSurface fast path (AFE246): equal-size rectangles, filter NONE. */
 {int32_t r[4]={100,50,120,70};memcpy(HP(0x4810),r,16);assert(!method(d,34,6,rts,0x4800u,lrt,0x4810u,0u));
  assert(calls[MOVIES_D3D9_COPY_TARGET]==2&&last_copy[4]==1&&last_copy[5]==10&&last_copy[6]==20&&last_copy[7]==100&&last_copy[8]==50&&last_copy[9]==20&&last_copy[10]==20);
  r[2]=600;memcpy(HP(0x4810),r,16);assert(method(d,34,6,rts,0x4800u,lrt,0x4810u,0u)==0x8876086c);} /*out of bounds*/
 assert(!method(d,34,6,rts,0u,lrt,0u,0u)&&last_copy[4]==0); /*whole copy carries no region*/
 if(!setjmp(trap)){method(d,34,6,rts,0u,mems,0u,0u);assert(0);} /*SYSTEMMEM destination unclaimed*/
 assert(!method(lrt,13,4,0x2064u,0u,0x10u));assert(calls[MOVIES_D3D9_READ_TARGET]==reads+1&&R32(0x2064)==2048);uint32_t lbits=R32(0x2068);assert(R8(lbits)==0x11&&R8(lbits+3)==0x44);
 assert(method(lrt,13,4,0x2064u,0u,0u)==0x8876086c); /*already locked*/
 assert(!method(lrt,14,1));assert(calls[MOVIES_D3D9_WRITE_TARGET]==writes); /*READONLY unlock: no upload*/
 assert(method(lrt,14,1)==0x8876086c);
 assert(!method(lrt,13,4,0x2064u,0u,0u));W8(R32(0x2068),0x07);assert(!method(lrt,14,1));assert(calls[MOVIES_D3D9_WRITE_TARGET]==writes+1&&uploaded[2]==0x07);
 unsigned freed=calls[MOVIES_D3D9_RELEASE_RESOURCE];assert(!method(lrt,2,1));assert(calls[MOVIES_D3D9_RELEASE_RESOURCE]==freed+1);(void)lgpu;
 /* D3DX lock helper B0E42C locks the implicit backbuffer (caller B0E839): readback in X8R8G8B8. */
 assert(!method(d,18,5,0u,0u,0u,0x2070u));uint32_t bb=R32(0x2070);assert(bb);
 assert(!method(bb,13,4,0x2074u,0u,0x10u));assert(calls[MOVIES_D3D9_READ_BACKBUFFER]==1&&R32(0x2074)==2560);uint32_t bbits=R32(0x2078);assert(R8(bbits)==0x11&&R8(bbits+2)==0x33&&R8(bbits+3)==0x44);
 assert(method(bb,13,4,0x2074u,0u,0x10u)==0x8876086c);assert(!method(bb,14,1));assert(method(bb,14,1)==0x8876086c);
 W32(0x4800,8);W32(0x4804,2);W32(0x4808,16);W32(0x480c,4);assert(!method(bb,13,4,0x2074u,0x4800u,0u));assert(R32(0x2078)==bbits+2*2560+8*4);assert(!method(bb,14,1));
 (void)method(bb,2,1);
 assert(!method(d,40,2,0x2070u));uint32_t dss=R32(0x2070);assert(method(dss,13,4,0x2074u,0u,0x10u)==0x8876086c);(void)method(dss,2,1); /*D16 not lockable*/
 uint32_t plain;assert(!method(d,28,9,64u,64u,22u,0u,0u,0u,0x2060u,0u));plain=R32(0x2060);assert(method(plain,13,4,0x2064u,0u,0u)==0x8876086c);assert(!method(plain,2,1));
 assert(method(mems,2,1)==1);assert(!method(memt,2,1));
 assert(!method(d,38,3,0u,0x2048u)&&R32(0x2048)==rts);assert(method(rts,2,1)==3); /*caller ref released; app, texture and device keep it*/
 unsigned released=calls[MOVIES_D3D9_RELEASE_RESOURCE];
 assert(method(rts,2,1)==2);assert(method(rtt,2,1)==1);assert(calls[MOVIES_D3D9_RELEASE_RESOURCE]==released); /*bound target outlives app refs*/
 assert(!method(d,37,3,0u,rt));assert(calls[MOVIES_D3D9_RELEASE_RESOURCE]==released+1&&last_target[0]==0&&last_target[1]==640&&last_target[2]==480);
 assert(!method(d,48,2,0x4100u)&&R32(0x4108)==640&&R32(0x410c)==480);assert(!method(d,38,3,0u,0x2048u)&&R32(0x2048)==rt);assert(method(rt,2,1)==2); /*test still holds its earlier backbuffer ref*/
 ready=0;assert(method(d,3,1)==0x88760868);ready=1;
 /* Resource references keep the device alive; bound references do not create cycles. */
 assert(method(vb,2,1)==0);assert(method(ib,2,1)==1);assert(method(rt,2,1)==1);assert(method(ds,2,1)==1);
 assert(method(f,2,1)==1);assert(!method(d,2,1));assert(calls[MOVIES_D3D9_DESTROY]==1);assert(movies_d3d9_init());
 f=factory(32);present_params();W32(0x1008,25);assert(!method(f,16,7,0u,1u,42u,0x24u,0x1000u,0x2000u));d=R32(0x2000);assert(!method(d,38,3,0u,0x2004u));rt=R32(0x2004);assert(!method(rt,12,2,0x4000u)&&R32(0x4000)==25);assert(method(rt,2,1)==1);assert(!method(d,2,1));assert(!method(f,2,1));assert(movies_d3d9_init());
#ifdef __EMSCRIPTEN__
 movies_d3d9_set_host(NULL,NULL);install_js_host();f=factory(32);if(!f)emscripten_run_script("console.error(Module.moviesGraphicsLastError,Module.fixtureCalls)");assert(f);present_params();assert(!method(f,16,7,0u,1u,42u,0x20u,0x1000u,0x2000u));d=R32(0x2000);assert(!method(d,43,7,0u,0u,3u,0xff000000u,0x3f800000u,0u));assert(!method(d,17,5,0u,0u,0u,0u));assert(js_called());assert(!method(d,2,1));assert(!method(f,2,1));
#endif
 free(g_mem);puts("D3D9 contract tests passed (fixture backend, not GPU rendering)");}
