/* MessageBoxA/W contract:
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-messageboxw
 * No automatic button selection. Owner windows, HELP, service notifications,
 * and unimplemented presentation flags fail explicitly. Modal scope is the
 * private game UI, never the host operating-system desktop.
 */
#include "dialog-api.h"
#include "startup-api.h"
#include "text-api.h"
#include <stdlib.h>
#include <stdio.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/em_macros.h>
#endif
#ifndef MOVIES_DIALOG_SPACE
#define MOVIES_DIALOG_SPACE GUEST_SPACE
#endif
#define STRING_LIMIT 32767u
static MoviesDialogCallback host_callback;
void movies_dialog_set_callback(MoviesDialogCallback callback) { host_callback=callback; }
static const uint8_t buttons[7][3]={{1,0,0},{1,2,0},{3,4,5},{6,7,2},{6,7,0},{4,2,0},{2,10,11}};
static const uint8_t counts[]={1,2,3,3,2,2,3};
static void ret(CPU *c,uint32_t value) { c->eax=value;c->esp+=20; }
static int range(uint32_t p,uint32_t n) { return p && (uint64_t)p+n<=MOVIES_DIALOG_SPACE; }
static unsigned encode(char *out,uint32_t u) {
    if(u<0x80) { out[0]=(char)u;return 1; }
    if(u<0x800) { out[0]=(char)(0xc0|(u>>6));out[1]=(char)(0x80|(u&63));return 2; }
    if(u<0x10000) { out[0]=(char)(0xe0|(u>>12));out[1]=(char)(0x80|((u>>6)&63));out[2]=(char)(0x80|(u&63));return 3; }
    out[0]=(char)(0xf0|(u>>18));out[1]=(char)(0x80|((u>>12)&63));out[2]=(char)(0x80|((u>>6)&63));out[3]=(char)(0x80|(u&63));return 4;
}
static char *read_string(uint32_t address,int wide,const char *fallback,uint32_t *failure) {
    if(!address) {
        char *s=malloc(strlen(fallback)+1);
        if(s) strcpy(s,fallback);else *failure=8;
        return s;
    }
    char *out=malloc(STRING_LIMIT*4u+1u);
    if(!out) { *failure=8;return NULL; }
    unsigned used=0;
    for(unsigned i=0;i<STRING_LIMIT;i++) {
        uint64_t offset=(uint64_t)address+(uint64_t)i*(wide?2:1);
        if(offset>0xffffffffull || !range((uint32_t)offset,wide?2:1)) { *failure=87;break; }
        uint32_t scalar=wide?R16((uint32_t)offset):movies_cp1252_decode(R8((uint32_t)offset));
        if(!scalar) { out[used]=0;return out; }
        if(wide && scalar>=0xd800 && scalar<=0xdbff) {
            if(++i==STRING_LIMIT || offset+2>0xffffffffull || !range((uint32_t)(offset+2),2)) { *failure=87;break; }
            uint32_t low=R16((uint32_t)(offset+2));
            if(low<0xdc00 || low>0xdfff) { *failure=87;break; }
            scalar=0x10000+((scalar-0xd800)<<10)+(low-0xdc00);
        } else if(wide && scalar>=0xdc00 && scalar<=0xdfff) { *failure=87;break; }
        used+=encode(out+used,scalar);
    }
    if(!*failure) *failure=206;
    free(out);return NULL;
}
#ifdef __EMSCRIPTEN__
EM_JS_DEPS(movies_dialog_dependencies,"$UTF8ToString");
EM_JS(int, movies_dialog_bridge, (const char *title,const char *text,uint32_t flags), {
    var show = Module['moviesShowDialog'];
    if (typeof show !== 'function') return -2147483648;
    var result = show(UTF8ToString(title), UTF8ToString(text), flags >>> 0);
    if (!Number.isInteger(result)) throw new Error('MessageBox bridge did not return a button ID');
    return result;
});
#endif
static void message_box(CPU *c,int wide) {
    uint32_t owner=R32(c->esp+4),text_ptr=R32(c->esp+8),title_ptr=R32(c->esp+12),flags=R32(c->esp+16);
    unsigned kind=flags&15,icon=flags&0xf0,def=(flags>>8)&3,modal=flags&0x3000;
    /* MB_SETFOREGROUND is implemented within the private browser UI: the
     * acknowledged host modal focuses its default button. No OS focus claim. */
    if(owner || (flags&~0x1337fu)) {
        fprintf(stderr,"[dialog] unsupported owner=%08x flags=%08x caller=%08x\n",owner,flags,R32(c->esp));
        movies_set_last_error(50);ret(c,0);return;
    }
    if(kind>6 || icon>0x40 || def>=counts[kind] || modal==0x3000) { movies_set_last_error(87);ret(c,0);return; }
    uint32_t failure=0;
    char *title=read_string(title_ptr,wide,"Error",&failure);
    char *text=failure?NULL:read_string(text_ptr,wide,"",&failure);
    if(failure) { free(title);free(text);movies_set_last_error(failure);ret(c,0);return; }
    int result;
    if(host_callback) result=host_callback(title,text,flags);
    else {
#ifdef __EMSCRIPTEN__
        result=movies_dialog_bridge(title,text,flags);
#else
        free(title);free(text);
        guest_trap(c,R32(c->esp),"MessageBox requires an interactive dialog callback");return;
#endif
    }
    free(title);free(text);
    if(result==(-2147483647-1)) { guest_trap(c,R32(c->esp),"MessageBox requires an interactive dialog callback");return; }
    int valid=0;for(unsigned i=0;i<counts[kind];i++) if(result==buttons[kind][i]) valid=1;
    if(!valid) { guest_trap(c,R32(c->esp),"MessageBox callback returned a button not offered by this dialog");return; }
    ret(c,(uint32_t)result);
}
static void message_a(CPU *c) { message_box(c,0); }
static void message_w(CPU *c) { message_box(c,1); }
ImportHandler *movies_dialog_handlers(uint32_t *count) {
    static ImportHandler h[]={{"USER32.DLL!MessageBoxA",message_a},{"USER32.DLL!MessageBoxW",message_w}};
    *count=sizeof h/sizeof h[0];return h;
}
