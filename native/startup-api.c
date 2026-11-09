/* Deliberately narrow Windows XP ABI used by the verified CRT startup path.
 * Unimplemented call forms trap instead of pretending to support a module.
 */
#include "startup-api.h"
#include <stdio.h>
#include <pthread.h>
#define KERNEL_HANDLE 0x07000000u
static uint32_t executable_base;
static __thread uint32_t last_error;
static uint32_t unhandled_filter;
uint32_t movies_unhandled_exception_filter(void) { return __atomic_load_n(&unhandled_filter, __ATOMIC_ACQUIRE); }
static pthread_mutex_t procedure_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { char name[96]; uint32_t address; } procedures[64];
static uint32_t procedure_count;
void movies_set_last_error(uint32_t value) { last_error = value; }
uint32_t movies_get_last_error(void) { return last_error; }

static void return_stdcall(CPU *cpu, uint32_t args, uint32_t value) {
    cpu->eax = value;
    cpu->esp += 4 * (args + 1);
}

/* https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getversion
 * XP NT packed DWORD: build in high word, minor then major in low word.
 * Bit31 stays clear for NT, matching GetVersionExA platform VER_PLATFORM_WIN32_NT. */
static void version_packed(CPU *cpu) { return_stdcall(cpu,0,(2600u<<16)|(1u<<8)|5u); }

static void version_a(CPU *cpu) {
    uint32_t output = R32(cpu->esp + 4);
    if (!output) { last_error = 87; return_stdcall(cpu, 1, 0); return; }
    uint32_t size = R32(output);
    if (size != 148 && size != 156) {
        last_error = 87; return_stdcall(cpu, 1, 0); return;
    }
    /* The guest compatibility OS is XP SP3; this is not the host macOS version. */
    memset(HP(output + 4), 0, size - 4);
    W32(output + 4, 5); W32(output + 8, 1);
    W32(output + 12, 2600); W32(output + 16, 2);
    memcpy(HP(output + 20), "Service Pack 3", 15);
    if (size == 156) {
        W16(output + 148, 3);
        W16(output + 152, 0x100); /* VER_SUITE_SINGLEUSERTS */
        W8(output + 154, 1); /* VER_NT_WORKSTATION */
    }
    return_stdcall(cpu, 1, 1);
}

/* OSVERSIONINFOW(EX)W: szCSDVersion is 128 WCHARs, so the sizes are 276/284. */
static void version_w(CPU *cpu) {
    uint32_t output = R32(cpu->esp + 4);
    if (!output) { last_error = 87; return_stdcall(cpu, 1, 0); return; }
    uint32_t size = R32(output);
    if (size != 276 && size != 284) { last_error = 87; return_stdcall(cpu, 1, 0); return; }
    memset(HP(output + 4), 0, size - 4);
    W32(output + 4, 5); W32(output + 8, 1); W32(output + 12, 2600); W32(output + 16, 2);
    const char *pack = "Service Pack 3";
    for (unsigned i = 0; pack[i]; i++) W16(output + 20 + 2 * i, (uint8_t)pack[i]);
    if (size == 284) { W16(output + 276, 3); W16(output + 280, 0x100); W8(output + 282, 1); }
    return_stdcall(cpu, 1, 1);
}
static void is_debugger_present(CPU *cpu) { return_stdcall(cpu, 0, 0); }

/* Explicit host-backed modules only; no disk PE loader, DllMain, or fabricated
 * exports. Core KERNEL32 and the already-loaded executable remain resident.
 * https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibrarya
 * https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-freelibrary */
#define MODULE_LIMIT 16
static struct Module { char name[64],path[260],required_export[64];uint32_t handle,references;int pinned; } modules[MODULE_LIMIT];
static unsigned module_count;
static uint32_t read_module_name(uint32_t pointer,int wide,char *name) {
    if(!pointer)return 87;
    for(unsigned i=0;i<260;i++) {
        if((uint64_t)pointer+(i+1)*(wide?2u:1u)>GUEST_SPACE)return 87;
        uint32_t ch=wide?R16(pointer+2*i):R8(pointer+i);
        if(ch>127)return 50;
        name[i]=ch=='/'?'\\':(char)toupper((unsigned char)ch);
        if(!ch)return i?0:126;
    }
    return 126;
}
static struct Module *module_by_handle(uint32_t handle) {
    for(unsigned i=0;i<module_count;i++)if(modules[i].handle==handle && (modules[i].pinned || modules[i].references))return &modules[i];
    return NULL;
}
static int canonical_module(char *name) {
    char *base=strrchr(name,'\\');base=base?base+1:name;
    if(!*base)return 0;
    size_t n=strlen(name);
    if(name[n-1]=='.')name[n-1]=0;
    else if(!strchr(base,'.')) { if(n+4>=260)return 0;strcat(name,".DLL"); }
    return 1;
}
static struct Module *module_by_name(const char *name) {
    int full=strchr(name,'\\')!=NULL;
    for(unsigned i=0;i<module_count;i++)if(!strcmp(name,full?modules[i].path:modules[i].name))return &modules[i];
    return NULL;
}
int movies_startup_register_module(const char *dll,const char *required_export) {
    if(!dll || !required_export || !*required_export || strlen(required_export)>=64 || strlen(dll)>=64 || strchr(dll,'\\') || strchr(dll,'/') || strchr(dll,':'))return 0;
    char name[260];for(size_t i=0;i<=strlen(dll);i++)name[i]=(char)toupper((unsigned char)dll[i]);
    if(!canonical_module(name) || strlen(name)>=64)return 0;
    char full[96];if(snprintf(full,sizeof full,"%s!%s",name,required_export)>=(int)sizeof full || !find_import_handler(full))return 0;
    pthread_mutex_lock(&procedure_mutex);
    struct Module *existing=module_by_name(name);
    if(existing) { int same=!strcmp(existing->required_export,required_export);pthread_mutex_unlock(&procedure_mutex);return same; }
    if(module_count==MODULE_LIMIT) { pthread_mutex_unlock(&procedure_mutex);return 0; }
    struct Module *m=&modules[module_count];strcpy(m->name,name);strcpy(m->required_export,required_export);
    snprintf(m->path,sizeof m->path,"C:\\WINDOWS\\SYSTEM32\\%s",name);
    m->handle=KERNEL_HANDLE+0x100*module_count;m->references=0;m->pinned=0;module_count++;
    pthread_mutex_unlock(&procedure_mutex);return 1;
}
int movies_startup_module_available(const char *dll,const char *required_export) {
    if(!dll || !required_export || !*required_export || strlen(required_export)>=64 || strlen(dll)>=64 || strchr(dll,'\\') || strchr(dll,'/') || strchr(dll,':'))return 0;
    char name[260];for(size_t i=0;i<=strlen(dll);i++)name[i]=(char)toupper((unsigned char)dll[i]);
    if(!canonical_module(name))return 0;
    pthread_mutex_lock(&procedure_mutex);struct Module *m=module_by_name(name);
    int registered=m && !strcmp(m->required_export,required_export);pthread_mutex_unlock(&procedure_mutex);
    char full[96];if(!registered || snprintf(full,sizeof full,"%s!%s",name,required_export)>=(int)sizeof full)return 0;
    return find_import_handler(full)!=NULL;
}
static void get_or_load_module(CPU *cpu,int wide,int load) {
    uint32_t pointer=R32(cpu->esp+4),result=0,error=0;
    if(!pointer && !load) { return_stdcall(cpu,1,executable_base);return; }
    char name[260];error=read_module_name(pointer,wide,name);
    if(!error && !canonical_module(name))error=126;
    pthread_mutex_lock(&procedure_mutex);
    if(!error && (!strcmp(name,"MOVIESSE.EXE") || !strcmp(name,"C:\\GAMES\\THEMOVIES\\MOVIESSE.EXE")))result=executable_base;
    else if(!error) {
        struct Module *m=module_by_name(name);
        if(!m || (!load && !m->pinned && !m->references))error=126;
        else if(load && m->references==UINT32_MAX)error=8;
        else { if(load)m->references++;result=m->handle; }
    }
    pthread_mutex_unlock(&procedure_mutex);
    if(error && load) { static unsigned logs;if(logs++<16)fprintf(stderr,"[module] LoadLibrary(%s) failed error=%u caller=%08x\n",name,error,R32(cpu->esp)); }
    if(error)last_error=error;return_stdcall(cpu,1,result);
}
static void module_a(CPU *cpu) { get_or_load_module(cpu,0,0); }
static void module_w(CPU *cpu) { get_or_load_module(cpu,1,0); }
static void load_a(CPU *cpu) { get_or_load_module(cpu,0,1); }
/* RaiseException: guest structured exception dispatch is not implemented. Report the
 * exception (for MSVC C++ throws 0xE06D7363: the catchable type names from the
 * ThrowInfo) and the code addresses on the stack, then stop with a clear message.
 * https://learn.microsoft.com/en-us/cpp/build/exception-handling-x86 (ThrowInfo layout) */
static void ascii_at(uint32_t p,char *out,size_t cap) { size_t n=0;while(p && n+1<cap && (uint64_t)p+n<GUEST_SPACE && R8(p+(uint32_t)n)>=32 && R8(p+(uint32_t)n)<127) { out[n]=(char)R8(p+(uint32_t)n);n++; }out[n]=0; }
static void raise_exception(CPU *cpu) {
    uint32_t code=R32(cpu->esp+4),flags=R32(cpu->esp+8),count=R32(cpu->esp+12),args=R32(cpu->esp+16);
    fprintf(stderr,"[exception] RaiseException code=%08x flags=%08x params=%u caller=%08x\n",code,flags,count,R32(cpu->esp));
    if(code==0xe06d7363u && count>=3 && args && (uint64_t)args+12<GUEST_SPACE) {
        uint32_t object=R32(args+4),info=R32(args+8);
        fprintf(stderr,"[exception] C++ throw magic=%08x object=%08x throwinfo=%08x\n",R32(args),object,info);
        if(info && (uint64_t)info+16<GUEST_SPACE) {
            uint32_t table=R32(info+12),n=table&&(uint64_t)table+4<GUEST_SPACE?R32(table):0;
            for(uint32_t i=0;i<n && i<8;i++) {
                uint32_t type=R32(table+4+i*4),descriptor=type&&(uint64_t)type+8<GUEST_SPACE?R32(type+4):0;char name[128];ascii_at(descriptor?descriptor+8:0,name,sizeof name);
                fprintf(stderr,"[exception]   catchable type %u: %s\n",i,name);
            }
            /* Common payloads: char* / std::exception-like objects with a message. */
            if(object && (uint64_t)object+8<GUEST_SPACE) { char text[160];ascii_at(R32(object),text,sizeof text);if(text[0])fprintf(stderr,"[exception]   object string: %s\n",text);
                ascii_at(R32(object+4),text,sizeof text);if(text[0])fprintf(stderr,"[exception]   object+4 string: %s\n",text); }
        }
    }
    fprintf(stderr,"[exception] stack:");
    for(unsigned k=1;k<160;k++) { uint32_t v=R32(cpu->esp+4*k);if(v>=0x401000u && v<0xd00000u)fprintf(stderr," +%u:%08x",k*4,v); }
    fprintf(stderr,"\n");
    guest_trap(cpu,R32(cpu->esp),"C++/SEH exception raised; guest exception dispatch is not implemented");
}
/* IsBadReadPtr(ptr, size): readable iff the whole span lies in guest memory
 * (the C++ runtime's typeid() probes object pointers with it).
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-isbadreadptr */
static void is_bad_read_ptr(CPU *cpu) {
    uint32_t p=R32(cpu->esp+4),n=R32(cpu->esp+8);
    int bad=n && (!p || p<0x10000u || (uint64_t)p+n>GUEST_SPACE);
    return_stdcall(cpu,2,bad?1:0);
}
static void load_w(CPU *cpu) { get_or_load_module(cpu,1,1); }
static void free_library(CPU *cpu) {
    uint32_t handle=R32(cpu->esp+4),ok=0;
    pthread_mutex_lock(&procedure_mutex);
    if(handle==executable_base && handle)ok=1;
    else { struct Module *m=module_by_handle(handle);if(m) { if(m->references)m->references--;ok=1; } }
    pthread_mutex_unlock(&procedure_mutex);
    if(!ok)last_error=6;return_stdcall(cpu,1,ok);
}

static void procedure(CPU *cpu) {
    uint32_t module = R32(cpu->esp + 4), pointer = R32(cpu->esp + 8);
    pthread_mutex_lock(&procedure_mutex);
    struct Module *m=module_by_handle(module);char dll[64];
    if(m)strcpy(dll,m->name);
    pthread_mutex_unlock(&procedure_mutex);
    if (!m || pointer <= 0xffff) {
        last_error = 127; return_stdcall(cpu, 2, 0); return;
    }
    char full[96];
    unsigned prefix=(unsigned)snprintf(full,sizeof full,"%s!",dll);
    unsigned i;
    for (i = prefix; i < sizeof full - 1; i++, pointer++) {
        if ((uint64_t)pointer >= GUEST_SPACE) guest_trap(cpu, pointer, "procedure name outside guest memory");
        full[i] = (char)R8(pointer);
        if (!full[i]) break;
    }
    if (i == sizeof full - 1) { last_error = 127; return_stdcall(cpu, 2, 0); return; }
    full[i] = 0;
    GuestFn handler = find_import_handler(full);
    if (!handler) { last_error = 127; return_stdcall(cpu, 2, 0); return; }
    pthread_mutex_lock(&procedure_mutex);
    uint32_t address = 0;
    for (i = 0; i < procedure_count; i++) {
        if (!strcmp(procedures[i].name, full)) { address = procedures[i].address; break; }
    }
    if (!address && procedure_count < sizeof procedures / sizeof procedures[0]) {
        address = add_dynamic_import(full, handler);
        strcpy(procedures[procedure_count].name, full);
        procedures[procedure_count++].address = address;
    }
    pthread_mutex_unlock(&procedure_mutex);
    if (!address) guest_trap(cpu, R32(cpu->esp), "procedure cache capacity exceeded");
    return_stdcall(cpu, 2, address);
}

/* Virtual compatibility directories, not paths into the host OS.
 * https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getsystemdirectorya */
static void system_directory(CPU *cpu,int wide,int windows) {
    const char *path=windows?"C:\\Windows":"C:\\Windows\\System32";
    uint32_t out=R32(cpu->esp+4),size=R32(cpu->esp+8),length=(uint32_t)strlen(path);
    if(size<=length) { return_stdcall(cpu,2,length+1);return; }
    if(!out || (uint64_t)out+(length+1)*(wide?2u:1u)>GUEST_SPACE) { last_error=87;return_stdcall(cpu,2,0);return; }
    for(uint32_t i=0;i<=length;i++)if(wide)W16(out+2*i,(unsigned char)path[i]);else W8(out+i,(unsigned char)path[i]);
    return_stdcall(cpu,2,length);
}
static void system_a(CPU *cpu) { system_directory(cpu,0,0); }
static void system_w(CPU *cpu) { system_directory(cpu,1,0); }
static void windows_a(CPU *cpu) { system_directory(cpu,0,1); }
static void windows_w(CPU *cpu) { system_directory(cpu,1,1); }
static void get_error(CPU *cpu) { return_stdcall(cpu, 0, last_error); }
static void set_error(CPU *cpu) { last_error = R32(cpu->esp + 4); return_stdcall(cpu, 1, 0); }
static void set_unhandled_filter(CPU *cpu) {
    uint32_t previous = __atomic_exchange_n(&unhandled_filter, R32(cpu->esp + 4), __ATOMIC_ACQ_REL);
    return_stdcall(cpu, 1, previous);
}

ImportHandler *movies_startup_handlers(uint32_t image_base, uint32_t *count) {
    static ImportHandler handlers[] = {
        { "KERNEL32.DLL!GetVersionExA", version_a },
        { "KERNEL32.DLL!GetVersionExW", version_w },
        { "KERNEL32.DLL!IsDebuggerPresent", is_debugger_present },
        { "KERNEL32.DLL!GetVersion", version_packed },
        { "KERNEL32.DLL!IsBadReadPtr", is_bad_read_ptr },
        { "KERNEL32.DLL!RaiseException", raise_exception },
        { "KERNEL32.DLL!GetModuleHandleA", module_a },
        { "KERNEL32.DLL!GetModuleHandleW", module_w },
        { "KERNEL32.DLL!LoadLibraryA", load_a },
        { "KERNEL32.DLL!LoadLibraryW", load_w },
        { "KERNEL32.DLL!FreeLibrary", free_library },
        { "KERNEL32.DLL!GetSystemDirectoryA", system_a },
        { "KERNEL32.DLL!GetSystemDirectoryW", system_w },
        { "KERNEL32.DLL!GetWindowsDirectoryA", windows_a },
        { "KERNEL32.DLL!GetWindowsDirectoryW", windows_w },
        { "KERNEL32.DLL!GetProcAddress", procedure },
        { "KERNEL32.DLL!GetLastError", get_error },
        { "KERNEL32.DLL!SetLastError", set_error },
        { "KERNEL32.DLL!SetUnhandledExceptionFilter", set_unhandled_filter },
    };
    executable_base = image_base;
    memset(modules,0,sizeof modules);module_count=1;
    strcpy(modules[0].name,"KERNEL32.DLL");strcpy(modules[0].path,"C:\\WINDOWS\\SYSTEM32\\KERNEL32.DLL");
    strcpy(modules[0].required_export,"GetLastError");
    modules[0].handle=KERNEL_HANDLE;modules[0].pinned=1;
    last_error = 0;
    unhandled_filter = 0;
    procedure_count = 0;
    *count = sizeof handlers / sizeof handlers[0];
    return handlers;
}
