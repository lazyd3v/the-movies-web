#include "resource-api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint8_t *g_mem;
static uint32_t error;
void movies_set_last_error(uint32_t e) { error=e; }
static uint32_t call(const char *name,unsigned argc,uint32_t a,uint32_t b) {
    CPU c={0};c.esp=0x100;c.edi=0x12345678;W32(c.esp,0xdeadbeef);W32(c.esp+4,a);W32(c.esp+8,b);
    uint32_t count;ImportHandler *h=movies_resource_handlers(&count);
    for(unsigned i=0;i<count;i++)if(!strcmp(strchr(h[i].name,'!')+1,name)) {
        h[i].fn(&c);assert(c.esp==0x104+argc*4 && c.edi==0x12345678);return c.eax;
    }
    assert(0);return 0;
}
static void fixture(void) {
    memset(HP(0x1000),0,0x5000);
    W16(0x1000,0x5a4d);W32(0x103c,0x80);W32(0x1080,0x4550);W16(0x1084,0x14c);W16(0x1094,224);
    W16(0x1098,0x10b);W32(0x1098+56,0x5000);W32(0x1098+92,16);W32(0x1098+112,0x1000);W32(0x1098+116,0x2000);
    W16(0x200e,2);W32(0x2010,3);W32(0x2014,0x80000030);W32(0x2018,14);W32(0x201c,0x80000050);
    W16(0x203e,1);W32(0x2040,1);W32(0x2044,0x80000070);
    W16(0x205e,1);W32(0x2060,1011);W32(0x2064,0x80000090);
    W16(0x207e,1);W32(0x2080,0x809);W32(0x2084,0xb0);
    W16(0x209e,1);W32(0x20a0,0x809);W32(0x20a4,0xc0);
    W32(0x20b0,0x1200);W32(0x20b4,4264);W32(0x20c0,0x1100);W32(0x20c4,20);
    W16(0x2102,1);W16(0x2104,1);W8(0x2106,32);W8(0x2107,32);W16(0x210a,1);W16(0x210c,32);W32(0x210e,4264);W16(0x2112,1);
    W32(0x2200,40);W32(0x2204,32);W32(0x2208,64);W16(0x220c,1);W16(0x220e,32);
}
int main(void) {
    g_mem=calloc(1,0x10000);assert(g_mem);fixture();assert(movies_resource_init(0x1000));error=123;
    uint32_t icon=call("LoadIconA",2,0x1000,1011);assert(icon && error==123);
    assert(call("LoadIconA",2,0x1000,1011)==icon);
    const MoviesIconResource *r=movies_resource_icon(icon);assert(r && r->id==1011 && r->language==0x809 && r->count==1 && r->selected==0);
    assert(r->images[0].width==32 && r->images[0].height==32 && r->images[0].bpp==32 && r->images[0].guest_address==0x2200 && r->images[0].bytes==4264);
    uint32_t cursor=call("LoadCursorA",2,0,32512),brush=call("GetStockObject",1,0,0);
    assert(cursor && brush && cursor!=brush && cursor!=icon);
    assert(movies_resource_cursor(cursor)->system_id==32512 && !strcmp(movies_resource_cursor(cursor)->css_name,"default"));
    assert(movies_resource_brush(brush)->style==0 && movies_resource_brush(brush)->color==0xffffff);
    assert(!movies_resource_icon(cursor) && !movies_resource_cursor(brush) && !movies_resource_brush(icon));
    assert(!call("LoadIconA",2,0x1000,999) && error==1814);
    assert(!call("LoadIconA",2,0,1011) && error==126);
    assert(!call("LoadIconA",2,0x1000,0x10000) && error==50);
    assert(!call("LoadCursorA",2,0,32513) && error==1814);
    assert(!call("GetStockObject",1,999,0) && error==50);
    /* Reject malformed tree offsets, counts, group records, image ranges and DIBs. */
    const struct { uint32_t address,value; } corrupt[]={
        {0x201c,0x80001fff},{0x2064,0x80000000},{0x20a4,0x80000090},{0x20b0,0x4fff},
        {0x20b4,0xffffffff},{0x210e,4263},{0x2104,257},{0x2204,33},{0x2208,32},{0x2210,1}
    };
    for(unsigned i=0;i<sizeof corrupt/sizeof corrupt[0];i++) {
        fixture();W32(corrupt[i].address,corrupt[i].value);assert(movies_resource_init(0x1000));
        assert(!call("LoadIconA",2,0x1000,1011));assert(error==13 || error==1814);
    }
    fixture();W32(0x1098+116,0xffffff00);assert(!movies_resource_init(0x1000) && error==13);
    fixture();W32(0x103c,0xfffffff0);assert(!movies_resource_init(0x1000) && error==13);
    assert(!movies_resource_init(0xfffffff0));
    fixture();assert(movies_resource_init(0x1000));assert(call("LoadIconA",2,0x1000,1011));
    /* Zero resources is valid PE but cannot invent an icon. */
    fixture();W32(0x1098+112,0);W32(0x1098+116,0);assert(movies_resource_init(0x1000));
    assert(!call("LoadIconA",2,0x1000,1011) && error==1814);
    free(g_mem);puts("PE icon resources, typed shared handles and bounds tests passed");
}
