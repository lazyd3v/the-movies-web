/* Synthetic ABI-to-real-GPU contract fixture. Does not execute game code. */
#include "d3d9-api.h"
#include "window-api.h"
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <emscripten.h>
uint8_t *g_mem;
static GuestFn methods[6][119];
void movies_set_last_error(uint32_t value){(void)value;}
void guest_trap(CPU*c,uint32_t address,const char*message){(void)c;fprintf(stderr,"Native D3D9 fixture trap at%08x: %s\n",address,message);abort();}
void register_function(uint32_t address,GuestFn fn){assert(address>=0x0e020000&&address<0x0e026000);methods[(address-0x0e020000)/0x1000][((address-0x0e020000)%0x1000)/16]=fn;}
int movies_window_get(uint32_t hwnd,MoviesWindowEvent*out){if(hwnd!=42)return 0;memset(out,0,sizeof*out);out->hwnd=42;out->client_width=190;out->client_height=128;return 1;}
static uint32_t invoke(GuestFn fn,unsigned n,uint32_t*args){CPU c={0};c.esp=0x10000;W32(c.esp,0x12345678);for(unsigned i=0;i<n;i++)W32(c.esp+4+i*4,args[i]);fn(&c);assert(c.esp==0x10000+4*(n+1));return c.eax;}
static uint32_t call(uint32_t object,unsigned slot,unsigned n,...){uint32_t args[10]={object};va_list ap;va_start(ap,n);for(unsigned i=1;i<n;i++)args[i]=va_arg(ap,uint32_t);va_end(ap);uint32_t address=R32(R32(object)+4*slot);return invoke(methods[(address-0x0e020000)/0x1000][((address-0x0e020000)%0x1000)/16],n,args);}
EM_JS(int,verify_readback,(int mode),{return Module.fixtureReadback(mode)?1:0;});
static void blend_draw(uint32_t device,const uint32_t *colors,unsigned triangles){
 const float xy[3][2]={{20,20},{148,20},{20,116}};
 for(unsigned i=0;i<triangles*3;i++){uint32_t p=0x80000+i*28;WF32(p,xy[i%3][0]);WF32(p+4,xy[i%3][1]);WF32(p+8,.5f);WF32(p+12,.5f);W32(p+16,colors[i/3]);WF32(p+20,.5f);WF32(p+24,.5f);W16(0x82000+i*2,(uint16_t)i);}
 assert(!call(device,41,1));assert(!call(device,84,9,4u,0u,triangles*3,triangles,0x82000u,101u,0x80000u,28u));assert(!call(device,42,1));
}
int main(int argc,char**argv){
 uint32_t target_format=argc>1&&atoi(argv[1])==25?25:22;
 g_mem=calloc(1,0x05900000);assert(g_mem);assert(movies_d3d9_init());
 uint32_t count,version=32;ImportHandler*h=movies_d3d9_handlers(&count);assert(count==1);
 uint32_t factory=invoke(h[0].fn,1,&version);assert(factory);
 assert(!call(factory,8,3,0u,0x2000u));assert(R32(0x2000)==190&&R32(0x2004)==128);
 memset(HP(0x1000),0,56);W32(0x1000,190);W32(0x1004,128);W32(0x1008,target_format);W32(0x100c,1);W32(0x1018,1);W32(0x101c,42);W32(0x1020,1);W32(0x1024,1);W32(0x1028,80);
 assert(!call(factory,16,7,0u,1u,42u,0x24u,0x1000u,0x2000u));uint32_t device=R32(0x2000);
 assert(!call(device,7,2,0x3000u));assert(R32(0x3000+196)==0&&R32(0x3000+204)==0);
 assert(!call(device,38,3,0u,0x2004u));uint32_t surface=R32(0x2004);assert(!call(surface,12,2,0x3000u));assert(R32(0x3018)==190&&R32(0x301c)==128);assert(call(surface,2,1)==1);
 assert(!call(device,26,7,48u,0x208u,0x42u,0u,0x2010u,0u));uint32_t vb=R32(0x2010);
 assert(!call(device,27,7,6u,0x208u,101u,0u,0x2014u,0u));uint32_t ib=R32(0x2014);
 assert(!call(vb,11,5,0u,0u,0x2020u,0x2800u));uint32_t vertices=R32(0x2020);const float positions[3][3]={{-.8f,-.8f,.5f},{.8f,-.8f,.5f},{0,.8f,.5f}};
 for(unsigned i=0;i<3;i++){memcpy(HP(vertices+i*16),positions[i],12);W32(vertices+i*16+12,0xff00ff00);}assert(!call(vb,12,1));
 assert(!call(ib,11,5,0u,0u,0x2020u,0x2800u));uint32_t indices=R32(0x2020);W16(indices,0);W16(indices+2,1);W16(indices+4,2);assert(!call(ib,12,1));
 assert(!call(device,89,2,0x42u));assert(!call(device,92,2,0u)&&!call(device,107,2,0u));W32(0x2100,0xdeadbeef);assert(!call(device,93,2,0x2100u)&&!R32(0x2100));W32(0x2100,0xdeadbeef);assert(!call(device,108,2,0x2100u)&&!R32(0x2100));assert(!call(device,90,2,0x2100u)&&R32(0x2100)==0x42);assert(!call(device,100,5,0u,vb,0u,16u));assert(!call(device,104,2,ib));assert(!call(device,57,3,137u,0u));assert(!call(device,57,3,22u,1u));
 assert(!call(device,43,7,0u,0u,3u,0xff112233u,0x3f800000u,0u));assert(!call(device,41,1));assert(!call(device,82,7,4u,0u,0u,3u,0u,1u));assert(!call(device,42,1));assert(verify_readback(0));assert(!call(device,17,5,0u,0u,0u,0u));
 /* Original video format/lock flags, exercised through real COM and GPU upload. */
 /* More than128 simultaneous real GPU textures must not prevent the later
  * CPU-only RenderTargetMemory resource from obtaining its COM slot. */
 uint32_t assetTextures[130];for(unsigned i=0;i<130;i++){assert(!call(device,23,9,1u,1u,1u,0u,21u,1u,0x2030u,0u));assetTextures[i]=R32(0x2030);}
 assert(!call(device,23,9,512u,512u,1u,0x200u,21u,2u,0x2030u,0u));uint32_t lateMemory=R32(0x2030);
 assert(!call(lateMemory,18,3,0u,0x2060u));assert(call(R32(0x2060),2,1)==1);assert(!call(lateMemory,2,1));
 for(unsigned i=0;i<130;i++)assert(!call(assetTextures[i],2,1));
 /* Real native SYSTEMMEM allocation remains separate from GPU textures. */
 assert(!call(device,23,9,3u,2u,1u,0x200u,21u,2u,0x2030u,0u));uint32_t systemTexture=R32(0x2030);
 assert(!call(systemTexture,18,3,0u,0x2060u));uint32_t systemSurface=R32(0x2060);
 assert(!call(systemSurface,13,4,0x2040u,0u,0u)&&R32(0x2040)==12);W32(R32(0x2044),0x12345678);assert(!call(systemSurface,14,1));
 assert(!call(systemTexture,19,5,0u,0x2040u,0u,0x10u)&&R32(R32(0x2044))==0x12345678);assert(!call(systemTexture,20,2,0u));
 assert(call(device,65,3,0u,systemTexture)==0x8876086c);
 assert(call(systemSurface,2,1)==1);assert(!call(systemTexture,2,1));
 assert(!call(device,23,9,1u,1u,1u,0u,21u,1u,0x2030u,0u));uint32_t texture=R32(0x2030);
 assert(!call(texture,19,5,0u,0x2040u,0u,0x800u));assert(R32(0x2040)==4);W32(R32(0x2044),0xff332211);assert(!call(texture,20,2,0u));
 assert(!call(device,26,7,72u,0x208u,0x142u,0u,0x2050u,0u));uint32_t uvb=R32(0x2050);assert(!call(uvb,11,5,0u,0u,0x2020u,0x1800u));uint32_t uv=R32(0x2020);
 for(unsigned i=0;i<3;i++){memcpy(HP(uv+i*24),positions[i],12);W32(uv+i*24+12,0x7f123456);WF32(uv+i*24+16,.5f);WF32(uv+i*24+20,.5f);}assert(!call(uvb,12,1));
 assert(!call(device,65,3,0u,texture));assert(!call(device,89,2,0x142u));assert(!call(device,100,5,0u,uvb,0u,24u));assert(!call(device,69,4,0u,1u,3u));assert(!call(device,69,4,0u,2u,3u));assert(!call(device,67,4,0u,1u,2u));
 assert(!call(device,43,7,0u,0u,3u,0xff112233u,0x3f800000u,0u));assert(!call(device,41,1));assert(!call(device,82,7,4u,0u,0u,3u,0u,1u));assert(!call(device,42,1));assert(verify_readback(1));
 assert(!call(texture,19,5,0u,0x2040u,0u,0x800u));W32(R32(0x2044),0xffabcdef);assert(!call(texture,20,2,0u));
 assert(!call(device,43,7,0u,0u,3u,0xff112233u,0x3f800000u,0u));assert(!call(device,41,1));assert(!call(device,82,7,4u,0u,0u,3u,0u,1u));assert(!call(device,42,1));assert(verify_readback(2));
 assert(call(texture,2,1)==1);
 assert(!call(device,23,9,2u,1u,1u,0u,21u,1u,0x2030u,0u));texture=R32(0x2030);assert(!call(texture,19,5,0u,0x2040u,0u,0x800u));W32(R32(0x2044),0xffff0000);W32(R32(0x2044)+4,0xff0000ff);assert(!call(texture,20,2,0u));assert(!call(device,65,3,0u,texture));
 for(unsigned mode=3;mode<=7;mode++){
   assert(!call(device,69,4,0u,5u,mode==3?2u:1u));assert(!call(device,69,4,0u,6u,2u));assert(!call(device,69,4,0u,7u,1u));assert(!call(device,69,4,0u,8u,0xbf800000u));
   if(mode>=5){assert(!call(uvb,11,5,0u,0u,0x2020u,0x1800u));for(unsigned i=0;i<3;i++)WF32(R32(0x2020)+i*24+16,1.25f);assert(!call(uvb,12,1));assert(!call(device,69,4,0u,1u,mode-4));}
   assert(!call(device,43,7,0u,0u,3u,0xff112233u,0x3f800000u,0u));assert(!call(device,41,1));assert(!call(device,82,7,4u,0u,0u,3u,0u,1u));assert(!call(device,42,1));assert(verify_readback(mode));
 }
 /* Alpha comparison occurs before logical16-bit output quantization. */
 assert(!call(device,67,4,0u,4u,4u));assert(!call(device,57,3,15u,1u));assert(!call(device,57,3,24u,100u));
 for(unsigned comparison=1;comparison<=8;comparison++){
  assert(!call(device,57,3,25u,comparison));assert(!call(device,43,7,0u,0u,3u,0xff112233u,0x3f800000u,0u));assert(!call(device,41,1));assert(!call(device,82,7,4u,0u,0u,3u,0u,1u));assert(!call(device,42,1));assert(verify_readback(comparison+7));
 }
 /* Screen-space UP: real GPU pixel tests for D3D9 pixel centers, linear
  * diffuse and perspective UV. The transient buffers are gone at readback. */
 assert(call(texture,2,1)==1);
 assert(!call(device,23,9,4u,4u,1u,0u,21u,1u,0x2030u,0u));texture=R32(0x2030);
 assert(!call(texture,19,5,0u,0x2040u,0u,0x800u));uint32_t texels=R32(0x2044);
 for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)W32(texels+y*R32(0x2040)+x*4,0xff000000u|((x*60+20)<<16)|((y*60+20)<<8)|40);
 assert(!call(texture,20,2,0u));assert(!call(device,65,3,0u,texture));assert(!call(device,89,2,0x144u));assert(!call(device,57,3,15u,0u));assert(!call(device,57,3,137u,1u));assert(!call(device,67,4,0u,1u,4u));assert(!call(device,67,4,0u,4u,4u));
 assert(!call(device,69,4,0u,1u,3u));assert(!call(device,69,4,0u,2u,3u));assert(!call(device,69,4,0u,5u,2u));assert(!call(device,69,4,0u,6u,2u));
 const float quad[4][4]={{19.5f,19.5f,0,0},{23.5f,19.5f,1,0},{19.5f,23.5f,0,1},{23.5f,23.5f,1,1}};
 for(unsigned i=0;i<4;i++){uint32_t p=0x7000+i*28;WF32(p,quad[i][0]);WF32(p+4,quad[i][1]);WF32(p+8,.5f);WF32(p+12,.5f);W32(p+16,0xffffffff);WF32(p+20,quad[i][2]);WF32(p+24,quad[i][3]);}
 const uint16_t qi[]={0,1,2,2,1,3};memcpy(HP(0x7100),qi,sizeof qi);
 assert(!call(device,43,7,0u,0u,3u,0xff112233u,0x3f800000u,0u));assert(!call(device,41,1));assert(!call(device,84,9,4u,0u,4u,2u,0x7100u,101u,0x7000u,28u));assert(!call(device,42,1));assert(verify_readback(16));
 assert(!call(device,101,5,0u,0x2100u,0x2104u,0x2108u)&&!R32(0x2100)&&!R32(0x2104)&&!R32(0x2108));assert(!call(device,105,2,0x2100u)&&!R32(0x2100));
 const float triangle[3][5]={{20,20,.5f,0,0},{148,20,1,1,0},{20,116,2,0,1}};const uint32_t colors[]={0xffff0000,0xff00ff00,0xff0000ff};
 for(unsigned i=0;i<3;i++){uint32_t p=0x7000+i*28;WF32(p,triangle[i][0]);WF32(p+4,triangle[i][1]);WF32(p+8,.5f);WF32(p+12,triangle[i][2]);W32(p+16,colors[i]);WF32(p+20,triangle[i][3]);WF32(p+24,triangle[i][4]);}
 for(unsigned mode=17;mode<=19;mode++){
  assert(!call(texture,19,5,0u,0x2040u,0u,0x800u));texels=R32(0x2044);
  for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++){uint32_t r=(2*x+1)*255/8,g=(2*y+1)*255/8;W32(texels+y*R32(0x2040)+x*4,mode==18?(0xff000000u|(r<<16)|(g<<8)):0xffffffff);}
  assert(!call(texture,20,2,0u));
  if(mode>=18)for(unsigned i=0;i<3;i++)W32(0x7000+i*28+16,0xffffffff);
  if(mode==19){W32(0x7200,10);W32(0x7204,10);W32(0x7208,160);W32(0x720c,100);WF32(0x7210,.8f);WF32(0x7214,.9f);assert(!call(device,47,2,0x7200u));}
  assert(!call(device,43,7,0u,0u,3u,0xff112233u,mode==19?0x3f19999au:0x3f800000u,0u));assert(!call(device,41,1));assert(!call(device,84,9,4u,0u,3u,1u,0x7100u,101u,0x7000u,28u));assert(!call(device,42,1));assert(verify_readback(mode));
 }
 /* Compare and write controls are independent: far ALWAYS fragments draw
  * without replacing near depth, and subsequent LESS_EQUAL still sees it. */
 assert(!call(device,43,7,0u,0u,3u,0xff112233u,0x3f800000u,0u));
 const float depths[]={.25f,.75f,.5f,.1f,.5f,.75f,.5f};
 const uint32_t depth_colors[]={0xffff0000,0xff0000ff,0xff00ff00,0xffffff00,0xffffffff,0xff00ffff,0xffff00ff};
 for(unsigned i=0;i<7;i++){
  assert(!call(device,57,3,23u,(i==1||i==5)?8u:4u));assert(!call(device,57,3,14u,(i==0||i>=4)?1u:0u));
  for(unsigned v=0;v<3;v++){WF32(0x7000+v*28+8,depths[i]);W32(0x7000+v*28+16,depth_colors[i]);}
  assert(!call(device,41,1));assert(!call(device,84,9,4u,0u,3u,1u,0x7100u,101u,0x7000u,28u));assert(!call(device,42,1));assert(verify_readback(20+i));
 }
 /* Audited mode6 equation, real overlapping triangles and draw boundaries. */
 assert(!call(device,57,3,19u,5u));assert(!call(device,57,3,20u,6u));assert(!call(device,57,3,27u,1u));assert(!call(device,57,3,23u,8u));assert(!call(device,57,3,14u,0u));
 assert(!call(device,67,4,0u,2u,2u));assert(!call(device,67,4,0u,3u,0u));assert(!call(device,67,4,0u,5u,0u));assert(!call(device,67,4,0u,6u,2u));
 for(unsigned mode=27;mode<=39;mode++){
  uint32_t colors[64];unsigned n=1;colors[0]=0x80ff0000;
  if(mode==27)assert(!call(device,43,7,0u,0u,3u,0xff0000ffu,0x3f800000u,0u));
  else if(mode>=31){uint32_t clear=mode==35?0:mode==36||mode==37?0xff112233u:0xff000000u;assert(!call(device,43,7,0u,0u,3u,clear,mode==37?0x3dcccccdu:0x3f800000u,0u));}
  if(mode==29)colors[0]=0x0000ff00;if(mode==30)colors[0]=0xff00ff00;
  if(mode==31||mode==32){colors[0]=colors[1]=0x03ffffff;n=2;}
  if(mode==33){colors[0]=0x80ff0000;colors[1]=0x800000ff;n=2;}
  if(mode==34){colors[0]=0x800000ff;colors[1]=0x80ff0000;n=2;}
  if(mode==36){assert(!call(device,57,3,15u,1u));assert(!call(device,57,3,25u,1u));}
  if(mode==37)assert(!call(device,57,3,23u,4u));
  if(mode>=38){n=64;for(unsigned i=0;i<n;i++)colors[i]=0x03ffffff;}
  if(mode==32||mode==39){for(unsigned i=0;i<n;i++)blend_draw(device,colors+i,1);}else blend_draw(device,colors,n);
  assert(verify_readback(mode));
  if(mode==28)assert(!call(device,17,5,0u,0u,0u,0u));
  assert(!call(device,57,3,15u,0u));assert(!call(device,57,3,23u,8u));
 }
 assert(!call(device,57,3,27u,0u));
 /* Scissor enable/disable and restoration; post-blend resolve must copy the
  * untouched target, and clipped clears must preserve both color and depth. */
 assert(!call(device,76,2,0x7300u)&&!R32(0x7300)&&!R32(0x7304)&&R32(0x7308)==190&&R32(0x730c)==128);
 assert(!call(device,43,7,0u,0u,3u,0xff000000u,0x3f800000u,0u));
 W32(0x7300,60);W32(0x7304,40);W32(0x7308,80);W32(0x730c,50);assert(!call(device,75,2,0x7300u));
 for(unsigned mode=40;mode<=47;mode++){
  uint32_t color=0xffff0000;
  if(mode==41){assert(!call(device,57,3,174u,1u));assert(!call(device,57,3,27u,1u));color=0x800000ff;}
  if(mode==42){assert(!call(device,57,3,174u,0u));assert(!call(device,57,3,27u,0u));color=0xff00ff00;}
  if(mode==43)assert(!call(device,57,3,174u,1u));
  if(mode==44){W32(0x7308,60);W32(0x730c,40);assert(!call(device,75,2,0x7300u));color=0xffffffff;}
  if(mode==45){W32(0x7308,80);W32(0x730c,50);assert(!call(device,75,2,0x7300u));assert(!call(device,43,7,0u,0u,3u,0xff0000ffu,0x3e800000u,0u));}
  if(mode==46){assert(!call(device,57,3,174u,0u));assert(!call(device,57,3,23u,4u));color=0xffffffff;}
  if(mode==47){W32(0x7300,0);W32(0x7304,0);W32(0x7308,190);W32(0x730c,128);assert(!call(device,75,2,0x7300u));assert(!call(device,57,3,174u,1u));assert(!call(device,43,7,0u,0u,3u,0xffffff00u,0x3f800000u,0u));}
  if(mode!=45&&mode!=47)blend_draw(device,&color,1);assert(verify_readback(mode));
 }
 assert(!call(device,57,3,174u,0u));
 assert(!call(device,100,5,0u,uvb,0u,24u));assert(!call(device,104,2,ib));
 assert(call(texture,2,1)==1);assert(call(uvb,2,1)==1);
 /* Packed and compressed formats decode into real sampled GPU pixels. */
 assert(!call(device,57,3,23u,8u)&&!call(device,57,3,15u,0u));
 for(unsigned mode=50;mode<=53;mode++){
  assert(!call(device,23,9,4u,4u,1u,0u,mode==50?26u:mode==51?0x33545844u:0x31545844u,1u,0x2030u,0u));uint32_t packedTexture=R32(0x2030);
  assert(!call(packedTexture,19,5,0u,0x2040u,0u,0u));uint32_t p=R32(0x2044);
  if(mode==50){assert(R32(0x2040)==8);for(unsigned i=0;i<16;i++)W16(p+i*2,0x8123);}
  else if(mode>=52){assert(R32(0x2040)==8);W16(p,mode==52?0x001f:0xf800);W16(p+2,mode==52?0xf800:0x001f);W32(p+4,0xffffffff);}
  else{assert(R32(0x2040)==16);for(unsigned i=0;i<8;i++)W8(p+i,0xaa);W16(p+8,0x001f);W16(p+10,0xf800);W32(p+12,0xaaaaaaaa);}
  assert(!call(packedTexture,20,2,0u));assert(!call(device,65,3,0u,packedTexture));uint32_t white=0xffffffff;blend_draw(device,&white,1);assert(verify_readback(mode));assert(call(packedTexture,2,1)==1);
 }
 assert(!call(device,65,3,0u,0u));
 /* A four-vertex UI quad in a4MiB dynamic VB. Every unused word isNaN;
  * nonzero stream/start offsets and negative base must still draw correctly. */
 assert(!call(device,26,7,4u*1024u*1024u,0x208u,0x142u,0u,0x2030u,0u));uint32_t slicedVB=R32(0x2030);
 assert(!call(device,27,7,64u,0x208u,101u,0u,0x2030u,0u));uint32_t slicedIB=R32(0x2030);
 assert(!call(slicedVB,11,5,0u,0u,0x2020u,0x2800u));uint32_t slicedData=R32(0x2020);memset(HP(slicedData),255,4u*1024u*1024u);
 const float sliceQuad[4][2]={{-.8f,-.8f},{.8f,-.8f},{.8f,.8f},{-.8f,.8f}};
 for(unsigned i=0;i<4;i++){uint32_t p=slicedData+96+i*24;WF32(p,sliceQuad[i][0]);WF32(p+4,sliceQuad[i][1]);WF32(p+8,.5f);W32(p+12,0xff336699);WF32(p+16,.5f);WF32(p+20,.5f);}assert(!call(slicedVB,12,1));
 assert(!call(slicedIB,11,5,0u,0u,0x2020u,0x2800u));uint32_t slicedIndices=R32(0x2020);memset(HP(slicedIndices),255,64);const uint16_t quadIndices[6]={5,6,7,5,7,8};memcpy(HP(slicedIndices+4),quadIndices,sizeof quadIndices);assert(!call(slicedIB,12,1));
 assert(!call(device,23,9,1u,1u,1u,0u,21u,1u,0x2030u,0u));uint32_t whiteTexture=R32(0x2030);assert(!call(whiteTexture,19,5,0u,0x2040u,0u,0u));W32(R32(0x2044),0xffffffff);assert(!call(whiteTexture,20,2,0u));assert(!call(device,65,3,0u,whiteTexture));
 assert(!call(device,89,2,0x142u));assert(!call(device,57,3,137u,0u));assert(!call(device,100,5,0u,slicedVB,24u,24u));assert(!call(device,104,2,slicedIB));assert(!call(device,41,1));assert(!call(device,82,7,4u,0xfffffffeu,5u,4u,2u,2u));assert(!call(device,42,1));assert(verify_readback(54));
 assert(R32(slicedData)==0xffffffff&&!memcmp(HP(slicedIndices+4),quadIndices,sizeof quadIndices));
 assert(!call(device,100,5,0u,0u,0u,0u));assert(!call(device,104,2,0u));assert(!call(device,65,3,0u,0u));assert(!call(slicedVB,2,1)&&!call(slicedIB,2,1)&&!call(whiteTexture,2,1));
 /* FVF104: guest XYZRHW+UV24B, default white diffuse, host144/28B. */
 assert(!call(device,26,7,120u,0x208u,0x104u,0u,0x2030u,0u));uint32_t noColorVB=R32(0x2030);
 assert(!call(device,27,7,16u,0x208u,101u,0u,0x2030u,0u));uint32_t noColorIB=R32(0x2030);
 assert(!call(device,23,9,4u,4u,1u,0u,21u,1u,0x2030u,0u));uint32_t gradient=R32(0x2030);
 assert(!call(gradient,19,5,0u,0x2040u,0u,0u));uint32_t gradientBits=R32(0x2044);
 for(unsigned y=0;y<4;y++)for(unsigned x=0;x<4;x++)W32(gradientBits+(y*4+x)*4,0x80000000u|(((2*x+1)*255/8)<<16)|(((2*y+1)*255/8)<<8));assert(!call(gradient,20,2,0u));
 assert(!call(noColorVB,11,5,0u,0u,0x2020u,0x2800u));uint32_t noColorData=R32(0x2020);memset(HP(noColorData),255,120);
 for(unsigned i=0;i<3;i++){uint32_t p=noColorData+48+i*24;WF32(p,triangle[i][0]);WF32(p+4,triangle[i][1]);WF32(p+8,.5f);WF32(p+12,triangle[i][2]);WF32(p+16,triangle[i][3]);WF32(p+20,triangle[i][4]);}assert(!call(noColorVB,12,1));
 assert(!call(noColorIB,11,5,0u,0u,0x2020u,0x2800u));uint32_t noColorIndices=R32(0x2020);W16(noColorIndices,0xffff);W16(noColorIndices+2,0xffff);W16(noColorIndices+4,0);W16(noColorIndices+6,1);W16(noColorIndices+8,2);assert(!call(noColorIB,12,1));
 assert(!call(device,65,3,0u,gradient));assert(!call(device,89,2,0x104u));assert(!call(device,57,3,137u,1u));assert(!call(device,69,4,0u,5u,2u)&&!call(device,69,4,0u,6u,2u));assert(!call(device,100,5,0u,noColorVB,24u,24u));assert(!call(device,104,2,noColorIB));
 assert(!call(device,41,1));assert(!call(device,82,7,4u,1u,0u,3u,2u,1u));assert(!call(device,42,1));assert(verify_readback(55));assert(R32(noColorData)==0xffffffff);
 /* Bound144 counterpart: identical perspective geometry with explicit diffuse. */
 assert(!call(device,26,7,140u,0x208u,0x144u,0u,0x2030u,0u));uint32_t colorRhwVB=R32(0x2030);
 assert(!call(colorRhwVB,11,5,0u,0u,0x2020u,0x2800u));uint32_t colorRhwData=R32(0x2020);memset(HP(colorRhwData),255,140);
 for(unsigned i=0;i<3;i++){uint32_t p=colorRhwData+56+i*28;memcpy(HP(p),HP(noColorData+48+i*24),16);W32(p+16,0xffffffff);memcpy(HP(p+20),HP(noColorData+48+i*24+16),8);}assert(!call(colorRhwVB,12,1));
 assert(!call(device,89,2,0x144u)&&!call(device,100,5,0u,colorRhwVB,28u,28u));assert(!call(device,41,1));assert(!call(device,82,7,4u,1u,0u,3u,2u,1u));assert(!call(device,42,1));assert(verify_readback(56));assert(R32(colorRhwData)==0xffffffff);
 assert(!call(device,89,2,0x104u)&&!call(device,100,5,0u,noColorVB,24u,24u)&&!call(colorRhwVB,2,1));
 /* Actual material35: RGB=TFactor.rgb; alpha=TFactor.a*texture.a. */
 assert(!call(device,67,4,0u,1u,3u)&&!call(device,67,4,0u,3u,3u));assert(!call(device,67,4,0u,4u,4u)&&!call(device,67,4,0u,5u,3u)&&!call(device,67,4,0u,6u,2u));
 for(unsigned mode=60;mode<=66;mode++){
  uint32_t factor=mode==62?0x00402010u:mode==63?0xff402010u:0x80402010u;
  uint32_t texel=mode==60?0x80000000u:mode==61?0x80ff00ffu:mode==62||mode==63?0xff00ffffu:mode==66?0xff123456u:0x80aabbccu;
  assert(!call(device,57,3,60u,factor));assert(!call(device,57,3,27u,mode>=62&&mode<=65?1u:0u));
  assert(!call(gradient,19,5,0u,0x2040u,0u,0u));for(unsigned i=0;i<16;i++)W32(R32(0x2044)+i*4,texel);assert(!call(gradient,20,2,0u));
  if(mode!=65)assert(!call(device,43,7,0u,0u,3u,0xff112233u,0x3f800000u,0u));
  if(mode==66){assert(!call(device,67,4,0u,1u,4u)&&!call(device,67,4,0u,2u,2u)&&!call(device,67,4,0u,3u,0u));assert(!call(device,67,4,0u,5u,0u));}
  assert(!call(device,41,1));assert(!call(device,82,7,4u,1u,0u,3u,2u,1u));assert(!call(device,42,1));assert(verify_readback(mode));
 }
 /* Original material20: untextured diffuse and componentwise source-color blend. */
 assert(!call(device,65,3,0u,0u)&&!call(device,89,2,0x144u));assert(!call(device,67,4,0u,1u,2u)&&!call(device,67,4,0u,2u,0u)&&!call(device,67,4,0u,4u,2u)&&!call(device,67,4,0u,5u,0u));
 assert(!call(device,57,3,27u,1u)&&!call(device,57,3,19u,3u)&&!call(device,57,3,20u,4u));
 const uint32_t diffuseColors[2]={0x804080c0,0x804080c0};
 for(unsigned mode=70;mode<=72;mode++){if(mode!=72)assert(!call(device,43,7,0u,0u,3u,0x60112233u,0x3f800000u,0u));blend_draw(device,diffuseColors,mode==71?2:1);assert(verify_readback(mode));}
 assert(!call(device,17,5,0u,0u,0u,0u));
 /* Material19: destination multiplied by diffuseRGB / textureAlpha. */
 assert(!call(device,65,3,0u,gradient)&&!call(device,67,4,0u,5u,2u));assert(!call(device,57,3,19u,1u)&&!call(device,57,3,20u,3u));
 for(unsigned mode=80;mode<=87;mode++){
  unsigned alpha=mode==80?0:mode==82?255:mode>=86?192:128;uint32_t texel=(alpha<<24)|(mode==83?0x00ff00ff:0x0000ff00);
  assert(!call(gradient,19,5,0u,0x2040u,0u,0u));for(unsigned i=0;i<16;i++)W32(R32(0x2044)+i*4,texel);assert(!call(gradient,20,2,0u));
  assert(!call(device,57,3,15u,mode==85?1u:0u));assert(!call(device,57,3,25u,5u)&&!call(device,57,3,24u,200u));
  if(mode!=87)assert(!call(device,43,7,0u,0u,3u,0xffc0a080u,0x3f800000u,0u));
  uint32_t color=mode==84?0xffc08040:0x114080c0;blend_draw(device,&color,1);assert(verify_readback(mode));
 }
 /* All121 classic ADD factor pairs, with an independent pixel reference. */
 assert(!call(device,65,3,0u,0u)&&!call(device,67,4,0u,1u,2u)&&!call(device,67,4,0u,2u,0u)&&!call(device,67,4,0u,4u,2u)&&!call(device,67,4,0u,5u,0u));assert(!call(device,57,3,15u,0u));
 uint32_t matrixColor=0x80604020u;
 for(unsigned src=1;src<=11;src++)for(unsigned dst=1;dst<=11;dst++){
  assert(!call(device,57,3,19u,src)&&!call(device,57,3,20u,dst));assert(!call(device,43,7,0u,0u,3u,0x9060a0c0u,0x3f800000u,0u));blend_draw(device,&matrixColor,1);assert(verify_readback(1000+(src-1)*11+dst-1));
 }
 /* First pair was evicted from64-entry LRU; recompilation must remain correct. */
 assert(!call(device,57,3,19u,1u)&&!call(device,57,3,20u,1u));assert(!call(device,43,7,0u,0u,3u,0x9060a0c0u,0x3f800000u,0u));blend_draw(device,&matrixColor,1);assert(verify_readback(1121));
 assert(!call(gradient,19,5,0u,0x2040u,0u,0u));for(unsigned i=0;i<16;i++)W32(R32(0x2044)+i*4,0xa04080c0u);assert(!call(gradient,20,2,0u));
 const unsigned pairs[3][2]={{2,2},{1,2},{1,3}};
 for(unsigned equation=0;equation<4;equation++){
  assert(!call(device,65,3,0u,equation==2?0u:gradient));
  assert(!call(device,67,4,0u,1u,equation==0?4u:equation==1?3u:2u)&&!call(device,67,4,0u,2u,equation==0?2u:0u)&&!call(device,67,4,0u,3u,equation==1?3u:0u));
  assert(!call(device,67,4,0u,4u,equation<2?4u:2u)&&!call(device,67,4,0u,5u,equation==1?3u:equation==3?2u:0u)&&!call(device,67,4,0u,6u,2u));assert(!call(device,57,3,60u,0x802060a0u));
  for(unsigned pair=0;pair<3;pair++){assert(!call(device,57,3,19u,pairs[pair][0])&&!call(device,57,3,20u,pairs[pair][1]));assert(!call(device,43,7,0u,0u,3u,0x9060a0c0u,0x3f800000u,0u));blend_draw(device,&matrixColor,1);assert(verify_readback(300+equation*3+pair));}
 }
 /* Repeated additive draw exercises resolve feedback rather than final-only quantization. */
 assert(!call(device,65,3,0u,0u)&&!call(device,67,4,0u,5u,0u)&&!call(device,57,3,19u,2u)&&!call(device,57,3,20u,2u));
 assert(!call(device,43,7,0u,0u,3u,0x20112233u,0x3f800000u,0u));uint32_t smallColor=0x2013210bu;
 blend_draw(device,&smallColor,1);assert(verify_readback(1200));blend_draw(device,&smallColor,1);assert(verify_readback(1201));
 assert(!call(device,100,5,0u,0u,0u,0u));assert(!call(device,104,2,0u));assert(!call(device,65,3,0u,0u));assert(!call(noColorVB,2,1)&&!call(noColorIB,2,1)&&!call(gradient,2,1));
 /* The final UP draw releases device-owned stream/index references. The
  * earlier uvb application reference was already released, so do not reuse it. */
 assert(!call(device,101,5,0u,0x2100u,0x2104u,0x2108u)&&!R32(0x2100));
 assert(!call(device,105,2,0x2100u)&&!R32(0x2100));
 assert(call(vb,2,1)==0);assert(call(ib,2,1)==0);assert(!call(device,2,1));assert(!call(factory,2,1));
 free(g_mem);puts("Native D3D9 ABI real GPU fixture passed");return 0;
}
