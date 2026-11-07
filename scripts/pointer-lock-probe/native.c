#include "window-api.h"
#include "resource-api.h"
#include <assert.h>
#include <stdlib.h>
#include <emscripten.h>
uint8_t *g_mem;static uint32_t last_error;
void movies_set_last_error(uint32_t error){last_error=error;}
int movies_process_ticks(uint32_t*out){*out=1;return 1;}
void guest_trap(CPU*c,uint32_t at,const char*s){(void)c;(void)at;emscripten_log(EM_LOG_ERROR,"pointer fixture trap: %s",s);abort();}
GuestFn lookup_function(uint32_t address){(void)address;return NULL;}
void guest_call(CPU*c,GuestFn fn,int n,const uint32_t*args){(void)c;(void)fn;(void)n;(void)args;abort();}
const MoviesCursorResource *movies_resource_cursor(uint32_t h){(void)h;return NULL;}
const MoviesIconResource *movies_resource_icon(uint32_t h){(void)h;return NULL;}
const MoviesBrushResource *movies_resource_brush(uint32_t h){(void)h;return NULL;}
static uint32_t call(const char*name,unsigned n,uint32_t x,uint32_t y){CPU c={0};c.esp=0x9000;W32(c.esp,0x12345678);W32(c.esp+4,x);W32(c.esp+8,y);uint32_t count;ImportHandler*h=movies_window_handlers(&count);for(unsigned i=0;i<count;i++)if(!strcmp(strchr(h[i].name,'!')+1,name)){h[i].fn(&c);assert(c.esp==0x9004+4*n);return c.eax;}abort();}
EMSCRIPTEN_KEEPALIVE int probe_init(void){g_mem=calloc(1,0x10000);return g_mem&&movies_window_init(1024,768);}
EMSCRIPTEN_KEEPALIVE int probe_warp(int x,int y){last_error=123;return call("SetCursorPos",2,(uint32_t)x,(uint32_t)y);}
EMSCRIPTEN_KEEPALIVE int probe_read(void){return call("GetCursorPos",1,0x1000,0);}
EMSCRIPTEN_KEEPALIVE int probe_x(void){return (int32_t)R32(0x1000);}
EMSCRIPTEN_KEEPALIVE int probe_y(void){return (int32_t)R32(0x1004);}
EMSCRIPTEN_KEEPALIVE int probe_error(void){return last_error;}
