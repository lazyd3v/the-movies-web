#include "dialog-api.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(void, setup_bridge, (), { Module["moviesShowDialog"]=function(title,text,flags) {
    if(title!=="Error" || text!=="" || flags!==4) throw new Error("Incorrect dialog bridge payload");return 6;
}; });
#endif
uint8_t *g_mem;
static uint32_t error,expected_flags;
static int selected,calls;
static const char *expected_title,*expected_text;
static jmp_buf trapped;
void movies_set_last_error(uint32_t value) { error=value; }
void guest_trap(CPU *c,uint32_t at,const char *why) { (void)c;(void)at;(void)why;longjmp(trapped,1); }
static int mock(const char *title,const char *text,uint32_t flags) {
    assert(!strcmp(title,expected_title));assert(!strcmp(text,expected_text));assert(flags==expected_flags);++calls;return selected;
}
static uint32_t call(int wide,uint32_t owner,uint32_t text,uint32_t title,uint32_t flags) {
    CPU c={0};c.esp=0x100;c.edi=0x12345678;
    W32(c.esp,0xdeadbeef);W32(c.esp+4,owner);W32(c.esp+8,text);W32(c.esp+12,title);W32(c.esp+16,flags);
    uint32_t count;ImportHandler *h=movies_dialog_handlers(&count);
    const char *name=wide?"MessageBoxW":"MessageBoxA";
    for(unsigned i=0;i<count;i++) if(!strcmp(strchr(h[i].name,'!')+1,name)) {
        h[i].fn(&c);assert(c.esp==0x114 && c.edi==0x12345678);return c.eax;
    }
    assert(0);return 0;
}
int main(void) {
    g_mem=calloc(1,0x20000);assert(g_mem);movies_dialog_set_callback(mock);
    strcpy((char *)HP(0x1000),"Title");strcpy((char *)HP(0x2000),"Price \x80\nNext");
    expected_title="Title";expected_text="Price \xe2\x82\xac\nNext";
    const unsigned ids[7][3]={{1,0,0},{1,2,0},{3,4,5},{6,7,2},{6,7,0},{4,2,0},{2,10,11}};
    for(unsigned type=0;type<7;type++) for(unsigned button=0;button<3 && ids[type][button];button++) {
        selected=(int)ids[type][button];expected_flags=type|0x1000|(button<<8)|0x30;error=789;
        assert(call(0,0,0x2000,0x1000,expected_flags)==(unsigned)selected && error==789);
    }
    expected_flags=0;selected=1;expected_title="Error";expected_text="";
    assert(call(0,0,0,0,0)==1);
    expected_flags=0x11010;assert(call(1,0,0,0,expected_flags)==1); /* actual fatal graphics dialog: foreground+system modal+error */
    expected_flags=0;
    expected_title="";W8(0x1000,0);assert(call(0,0,0,0x1000,0)==1);
    const uint16_t title[]={0x0041,0xd83c,0xdfac,0};
    const uint16_t text[]={0x041f,0x0440,0x0438,0x0432,0x0435,0x0442,0};
    for(unsigned i=0;i<sizeof title/sizeof title[0];i++)W16(0x1000+2*i,title[i]);
    for(unsigned i=0;i<sizeof text/sizeof text[0];i++)W16(0x2000+2*i,text[i]);
    expected_title="A\xf0\x9f\x8e\xac";expected_text="\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";
    assert(call(1,0,0x2000,0x1000,0)==1);
    int previous=calls;
    assert(!call(1,1,0x2000,0x1000,0) && error==50);
    assert(!call(1,0,0x2000,0x1000,0x4000) && error==50); /*HELP needs window messages*/
    assert(!call(1,0,0x2000,0x1000,0x200000) && error==50); /*service notification*/
    assert(!call(1,0,0x2000,0x1000,7) && error==87);
    assert(!call(1,0,0x2000,0x1000,0x300) && error==87); /*nonexistent default button*/
    assert(!call(1,0,0x2000,0x1000,0x3000) && error==87);
    assert(!call(1,0,0x2000,0x1000,0x50) && error==87);
    W16(0x1000,0xd800);W16(0x1002,'x');assert(!call(1,0,0x2000,0x1000,0) && error==87);
    W16(0x1000,0xdc00);assert(!call(1,0,0x2000,0x1000,0) && error==87);
    assert(!call(1,0,0x2000,0x1ffff,0) && error==87);
    assert(!call(0,0,0x20000,0,0) && error==87);
    memset(HP(0x4000),'x',32767);assert(!call(0,0,0x4000,0,0) && error==206);
    assert(calls==previous);
    expected_title="Error";expected_text="";expected_flags=0;selected=7;
    if(!setjmp(trapped)) { call(0,0,0,0,0);assert(0); } /*result must be offered*/
    movies_dialog_set_callback(NULL);
#ifdef __EMSCRIPTEN__
    if(!setjmp(trapped)) { call(0,0,0,0,0);assert(0); } /*missing JS callback traps through C*/
    setup_bridge();
    assert(call(0,0,0,0,4)==6); /*exercise real EM_JS and UTF8ToString dependency*/
#else
    if(!setjmp(trapped)) { call(0,0,0,0,0);assert(0); } /*no native auto-OK*/
#endif
    free(g_mem);puts("dialog button, encoding, flags, bounds and strict callback tests passed");
}
