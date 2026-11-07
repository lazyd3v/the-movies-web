#ifndef MOVIES_D3D9_API_H
#define MOVIES_D3D9_API_H
#include "runtime.h"
enum MoviesD3D9Command { MOVIES_D3D9_CAPS=1, MOVIES_D3D9_CREATE, MOVIES_D3D9_CLEAR,
 MOVIES_D3D9_PRESENT, MOVIES_D3D9_UPLOAD_VERTEX, MOVIES_D3D9_UPLOAD_INDEX,
 MOVIES_D3D9_DRAW, MOVIES_D3D9_RELEASE_RESOURCE, MOVIES_D3D9_DESTROY, MOVIES_D3D9_UPLOAD_TEXTURE,
 MOVIES_D3D9_CREATE_TARGET, MOVIES_D3D9_SET_TARGET, MOVIES_D3D9_READ_TARGET, MOVIES_D3D9_WRITE_TARGET };
typedef struct MoviesD3D9Request {
 uint32_t command, args[33];
 const void *data; uint32_t bytes;
 float matrices[48];
} MoviesD3D9Request;
typedef struct MoviesD3D9Reply { uint32_t values[8]; } MoviesD3D9Reply;
/* Return1 only for an acknowledged real operation. CAPS reply: width,height,
 * depth16,maxBufferBytes,presentationIntervalMask,maxTextureDimension,renderTargetMask(bit0=22,bit1=25). CREATE args: width,height,
 * hwnd,depth16,windowed,presentationInterval,backBufferFormat. DRAW args: vertexHandle,indexHandle,
 * firstIndex,indexCount,signedBaseVertexBits,depthTest,viewportXYWH and min/max
 * depth float bits,textureHandle,addressUVW,minFilter,magFilter,mipFilter,lodBiasBits,alphaTestEnabled/func/ref,depthCompare,depthWrite,sourceAlphaBlend,scissorEnabled,left,top,right,bottom; matrices are raw D3D row-major world/view/projection.
 * Upload replies values[0]=positive backend handle. CLEAR args=color,depthbits.
 * CREATE_TARGET args=width,height replies a sampleable render-target texture handle.
 * SET_TARGET args=handle (0 selects the backbuffer); later CLEAR/DRAW use it.
 * READ_TARGET args=handle,width,height fills data (bytes=w*h*4) with guest BGRA.
 * WRITE_TARGET args=handle,width,height uploads RGBA data to the target texture.
 * No native default backend. UI-thread confined; initialization registers
 * guest metadata05000000..05040000 and trampolines0e020000..0e026000. */
typedef int (*MoviesD3D9Host)(const MoviesD3D9Request *, MoviesD3D9Reply *,void *);
int movies_d3d9_init(void);
void movies_d3d9_set_host(MoviesD3D9Host,void *);
ImportHandler *movies_d3d9_handlers(uint32_t *count);
#endif
