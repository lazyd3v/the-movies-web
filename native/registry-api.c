/* Private Win32 registry subset. Contracts:
 * https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regqueryvalueexa
 * .../nf-winreg-regcreatekeyexa, nf-winreg-regsetvalueexa, nf-winreg-regopenkeyexa
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getusernamea
 * Reg* returns LSTATUS, not BOOL/HRESULT; it does not change thread LastError.
 * String data is stored as explicit-length UTF16, preserving missing terminators.
 */
#include "registry-api.h"
#include "startup-api.h"
#include "text-api.h"
#include <pthread.h>
#include <stdlib.h>
#ifndef MOVIES_REGISTRY_SPACE
#define MOVIES_REGISTRY_SPACE GUEST_SPACE
#endif
#define ARG(n) R32(c->esp+4+4*(n))
#define ALL_ACCESS 0x000f003fu
enum { OK=0, NOT_FOUND=2, DENIED=5, BAD_HANDLE=6, NO_MEMORY=8, UNSUPPORTED=50, INVALID=87, MORE_DATA=234 };
typedef struct Value { uint16_t *name; uint8_t *data; uint32_t bytes,type; struct Value *next; } Value;
typedef struct Key { uint16_t *name; struct Key *children,*next; Value *values; } Key;
typedef struct Handle { uint32_t id,rights; Key *key; struct Handle *next; } Handle;
static Key roots[5];
static Handle *handles;
static uint32_t next_handle=0x73000000u;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static void ret(CPU *c,unsigned n,uint32_t result) { c->eax=result;c->esp+=4*(n+1); }
static int range(uint32_t p,uint64_t n) { return p && (uint64_t)p+n<=MOVIES_REGISTRY_SPACE; }
static int string_type(uint32_t type) { return type==1 || type==2 || type==7; }
static int valid_type(uint32_t type) { return type<=5 || type==7 || type==11; }
static Key *root(uint32_t id) {
    if(id>=0x80000000u && id<=0x80000003u)return &roots[id-0x80000000u];
    return id==0x80000005u?&roots[4]:NULL;
}
static Key *key_for(uint32_t id,uint32_t *rights) {
    Key *key=root(id); if(key) { *rights=ALL_ACCESS;return key; }
    for(Handle *h=handles;h;h=h->next)if(h->id==id) { *rights=h->rights;return h->key; }
    return NULL;
}
static int equal(const uint16_t *a,const uint16_t *b) {
    while(*a && *b)if(movies_western_upper(*a++)!=movies_western_upper(*b++))return 0;
    return *a==*b;
}
static uint16_t *copy_name(const uint16_t *s) {
    size_t n=0;while(s[n])n++;
    uint16_t *p=malloc((n+1)*sizeof *p);if(p)memcpy(p,s,(n+1)*sizeof *p);return p;
}
static uint32_t name_from_guest(uint32_t address,int wide,uint16_t **out) {
    size_t n=0; unsigned width=wide?2:1;
    if(address)for(;;n++) {
        if(n>32767 || !range(address,(n+1)*width))return INVALID;
        if(!(wide?R16(address+(uint32_t)n*2):R8(address+(uint32_t)n)))break;
    }
    uint16_t *s=malloc((n+1)*sizeof *s);if(!s)return NO_MEMORY;
    for(size_t i=0;i<n;i++)s[i]=wide?R16(address+(uint32_t)i*2):movies_cp1252_decode(R8(address+(uint32_t)i));
    s[n]=0;*out=s;return OK;
}
static uint32_t requested_rights(uint32_t rights) {
    /* 32-bit view is native; reject 64-bit view, security-system and generic masks.
     * Standard rights are retained, not interpreted as emulated host ACLs. */
    return rights & ~(ALL_ACCESS|0x200u) ? UNSUPPORTED:OK;
}
static uint32_t add_handle(Key *key,uint32_t rights,uint32_t *result) {
    if(next_handle==0x74000000u)return NO_MEMORY;
    Handle *h=malloc(sizeof *h);if(!h)return NO_MEMORY;
    *h=(Handle){next_handle++,rights,key,handles};handles=h;*result=h->id;return OK;
}
static uint32_t walk(Key *key,uint16_t *path,int create,Key **result,uint32_t *disposition) {
    unsigned depth=0;uint16_t *part=path;
    *disposition=2;
    while(*part) {
        if(++depth>32)return INVALID;
        uint16_t *end=part;while(*end && *end!='\\')end++;
        if(end==part || end-part>255)return INVALID;
        uint16_t saved=*end;*end=0;
        Key *child=key->children;while(child && !equal(child->name,part))child=child->next;
        if(!child && create) {
            child=calloc(1,sizeof *child);
            if(child)child->name=copy_name(part);
            if(!child || !child->name) { free(child);*end=saved;return NO_MEMORY; }
            child->next=key->children;key->children=child;*disposition=1;
        }
        *end=saved;
        if(!child)return NOT_FOUND;
        key=child;part=saved?end+1:end;
    }
    *result=key;return OK;
}
static void create_key(CPU *c) {
    uint32_t id=ARG(0),path=ARG(1),reserved=ARG(2),options=ARG(4),rights=ARG(5),security=ARG(6),out=ARG(7),disp=ARG(8);
    uint32_t error=OK,result=0,disposition=2,oldrights;uint16_t *name=NULL;
    if(reserved || !path || !range(out,4) || (disp && !range(disp,4)))error=INVALID;
    else if(options>1 || security)error=UNSUPPORTED;
    else if(!(error=requested_rights(rights)))error=name_from_guest(path,0,&name);
    if(!error) {
        pthread_mutex_lock(&mutex);
        Key *key=key_for(id,&oldrights);
        if(!key)error=BAD_HANDLE;
        else error=walk(key,name,1,&key,&disposition);
        if(!error)error=add_handle(key,rights,&result);
        pthread_mutex_unlock(&mutex);
    }
    free(name);
    if(!error) { W32(out,result);if(disp)W32(disp,disposition); }
    ret(c,9,error);
}
static void open_key(CPU *c,int wide,int legacy) {
    uint32_t id=ARG(0),path=ARG(1),options=legacy?0:ARG(2),rights=legacy?ALL_ACCESS:ARG(3),out=ARG(legacy?2:4);
    uint32_t error=OK,result=0,disposition,oldrights;uint16_t *name=NULL;
    if(!range(out,4))error=INVALID;
    else if(options)error=UNSUPPORTED;
    else if(!(error=requested_rights(rights)))error=name_from_guest(path,wide,&name);
    if(!error) {
        pthread_mutex_lock(&mutex);
        Key *key=key_for(id,&oldrights);
        if(!key)error=BAD_HANDLE;
        else if(!name[0] && root(id))result=id;
        else {
            error=walk(key,name,0,&key,&disposition);
            if(!error)error=add_handle(key,rights,&result);
        }
        pthread_mutex_unlock(&mutex);
    }
    free(name);if(!error)W32(out,result);ret(c,legacy?3:5,error);
}
static void open_a(CPU *c) { open_key(c,0,0); }
static void open_w(CPU *c) { open_key(c,1,0); }
static void open_legacy(CPU *c) { open_key(c,0,1); }
static void close_key(CPU *c) {
    uint32_t id=ARG(0),error=BAD_HANDLE;
    pthread_mutex_lock(&mutex);
    if(root(id))error=OK;
    else for(Handle **p=&handles;*p;p=&(*p)->next)if((*p)->id==id) {
        Handle *old=*p;*p=old->next;free(old);error=OK;break;
    }
    pthread_mutex_unlock(&mutex);ret(c,1,error);
}
static Value *find_value(Key *key,const uint16_t *name) {
    for(Value *v=key->values;v;v=v->next)if(equal(v->name,name))return v;
    return NULL;
}
static void set_value(CPU *c) {
    uint32_t id=ARG(0),pname=ARG(1),reserved=ARG(2),type=ARG(3),data=ARG(4),bytes=ARG(5),rights,error=OK;
    uint16_t *name=NULL;uint8_t *stored=NULL;
    if(reserved || (bytes && !range(data,bytes)))error=INVALID;
    else if(!valid_type(type))error=UNSUPPORTED;
    else if(bytes>16u*1024*1024)error=NO_MEMORY;
    else error=name_from_guest(pname,0,&name);
    uint32_t size=string_type(type)?bytes*2:bytes;
    if(!error && size) {
        stored=malloc(size);if(!stored)error=NO_MEMORY;
        else if(string_type(type))for(uint32_t i=0;i<bytes;i++) {
            uint16_t v=movies_cp1252_decode(R8(data+i));stored[i*2]=(uint8_t)v;stored[i*2+1]=(uint8_t)(v>>8);
        }
        else memcpy(stored,HP(data),bytes);
    }
    if(!error) {
        pthread_mutex_lock(&mutex);Key *key=key_for(id,&rights);
        if(!key)error=BAD_HANDLE;
        else if(!(rights&2))error=DENIED;
        else {
            Value *value=find_value(key,name);
            if(!value) {
                value=calloc(1,sizeof *value);
                if(!value)error=NO_MEMORY;
                else { value->name=name;name=NULL;value->next=key->values;key->values=value; }
            }
            if(!error) { free(value->data);value->data=stored;stored=NULL;value->type=type;value->bytes=size; }
        }
        pthread_mutex_unlock(&mutex);
    }
    free(name);free(stored);ret(c,6,error);
}
static void query_value(CPU *c,int wide) {
    uint32_t id=ARG(0),pname=ARG(1),reserved=ARG(2),ptype=ARG(3),data=ARG(4),psize=ARG(5),rights,error=OK;
    uint16_t *name=NULL;
    if(reserved || (data && !psize) || (ptype && !range(ptype,4)) || (psize && !range(psize,4)))error=INVALID;
    else error=name_from_guest(pname,wide,&name);
    if(!error) {
        pthread_mutex_lock(&mutex);Key *key=key_for(id,&rights);
        Value *value=NULL;
        if(!key)error=BAD_HANDLE;
        else if(!(rights&1))error=DENIED;
        else if(!(value=find_value(key,name)))error=NOT_FOUND;
        if(!error) {
            uint32_t size=(!wide && string_type(value->type))?value->bytes/2:value->bytes;
            uint32_t capacity=psize?R32(psize):0;
            if(ptype)W32(ptype,value->type);
            if(psize)W32(psize,size);
            if(data && capacity<size)error=MORE_DATA;
            else if(data && size && !range(data,size))error=INVALID;
            else if(data && size) {
                if(!wide && string_type(value->type))for(uint32_t i=0;i<size;i++) {
                    uint16_t v=(uint16_t)(value->data[2*i]|value->data[2*i+1]<<8);
                    int byte=movies_cp1252_encode(v,1);W8(data+i,(uint8_t)(byte<0?'?':byte));
                }
                else memcpy(HP(data),value->data,size);
            }
        }
        pthread_mutex_unlock(&mutex);
    }
    free(name);ret(c,6,error);
}
static void query_a(CPU *c) { query_value(c,0); }
static void query_w(CPU *c) { query_value(c,1); }
static void delete_value(CPU *c) {
    uint32_t id=ARG(0),rights,error;uint16_t *name=NULL;
    error=name_from_guest(ARG(1),0,&name);
    if(!error) {
        pthread_mutex_lock(&mutex);Key *key=key_for(id,&rights);
        if(!key)error=BAD_HANDLE;
        else if(!(rights&2))error=DENIED;
        else {
            error=NOT_FOUND;
            for(Value **p=&key->values;*p;p=&(*p)->next)if(equal((*p)->name,name)) {
                Value *old=*p;*p=old->next;free(old->name);free(old->data);free(old);error=OK;break;
            }
        }
        pthread_mutex_unlock(&mutex);
    }
    free(name);ret(c,2,error);
}
static void username(CPU *c) {
    uint32_t data=ARG(0),size=ARG(1),error=OK;
    if(!range(size,4))error=INVALID;
    else { uint32_t capacity=R32(size);W32(size,7);
        if(capacity<7)error=122;
        else if(!range(data,7))error=INVALID;
        else memcpy(HP(data),"Player",7);
    }
    if(error)movies_set_last_error(error);ret(c,2,!error);
}
static void free_values(Value *v) { while(v) { Value *next=v->next;free(v->name);free(v->data);free(v);v=next; } }
static void free_keys(Key *k) { while(k) { Key *next=k->next;free_keys(k->children);free_values(k->values);free(k->name);free(k);k=next; } }
void movies_registry_init(const char *userdata_root) {
    (void)userdata_root;pthread_mutex_lock(&mutex);
    for(unsigned i=0;i<5;i++) { free_keys(roots[i].children);free_values(roots[i].values);memset(&roots[i],0,sizeof roots[i]); }
    while(handles) { Handle *next=handles->next;free(handles);handles=next; }
    /* Keep IDs monotonic even across explicit resets: stale handles stay invalid. */
    pthread_mutex_unlock(&mutex);
}
ImportHandler *movies_registry_handlers(uint32_t *count) {
    static ImportHandler handlers[]={
        {"ADVAPI32.DLL!RegCreateKeyExA",create_key}, {"ADVAPI32.DLL!RegOpenKeyA",open_legacy},
        {"ADVAPI32.DLL!RegOpenKeyExA",open_a}, {"ADVAPI32.DLL!RegOpenKeyExW",open_w},
        {"ADVAPI32.DLL!RegCloseKey",close_key}, {"ADVAPI32.DLL!RegSetValueExA",set_value},
        {"ADVAPI32.DLL!RegQueryValueExA",query_a}, {"ADVAPI32.DLL!RegQueryValueExW",query_w},
        {"ADVAPI32.DLL!RegDeleteValueA",delete_value}, {"ADVAPI32.DLL!GetUserNameA",username}};
    *count=sizeof handlers/sizeof handlers[0];return handlers;
}
