/* Observed LoadIconA/LoadCursorA/GetStockObject contracts:
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadicona
 * https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadcursora
 * https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getstockobject
 * PE32 resource offsets are checked against both resource directory and image.
 * Numeric module icons only; cursor is the shared browser arrow presentation,
 * brush is solid white. Other resources fail, never receive invented handles.
 */
#include "resource-api.h"
#include "startup-api.h"
#include <pthread.h>
#include <stdlib.h>
#ifndef MOVIES_RESOURCE_SPACE
#define MOVIES_RESOURCE_SPACE GUEST_SPACE
#endif
#define CURSOR_HANDLE 0x0c000001u
#define BRUSH_HANDLE 0x0c000002u
#define ICON_BASE 0x0c001000u
#define ICON_LIMIT 256u
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static uint32_t base,image_size,res_rva,res_size,icon_count;
static MoviesIconResource icons[ICON_LIMIT];
static const MoviesCursorResource arrow={32512,"default"};
static const MoviesBrushResource white={0,0x00ffffffu};
static int guest_range(uint32_t p,uint32_t n) { return p && (uint64_t)p+n<=MOVIES_RESOURCE_SPACE; }
static int image_range(uint32_t rva,uint32_t n) { return (uint64_t)rva+n<=image_size && (uint64_t)base+rva+n<=MOVIES_RESOURCE_SPACE; }
static int resource_range(uint32_t off,uint32_t n) { return (uint64_t)off+n<=res_size && image_range(res_rva+off,n); }
static uint32_t ra(uint32_t off) { return base+res_rva+off; }
int movies_resource_init(uint32_t image_base) {
    pthread_mutex_lock(&guard);
    for(unsigned i=0;i<icon_count;i++)free((void *)icons[i].images);
    memset(icons,0,sizeof icons);icon_count=0;base=image_size=res_rva=res_size=0;
    int ok=0;
    if(!guest_range(image_base,64) || R16(image_base)!=0x5a4d)goto done;
    uint32_t nt=R32(image_base+60);
    if(nt>0x100000u || (uint64_t)image_base+nt+24+120>MOVIES_RESOURCE_SPACE)goto done;
    uint32_t header=image_base+nt;
    if(R32(header)!=0x4550 || R16(header+4)!=0x14c || R16(header+20)<120 || R16(header+24)!=0x10b)goto done;
    uint32_t size=R32(header+24+56),rva=R32(header+24+112),bytes=R32(header+24+116);
    if(!size || (uint64_t)image_base+size>MOVIES_RESOURCE_SPACE || (uint64_t)nt+24+R16(header+20)>size || R32(header+24+92)<3)goto done;
    if(rva && (bytes<16 || (uint64_t)rva+bytes>size))goto done;
    if(!rva && bytes)goto done;
    base=image_base;image_size=size;res_rva=rva;res_size=bytes;ok=1;
done:
    pthread_mutex_unlock(&guard);
    if(!ok)movies_set_last_error(13);
    return ok;
}
/* Returns0 absent,1 found,-1 malformed; no recursive traversal/cycle risk. */
static int entry(uint32_t dir,uint32_t wanted,uint32_t *target) {
    if(!resource_range(dir,16))return -1;
    uint32_t count=(uint32_t)R16(ra(dir)+12)+R16(ra(dir)+14);
    if(count>4096 || !resource_range(dir+16,count*8))return -1;
    for(uint32_t i=0;i<count;i++) {
        uint32_t at=ra(dir+16+i*8),name=R32(at);
        if(!(name&0x80000000u) && name==wanted) { *target=R32(at+4);return 1; }
    }
    return 0;
}
static int leaf(uint32_t type,uint32_t id,uint16_t preferred,uint32_t *address,uint32_t *bytes,uint16_t *lang) {
    if(!res_rva || !res_size)return 0;
    uint32_t t,n;int found=entry(0,type,&t);if(found!=1)return found;
    if(!(t&0x80000000u))return -1;
    found=entry(t&0x7fffffffu,id,&n);if(found!=1)return found;
    if(!(n&0x80000000u))return -1;
    uint32_t dir=n&0x7fffffffu;
    if(!resource_range(dir,16))return -1;
    uint32_t count=(uint32_t)R16(ra(dir)+12)+R16(ra(dir)+14);
    if(!count || count>4096 || !resource_range(dir+16,count*8))return -1;
    uint32_t chosen=0,chosen_lang=0;int rank=-1;
    for(uint32_t i=0;i<count;i++) {
        uint32_t at=ra(dir+16+i*8),l=R32(at),d=R32(at+4);
        if(l>0xffff || (d&0x80000000u))return -1;
        int r=l==preferred?3:l==0?2:1;
        if(r>rank) { rank=r;chosen=d;chosen_lang=l; }
    }
    if(!resource_range(chosen,16))return -1;
    uint32_t rva=R32(ra(chosen)),length=R32(ra(chosen)+4);
    if(!length || length>16*1024*1024u || !image_range(rva,length))return -1;
    *address=base+rva;*bytes=length;*lang=(uint16_t)chosen_lang;return 1;
}
/* The observed executable uses uncompressed Windows DIB icon images. Other
 * codecs are rejected until decoded; a blob's presence alone is not an icon. */
static int valid_dib(const MoviesIconImage *im) {
    uint32_t p=im->guest_address;
    if(im->bytes<40 || R32(p)!=40 || R32(p+4)!=im->width || R32(p+8)!=(uint32_t)im->height*2 ||
       R16(p+12)!=1 || im->planes!=1 || R16(p+14)!=im->bpp || R32(p+16)!=0) return 0;
    unsigned bpp=im->bpp;
    if(bpp!=1 && bpp!=4 && bpp!=8 && bpp!=16 && bpp!=24 && bpp!=32)return 0;
    uint32_t palette=R32(p+32);
    if(bpp<=8) { if(palette>(1u<<bpp))return 0;if(!palette)palette=1u<<bpp; }
    else if(palette)return 0;
    uint64_t color_row=(((uint64_t)im->width*bpp+31)/32)*4;
    uint64_t mask_row=(((uint64_t)im->width+31)/32)*4;
    uint64_t required=40+(uint64_t)palette*4+(color_row+mask_row)*im->height;
    return required<=im->bytes;
}
const MoviesIconResource *movies_resource_icon(uint32_t handle) {
    pthread_mutex_lock(&guard);const MoviesIconResource *result=NULL;
    if(handle>=ICON_BASE && handle-ICON_BASE<icon_count)result=&icons[handle-ICON_BASE];
    pthread_mutex_unlock(&guard);return result;
}
const MoviesCursorResource *movies_resource_cursor(uint32_t handle) { return handle==CURSOR_HANDLE?&arrow:NULL; }
const MoviesBrushResource *movies_resource_brush(uint32_t handle) { return handle==BRUSH_HANDLE?&white:NULL; }
static void ret(CPU *c,unsigned n,uint32_t value) { c->eax=value;c->esp+=4*(n+1); }
static void load_icon(CPU *c) {
    uint32_t module=R32(c->esp+4),id=R32(c->esp+8),result=0,failure=0;
    pthread_mutex_lock(&guard);
    if(!base || module!=base)failure=126;
    else if(id>0xffffu)failure=50; /*named resources not implemented*/
    else {
        for(unsigned i=0;i<icon_count;i++)if(icons[i].id==id)result=icons[i].handle;
        if(!result) {
            uint32_t address,bytes;uint16_t lang;int found=leaf(14,id,0x809,&address,&bytes,&lang);
            if(found!=1)failure=found<0?13:1814;
            else if(bytes<6 || R16(address)!=0 || R16(address+2)!=1)failure=13;
            else {
                unsigned count=R16(address+4);
                if(!count || count>256 || (uint64_t)6+14*count>bytes)failure=13;
                else if(icon_count==ICON_LIMIT)failure=8;
                else {
                    MoviesIconImage *images=calloc(count,sizeof *images);
                    if(!images)failure=8;
                    unsigned selected=0,best=0xffffffffu;
                    for(unsigned i=0;images && i<count && !failure;i++) {
                        uint32_t at=address+6+14*i;MoviesIconImage *im=&images[i];uint16_t image_lang;
                        im->width=R8(at)?R8(at):256;im->height=R8(at+1)?R8(at+1):256;
                        im->planes=R16(at+4);im->bpp=R16(at+6);im->id=R16(at+12);
                        if(R8(at+3) || !im->planes || !im->bpp || leaf(3,im->id,lang,&im->guest_address,&im->bytes,&image_lang)!=1 || im->bytes!=R32(at+8) || !valid_dib(im)) { failure=13;break; }
                        /* Default virtual icon metric32x32; prefer closest size,
                         * then highest depth. Retain all image bytes for renderers. */
                        unsigned distance=(unsigned)abs((int)im->width-32)+(unsigned)abs((int)im->height-32);
                        unsigned score=distance*256+(255-(im->bpp>255?255:im->bpp));
                        if(score<best) { best=score;selected=i; }
                    }
                    if(failure)free(images);
                    else {
                        result=ICON_BASE+icon_count;
                        icons[icon_count++]=(MoviesIconResource){result,(uint16_t)id,lang,(uint16_t)count,(uint16_t)selected,images};
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&guard);if(failure)movies_set_last_error(failure);ret(c,2,result);
}
static void load_cursor(CPU *c) {
    uint32_t module=R32(c->esp+4),id=R32(c->esp+8);
    if(module || id!=32512) { movies_set_last_error(module?50:1814);ret(c,2,0);return; }
    ret(c,2,CURSOR_HANDLE);
}
static void stock_object(CPU *c) {
    if(R32(c->esp+4)!=0) { movies_set_last_error(50);ret(c,1,0);return; }
    ret(c,1,BRUSH_HANDLE);
}
/* ---- Generic resources: FindResourceA/W, LoadResource, LockResource, SizeofResource
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-findresourcea
 * https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadresource
 * HRSRC is the guest address of the IMAGE_RESOURCE_DATA_ENTRY; HGLOBAL and the
 * locked pointer are the mapped image bytes (resources are never unloaded).
 * Names/types: MAKEINTRESOURCE ids, "#123" decimal ids, or case-insensitive
 * strings (directory names are compared ASCII-uppercased). Language preference:
 * neutral, then en-US, then the lowest-ranked other entry. */
typedef struct ResName { int is_id; uint32_t id,length; uint16_t text[256]; } ResName;
static int res_name(uint32_t p,int wide,ResName *out) {
    memset(out,0,sizeof *out);
    if(p<=0xffffu) { if(!p)return 0;out->is_id=1;out->id=p;return 1; }
    uint32_t n=0;
    for(;;n++) {
        if(n>=256 || !guest_range(p+n*(wide?2:1),wide?2:1))return 0;
        uint16_t ch=wide?R16(p+n*2):R8(p+n);
        if(!ch)break;
        if(ch>='a' && ch<='z')ch-=32;
        out->text[n]=ch;
    }
    if(!n)return 0;
    if(out->text[0]=='#') {
        uint32_t v=0;if(n<2)return 0;
        for(uint32_t i=1;i<n;i++){if(out->text[i]<'0'||out->text[i]>'9'||v>0xffffu)return 0;v=v*10+(out->text[i]-'0');}
        if(!v || v>0xffffu)return 0;out->is_id=1;out->id=v;return 1;
    }
    out->length=n;return 1;
}
static int named_entry(uint32_t dir,const ResName *want,uint32_t *target) {
    if(want->is_id)return entry(dir,want->id,target);
    if(!resource_range(dir,16))return -1;
    uint32_t count=(uint32_t)R16(ra(dir)+12)+R16(ra(dir)+14);
    if(count>4096 || !resource_range(dir+16,count*8))return -1;
    for(uint32_t i=0;i<count;i++) {
        uint32_t at=ra(dir+16+i*8),name=R32(at);
        if(!(name&0x80000000u))continue;
        uint32_t off=name&0x7fffffffu;if(!resource_range(off,2))return -1;
        uint32_t length=R16(ra(off));if(!resource_range(off+2,length*2))return -1;
        if(length!=want->length)continue;
        uint32_t k=0;
        for(;k<length;k++){uint16_t ch=R16(ra(off+2+k*2));if(ch>='a'&&ch<='z')ch-=32;if(ch!=want->text[k])break;}
        if(k==length){*target=R32(at+4);return 1;}
    }
    return 0;
}
static int data_entry(uint32_t hrsrc,uint32_t *address,uint32_t *bytes) {
    if(!base || !res_size || hrsrc<base+res_rva || hrsrc-(base+res_rva)>res_size || ((hrsrc-base-res_rva)&3))return 0;
    uint32_t off=hrsrc-base-res_rva;if(!resource_range(off,16))return 0;
    uint32_t rva=R32(hrsrc),length=R32(hrsrc+4);
    if(!length || !image_range(rva,length))return 0;
    *address=base+rva;*bytes=length;return 1;
}
static void find_resource(CPU *c,int wide) {
    uint32_t module=R32(c->esp+4),name_p=R32(c->esp+8),type_p=R32(c->esp+12),result=0,failure=0;
    ResName name,type;
    pthread_mutex_lock(&guard);
    if(!base || (module && module!=base))failure=126;
    else if(!res_name(name_p,wide,&name) || !res_name(type_p,wide,&type))failure=87;
    else if(!res_rva || !res_size)failure=1812; /*ERROR_RESOURCE_DATA_NOT_FOUND*/
    else {
        uint32_t t,n;int found=named_entry(0,&type,&t);
        if(found!=1 || !(t&0x80000000u))failure=found<0||found==1?13:1813;
        else if((found=named_entry(t&0x7fffffffu,&name,&n))!=1 || !(n&0x80000000u))failure=found<0||found==1?13:1814;
        else {
            uint32_t dir=n&0x7fffffffu,count=0;
            if(!resource_range(dir,16))failure=13;
            else count=(uint32_t)R16(ra(dir)+12)+R16(ra(dir)+14);
            if(!failure && (count>4096 || !resource_range(dir+16,count*8)))failure=13;
            if(!failure && !count)failure=1815;
            int rank=-1;uint32_t chosen=0;
            for(uint32_t i=0;!failure && i<count;i++) {
                uint32_t at=ra(dir+16+i*8),l=R32(at),d=R32(at+4);
                if(l>0xffff || (d&0x80000000u)){failure=13;break;}
                int r=l==0?3:l==0x409?2:1;
                if(r>rank){rank=r;chosen=d;}
            }
            uint32_t address,bytes;
            if(!failure && (!resource_range(chosen,16) || !data_entry(ra(chosen),&address,&bytes)))failure=13;
            if(!failure)result=ra(chosen);
        }
    }
    pthread_mutex_unlock(&guard);
    if(failure)movies_set_last_error(failure);ret(c,3,result);
}
static void find_resource_a(CPU *c) { find_resource(c,0); }
static void find_resource_w(CPU *c) { find_resource(c,1); }
static void load_resource(CPU *c) {
    uint32_t module=R32(c->esp+4),hrsrc=R32(c->esp+8),address=0,bytes;
    pthread_mutex_lock(&guard);
    int ok=base && (!module || module==base) && data_entry(hrsrc,&address,&bytes);
    pthread_mutex_unlock(&guard);
    if(!ok){movies_set_last_error(module && module!=base?126:6);address=0;}
    ret(c,2,address);
}
static void lock_resource(CPU *c) { ret(c,1,R32(c->esp+4)); }
static void sizeof_resource(CPU *c) {
    uint32_t module=R32(c->esp+4),hrsrc=R32(c->esp+8),address,bytes=0;
    pthread_mutex_lock(&guard);
    int ok=base && (!module || module==base) && data_entry(hrsrc,&address,&bytes);
    pthread_mutex_unlock(&guard);
    if(!ok){movies_set_last_error(module && module!=base?126:6);bytes=0;}
    ret(c,2,bytes);
}
ImportHandler *movies_resource_handlers(uint32_t *count) {
    static ImportHandler h[]={{"USER32.DLL!LoadIconA",load_icon},{"USER32.DLL!LoadCursorA",load_cursor},{"GDI32.DLL!GetStockObject",stock_object},
        {"KERNEL32.DLL!FindResourceA",find_resource_a},{"KERNEL32.DLL!FindResourceW",find_resource_w},{"KERNEL32.DLL!LoadResource",load_resource},
        {"KERNEL32.DLL!LockResource",lock_resource},{"KERNEL32.DLL!SizeofResource",sizeof_resource}};
    *count=sizeof h/sizeof h[0];return h;
}
