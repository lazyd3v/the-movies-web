/* Actual original A2C470 startup graph slice, not a general DirectShow engine.
 * Contracts: Microsoft Learn CoInitializeEx, IFilterGraph::AddFilter,
 * ICaptureGraphBuilder2::SetFiltergraph, IEnumFilters.
 * Unknown methods trap; writer creation requires a real host encoder session.
 * UI-owner STA only; MTA/marshalling and guest-owned filters are not implemented.
 */
#include "media-com.h"
#include "heap-api.h"
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifndef MOVIES_MEDIA_SPACE
#define MOVIES_MEDIA_SPACE GUEST_SPACE
#endif
#define LIMIT 64u
#define FILTERS 32u
#define E_FAIL 0x80004005u
#define E_POINTER 0x80004003u
#define E_INVALIDARG 0x80070057u
#define E_NOINTERFACE 0x80004002u
#define E_UNEXPECTED 0x8000ffffu
#define E_OUTOFMEMORY 0x8007000eu
#define E_NOTIMPL 0x80004001u
#define NOTINIT 0x800401f0u
#define NOTREGISTERED 0x80040154u
#define WRONGTHREAD 0x8001010eu
#define ARG(n) R32(c->esp+4+4*(n))
enum { GRAPH=1,CAPTURE,WRITER,ENUMERATOR,MANAGER,PROFILE,CONFIG_TABLE,READER,HEADER_TABLE,PROPS,SAMPLE };
typedef struct Profile {
    uint32_t version,streams,video_number,audio_number;
    uint16_t name[512],description[512];
    MoviesMediaProfile media;
} Profile;
typedef struct Object {
    uint32_t address,refs,kind,token,graph,generation,index;
    uint32_t filters[FILTERS],count;
    uint16_t name[128];
    Profile *profile;
    MoviesMediaPCM pcm;
    uint32_t stream,sample_data,sample_length,sample_capacity;
    int native_reader;
    int (*configure_backend)(uint32_t,const MoviesMediaProfile *);
    int (*release_backend)(uint32_t);
} Object;
static Object objects[LIMIT];
static uint32_t sequence,apartment_depth;
static MoviesMediaReaderHost reader_host;
static int close_reader(CPU*,Object*);
static int initialized;static pthread_t owner;static MoviesMediaHost host;
static const uint8_t id_unknown[16]={0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0xc0,0x0,0x0,0x0,0x0,0x0,0x0,0x46};
static const uint8_t id_graph[16]={0xa9,0x68,0xa8,0x56,0xd4,0xa,0xce,0x11,0xb0,0x3a,0x0,0x20,0xaf,0xb,0xa7,0x70};
static const uint8_t id_filtergraph[16]={0x9f,0x68,0xa8,0x56,0xd4,0xa,0xce,0x11,0xb0,0x3a,0x0,0x20,0xaf,0xb,0xa7,0x70};
static const uint8_t id_capture[16]={0xe0,0xa4,0xe5,0x93,0x50,0x2d,0xd2,0x11,0xab,0xfa,0x0,0xa0,0xc9,0xc6,0xe3,0x8d};
static const uint8_t id_filter[16]={0x95,0x68,0xa8,0x56,0xd4,0xa,0xce,0x11,0xb0,0x3a,0x0,0x20,0xaf,0xb,0xa7,0x70};
static const uint8_t id_persist[16]={0xc,0x1,0x0,0x0,0x0,0x0,0x0,0x0,0xc0,0x0,0x0,0x0,0x0,0x0,0x0,0x46};
static const uint8_t id_mediafilter[16]={0x99,0x68,0xa8,0x56,0xd4,0xa,0xce,0x11,0xb0,0x3a,0x0,0x20,0xaf,0xb,0xa7,0x70};
static const uint8_t id_enumfilters[16]={0x93,0x68,0xa8,0x56,0xd4,0xa,0xce,0x11,0xb0,0x3a,0x0,0x20,0xaf,0xb,0xa7,0x70};
static const uint8_t id_class_graph[16]={0xb3,0xeb,0x36,0xe4,0x4f,0x52,0xce,0x11,0x9f,0x53,0x0,0x20,0xaf,0xb,0xa7,0x70};
static const uint8_t id_class_capture[16]={0xe1,0xb6,0x87,0xbf,0x27,0x8c,0xd0,0x11,0xb3,0xf0,0x0,0xaa,0x0,0x37,0x61,0xc5};
static const uint8_t id_class_writer[16]={0xe,0x22,0x23,0x7c,0xbb,0x55,0xd3,0x11,0x8b,0x16,0x0,0xc0,0x4f,0xb6,0xbd,0x3d};
static const uint8_t id_manager[16]={0xf2,0x79,0x66,0xd1,0xa0,0x6c,0x2d,0x47,0x8d,0x31,0x2f,0x5d,0x55,0xae,0xe1,0x55};
static const uint8_t id_profile[16]={0xdb,0x6b,0x40,0x96,0x2b,0x2b,0xd3,0x11,0xb3,0x6b,0x0,0xc0,0x4f,0x61,0x8,0xff};
static const uint8_t id_configwriter[16]={0x30,0x60,0x8,0x45,0xe4,0xf7,0x6a,0x48,0xb5,0x4,0x82,0x6b,0xb5,0x79,0x2a,0x3b};
static const uint8_t id_reader[16]={0x21,0xf1,0x97,0x93,0x5,0x77,0xc9,0x4d,0xb0,0x49,0x98,0xb6,0x98,0x18,0x84,0x14};
static const uint8_t id_header[16]={0xda,0x6b,0x40,0x96,0x2b,0x2b,0xd3,0x11,0xb3,0x6b,0x0,0xc0,0x4f,0x61,0x8,0xff};
static const uint8_t id_header2[16]={0x81,0x97,0xcf,0x15,0x4e,0x45,0x2e,0x48,0xb3,0x93,0x85,0xfa,0xe4,0x87,0xa8,0x10};
static const uint8_t id_header3[16]={0xe3,0x68,0xcc,0x15,0xcc,0x27,0xcd,0x4e,0xb2,0x22,0x3f,0x5d,0x2,0xd8,0xb,0xd5};
static const uint8_t id_props[16]={0xd7,0x6b,0x40,0x96,0x2b,0x2b,0xd3,0x11,0xb3,0x6b,0x0,0xc0,0x4f,0x61,0x8,0xff};
static const uint8_t id_mediaprops[16]={0xce,0x6b,0x40,0x96,0x2b,0x2b,0xd3,0x11,0xb3,0x6b,0x0,0xc0,0x4f,0x61,0x8,0xff};
static const uint8_t id_buffer[16]={0x24,0x35,0xcd,0xe1,0xd7,0x3,0xd2,0x11,0x9e,0xed,0x0,0x60,0x97,0xd2,0xd7,0xcf};
static const uint8_t id_audio[16]={0x61,0x75,0x64,0x73,0x0,0x0,0x10,0x0,0x80,0x0,0x0,0xaa,0x0,0x38,0x9b,0x71};
static const uint8_t id_pcm[16]={0x1,0x0,0x0,0x0,0x0,0x0,0x10,0x0,0x80,0x0,0x0,0xaa,0x0,0x38,0x9b,0x71};
static const uint8_t id_waveformat[16]={0x81,0x9f,0x58,0x5,0x56,0xc3,0xce,0x11,0xbf,0x1,0x0,0xaa,0x0,0x55,0x59,0x5a};
static int range(uint32_t p,uint32_t n){return p&&(uint64_t)p+n<=MOVIES_MEDIA_SPACE;}
static void ret(CPU*c,unsigned n,uint32_t v){c->eax=v;c->esp+=4*(n+1);}
static int on_owner(void){return initialized&&pthread_equal(owner,pthread_self());}
static Object *lookup(uint32_t p){for(unsigned i=0;i<LIMIT;i++)if((objects[i].address==p||((objects[i].kind==WRITER||objects[i].kind==READER)&&objects[i].address+4==p))&&p)return &objects[i];return NULL;}
static Object *self(CPU*c){Object*o=on_owner()?lookup(ARG(0)):NULL;if(!o)guest_trap(c,R32(c->esp),"invalid media COM object/thread");return o;}
static Object *typed(CPU*c,unsigned kind){Object*o=self(c);if(o&&o->kind!=kind){guest_trap(c,R32(c->esp),"wrong media COM interface kind");return NULL;}return o;}
static int guid(uint32_t p,const uint8_t *id){return range(p,16)&&!memcmp(HP(p),id,16);}
static int supports(unsigned kind,uint32_t iid){return guid(iid,id_unknown)||
    (kind==GRAPH&&(guid(iid,id_graph)||guid(iid,id_filtergraph)))||
    (kind==CAPTURE&&guid(iid,id_capture))||
    (kind==WRITER&&(guid(iid,id_filter)||guid(iid,id_persist)||guid(iid,id_mediafilter)||guid(iid,id_configwriter)))||
    (kind==ENUMERATOR&&guid(iid,id_enumfilters))||
    (kind==MANAGER&&guid(iid,id_manager))||(kind==PROFILE&&guid(iid,id_profile))||
    (kind==READER&&(guid(iid,id_reader)||guid(iid,id_header)||guid(iid,id_header2)||guid(iid,id_header3)))||
    (kind==PROPS&&(guid(iid,id_props)||guid(iid,id_mediaprops)))||(kind==SAMPLE&&guid(iid,id_buffer));}
static Object *allocate(unsigned kind){
    if(sequence>=0xf000u/16)return NULL;
    for(unsigned i=0;i<LIMIT;i++)if(!objects[i].address){Object*o=&objects[i];memset(o,0,sizeof *o);
        o->address=MOVIES_MEDIA_BASE+0x1000+16*sequence++;o->refs=1;o->kind=kind;
        W32(o->address,MOVIES_MEDIA_BASE+(kind-1)*0x100);
        if(kind==WRITER)W32(o->address+4,MOVIES_MEDIA_BASE+0x600);
        if(kind==READER)W32(o->address+4,MOVIES_MEDIA_BASE+0x800);return o;}
    return NULL;
}
static uint32_t retain(CPU*c,Object*o){if(o->refs==UINT32_MAX){guest_trap(c,R32(c->esp),"media COM reference overflow");return 0;}return ++o->refs;}
#ifdef __EMSCRIPTEN__
EM_JS(uint32_t,writer_create_js,(),{
    if(typeof Module.moviesMediaCreateWriter!=='function'||typeof Module.moviesMediaReleaseWriter!=='function')return 0;
    var token=Module.moviesMediaCreateWriter();
    if(!Number.isInteger(token)||token<0||token>4294967295)throw new Error('Invalid media writer token');
    return token>>>0;
});
EM_JS(int,writer_release_js,(uint32_t token),{
    if(typeof Module.moviesMediaReleaseWriter!=='function')return 0;
    var result=Module.moviesMediaReleaseWriter(token>>>0);return result===true||result===1?1:0;
});
#endif
static uint32_t release_object(CPU*c,Object*o){
    if(--o->refs)return o->refs;
    if(o->kind==GRAPH){while(o->count){Object*f=lookup(o->filters[--o->count]);if(f){f->graph=0;release_object(c,f);}}}
    if((o->kind==CAPTURE||o->kind==ENUMERATOR)&&o->graph){Object*g=lookup(o->graph);if(g)release_object(c,g);}
    if(o->kind==WRITER&&o->token){int ok=0;
        if(o->release_backend)ok=o->release_backend(o->token);
#ifdef __EMSCRIPTEN__
        else ok=writer_release_js(o->token);
#endif
        if(!ok){guest_trap(c,R32(c->esp),"media writer backend failed to release resource");return 0;}
    }
    if(o->kind==READER&&!close_reader(c,o)){guest_trap(c,R32(c->esp),"media reader backend failed release");return 0;}
    if(o->kind==SAMPLE&&o->sample_data&&!movies_backend_free(o->sample_data)){guest_trap(c,R32(c->esp),"media sample allocation ownership lost");return 0;}
    free(o->profile);W32(o->address,0);W32(o->address+4,0);memset(o,0,sizeof *o);return 0;
}
int movies_media_set_host(const MoviesMediaHost *provider){
    if(initialized&&!on_owner())return 0;
    for(unsigned i=0;i<LIMIT;i++)if(objects[i].address&&objects[i].kind==WRITER)return 0;
    if(provider&&(!provider->create_writer||!provider->release_writer))return 0;
    host=provider?*provider:(MoviesMediaHost){0};return 1;
}
static void query(CPU*c){Object*o=self(c);if(!o)return;uint32_t iid=ARG(1),out=ARG(2);
    if(!range(out,4)){ret(c,3,E_POINTER);return;}W32(out,0);
    if(!range(iid,16)){ret(c,3,E_POINTER);return;}
    if(!supports(o->kind,iid)){ret(c,3,E_NOINTERFACE);return;}retain(c,o);W32(out,o->address+((o->kind==WRITER&&guid(iid,id_configwriter))||(o->kind==READER&&(guid(iid,id_header)||guid(iid,id_header2)||guid(iid,id_header3)))?4:0));ret(c,3,0);
}
static void addref(CPU*c){Object*o=self(c);if(o)ret(c,1,retain(c,o));}
static void release(CPU*c){Object*o=self(c);if(o)ret(c,1,release_object(c,o));}
static void unsupported(CPU*c){guest_trap(c,R32(c->esp),"unsupported media COM method; no simulated media success");}
static uint32_t initialize(uint32_t reserved,uint32_t flags){
    if(!on_owner())return WRONGTHREAD;
    if(reserved||flags&~0xeu)return E_INVALIDARG;
    if(!(flags&2))return apartment_depth?0x80010106u:E_NOTIMPL;
    if(apartment_depth==UINT32_MAX)return E_OUTOFMEMORY;
    return apartment_depth++?1:0;
}
static unsigned diagnostic_count;
static void diagnostic_return(CPU*c,const char*operation,unsigned argc,uint32_t result){
    if(diagnostic_count++<64){
        fprintf(stderr,"[media-com] %s hr=%08x",operation,result);
        for(unsigned i=0;i<argc&&i<5;i++)fprintf(stderr," arg%u=%08x",i,ARG(i));
        if(!strcmp(operation,"CoCreateInstance")){
            unsigned selectors[]={0,3};for(unsigned i=0;i<2;i++){uint32_t p=ARG(selectors[i]);if(range(p,16)){
                fprintf(stderr," %s=%08x-%04x-%04x-",i?"iid":"clsid",R32(p),R16(p+4),R16(p+6));
                for(unsigned j=8;j<16;j++){if(j==10)fputc('-',stderr);fprintf(stderr,"%02x",R8(p+j));}
            }}
        }
        fputc('\n',stderr);
    }
    ret(c,argc,result);
}
static void coinit(CPU*c){diagnostic_return(c,"CoInitialize",1,initialize(ARG(0),2));}
static void coinitex(CPU*c){diagnostic_return(c,"CoInitializeEx",2,initialize(ARG(0),ARG(1)));}
static void couninit(CPU*c){if(!on_owner()){guest_trap(c,R32(c->esp),"CoUninitialize unsupported thread");return;}if(apartment_depth)--apartment_depth;c->esp+=4;}
static void cocreate(CPU*c){
    uint32_t cls=ARG(0),outer=ARG(1),context=ARG(2),iid=ARG(3),out=ARG(4),kind=0;
    if(!range(out,4)){diagnostic_return(c,"CoCreateInstance",5,E_POINTER);return;}W32(out,0);
    if(!on_owner()){diagnostic_return(c,"CoCreateInstance",5,WRONGTHREAD);return;}if(!apartment_depth){diagnostic_return(c,"CoCreateInstance",5,NOTINIT);return;}
    if(!range(cls,16)||!range(iid,16)){diagnostic_return(c,"CoCreateInstance",5,E_POINTER);return;}
    if(outer){diagnostic_return(c,"CoCreateInstance",5,0x80040110u);return;} /*CLASS_E_NOAGGREGATION*/
    if(context!=1){diagnostic_return(c,"CoCreateInstance",5,E_NOTIMPL);return;}
    if(guid(cls,id_class_graph))kind=GRAPH;else if(guid(cls,id_class_capture))kind=CAPTURE;else if(guid(cls,id_class_writer))kind=WRITER;
    if(!kind){diagnostic_return(c,"CoCreateInstance",5,NOTREGISTERED);return;}if(!supports(kind,iid)){diagnostic_return(c,"CoCreateInstance",5,E_NOINTERFACE);return;}
    Object*o=allocate(kind);if(!o){diagnostic_return(c,"CoCreateInstance",5,E_OUTOFMEMORY);return;}
    if(kind==WRITER){uint32_t token=0;int ok=0;
        if(host.create_writer){ok=host.create_writer(&token);o->release_backend=host.release_writer;o->configure_backend=host.configure_writer;}
#ifdef __EMSCRIPTEN__
        else {token=writer_create_js();ok=token!=0;}
#endif
        if(!ok||!token){W32(o->address,0);memset(o,0,sizeof *o);diagnostic_return(c,"CoCreateInstance",5,NOTREGISTERED);return;}o->token=token;
    }
    W32(out,o->address);diagnostic_return(c,"CoCreateInstance",5,0);
}
static int read_name(uint32_t p,uint16_t out[128]){
    if(!p){const char*s="ASF Enc/Writer";unsigned i=0;for(;s[i];i++)out[i]=(uint16_t)s[i];out[i]=0;return 1;}
    for(unsigned i=0;i<128;i++){if((uint64_t)p+i*2>UINT32_MAX||!range(p+i*2,2))return 0;out[i]=R16(p+i*2);if(!out[i])return 1;}return 0;
}
static int same_name(const uint16_t*a,const uint16_t*b){for(unsigned i=0;i<128;i++){if(a[i]!=b[i])return 0;if(!a[i])return 1;}return 1;}
static int used_name(Object*g,const uint16_t *name){for(unsigned i=0;i<g->count;i++){Object*f=lookup(g->filters[i]);if(f&&same_name(name,f->name))return 1;}return 0;}
static void set_graph(CPU*c){Object*b=typed(c,CAPTURE);if(!b)return;Object*g=lookup(ARG(1));
    if(!g||g->kind!=GRAPH){diagnostic_return(c,"SetFiltergraph",2,ARG(1)?E_NOINTERFACE:E_POINTER);return;}
    if(b->graph){diagnostic_return(c,"SetFiltergraph",2,E_UNEXPECTED);return;}retain(c,g);b->graph=g->address;diagnostic_return(c,"SetFiltergraph",2,0);
}
static void get_graph(CPU*c){Object*b=typed(c,CAPTURE);if(!b)return;uint32_t out=ARG(1);
    if(!range(out,4)){ret(c,2,E_POINTER);return;}W32(out,0);Object*g=lookup(b->graph);
    if(!g){g=allocate(GRAPH);if(!g){ret(c,2,E_OUTOFMEMORY);return;}b->graph=g->address;}
    retain(c,g);W32(out,g->address);ret(c,2,0);
}
static void add_filter(CPU*c){Object*g=typed(c,GRAPH);if(!g)return;Object*f=lookup(ARG(1));uint16_t name[128]={0};
    if(!f||f->kind!=WRITER){diagnostic_return(c,"AddFilter",3,ARG(1)?E_NOINTERFACE:E_POINTER);return;}
    if(f->graph){diagnostic_return(c,"AddFilter",3,E_FAIL);return;}
    if(g->count==FILTERS){diagnostic_return(c,"AddFilter",3,E_OUTOFMEMORY);return;}
    if(!read_name(ARG(2),name)){diagnostic_return(c,"AddFilter",3,E_INVALIDARG);return;}
    uint32_t hr=0;
    if(used_name(g,name)){
        unsigned len=0;while(name[len])len++;if(len>121)len=121;
        for(unsigned n=1;n<=FILTERS+1;n++){name[len]=' ';name[len+1]='0'+n/1000%10;name[len+2]='0'+n/100%10;name[len+3]='0'+n/10%10;name[len+4]='0'+n%10;name[len+5]=0;if(!used_name(g,name))break;}
        hr=0x4022du;
    }
    memcpy(f->name,name,sizeof name);f->graph=g->address;retain(c,f);g->filters[g->count++]=f->address;++g->generation;diagnostic_return(c,"AddFilter",3,hr);
}
static void remove_filter(CPU*c){Object*g=typed(c,GRAPH);if(!g)return;uint32_t address=ARG(1);
    if(!address){ret(c,2,E_POINTER);return;}
    for(unsigned i=0;i<g->count;i++)if(g->filters[i]==address){Object*f=lookup(address);memmove(g->filters+i,g->filters+i+1,(g->count-i-1)*4);--g->count;++g->generation;f->graph=0;release_object(c,f);ret(c,2,0);return;}ret(c,2,1);
}
static void find_filter(CPU*c){Object*g=typed(c,GRAPH);if(!g)return;uint32_t out=ARG(2);uint16_t name[128]={0};
    if(!range(out,4)){ret(c,3,E_POINTER);return;}W32(out,0);
    if(!ARG(1)||!read_name(ARG(1),name)){ret(c,3,E_POINTER);return;}
    for(unsigned i=0;i<g->count;i++){Object*f=lookup(g->filters[i]);if(same_name(name,f->name)){retain(c,f);W32(out,f->address);ret(c,3,0);return;}}ret(c,3,0x80040216u);
}
static void enum_filters(CPU*c){Object*g=typed(c,GRAPH);if(!g)return;uint32_t out=ARG(1);
    if(!range(out,4)){ret(c,2,E_POINTER);return;}W32(out,0);Object*e=allocate(ENUMERATOR);if(!e){ret(c,2,E_OUTOFMEMORY);return;}
    retain(c,g);e->graph=g->address;e->generation=g->generation;W32(out,e->address);ret(c,2,0);
}
static void enum_next(CPU*c){Object*e=typed(c,ENUMERATOR);if(!e)return;Object*g=lookup(e->graph);uint32_t count=ARG(1),out=ARG(2),fetched=ARG(3);
    if((count&&(!out||(uint64_t)out+4ull*count>MOVIES_MEDIA_SPACE))||(!fetched&&count!=1)||(fetched&&!range(fetched,4))){ret(c,4,E_POINTER);return;}
    if(fetched)W32(fetched,0);if(e->generation!=g->generation){ret(c,4,0x80040203u);return;}
    uint32_t n=0;while(n<count&&e->index<g->count){Object*f=lookup(g->filters[e->index++]);retain(c,f);W32(out+4*n++,f->address);}if(fetched)W32(fetched,n);ret(c,4,n==count?0:1);
}
static void enum_skip(CPU*c){Object*e=typed(c,ENUMERATOR);if(!e)return;Object*g=lookup(e->graph);uint32_t n=ARG(1);
    if(e->generation!=g->generation){ret(c,2,0x80040203u);return;}uint32_t available=g->count-e->index;e->index+=n<available?n:available;ret(c,2,n<=available?0:1);
}
static void enum_reset(CPU*c){Object*e=typed(c,ENUMERATOR);if(!e)return;Object*g=lookup(e->graph);e->index=0;e->generation=g->generation;ret(c,1,0);}
static void enum_clone(CPU*c){Object*e=typed(c,ENUMERATOR);if(!e)return;uint32_t out=ARG(1);if(!range(out,4)){ret(c,2,E_POINTER);return;}W32(out,0);
    Object*n=allocate(ENUMERATOR);if(!n){ret(c,2,E_OUTOFMEMORY);return;}n->graph=e->graph;n->generation=e->generation;n->index=e->index;retain(c,lookup(e->graph));W32(out,n->address);ret(c,2,0);
}
static void get_class(CPU*c){Object*f=typed(c,WRITER);if(!f)return;uint32_t out=ARG(1);if(!range(out,16)){ret(c,2,E_POINTER);return;}memcpy(HP(out),id_class_writer,16);ret(c,2,0);}
static void stopped(CPU*c){Object*f=typed(c,WRITER);if(f)ret(c,1,0);}
static void get_state(CPU*c){Object*f=typed(c,WRITER);if(!f)return;uint32_t out=ARG(2);if(!range(out,4)){ret(c,3,E_POINTER);return;}W32(out,0);ret(c,3,0);}
static void filter_info(CPU*c){Object*f=typed(c,WRITER);if(!f)return;uint32_t out=ARG(1);if(!range(out,260)){ret(c,2,E_POINTER);return;}memset(HP(out),0,260);
    for(unsigned i=0;i<128&&f->name[i];i++)W16(out+i*2,f->name[i]);Object*g=lookup(f->graph);if(g){retain(c,g);W32(out+256,g->address);}ret(c,2,0);
}
/* Strict bounded XML subset: UTF16, quoted attributes, predefined/numeric
 * entities, comments and matching tags. DTD/PI/namespaces and unknown profile
 * features reject explicitly. No substring matching or external entities. */
typedef struct XmlAttr {char name[40];uint16_t value[512];int used;} XmlAttr;
typedef struct XmlToken {char name[40];XmlAttr attrs[24];unsigned count;int close,empty;} XmlToken;
typedef struct Xml {uint16_t *data;unsigned at,length;} Xml;
static int xml_space(uint16_t ch){return ch==' '||ch=='\t'||ch=='\r'||ch=='\n';}
static void xml_ws(Xml*x){while(x->at<x->length&&xml_space(x->data[x->at]))x->at++;}
static int xml_char(uint32_t u){return u==9||u==10||u==13||(u>=32&&u<=0xd7ff)||(u>=0xe000&&u<=0xfffd)||(u>=0x10000&&u<=0x10ffff);}
static uint16_t *copy_guest_wide(uint32_t p,unsigned limit){
    uint16_t*out=calloc(limit+1,2);if(!out)return NULL;
    for(unsigned i=0;i<=limit;i++){
        if((uint64_t)p+2ull*i>UINT32_MAX||!range(p+2*i,2)){free(out);return NULL;}uint16_t ch=R16(p+2*i);out[i]=ch;
        if(!ch)return out;if(i==limit||(!xml_char(ch)&&!(ch>=0xd800&&ch<=0xdbff))){free(out);return NULL;}
        if(ch>=0xd800&&ch<=0xdbff){if(++i>=limit||(uint64_t)p+2ull*i>UINT32_MAX||!range(p+2*i,2)){free(out);return NULL;}ch=R16(p+2*i);if(ch<0xdc00||ch>0xdfff){free(out);return NULL;}out[i]=ch;}
    }free(out);return NULL;
}
static int xml_name(Xml*x,char out[40]){unsigned n=0;while(x->at<x->length){uint16_t c=x->data[x->at];int letter=(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_';if(!letter&&!(n&&((c>='0'&&c<='9')||c=='-'||c=='.')))break;if(n==39)return 0;out[n++]=(char)c;x->at++;}out[n]=0;return n!=0;}
static int xml_value(Xml*x,uint16_t out[512]){
    uint16_t quote=x->data[x->at++];if(quote!='\''&&quote!='"')return 0;unsigned n=0;
    while(x->at<x->length&&x->data[x->at]!=quote){uint32_t ch=x->data[x->at++];
        if(ch=='<')return 0;
        if(ch=='&'){char entity[20];unsigned k=0;while(x->at<x->length&&x->data[x->at]!=';'){if(k==19||x->data[x->at]>127)return 0;entity[k++]=(char)x->data[x->at++];}if(x->at==x->length)return 0;x->at++;entity[k]=0;
            if(!strcmp(entity,"amp"))ch='&';else if(!strcmp(entity,"lt"))ch='<';else if(!strcmp(entity,"gt"))ch='>';else if(!strcmp(entity,"quot"))ch='"';else if(!strcmp(entity,"apos"))ch='\'';
            else if(entity[0]=='#'){unsigned at=1,base=10;if(entity[at]=='x'){base=16;at++;}ch=0;if(!entity[at])return 0;for(;entity[at];at++){unsigned c=(unsigned char)entity[at],d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:99;if(d>=base||ch>(0x10ffff-d)/base)return 0;ch=ch*base+d;}if(!xml_char(ch))return 0;}else return 0;
        }else if(xml_space((uint16_t)ch)){if(ch=='\r'&&x->at<x->length&&x->data[x->at]=='\n')x->at++;ch=' ';}
        if(ch>0xffff){if(n>509)return 0;ch-=0x10000;out[n++]=(uint16_t)(0xd800+(ch>>10));out[n++]=(uint16_t)(0xdc00+(ch&1023));}
        else {if(n==511)return 0;out[n++]=(uint16_t)ch;}
    }if(x->at==x->length)return 0;x->at++;out[n]=0;return 1;
}
static int xml_next(Xml*x,XmlToken*t){
    memset(t,0,sizeof *t);
    for(;;){xml_ws(x);if(x->at==x->length)return 0;
        if(x->data[x->at]!='<')return -1;
        if(x->at+3<x->length&&x->data[x->at+1]=='!'&&x->data[x->at+2]=='-'&&x->data[x->at+3]=='-'){
            x->at+=4;int ended=0;while(x->at+2<x->length){if(x->data[x->at]=='-'&&x->data[x->at+1]=='-'){if(x->data[x->at+2]!='>')return -1;x->at+=3;ended=1;break;}x->at++;}if(!ended)return -1;continue;
        }break;
    }
    x->at++;if(x->at==x->length)return -1;if(x->data[x->at]=='?'||x->data[x->at]=='!')return -2;
    if(x->data[x->at]=='/'){t->close=1;x->at++;}if(!xml_name(x,t->name))return -1;
    for(;;){unsigned before=x->at;xml_ws(x);if(x->at==x->length)return -1;
        if(x->data[x->at]=='>'){x->at++;return 1;}
        if(!t->close&&x->data[x->at]=='/'&&x->at+1<x->length&&x->data[x->at+1]=='>'){x->at+=2;t->empty=1;return 1;}
        if(t->close||before==x->at||t->count==24)return -1;
        XmlAttr*a=&t->attrs[t->count];if(!xml_name(x,a->name))return -1;
        for(unsigned i=0;i<t->count;i++)if(!strcmp(a->name,t->attrs[i].name))return -1;
        xml_ws(x);if(x->at==x->length||x->data[x->at++]!='=')return -1;xml_ws(x);
        if(x->at==x->length||!xml_value(x,a->value))return -1;t->count++;
    }
}
static const uint16_t *attr(XmlToken*t,const char*name){for(unsigned i=0;i<t->count;i++)if(!strcmp(name,t->attrs[i].name)){t->attrs[i].used=1;return t->attrs[i].value;}return NULL;}
static int text_is(const uint16_t*s,const char*ascii,int fold){if(!s)return 0;for(;*s&&*ascii;s++,ascii++){unsigned a=*s,b=(unsigned char)*ascii;if(fold){if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;}if(a!=b)return 0;}return !*s&&!*ascii;}
static int number(const uint16_t*s,uint32_t*out){if(!s||!*s)return 0;uint32_t n=0;for(;*s;s++){if(*s<'0'||*s>'9'||n>(UINT32_MAX-(*s-'0'))/10)return 0;n=n*10+*s-'0';}*out=n;return 1;}
static int exact_optional(XmlToken*t,const char*n,const char*v){const uint16_t*s=attr(t,n);return !s||text_is(s,v,0);}
static int all_used(XmlToken*t){for(unsigned i=0;i<t->count;i++)if(!t->attrs[i].used)return 0;return 1;}
static void copy_text(uint16_t out[512],const uint16_t*s){if(!s){out[0]=0;return;}unsigned n=0;do{out[n]=s[n];}while(s[n++]&&n<512);}
enum {TAG_PROFILE=1,TAG_STREAM,TAG_MEDIA,TAG_VIDEO,TAG_BITMAP,TAG_WAVE,TAG_SOURCE,TAG_TARGET};
static unsigned tag(const char*s){const char*names[]={"","profile","streamconfig","wmmediatype","videoinfoheader","bitmapinfoheader","waveformatex","rcsource","rctarget"};for(unsigned i=1;i<9;i++)if(!strcmp(s,names[i]))return i;return 0;}
static uint32_t parse_profile(uint32_t address,Profile*out){
    uint16_t*data=copy_guest_wide(address,32767);if(!data)return E_INVALIDARG;XmlToken*t=calloc(1,sizeof *t);if(!t){free(data);return E_OUTOFMEMORY;}
    Xml x={data,0,0};while(data[x.length])x.length++;if(x.length&&data[0]==0xfeff)x.at=1;
    unsigned stack[8],depth=0,root=0,stream=0,media_seen=0,format_seen=0,bitmap_seen=0,source_seen=0,target_seen=0;uint32_t result=0;
    for(;;){int next=xml_next(&x,t);if(!next)break;if(next<0){result=next==-2?E_NOTIMPL:E_INVALIDARG;break;}
        unsigned id=tag(t->name),parent=depth?stack[depth-1]:0;if(!id){result=E_NOTIMPL;break;}
        if(t->close){if(!depth||parent!=id){result=E_INVALIDARG;break;}depth--;if(id==TAG_STREAM&&(!media_seen||!format_seen||(stream==1&&!bitmap_seen))){result=E_INVALIDARG;break;}continue;}
        if(!parent){if(root++||id!=TAG_PROFILE){result=E_INVALIDARG;break;}}
        else if(!((parent==TAG_PROFILE&&id==TAG_STREAM)||(parent==TAG_STREAM&&id==TAG_MEDIA)||(parent==TAG_MEDIA&&(id==TAG_VIDEO||id==TAG_WAVE))||(parent==TAG_VIDEO&&(id==TAG_BITMAP||id==TAG_SOURCE||id==TAG_TARGET)))){result=E_INVALIDARG;break;}
        if(id==TAG_PROFILE){uint32_t v;if(!number(attr(t,"version"),&v)||(v!=0x70000&&v!=0x80000&&v!=0x90000)){result=E_INVALIDARG;break;}out->version=v;copy_text(out->name,attr(t,"name"));copy_text(out->description,attr(t,"description"));if(!exact_optional(t,"storageformat","1")){result=E_NOTIMPL;break;}}
        else if(id==TAG_STREAM){const uint16_t*major=attr(t,"majortype");uint32_t n,bitrate;
            if(!number(attr(t,"streamnumber"),&n)||!n||n>65535||!number(attr(t,"bitrate"),&bitrate)||!bitrate){result=E_INVALIDARG;break;}
            if(text_is(major,"{73646976-0000-0010-8000-00aa00389b71}",1)){stream=1;if(out->video_number||out->audio_number==n){result=E_INVALIDARG;break;}out->video_number=n;out->media.video_bitrate=bitrate;}
            else if(text_is(major,"{73647561-0000-0010-8000-00aa00389b71}",1)){stream=2;if(out->audio_number||out->video_number==n){result=E_INVALIDARG;break;}out->audio_number=n;out->media.audio_bitrate=bitrate;}
            else{result=E_NOTIMPL;break;}out->streams++;media_seen=format_seen=bitmap_seen=source_seen=target_seen=0;attr(t,"streamname");attr(t,"inputname");if(!exact_optional(t,"bufferwindow","-1")){result=E_NOTIMPL;break;}}
        else if(id==TAG_MEDIA){if(media_seen++){result=E_INVALIDARG;break;}const uint16_t*sub=attr(t,"subtype");if(!text_is(sub,stream==1?"{32564d57-0000-0010-8000-00aa00389b71}":"{00000161-0000-0010-8000-00aa00389b71}",1)){result=E_NOTIMPL;break;}
            if(!exact_optional(t,"bfixedsizesamples",stream==1?"0":"1")||!exact_optional(t,"btemporalcompression",stream==1?"1":"0")||!exact_optional(t,"lsamplesize",stream==1?"0":"743")){result=E_NOTIMPL;break;}}
        else if(id==TAG_VIDEO){uint32_t bitrate,period;if(stream!=1||format_seen++||!number(attr(t,"dwbitrate"),&bitrate)||bitrate!=out->media.video_bitrate||!number(attr(t,"avgtimeperframe"),&period)||!period){result=E_INVALIDARG;break;}out->media.fps_numerator=10000000;out->media.fps_denominator=period;uint32_t a=10000000,b=period;while(b){uint32_t r=a%b;a=b;b=r;}out->media.fps_numerator/=a;out->media.fps_denominator/=a;if(!exact_optional(t,"dwbiterrorrate","0")){result=E_NOTIMPL;break;}}
        else if(id==TAG_BITMAP){if(bitmap_seen++||!number(attr(t,"biwidth"),&out->media.width)||!number(attr(t,"biheight"),&out->media.height)||!out->media.width||!out->media.height){result=E_INVALIDARG;break;}
            if(!text_is(attr(t,"bicompression"),"WMV2",0)||!exact_optional(t,"biplanes","1")||!exact_optional(t,"bibitcount","24")||!exact_optional(t,"bisizeimage","0")||!exact_optional(t,"bixpelspermeter","0")||!exact_optional(t,"biypelspermeter","0")||!exact_optional(t,"biclrused","0")||!exact_optional(t,"biclrimportant","0")){result=E_NOTIMPL;break;}}
        else if(id==TAG_WAVE){uint32_t format,average;if(stream!=2||format_seen++||!number(attr(t,"wFormatTag"),&format)||format!=353||!number(attr(t,"nChannels"),&out->media.channels)||!number(attr(t,"nSamplesPerSec"),&out->media.sample_rate)||!number(attr(t,"nAvgBytesPerSec"),&average)||(uint64_t)average*8!=out->media.audio_bitrate){result=E_INVALIDARG;break;}
            if(!exact_optional(t,"nBlockAlign","743")||!exact_optional(t,"wBitsPerSample","16")||!exact_optional(t,"codecdata","00000000010000000000")){result=E_NOTIMPL;break;}}
        else {if((id==TAG_SOURCE?source_seen++:target_seen++)){result=E_INVALIDARG;break;}if(!exact_optional(t,"left","0")||!exact_optional(t,"top","0")||!exact_optional(t,"right","0")||!exact_optional(t,"bottom","0")){result=E_NOTIMPL;break;}}
        if(!all_used(t)){result=E_NOTIMPL;break;}
        if(t->empty){if(id==TAG_STREAM){result=E_INVALIDARG;break;}}
        else {if(depth==8){result=E_INVALIDARG;break;}stack[depth++]=id;}
    }
    if(!result&&(depth||!root))result=E_INVALIDARG;free(t);free(data);return result;
}
static Object *new_profile(uint32_t version){Object*p=allocate(PROFILE);if(!p)return NULL;p->profile=calloc(1,sizeof *p->profile);if(!p->profile){W32(p->address,0);memset(p,0,sizeof *p);return NULL;}p->profile->version=version;return p;}
static void manager_factory(CPU*c){uint32_t out=ARG(0);if(!range(out,4)){ret(c,1,E_POINTER);return;}W32(out,0);if(!on_owner()){ret(c,1,WRONGTHREAD);return;}Object*p=allocate(MANAGER);if(!p){ret(c,1,E_OUTOFMEMORY);return;}W32(out,p->address);ret(c,1,0);}
static void empty_profile(CPU*c){if(!typed(c,MANAGER))return;uint32_t version=ARG(1),out=ARG(2);if(!range(out,4)){ret(c,3,E_POINTER);return;}W32(out,0);if(version!=0x70000&&version!=0x80000&&version!=0x90000){ret(c,3,E_INVALIDARG);return;}Object*p=new_profile(version);if(!p){ret(c,3,E_OUTOFMEMORY);return;}W32(out,p->address);ret(c,3,0);}
static void load_profile(CPU*c){if(!typed(c,MANAGER))return;uint32_t xml=ARG(1),out=ARG(2);if(!range(out,4)){ret(c,3,E_POINTER);return;}W32(out,0);if(!xml){ret(c,3,E_POINTER);return;}Object*p=new_profile(0);if(!p){ret(c,3,E_OUTOFMEMORY);return;}uint32_t result=parse_profile(xml,p->profile);if(result)release_object(c,p);else W32(out,p->address);ret(c,3,result);}
static void profile_version(CPU*c){Object*p=typed(c,PROFILE);if(!p)return;uint32_t out=ARG(1);if(!range(out,4)){ret(c,2,E_POINTER);return;}W32(out,p->profile->version);ret(c,2,0);}
static void profile_count(CPU*c){Object*p=typed(c,PROFILE);if(!p)return;uint32_t out=ARG(1);if(!range(out,4)){ret(c,2,E_POINTER);return;}W32(out,p->profile->streams);ret(c,2,0);}
static void profile_get_string(CPU*c,int description){Object*p=typed(c,PROFILE);if(!p)return;uint32_t out=ARG(1),size=ARG(2);if(!range(size,4)){ret(c,3,E_POINTER);return;}const uint16_t*s=description?p->profile->description:p->profile->name;uint32_t need=1;while(s[need-1])need++;uint32_t cap=R32(size);W32(size,need);if(!out){ret(c,3,0);return;}if(cap<need){ret(c,3,0xc00d07d3u);return;}if((uint64_t)out+2ull*need>MOVIES_MEDIA_SPACE){ret(c,3,E_POINTER);return;}for(unsigned i=0;i<need;i++)W16(out+2*i,s[i]);ret(c,3,0);}
static void profile_name(CPU*c){profile_get_string(c,0);}static void profile_description(CPU*c){profile_get_string(c,1);}
static void profile_set_string(CPU*c,int description){Object*p=typed(c,PROFILE);if(!p)return;uint16_t*s=copy_guest_wide(ARG(1),511);if(!s){ret(c,2,E_INVALIDARG);return;}copy_text(description?p->profile->description:p->profile->name,s);free(s);ret(c,2,0);}
static void profile_set_name(CPU*c){profile_set_string(c,0);}static void profile_set_description(CPU*c){profile_set_string(c,1);}
static int backend_profile(const Profile*p){const MoviesMediaProfile*m=&p->media;return p->streams==2&&p->video_number&&p->audio_number&&m->width==640&&m->height==480&&m->fps_numerator==25&&m->fps_denominator==1&&m->video_bitrate==1000000&&m->sample_rate==44100&&m->channels==2&&m->audio_bitrate==128000;}
#ifdef __EMSCRIPTEN__
EM_JS(int,configure_writer_js,(uint32_t token),{
 if(typeof Module.moviesMediaConfigureWriter!=='function')return 0;
 var profile={width:640,height:480,fpsNumerator:25,fpsDenominator:1,pixelFormat:'bgr24',videoCodec:'wmv2',videoBitrate:1000000,audioCodec:'wmav2',sampleRate:44100,channels:2,sampleFormat:'s16le',audioBitrate:128000};
 var result=Module.moviesMediaConfigureWriter(token>>>0,profile);return result===true||result===1?1:0;
});
#endif
static void configure_profile(CPU*c){Object*w=typed(c,WRITER);if(!w)return;Object*p=lookup(ARG(1));if(!p||p->kind!=PROFILE){ret(c,2,ARG(1)?E_NOINTERFACE:E_POINTER);return;}if(!backend_profile(p->profile)){ret(c,2,E_NOTIMPL);return;}if(w->profile){ret(c,2,E_UNEXPECTED);return;}
 Profile*copy=malloc(sizeof *copy);if(!copy){ret(c,2,E_OUTOFMEMORY);return;}*copy=*p->profile;int ok=0;if(w->configure_backend)ok=w->configure_backend(w->token,&copy->media);
#ifdef __EMSCRIPTEN__
 else if(!w->release_backend)ok=configure_writer_js(w->token);
#endif
 if(!ok){free(copy);ret(c,2,E_FAIL);return;}w->profile=copy;ret(c,2,0);
}
static void current_profile(CPU*c){Object*w=typed(c,WRITER);if(!w)return;uint32_t out=ARG(1);if(!range(out,4)){ret(c,2,E_POINTER);return;}W32(out,0);if(!w->profile){ret(c,2,E_FAIL);return;}Object*p=new_profile(w->profile->version);if(!p){ret(c,2,E_OUTOFMEMORY);return;}*p->profile=*w->profile;W32(out,p->address);ret(c,2,0);}
#define READER_MAX_SOURCE (32u*1024u*1024u)
#define READER_CHUNK 65536u
#define READER_MAX_TIME 36000000000ull
#define NS_INVALID_REQUEST 0xc00d002bu
#define NS_NO_MORE_SAMPLES 0xc00d0bcfu
int movies_media_set_reader_host(const MoviesMediaReaderHost*provider){
 if(initialized&&!on_owner())return 0;
 for(unsigned i=0;i<LIMIT;i++)if(objects[i].address&&objects[i].kind==READER)return 0;
 if(provider&&(!provider->available||!provider->create||!provider->append||!provider->open||!provider->read||!provider->set_range||!provider->release))return 0;
 reader_host=provider?*provider:(MoviesMediaReaderHost){0};return 1;
}
static uint32_t stream_call(CPU*c,uint32_t stream,unsigned slot,unsigned n,const uint32_t*extra){
 if(!range(stream,4)||!range(R32(stream),4*(slot+1))){guest_trap(c,R32(c->esp),"invalid guest media IStream vtable");return E_POINTER;}
 uint32_t address=R32(R32(stream)+4*slot);GuestFn fn=lookup_function(address);
 if(!fn){guest_trap(c,address,"untranslated original media IStream method");return E_NOTIMPL;}
 CPU nested=*c;if(n>5||nested.esp<256||!range(nested.esp-256,256)){guest_trap(c,address,"media callback guest stack invalid");return E_POINTER;}
 nested.esp-=128;uint32_t before=nested.esp,args[6]={stream};for(unsigned i=0;i<n;i++)args[i+1]=extra[i];guest_call(&nested,fn,(int)n+1,args);
 if(nested.esp!=before){guest_trap(c,address,"media IStream callback violated stdcall");return E_FAIL;}return nested.eax;
}
#ifdef __EMSCRIPTEN__
EM_JS(int,reader_available_js,(),{
 var names=['moviesMediaReaderAvailable','moviesMediaCreateReader','moviesMediaAppendReaderInput','moviesMediaOpenReader','moviesMediaReadReaderSample','moviesMediaSetReaderRange','moviesMediaReleaseReader'];
 return names.every(n=>typeof Module[n]==='function')&&Module.moviesMediaReaderAvailable()?1:0;
});
EM_JS(uint32_t,reader_create_js,(uint32_t bytes),{var token=Module.moviesMediaCreateReader(bytes);return Number.isInteger(token)&&token>0&&token<=4294967295?token>>>0:0;});
EM_JS(int,reader_append_js,(uint32_t token,uint32_t offset,const uint8_t * data,uint32_t bytes),{var r=Module.moviesMediaAppendReaderInput(token,offset,HEAPU8.slice(data,data+bytes));return r===true||r===1?1:0;});
EM_JS(int,reader_open_js,(uint32_t token,uint32_t * out),{
 var m=Module.moviesMediaOpenReader(token);if(!m||m.container!=='asf'||m.codec!=='wmav2'||m.format!=='S16LE')return 0;
 var fields=[m.sampleRate,m.channels,m.bitsPerSample,m.blockAlign,m.maxSampleBytes,m.audioStreamNumber,m.sourceStreamCount];
 if(fields.some(x=>!Number.isInteger(x)||x<0||x>4294967295)||!Number.isSafeInteger(m.containerDuration100ns)||m.containerDuration100ns<0||m.containerDuration100ns>36000000000)return 0;
 fields.forEach((x,i)=>HEAPU32[(out>>>2)+i]=x>>>0);HEAPU32[(out>>>2)+7]=m.containerDuration100ns>>>0;HEAPU32[(out>>>2)+8]=Math.floor(m.containerDuration100ns/4294967296);return 1;
});
EM_JS(int,reader_read_js,(uint32_t token,uint8_t * data,uint32_t max,uint32_t * out),{
 var s=Module.moviesMediaReadReaderSample(token,max);if(!s)return 0;if(s.eos===true)return 2;
 if(s.eos!==false||!(s.bytes instanceof Uint8Array)||!s.bytes.length||s.bytes.length>max||s.bytes.length%4||s.sampleRate!==44100||s.channels!==2||s.bitsPerSample!==16)return 0;
 if(!Number.isSafeInteger(s.start100ns)||!Number.isSafeInteger(s.duration100ns)||s.start100ns<0||s.duration100ns<=0||s.start100ns>36000000000||s.duration100ns>36000000000)return 0;
 HEAPU8.set(s.bytes,data);HEAPU32[out>>>2]=s.bytes.length;HEAPU32[(out>>>2)+1]=s.start100ns>>>0;HEAPU32[(out>>>2)+2]=Math.floor(s.start100ns/4294967296);HEAPU32[(out>>>2)+3]=s.duration100ns>>>0;HEAPU32[(out>>>2)+4]=Math.floor(s.duration100ns/4294967296);return 1;
});
EM_JS(int,reader_range_js,(uint32_t token,uint32_t slo,uint32_t shi,uint32_t dlo,uint32_t dhi,int to_end),{
 var r=Module.moviesMediaSetReaderRange(token,(shi>>>0)*4294967296+(slo>>>0),to_end?null:(dhi>>>0)*4294967296+(dlo>>>0));return r===true||r===1?1:0;
});
EM_JS(int,reader_release_js,(uint32_t token),{var r=Module.moviesMediaReleaseReader(token);return r===true||r===1?1:0;});
#endif
static int close_reader(CPU*c,Object*r){
 if(r->token){int ok=0;if(r->native_reader)ok=reader_host.release(r->token);
#ifdef __EMSCRIPTEN__
 else ok=reader_release_js(r->token);
#endif
 if(!ok)return 0;r->token=0;}
 if(r->stream){stream_call(c,r->stream,2,0,NULL);r->stream=0;}
 memset(&r->pcm,0,sizeof r->pcm);return 1;
}
static void reader_factory(CPU*c){uint32_t out=ARG(2);if(!range(out,4)){ret(c,3,E_POINTER);return;}W32(out,0);
 if(!on_owner()){ret(c,3,WRONGTHREAD);return;}if(ARG(0)||ARG(1)>1){ret(c,3,E_NOTIMPL);return;}int available=0;
 if(reader_host.available)available=reader_host.available();
#ifdef __EMSCRIPTEN__
 else available=reader_available_js();
#endif
 if(!available){ret(c,3,NOTREGISTERED);return;}Object*r=allocate(READER);if(!r){ret(c,3,E_OUTOFMEMORY);return;}r->native_reader=reader_host.available!=NULL;W32(out,r->address);ret(c,3,0);
}
static void reader_open(CPU*c){Object*r=typed(c,READER);if(!r)return;uint32_t stream=ARG(1),scratch=0,hr=0;
 if(r->stream||r->token){ret(c,2,E_UNEXPECTED);return;}if(!range(stream,4)){ret(c,2,E_POINTER);return;}
 scratch=movies_backend_alloc(READER_CHUNK+16);if(!scratch){ret(c,2,E_OUTOFMEMORY);return;}
 stream_call(c,stream,1,0,NULL);r->stream=stream;uint32_t seek[]={0,0,2,scratch+READER_CHUNK};
 hr=stream_call(c,stream,5,4,seek);if(hr&0x80000000u)goto failed;
 uint64_t size=R64(scratch+READER_CHUNK);if(!size||size>READER_MAX_SOURCE){hr=E_NOTIMPL;goto failed;}
 seek[2]=0;hr=stream_call(c,stream,5,4,seek);if(hr&0x80000000u)goto failed;if(R64(scratch+READER_CHUNK)){hr=E_FAIL;goto failed;}
 if(r->native_reader)r->token=reader_host.create((uint32_t)size);
#ifdef __EMSCRIPTEN__
 else r->token=reader_create_js((uint32_t)size);
#endif
 if(!r->token){hr=E_FAIL;goto failed;}
 for(uint32_t offset=0;offset<size;){uint32_t count=(uint32_t)size-offset;if(count>READER_CHUNK)count=READER_CHUNK;W32(scratch+READER_CHUNK,0);
  uint32_t args[]={scratch,count,scratch+READER_CHUNK};hr=stream_call(c,stream,3,3,args);if(hr&0x80000000u)goto failed;
  uint32_t got=R32(scratch+READER_CHUNK);if(!got||got>count){hr=0x8003001eu;goto failed;}int ok=0;
  if(r->native_reader)ok=reader_host.append(r->token,offset,HP(scratch),got);
#ifdef __EMSCRIPTEN__
  else ok=reader_append_js(r->token,offset,HP(scratch),got);
#endif
  if(!ok){hr=E_FAIL;goto failed;}offset+=got;
 }
 int opened=0;if(r->native_reader)opened=reader_host.open(r->token,&r->pcm);
#ifdef __EMSCRIPTEN__
 else {uint32_t fields[9];opened=reader_open_js(r->token,fields);if(opened)r->pcm=(MoviesMediaPCM){fields[0],fields[1],fields[2],fields[3],fields[4],fields[5],fields[6],(uint64_t)fields[7]|((uint64_t)fields[8]<<32)};}
#endif
 if(!opened){hr=E_FAIL;goto failed;}
 /* Full SyncReader default selects all streams. This audio-only implementation
  * rejects multi-stream input instead of silently dropping video samples. */
 if(r->pcm.source_stream_count!=1||r->pcm.sample_rate!=44100||r->pcm.channels!=2||r->pcm.bits!=16||r->pcm.block_align!=4||!r->pcm.stream_number||r->pcm.stream_number>127||r->pcm.max_sample_bytes!=8192||r->pcm.duration_100ns>READER_MAX_TIME){hr=E_NOTIMPL;goto failed;}
 movies_backend_free(scratch);ret(c,2,0);return;
 failed:if(!close_reader(c,r)){guest_trap(c,R32(c->esp),"media reader failed-open backend cleanup");return;}movies_backend_free(scratch);ret(c,2,hr);
}
static void reader_close(CPU*c){Object*r=typed(c,READER);if(r)ret(c,1,close_reader(c,r)?0:E_FAIL);}
static int reader_ready(CPU*c,Object*r,unsigned argc){if(!r->token){ret(c,argc,NS_INVALID_REQUEST);return 0;}return 1;}
static void reader_range(CPU*c){Object*r=typed(c,READER);if(!r||!reader_ready(c,r,5))return;uint64_t start=(uint64_t)ARG(1)|((uint64_t)ARG(2)<<32),duration=(uint64_t)ARG(3)|((uint64_t)ARG(4)<<32);
 if(start>READER_MAX_TIME||duration>READER_MAX_TIME){ret(c,5,E_INVALIDARG);return;}int ok=0;
 if(r->native_reader)ok=reader_host.set_range(r->token,start,duration,duration==0);
#ifdef __EMSCRIPTEN__
 else ok=reader_range_js(r->token,(uint32_t)start,(uint32_t)(start>>32),(uint32_t)duration,(uint32_t)(duration>>32),duration==0);
#endif
 ret(c,5,ok?0:E_FAIL);
}
static void reader_next(CPU*c){Object*r=typed(c,READER);if(!r)return;uint32_t number=ARG(1),out=ARG(2),time=ARG(3),duration=ARG(4),flags=ARG(5),output=ARG(6),stream=ARG(7);
 if(!range(out,4)||!range(time,8)||!range(duration,8)||!range(flags,4)||(output&&!range(output,4))||(stream&&!range(stream,2))){ret(c,8,E_POINTER);return;}W32(out,0);
 if(!reader_ready(c,r,8))return;if(number&&number!=r->pcm.stream_number){ret(c,8,E_INVALIDARG);return;}
 Object*s=allocate(SAMPLE);if(!s){ret(c,8,E_OUTOFMEMORY);return;}s->sample_capacity=r->pcm.max_sample_bytes;s->sample_data=movies_backend_alloc(s->sample_capacity);
 if(!s->sample_data){release_object(c,s);ret(c,8,E_OUTOFMEMORY);return;}uint64_t pts=0,span=0;int result=0;
 if(r->native_reader)result=reader_host.read(r->token,HP(s->sample_data),s->sample_capacity,&s->sample_length,&pts,&span);
#ifdef __EMSCRIPTEN__
 else {uint32_t info[5]={0};result=reader_read_js(r->token,HP(s->sample_data),s->sample_capacity,info);s->sample_length=info[0];pts=(uint64_t)info[1]|((uint64_t)info[2]<<32);span=(uint64_t)info[3]|((uint64_t)info[4]<<32);}
#endif
 if(result!=1){release_object(c,s);ret(c,8,result==2?NS_NO_MORE_SAMPLES:E_FAIL);return;}
 if(!s->sample_length||s->sample_length>s->sample_capacity||s->sample_length%r->pcm.block_align||pts>READER_MAX_TIME||!span||span>READER_MAX_TIME){release_object(c,s);ret(c,8,E_FAIL);return;}
 W32(out,s->address);W64(time,pts);W64(duration,span);W32(flags,1);if(output)W32(output,0);if(stream)W16(stream,(uint16_t)r->pcm.stream_number);ret(c,8,0);
}
static void header_attribute(CPU*c){Object*r=typed(c,READER);if(!r)return;uint32_t stream=ARG(1),name=ARG(2),type=ARG(3),value=ARG(4),size=ARG(5);
 if(!range(stream,2)||!range(type,4)||!range(size,2)){ret(c,6,E_POINTER);return;}if(!reader_ready(c,r,6))return;
 uint16_t*n=copy_guest_wide(name,64);if(!n){ret(c,6,E_INVALIDARG);return;}int known=text_is(n,"Duration",0);free(n);if(R16(stream)||!known){ret(c,6,0xc00d07f0u);return;}
 uint16_t cap=R16(size);W16(size,8);W32(type,4);if(!value){ret(c,6,0);return;}if(cap<8){ret(c,6,0xc00d07d3u);return;}if(!range(value,8)){ret(c,6,E_POINTER);return;}W64(value,r->pcm.duration_100ns);ret(c,6,0);
}
static void reader_output_count(CPU*c){Object*r=typed(c,READER);if(!r||!reader_ready(c,r,2))return;uint32_t out=ARG(1);if(!range(out,4)){ret(c,2,E_POINTER);return;}W32(out,1);ret(c,2,0);}
static void reader_format_count(CPU*c){Object*r=typed(c,READER);if(!r||!reader_ready(c,r,3))return;uint32_t out=ARG(2);if(ARG(1)){ret(c,3,E_INVALIDARG);return;}if(!range(out,4)){ret(c,3,E_POINTER);return;}W32(out,1);ret(c,3,0);}
static void get_reader_props(CPU*c,int format){unsigned argc=format?4:3;Object*r=typed(c,READER);if(!r||!reader_ready(c,r,argc))return;uint32_t out=ARG(format?3:2);if(!range(out,4)){ret(c,argc,E_POINTER);return;}W32(out,0);if(ARG(1)||(format&&ARG(2))){ret(c,argc,E_INVALIDARG);return;}Object*p=allocate(PROPS);if(!p){ret(c,argc,E_OUTOFMEMORY);return;}p->pcm=r->pcm;W32(out,p->address);ret(c,argc,0);}
static void reader_props(CPU*c){get_reader_props(c,0);}static void reader_format(CPU*c){get_reader_props(c,1);}
static void reader_mapping(CPU*c,int to_stream){Object*r=typed(c,READER);if(!r||!reader_ready(c,r,3))return;uint32_t out=ARG(2);if(ARG(1)!=(to_stream?0:r->pcm.stream_number)){ret(c,3,E_INVALIDARG);return;}if(!range(out,to_stream?2:4)){ret(c,3,E_POINTER);return;}if(to_stream)W16(out,(uint16_t)r->pcm.stream_number);else W32(out,0);ret(c,3,0);}
static void reader_output_number(CPU*c){reader_mapping(c,0);}static void reader_stream_number(CPU*c){reader_mapping(c,1);}
static void reader_maximum(CPU*c,int stream){Object*r=typed(c,READER);if(!r||!reader_ready(c,r,3))return;if(ARG(1)!=(stream?r->pcm.stream_number:0)){ret(c,3,E_INVALIDARG);return;}uint32_t out=ARG(2);if(!range(out,4)){ret(c,3,E_POINTER);return;}W32(out,r->pcm.max_sample_bytes);ret(c,3,0);}
static void reader_max_output(CPU*c){reader_maximum(c,0);}static void reader_max_stream(CPU*c){reader_maximum(c,1);}
static void props_type(CPU*c){if(!typed(c,PROPS))return;uint32_t out=ARG(1);if(!range(out,16)){ret(c,2,E_POINTER);return;}memcpy(HP(out),id_audio,16);ret(c,2,0);}
static void props_media(CPU*c){Object*p=typed(c,PROPS);if(!p)return;uint32_t out=ARG(1),size=ARG(2);if(!range(size,4)){ret(c,3,E_POINTER);return;}uint32_t cap=R32(size);W32(size,90);if(!out){ret(c,3,0);return;}if(cap<90){ret(c,3,0xc00d07d3u);return;}if(!range(out,90)){ret(c,3,E_POINTER);return;}
 memset(HP(out),0,90);memcpy(HP(out),id_audio,16);memcpy(HP(out+16),id_pcm,16);W32(out+32,1);W32(out+40,p->pcm.block_align);memcpy(HP(out+44),id_waveformat,16);W32(out+64,18);W32(out+68,out+72);
 W16(out+72,1);W16(out+74,(uint16_t)p->pcm.channels);W32(out+76,p->pcm.sample_rate);W32(out+80,p->pcm.sample_rate*p->pcm.block_align);W16(out+84,(uint16_t)p->pcm.block_align);W16(out+86,(uint16_t)p->pcm.bits);ret(c,3,0);
}
static void sample_length(CPU*c){Object*s=typed(c,SAMPLE);if(!s)return;uint32_t out=ARG(1);if(!range(out,4)){ret(c,2,E_POINTER);return;}W32(out,s->sample_length);ret(c,2,0);}
static void sample_set_length(CPU*c){Object*s=typed(c,SAMPLE);if(!s)return;uint32_t size=ARG(1);if(size>s->sample_capacity){ret(c,2,E_INVALIDARG);return;}s->sample_length=size;ret(c,2,0);}
static void sample_capacity(CPU*c){Object*s=typed(c,SAMPLE);if(!s)return;uint32_t out=ARG(1);if(!range(out,4)){ret(c,2,E_POINTER);return;}W32(out,s->sample_capacity);ret(c,2,0);}
static void sample_buffer(CPU*c){Object*s=typed(c,SAMPLE);if(!s)return;uint32_t out=ARG(1);if(!range(out,4)){ret(c,2,E_POINTER);return;}W32(out,s->sample_data);ret(c,2,0);}
static void sample_buffer_length(CPU*c){Object*s=typed(c,SAMPLE);if(!s)return;uint32_t out=ARG(1),length=ARG(2);if(!range(out,4)||!range(length,4)){ret(c,3,E_POINTER);return;}W32(out,s->sample_data);W32(length,s->sample_length);ret(c,3,0);}
int movies_media_init(void){
    if(initialized&&!on_owner())return 0;if(apartment_depth)return 0;
    for(unsigned i=0;i<LIMIT;i++)if(objects[i].address)return 0;
    if(MOVIES_MEDIA_END>MOVIES_MEDIA_SPACE||!g_mem)return 0;
    owner=pthread_self();memset(objects,0,sizeof objects);memset(HP(MOVIES_MEDIA_BASE),0,0x10000);sequence=0;
    GuestFn methods[]={query,addref,release,unsupported,set_graph,get_graph,add_filter,remove_filter,enum_filters,find_filter,enum_next,enum_skip,enum_reset,enum_clone,get_class,stopped,get_state,filter_info,empty_profile,load_profile,profile_version,profile_count,profile_name,profile_set_name,profile_description,profile_set_description,configure_profile,current_profile,reader_close,reader_range,reader_next,header_attribute,reader_output_count,reader_format_count,reader_props,reader_format,reader_output_number,reader_stream_number,reader_max_output,reader_max_stream,reader_open,props_type,props_media,sample_length,sample_set_length,sample_capacity,sample_buffer,sample_buffer_length};
    for(unsigned i=0;i<sizeof methods/sizeof*methods;i++)register_function(MOVIES_MEDIA_TRAMP+16*i,methods[i]);
    for(unsigned kind=1;kind<=11;kind++){uint32_t table=MOVIES_MEDIA_BASE+(kind-1)*0x100;for(unsigned i=0;i<32;i++)W32(table+4*i,MOVIES_MEDIA_TRAMP+16*(i<3?i:3));}
    const unsigned map[][3]={{GRAPH,3,6},{GRAPH,4,7},{GRAPH,5,8},{GRAPH,6,9},{CAPTURE,3,4},{CAPTURE,4,5},{WRITER,3,14},{WRITER,4,15},{WRITER,7,16},{WRITER,12,17},{ENUMERATOR,3,10},{ENUMERATOR,4,11},{ENUMERATOR,5,12},{ENUMERATOR,6,13},{MANAGER,3,18},{MANAGER,5,19},{PROFILE,3,20},{PROFILE,4,22},{PROFILE,5,23},{PROFILE,6,24},{PROFILE,7,25},{PROFILE,8,21},{7,7,26},{7,8,27},{READER,4,28},{READER,5,29},{READER,7,30},{HEADER_TABLE,5,31},{READER,14,32},{READER,17,33},{READER,15,34},{READER,18,35},{READER,19,36},{READER,20,37},{READER,21,38},{READER,22,39},{READER,23,40},{PROPS,3,41},{PROPS,4,42},{SAMPLE,3,43},{SAMPLE,4,44},{SAMPLE,5,45},{SAMPLE,6,46},{SAMPLE,7,47}};
    for(unsigned i=0;i<sizeof map/sizeof*map;i++)W32(MOVIES_MEDIA_BASE+(map[i][0]-1)*0x100+4*map[i][1],MOVIES_MEDIA_TRAMP+16*map[i][2]);
    initialized=1;return 1;
}
ImportHandler *movies_media_handlers(uint32_t *count){static ImportHandler handlers[]={{"WMVCORE.DLL!WMCreateSyncReader",reader_factory},{"WMVCORE.DLL!WMCreateProfileManager",manager_factory},{"OLE32.DLL!CoInitialize",coinit},{"OLE32.DLL!CoInitializeEx",coinitex},{"OLE32.DLL!CoUninitialize",couninit},{"OLE32.DLL!CoCreateInstance",cocreate}};*count=sizeof handlers/sizeof*handlers;return handlers;}
