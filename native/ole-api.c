/* OLE32/OLEAUT32 utility imports. Contracts:
 * https://learn.microsoft.com/en-us/windows/win32/api/oleauto/nf-oleauto-sysallocstring
 * https://learn.microsoft.com/en-us/windows/win32/api/oleauto/nf-oleauto-sysfreestring
 * https://learn.microsoft.com/en-us/windows/win32/api/oleauto/nf-oleauto-variantinit
 * https://learn.microsoft.com/en-us/windows/win32/api/oleauto/nf-oleauto-variantclear
 * https://learn.microsoft.com/en-us/windows/win32/api/objbase/nf-objbase-cofreeunusedlibraries
 * No running object table or monikers exist in this single virtual process.
 */
#include "ole-api.h"
#include "heap-api.h"
#ifndef MOVIES_OLE_SPACE
#define MOVIES_OLE_SPACE GUEST_SPACE
#endif
#define ARG(n) R32(c->esp+4+4*(n))
#define E_NOTIMPL 0x80004001u
#define E_INVALIDARG 0x80070057u
#define DISP_E_BADVARTYPE 0x80020008u
static void ret(CPU *c,unsigned n,uint32_t v) { c->eax=v;c->esp+=4*(n+1); }
static int range(uint32_t p,uint32_t n) { return p && (uint64_t)p+n<=MOVIES_OLE_SPACE; }
/* No in-process COM servers are loaded from disk, so nothing can be unloaded. */
static void free_unused_libraries(CPU *c) { c->esp+=4; }
static void running_object_table(CPU *c) {
    uint32_t out=ARG(1);if(range(out,4))W32(out,0);ret(c,2,E_NOTIMPL);
}
static void item_moniker(CPU *c) {
    uint32_t out=ARG(2);if(range(out,4))W32(out,0);ret(c,3,E_NOTIMPL);
}
/* OLEAUT32 #2. NULL input yields NULL; OOM yields NULL. */
static void sys_alloc_string(CPU *c) {
    uint32_t text=ARG(0),units=0;
    if(!text){ret(c,1,0);return;}
    while(1){if(!range(text+units*2,2) || units>=0x3fffffffu){ret(c,1,0);return;}if(!R16(text+units*2))break;units++;}
    uint64_t bytes=(uint64_t)units*2+6;
    uint32_t block=bytes<=(32u<<20)?movies_backend_alloc((uint32_t)bytes):0;
    if(!block){ret(c,1,0);return;}
    W32(block,units*2);memmove(HP(block+4),HP(text),(size_t)units*2);W16(block+4+units*2,0);
    ret(c,1,block+4);
}
/* OLEAUT32 #6. Only BSTRs from this allocator are released; others are ignored. */
static void sys_free_string(CPU *c) {
    uint32_t bstr=ARG(0);if(bstr>=4)movies_backend_free(bstr-4);c->esp+=8;
}
/* OLEAUT32 #8: vt=VT_EMPTY, reserved words cleared; data is left untouched. */
static void variant_init(CPU *c) {
    uint32_t v=ARG(0);if(range(v,8)){W16(v,0);W16(v+2,0);W16(v+4,0);W16(v+6,0);}c->esp+=8;
}
/* OLEAUT32 #9: releases owned BSTR/IUnknown/IDispatch payloads, then VT_EMPTY.
 * Arrays, records and nested VARIANT ownership are not modeled. */
static void variant_clear(CPU *c) {
    uint32_t v=ARG(0);
    if(!range(v,16)){ret(c,1,E_INVALIDARG);return;}
    uint16_t vt=R16(v);
    if(!(vt&0x4000u)) {
        if(vt&0x2000u){ret(c,1,DISP_E_BADVARTYPE);return;} /*SAFEARRAY ownership not modeled*/
        switch(vt&0xfffu) {
        case 0:case 1:case 2:case 3:case 4:case 5:case 6:case 7:case 10:case 11:case 14:
        case 16:case 17:case 18:case 19:case 20:case 21:case 22:case 23:break;
        case 8:{uint32_t b=R32(v+8);if(b>=4)movies_backend_free(b-4);}break;
        case 9:case 13: {
            uint32_t object=R32(v+8);
            if(object) {
                uint32_t table=range(object,4)?R32(object):0,release=range(table+8,4)?R32(table+8):0;
                GuestFn fn=release?lookup_function(release):NULL;
                if(!fn){guest_trap(c,release,"VariantClear interface Release is not callable");return;}
                CPU nested=*c;
                if(nested.esp<256 || !range(nested.esp-256,256)){guest_trap(c,R32(c->esp),"VariantClear stack outside guest memory");return;}
                nested.esp-=128;uint32_t before=nested.esp;
                guest_call(&nested,fn,1,&object);
                if(nested.esp!=before){guest_trap(c,release,"Release violated stdcall stack contract");return;}
            }
            break;
        }
        default:ret(c,1,DISP_E_BADVARTYPE);return;
        }
    }
    W16(v,0);ret(c,1,0);
}
ImportHandler *movies_ole_handlers(uint32_t *count) {
    static ImportHandler h[]={
        {"OLE32.DLL!CoFreeUnusedLibraries",free_unused_libraries},
        {"OLE32.DLL!GetRunningObjectTable",running_object_table},
        {"OLE32.DLL!CreateItemMoniker",item_moniker},
        {"OLEAUT32.DLL!#2",sys_alloc_string},{"OLEAUT32.DLL!#6",sys_free_string},
        {"OLEAUT32.DLL!#8",variant_init},{"OLEAUT32.DLL!#9",variant_clear}};
    *count=sizeof h/sizeof h[0];return h;
}
