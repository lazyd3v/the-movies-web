/* Narrow process/clock ABI for original CRT startup, not a general Win32 layer.
 * https://learn.microsoft.com/en-us/windows/console/setconsolectrlhandler
 * Contracts: Microsoft Learn GetStartupInfoA, GetStdHandle, GetFileType,
 * GetModuleFileNameA (documented XP truncation), GetEnvironmentStringsW,
 * QueryPerformanceCounter/Frequency, GetSystemTimeAsFileTime, SetHandleCount.
 * https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulefilenamea
 * https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getenvironmentstringsw
 * https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter
 */
#include "process-api.h"
#include "startup-api.h"
#include "text-api.h"
#include "heap-api.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "sync-api.h"

#ifndef MOVIES_PROCESS_SPACE
#define MOVIES_PROCESS_SPACE GUEST_SPACE
#endif
#define COMMAND_LINE (MOVIES_PROCESS_DATA_BASE + 0x8000u)
#define ENV_BASE (MOVIES_PROCESS_DATA_BASE + 0x10000u)
#define ENV_SLOTS 256u
#define ENV_ENTRY_CAP 256u
#define ENV_TOTAL_CAP 65536u
#define ENV_STRING_CAP 32767u
#define PATH_CAPACITY 32768u
#define INVALID_HANDLE 0xffffffffu
_Static_assert(MOVIES_PROCESS_DATA_END <= GUEST_HEAP_BASE, "process data overlaps heap");
_Static_assert(ENV_BASE < MOVIES_PROCESS_DATA_END, "process data overflow");
static pthread_mutex_t process_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t executable_base, standard_handles[3], initial_handles[3];
static char executable_path[PATH_CAPACITY];
static uint32_t path_length;
typedef struct EnvSnapshot { uint32_t address,bytes; uint8_t kind; } EnvSnapshot;
static EnvSnapshot environment_slots[ENV_SLOTS];
typedef struct EnvEntry { char *name,*value; } EnvEntry;
static EnvEntry environment_entries[ENV_ENTRY_CAP];
static uint64_t clock_epoch;
static int initialized;
static uint32_t *console_handlers;
static size_t console_count, console_capacity;
static int ignore_ctrl_c;
static int (*external_file_resolver)(uint32_t,uint32_t *);
void movies_process_set_file_resolver(int (*resolver)(uint32_t,uint32_t *)) { external_file_resolver=resolver; }

static void ret(CPU *c, uint32_t argc, uint32_t value) { c->eax=value; c->esp+=4*(argc+1); }
static uint32_t arg(CPU *c, uint32_t index) { return R32(c->esp+4*(index+1)); }
static int range(uint32_t address, uint32_t bytes) { return address && (uint64_t)address+bytes<=MOVIES_PROCESS_SPACE; }
static void require_init(CPU *c) {
    if (!initialized) guest_trap(c,R32(c->esp),"process API used before movies_process_init");
}
static int monotonic_ns(uint64_t *value) {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)) return 0;
    *value=(uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
    return 1;
}
static int standard_index(uint32_t selector) {
    if(selector==(uint32_t)-10) return 0;
    if(selector==(uint32_t)-11) return 1;
    if(selector==(uint32_t)-12) return 2;
    return -1;
}
static int descriptor(uint32_t handle) {
    if(handle>=MOVIES_STD_INPUT_HANDLE && handle<=MOVIES_STD_ERROR_HANDLE)
        return (int)(handle-MOVIES_STD_INPUT_HANDLE);
    return -1;
}
int movies_process_init(uint32_t image_base,const char *path) {
    if(!g_mem || !image_base || !path) { movies_set_last_error(87); return 0; }
    size_t length=strnlen(path,PATH_CAPACITY);
    if(!length || length>PATH_CAPACITY-3 || memchr(path,'"',length)) {
        movies_set_last_error(87); return 0;
    }
    uint64_t epoch;
    if(!monotonic_ns(&epoch)) { movies_set_last_error(31); return 0; }
    pthread_mutex_lock(&process_mutex);
    executable_base=image_base; path_length=(uint32_t)length;
    memcpy(executable_path,path,length+1);
    memset(HP(MOVIES_PROCESS_DATA_BASE),0,MOVIES_PROCESS_DATA_END-MOVIES_PROCESS_DATA_BASE);
    memcpy(HP(MOVIES_PROCESS_DATA_BASE),path,length+1);
    W8(COMMAND_LINE,'"'); memcpy(HP(COMMAND_LINE+1),path,length);
    W8(COMMAND_LINE+1+(uint32_t)length,'"'); W8(COMMAND_LINE+2+(uint32_t)length,0);
    memset(environment_slots,0,sizeof environment_slots);
    for(unsigned i=0;i<ENV_ENTRY_CAP;i++) {
        free(environment_entries[i].name);free(environment_entries[i].value);
        environment_entries[i]=(EnvEntry){0};
    }
    for(int i=0;i<3;i++) {
        struct stat st;
        standard_handles[i]=fstat(i,&st)==0?MOVIES_STD_INPUT_HANDLE+(uint32_t)i:0;
        initial_handles[i]=standard_handles[i];
    }
    free(console_handlers); console_handlers=NULL; console_count=console_capacity=0; ignore_ctrl_c=0;
    clock_epoch=epoch; initialized=1;
    pthread_mutex_unlock(&process_mutex);
    movies_set_last_error(0); return 1;
}
static void startup_info(CPU *c) {
    require_init(c); uint32_t out=arg(c,0);
    if(!range(out,68)) { guest_trap(c,R32(c->esp),"GetStartupInfoA output outside guest memory"); return; }
    memset(HP(out),0,68); W32(out,68); W32(out+44,0x100); /* STARTF_USESTDHANDLES */
    for(unsigned i=0;i<3;i++) W32(out+56+i*4,initial_handles[i]);
    ret(c,1,0);
}
static void get_standard(CPU *c) {
    require_init(c); int index=standard_index(arg(c,0));
    if(index<0) { movies_set_last_error(87); ret(c,1,INVALID_HANDLE); return; }
    pthread_mutex_lock(&process_mutex); uint32_t value=standard_handles[index]; pthread_mutex_unlock(&process_mutex);
    ret(c,1,value);
}
static void set_standard(CPU *c) {
    require_init(c); int index=standard_index(arg(c,0)); uint32_t handle=arg(c,1);
    if(index<0) { movies_set_last_error(87); ret(c,2,0); return; }
    int fd=descriptor(handle); struct stat st;
    /* NULL/INVALID_HANDLE deliberately detach a stream. Unknown objects are
     * rejected: this subset cannot pretend that an arbitrary handle is usable. */
    uint32_t external_type;
    if(handle!=0 && handle!=INVALID_HANDLE && (fd<0 || fstat(fd,&st)) &&
       !(external_file_resolver && external_file_resolver(handle,&external_type))) {
        movies_set_last_error(6); ret(c,2,0); return;
    }
    pthread_mutex_lock(&process_mutex); standard_handles[index]=handle; pthread_mutex_unlock(&process_mutex);
    ret(c,2,1);
}
static void file_type(CPU *c) {
    require_init(c); int fd=descriptor(arg(c,0)); struct stat st;
    if(fd<0 || fstat(fd,&st)) {
        uint32_t type;
        if(external_file_resolver && external_file_resolver(arg(c,0),&type)) { ret(c,1,type);return; }
        movies_set_last_error(6); ret(c,1,0); return;
    }
    uint32_t type=0;
    if(S_ISCHR(st.st_mode)) type=2;
    else if(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) || S_ISBLK(st.st_mode)) type=1;
    else if(S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) type=3;
    if(!type) movies_set_last_error(0); /* valid unknown type */
    ret(c,1,type);
}
static void handle_count(CPU *c) { ret(c,1,arg(c,0)); } /* Documented NT no-op; allocates nothing. */
static void command_line(CPU *c) { require_init(c); ret(c,0,COMMAND_LINE); }
static void module_filename(CPU *c) {
    require_init(c); uint32_t module=arg(c,0),out=arg(c,1),size=arg(c,2);
    if(module && module!=executable_base) { movies_set_last_error(126); ret(c,3,0); return; }
    if(!size) { movies_set_last_error(0); ret(c,3,0); return; }
    if(!range(out,size)) { movies_set_last_error(87); ret(c,3,0); return; }
    if(size<=path_length) {
        /* XP: exactly nSize characters, no NUL, ERROR_SUCCESS on truncation. */
        memcpy(HP(out),executable_path,size); movies_set_last_error(0); ret(c,3,size);
    } else { memcpy(HP(out),executable_path,path_length+1); ret(c,3,path_length); }
}
/* Environment starts empty and never imports host values. Names use the same
 * Western CP1252 case mapping as the text ABI, not a claimed full Unicode fold.
 * NT permits an initial '=' (CRT drive-current-directory entries such as =C:).
 * See wine-mirror/wine dlls/ntdll/env.c RtlSetEnvironmentVariable. */
static int env_compare(const char *a,const char *b) {
    while(*a && *b) {
        uint16_t x=movies_western_upper(movies_cp1252_decode((uint8_t)*a++));
        uint16_t y=movies_western_upper(movies_cp1252_decode((uint8_t)*b++));
        if(x!=y) return x<y?-1:1;
    }
    return (uint8_t)*a<(uint8_t)*b?-1:(uint8_t)*a>(uint8_t)*b;
}
static char *env_read(uint32_t address,uint32_t *failure) {
    if(!address) { *failure=87;return NULL; }
    size_t length=0;
    for(;length<ENV_STRING_CAP;length++) {
        if(!range(address,(uint32_t)length+1)) { *failure=87;return NULL; }
        if(!R8(address+(uint32_t)length)) break;
    }
    if(length==ENV_STRING_CAP) { *failure=206;return NULL; }
    char *copy=malloc(length+1);
    if(!copy) { *failure=8;return NULL; }
    memcpy(copy,HP(address),length+1);return copy;
}
static void set_environment_a(CPU *c) {
    require_init(c);uint32_t failure=0;
    char *name=env_read(arg(c,0),&failure),*value=NULL;
    if(!failure && (!*name || strchr(name+1,'='))) failure=87;
    if(!failure && arg(c,1)) value=env_read(arg(c,1),&failure);
    if(failure) { free(name);free(value);movies_set_last_error(failure);ret(c,2,0);return; }
    pthread_mutex_lock(&process_mutex);
    unsigned found=ENV_ENTRY_CAP,empty=ENV_ENTRY_CAP;size_t total=1;
    for(unsigned i=0;i<ENV_ENTRY_CAP;i++) {
        EnvEntry *e=&environment_entries[i];
        if(!e->name) { if(empty==ENV_ENTRY_CAP) empty=i;continue; }
        if(!env_compare(name,e->name)) found=i;
        else total+=strlen(e->name)+strlen(e->value)+2;
    }
    if(value && total+strlen(name)+strlen(value)+2>ENV_TOTAL_CAP) failure=8;
    if(!failure && value && found==ENV_ENTRY_CAP && empty==ENV_ENTRY_CAP) failure=8;
    if(!failure) {
        if(found!=ENV_ENTRY_CAP) {
            free(environment_entries[found].name);free(environment_entries[found].value);
            environment_entries[found]=(EnvEntry){0};
        }
        if(value) {
            unsigned slot=found!=ENV_ENTRY_CAP?found:empty;
            environment_entries[slot]=(EnvEntry){name,value};name=value=NULL;
        }
    }
    pthread_mutex_unlock(&process_mutex);
    free(name);free(value);
    if(failure) movies_set_last_error(failure);
    ret(c,2,failure?0:1);
}
static void environment(CPU *c,uint8_t kind) {
    require_init(c);uint32_t address=0;
    pthread_mutex_lock(&process_mutex);
    unsigned order[ENV_ENTRY_CAP],count=0,slot=ENV_SLOTS;size_t units=1;
    for(unsigned i=0;i<ENV_ENTRY_CAP;i++) if(environment_entries[i].name) {
        order[count++]=i;units+=strlen(environment_entries[i].name)+strlen(environment_entries[i].value)+2;
    }
    if(!count) units=2; /* explicitly double-NUL empty snapshot */
    for(unsigned i=1;i<count;i++) {
        unsigned key=order[i],j=i;
        while(j && env_compare(environment_entries[order[j-1]].name,environment_entries[key].name)>0) {
            order[j]=order[j-1];--j;
        }
        order[j]=key;
    }
    uint32_t bytes=((uint32_t)units*(kind==2?2u:1u)+7u)&~7u;
    for(unsigned i=0;i<ENV_SLOTS;i++) if(!environment_slots[i].kind) { slot=i;break; }
    if(slot!=ENV_SLOTS) {
        uint32_t candidate=ENV_BASE;
        for(;;) {
            if((uint64_t)candidate+bytes>MOVIES_PROCESS_DATA_END) break;
            uint32_t next=candidate;
            for(unsigned i=0;i<ENV_SLOTS;i++) {
                EnvSnapshot *s=&environment_slots[i];
                if(s->kind && candidate<s->address+s->bytes && candidate+bytes>s->address && s->address+s->bytes>next)
                    next=s->address+s->bytes;
            }
            if(next==candidate) { address=candidate;break; }
            candidate=next;
        }
    }
    if(address) {
        environment_slots[slot]=(EnvSnapshot){address,bytes,kind};
        memset(HP(address),0,bytes);uint32_t at=address;
        for(unsigned i=0;i<count;i++) {
            EnvEntry *e=&environment_entries[order[i]];
            const char *parts[]={e->name,"=",e->value};
            for(unsigned part=0;part<3;part++) for(const unsigned char *p=(const unsigned char *)parts[part];*p;p++) {
                if(kind==2) { W16(at,movies_cp1252_decode(*p));at+=2; }
                else { W8(at,*p);++at; }
            }
            at+=kind==2?2:1; /* already-zero string terminator */
        }
    }
    pthread_mutex_unlock(&process_mutex);
    if(!address) movies_set_last_error(8);
    ret(c,0,address);
}
static void environment_a(CPU *c) { environment(c,1); }
static void environment_w(CPU *c) { environment(c,2); }
static void free_environment(CPU *c,uint8_t kind) {
    require_init(c);uint32_t address=arg(c,0);int ok=0;
    pthread_mutex_lock(&process_mutex);
    for(unsigned i=0;i<ENV_SLOTS;i++) if(environment_slots[i].kind==kind && environment_slots[i].address==address) {
        environment_slots[i]=(EnvSnapshot){0};ok=1;break;
    }
    pthread_mutex_unlock(&process_mutex);
    if(!ok) movies_set_last_error(87);
    ret(c,1,(uint32_t)ok);
}
static void free_environment_a(CPU *c) { free_environment(c,1); }
static void free_environment_w(CPU *c) { free_environment(c,2); }
static void process_id(CPU *c) { require_init(c); ret(c,0,1); } /* sole virtual process */
int movies_process_elapsed_ticks(uint64_t now_ns,uint64_t epoch_ns,uint32_t *out) {
    if(!out || now_ns<epoch_ns)return 0;
    *out=(uint32_t)((now_ns-epoch_ns)/1000000ull);return 1;
}
int movies_process_ticks(uint32_t *out) {
    uint64_t now;
    if(!initialized || !out || !monotonic_ns(&now))return 0;
    return movies_process_elapsed_ticks(now,clock_epoch,out);
}
static void tick_count(CPU *c) {
    require_init(c);uint32_t now;
    if(!movies_process_ticks(&now)) { guest_trap(c,R32(c->esp),"GetTickCount/timeGetTime virtual monotonic clock failed");return; }
    ret(c,0,now);
}
static void performance_counter(CPU *c) {
    require_init(c); uint32_t out=arg(c,0); uint64_t now;
    if(!range(out,8)) { movies_set_last_error(87); ret(c,1,0); return; }
    if(!monotonic_ns(&now)) { movies_set_last_error(31); ret(c,1,0); return; }
    W64(out,now); ret(c,1,1);
}
static void performance_frequency(CPU *c) {
    uint32_t out=arg(c,0);
    if(!range(out,8)) { movies_set_last_error(87); ret(c,1,0); return; }
    W64(out,1000000000ull); ret(c,1,1); /* nanosecond counter units */
}
static void system_filetime(CPU *c) {
    uint32_t out=arg(c,0); struct timespec ts;
    if(!range(out,8)) { guest_trap(c,R32(c->esp),"GetSystemTimeAsFileTime output outside guest memory"); return; }
    if(clock_gettime(CLOCK_REALTIME,&ts)) { guest_trap(c,R32(c->esp),"GetSystemTimeAsFileTime host clock failed"); return; }
    uint64_t ticks=((uint64_t)ts.tv_sec+11644473600ull)*10000000ull+(uint64_t)ts.tv_nsec/100;
    W64(out,ticks); ret(c,1,0);
}
/* Stored guest callback addresses only. Events are not fabricated or forwarded
 * from host signals; a future virtual-console dispatcher can take a snapshot. */
size_t movies_process_console_snapshot(uint32_t *out,size_t capacity,int *ignore) {
    pthread_mutex_lock(&process_mutex);
    size_t count=console_count;
    if(ignore) *ignore=ignore_ctrl_c;
    if(out) for(size_t i=0;i<count && i<capacity;i++) out[i]=console_handlers[count-1-i];
    pthread_mutex_unlock(&process_mutex);
    return count;
}
static void console_control(CPU *c) {
    require_init(c);
    uint32_t callback=arg(c,0), add=arg(c,1), failure=0;
    pthread_mutex_lock(&process_mutex);
    if(!callback) ignore_ctrl_c=add!=0;
    else if(add) {
        if(console_count==console_capacity) {
            size_t next=console_capacity?console_capacity*2:8;
            if(next<console_capacity || next>SIZE_MAX/sizeof *console_handlers) failure=8;
            else {
                uint32_t *grown=realloc(console_handlers,next*sizeof *grown);
                if(!grown) failure=8;
                else { console_handlers=grown;console_capacity=next; }
            }
        }
        if(!failure) console_handlers[console_count++]=callback;
    } else {
        size_t i=console_count;
        while(i && console_handlers[i-1]!=callback) --i;
        if(!i) failure=87;
        else {
            --i;
            memmove(console_handlers+i,console_handlers+i+1,(console_count-i-1)*sizeof *console_handlers);
            --console_count;
        }
    }
    pthread_mutex_unlock(&process_mutex);
    if(failure) movies_set_last_error(failure);
    ret(c,2,failure?0:1);
}
/* https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitprocess
 * This terminates the host diagnostic process/runtime, never returns to guest.
 * Guest DLL detach notifications are not synthesized (no PE DLL loader exists).
 * Preserve the full DWORD in diagnostics before POSIX narrows process status. */
static void process_exit(CPU *c,uint32_t code,const char *api) {
    fprintf(stderr,"[process] %s code=%u (0x%08x)\n",api,code,code);
    if (range(c->esp, 4)) fprintf(stderr,"[process] exit caller=%08x esp=%08x ebp=%08x\n",R32(c->esp),c->esp,c->ebp);
    /* Bounded diagnostic frame-pointer walk. Optimized guest code may omit
     * frames; these words are evidence, not a guaranteed complete backtrace. */
    uint32_t frame = c->ebp;
    for (unsigned i = 0; i < 8 && frame >= c->esp && range(frame, 8); ++i) {
        uint32_t next = R32(frame), caller = R32(frame + 4);
        fprintf(stderr,"[process] exit frame=%08x return=%08x\n",frame,caller);
        if (next <= frame || (uint64_t)next - frame > 0x100000u) break;
        frame = next;
    }
    fflush(NULL);
#ifdef __EMSCRIPTEN__
    emscripten_force_exit((int)code);
#else
    exit((int)(code&0xffu));
#endif
}
static void exit_process(CPU *c) { process_exit(c,arg(c,0),"ExitProcess"); }
/* Match runtime.c cpuid(): CX8 and TSC, no MMX/SSE/3DNow extensions.
 * Report the guest execution environment, never the host Apple/Intel CPU.
 * Microsoft permits FALSE when feature detection is unavailable; unrecognized
 * features therefore stay disabled. No LastError mutation is specified.
 * https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-isprocessorfeaturepresent
 */
static void processor_feature(CPU *c) {
    uint32_t feature=arg(c,0);
    ret(c,1,feature==2 || feature==8);
}
/* Signed 32x32 -> 64 intermediate; half integers round away from zero.
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-muldiv */
static void multiply_divide(CPU *c) {
    int64_t product=(int64_t)(int32_t)arg(c,0)*(int32_t)arg(c,1);
    int64_t denominator=(int32_t)arg(c,2);
    if (!denominator) { ret(c,3,UINT32_MAX); return; }
    int negative=(product<0)!=(denominator<0);
    uint64_t magnitude=product<0?(uint64_t)-product:(uint64_t)product;
    uint64_t divisor=denominator<0?(uint64_t)-denominator:(uint64_t)denominator;
    uint64_t rounded=(magnitude+divisor/2)/divisor;
    uint64_t limit=negative?2147483648ull:2147483647ull;
    if (rounded>limit) { ret(c,3,UINT32_MAX); return; }
    int64_t result=negative?-(int64_t)rounded:(int64_t)rounded;
    ret(c,3,(uint32_t)result);
}
/* MEMORYSTATUS x86 ABI. This private guest exposes only its implemented
 * allocation arena as the memory budget; never host RAM or fictitious swap.
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-memorystatus */
static void global_memory_status(CPU *c) {
    uint32_t out=arg(c,0);
    if(!out||(uint64_t)out+32>MOVIES_PROCESS_SPACE){guest_trap(c,R32(c->esp),"GlobalMemoryStatus invalid output");return;}
    MoviesHeapBudget b;movies_heap_budget(&b);
    W32(out,32);W32(out+4,b.total?(uint32_t)(((uint64_t)b.total-b.free_bytes)*100/b.total):100);
    W32(out+8,b.total);W32(out+12,b.free_bytes);
    W32(out+16,b.total);W32(out+20,b.free_bytes); /* no swap-backed extra commit */
    W32(out+24,b.total);W32(out+28,b.unreserved);
    c->esp+=8; /* VOID: preserve register result and LastError. */
}
/* Original C9A700 consumes SYSTEM_INFO+20 after return C9A73E. Match
 * runtime CPUID leaf1 EAX=0x683 (family6/model8/stepping3), single logical CPU.
 * Address bounds describe the runtime reservation, not the 1GiB browser heap arena.
 * Page/granularity are the x86 virtual ABI; no VirtualAlloc capability is added.
 * https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/ns-sysinfoapi-system_info
 * https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getsysteminfo */
/* No physical battery provider is attached to this virtual process. Use the
 * documented unknown values, not fabricated AC/charge/no-battery readings.
 * Byte3 is Reserved1=0 for the guest's pre-Windows10 ABI; this environment has
 * no virtual battery-saver policy. The12-byte wire layout has no host padding.
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-system_power_status */
static void system_power_status(CPU *c) {
    uint32_t out=arg(c,0);
    if(!range(out,12)){movies_set_last_error(87);ret(c,1,0);return;}
    static const uint8_t unknown[12]={255,255,255,0,255,255,255,255,255,255,255,255};
    memcpy(HP(out),unknown,sizeof unknown);ret(c,1,1);
}
static void system_info(CPU *c) {
    uint32_t out=arg(c,0);
    if(!range(out,36)) { guest_trap(c,R32(c->esp),"GetSystemInfo output outside guest memory");return; }
    const uint32_t fields[9]={0,4096,GUEST_TIB,(uint32_t)(GUEST_SPACE-1),1,1,586,65536,0x08030006};
    /* Explicit little-endian writes support unaligned x86 output pointers. */
    uint8_t bytes[36];for(unsigned i=0;i<9;i++)for(unsigned j=0;j<4;j++)bytes[i*4+j]=(uint8_t)(fields[i]>>(j*8));
    memcpy(HP(out),bytes,sizeof bytes);
    c->esp+=8; /* VOID: preserve CPU result/flags and thread LastError. */
}
/* Observed original affinity fallback: GetCurrentProcess C9A95B,
 * GetProcessAffinityMask C9A96E, SetProcessAffinityMask C9A9A4 then Sleep(0).
 * One virtual processor identity is exposed by CPUID. Host pthread parallelism
 * and host CPU affinity are implementation details, not guest processor IDs.
 * https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getcurrentprocess
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessaffinitymask
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setprocessaffinitymask */
static void current_process(CPU *c) { ret(c,0,UINT32_MAX); }
static void get_process_affinity(CPU *c) {
    uint32_t process=arg(c,0),process_out=arg(c,1),system_out=arg(c,2);
    if(process!=UINT32_MAX) { movies_set_last_error(6);ret(c,3,0);return; }
    if(!range(process_out,4)||!range(system_out,4)) { movies_set_last_error(87);ret(c,3,0);return; }
    const uint8_t one[4]={1,0,0,0};memcpy(HP(process_out),one,4);memcpy(HP(system_out),one,4);ret(c,3,1);
}
static void set_process_affinity(CPU *c) {
    if(arg(c,0)!=UINT32_MAX) { movies_set_last_error(6);ret(c,2,0);return; }
    if(arg(c,1)!=1) { movies_set_last_error(87);ret(c,2,0);return; }
    ret(c,2,1); /* The sole valid mask already describes this virtual process. */
}
/* Original call return BBE684. Neither an application debugger nor a system
 * debugger is attached to this virtual process, so the documented operation is
 * no effect. Do not dereference or scan the optional LPCSTR when there is no
 * recipient: NULL, inaccessible or unterminated input cannot cause an unbounded
 * host read. This does not simulate debugger output or delivery.
 * https://learn.microsoft.com/en-us/windows/win32/api/debugapi/nf-debugapi-outputdebugstringa */
static void output_debug_string_a(CPU *c) {
    c->esp+=8; /* VOID: preserve registers/flags and thread LastError. */
}
/* ---- Cross-module helpers. Production links the owning modules; isolated
 * fixtures without them observe the documented failure paths. */
__attribute__((weak)) int movies_sync_is_process_handle(uint32_t handle){return handle==UINT32_MAX;}
__attribute__((weak)) uint32_t movies_sync_create_tagged(uint32_t tag,uint32_t value){(void)tag;(void)value;return 0;}
__attribute__((weak)) int movies_sync_tagged_get(uint32_t h,uint32_t tag,uint32_t *value){(void)h;(void)tag;(void)value;return 0;}
__attribute__((weak)) int movies_sync_tagged_set(uint32_t h,uint32_t tag,uint32_t value){(void)h;(void)tag;(void)value;return 0;}
__attribute__((weak)) uint32_t movies_local_alloc(uint32_t bytes){(void)bytes;return 0;}
__attribute__((weak)) uint32_t movies_unhandled_exception_filter(void){return 0;}
__attribute__((weak)) GuestFn lookup_function(uint32_t address){(void)address;return NULL;}
__attribute__((weak)) void guest_call(CPU *c,GuestFn fn,int n,const uint32_t *args){(void)c;(void)fn;(void)n;(void)args;}
/* ---- Process creation and devices: unavailable inside a browser tab.
 * https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessa */
static void create_process(CPU *c) { movies_set_last_error(50);ret(c,10,0); }
static void create_pipe(CPU *c) { movies_set_last_error(50);ret(c,4,0); }
static void device_io_control(CPU *c) { movies_set_last_error(50);ret(c,8,0); }
/* Current process (pseudo or duplicated handle) terminates like ExitProcess;
 * there are no other processes to terminate.
 * https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminateprocess */
static void terminate_process(CPU *c) {
    if(!movies_sync_is_process_handle(arg(c,0))) { movies_set_last_error(6);ret(c,2,0);return; }
    process_exit(c,arg(c,1),"TerminateProcess");
}
/* ---- Toolhelp module snapshot: exactly the executable image.
 * MODULEENTRY32 is 548 bytes: 8 DWORD/pointer fields, szModule[256], szExePath[260].
 * https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/ns-tlhelp32-moduleentry32 */
#define SNAPSHOT_TAG 0x534e4150u
static void toolhelp_snapshot(CPU *c) {
    require_init(c);uint32_t flags=arg(c,0),pid=arg(c,1);
    if(pid && pid!=1) { movies_set_last_error(87);ret(c,2,INVALID_HANDLE);return; }
    if(flags&~0x8000001fu) { movies_set_last_error(87);ret(c,2,INVALID_HANDLE);return; }
    /* Heap/process/thread lists are not modeled: an empty snapshot for them. */
    uint32_t handle=movies_sync_create_tagged(SNAPSHOT_TAG,(flags&0x18u)?1u:0u);
    if(!handle) { movies_set_last_error(8);ret(c,2,INVALID_HANDLE);return; }
    ret(c,2,handle);
}
static uint32_t image_size(void) {
    uint32_t nt=range(executable_base,64)?R32(executable_base+0x3c):0;
    if(!nt || nt>0x100000u || !range(executable_base+nt,0x54) || R32(executable_base+nt)!=0x4550)return 0;
    return R32(executable_base+nt+0x50);
}
static void module_entry(CPU *c,int first) {
    require_init(c);uint32_t snapshot=arg(c,0),out=arg(c,1),state;
    if(!movies_sync_tagged_get(snapshot,SNAPSHOT_TAG,&state)) { movies_set_last_error(6);ret(c,2,0);return; }
    if(!range(out,4) || R32(out)<548 || !range(out,548)) { movies_set_last_error(24);ret(c,2,0);return; }
    if(first)state=state?1u:0u;
    if(state!=1u) { movies_set_last_error(18);ret(c,2,0);return; } /* ERROR_NO_MORE_FILES */
    movies_sync_tagged_set(snapshot,SNAPSHOT_TAG,2u);
    memset(HP(out+4),0,544);W32(out,548);
    W32(out+4,1);W32(out+8,1);W32(out+12,0xffff);W32(out+16,0xffff);
    W32(out+20,executable_base);W32(out+24,image_size());W32(out+28,executable_base);
    const char *name=executable_path;for(const char *q=executable_path;*q;q++)if(*q=='\\' || *q=='/')name=q+1;
    size_t n=strlen(name);if(n>255)n=255;memcpy(HP(out+32),name,n);
    n=path_length<259?path_length:259;memcpy(HP(out+288),executable_path,n);
    ret(c,2,1);
}
static void module_first(CPU *c) { module_entry(c,1); }
static void module_next(CPU *c) {
    /* After the first entry state 2 means exhausted; state 0 lists nothing. */
    module_entry(c,0);
}
/* ---- Pointer probes mirror IsBadReadPtr: valid iff the span is guest memory
 * above the reserved null region. Page protection is not modeled. */
static int bad_span(uint32_t p,uint32_t n) { return n && (!p || p<0x10000u || (uint64_t)p+n>GUEST_SPACE); }
static void is_bad_write_ptr(CPU *c) { ret(c,2,bad_span(arg(c,0),arg(c,1))); }
static void is_bad_code_ptr(CPU *c) { ret(c,1,bad_span(arg(c,0),1)); }
/* ---- Console: no console window exists; diagnostics already go to stderr.
 * Console modes are reported only for the standard handles. */
static void console_success(CPU *c) { ret(c,0,1); }
static void set_console_title(CPU *c) { ret(c,1,1); }
static int console_handle(uint32_t h) { return descriptor(h)>=0; }
static void get_console_mode(CPU *c) {
    uint32_t h=arg(c,0),out=arg(c,1);
    if(!console_handle(h)) { movies_set_last_error(6);ret(c,2,0);return; }
    if(!range(out,4)) { movies_set_last_error(87);ret(c,2,0);return; }
    W32(out,h==MOVIES_STD_INPUT_HANDLE?0x7u:0x3u);ret(c,2,1);
}
static void set_console_mode(CPU *c) {
    if(!console_handle(arg(c,0))) { movies_set_last_error(6);ret(c,2,0);return; }
    ret(c,2,1);
}
/* ---- FormatMessageA: FROM_SYSTEM uses a small English table (Win32 codes and
 * common HRESULTs; 0x8007xxxx maps to xxxx). FROM_STRING supports %1..%99
 * with !fmt! (s,c,d,i,u,x,X,lu,ld,lx), %0 %n %r %t %b %% and escapes.
 * FROM_HMODULE has no message tables. ALLOCATE_BUFFER uses LocalAlloc storage.
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-formatmessagea */
static const struct { uint32_t code; const char *text; } system_messages[]={
    {0,"The operation completed successfully."},{1,"Incorrect function."},
    {2,"The system cannot find the file specified."},{3,"The system cannot find the path specified."},
    {4,"The system cannot open the file."},{5,"Access is denied."},{6,"The handle is invalid."},
    {8,"Not enough storage is available to process this command."},{13,"The data is invalid."},
    {14,"Not enough storage is available to complete this operation."},{15,"The system cannot find the drive specified."},
    {18,"There are no more files."},{21,"The device is not ready."},
    {31,"A device attached to the system is not functioning."},
    {32,"The process cannot access the file because it is being used by another process."},
    {38,"Reached the end of the file."},{50,"The request is not supported."},{80,"The file exists."},
    {87,"The parameter is incorrect."},{112,"There is not enough space on the disk."},
    {122,"The data area passed to a system call is too small."},
    {123,"The filename, directory name, or volume label syntax is incorrect."},
    {126,"The specified module could not be found."},{127,"The specified procedure could not be found."},
    {145,"The directory is not empty."},{183,"Cannot create a file when that file already exists."},
    {206,"The filename or extension is too long."},{258,"The wait operation timed out."},
    {487,"Attempt to access invalid address."},{998,"Invalid access to memory location."},
    {1400,"Invalid window handle."},{1813,"The specified resource type cannot be found in the image file."},
    {1814,"The specified resource name cannot be found in the image file."},
    {0x80004001u,"Not implemented"},{0x80004002u,"No such interface supported"},{0x80004003u,"Invalid pointer"},
    {0x80004004u,"Operation aborted"},{0x80004005u,"Unspecified error"},{0x8000ffffu,"Catastrophic failure"},
    {0x80040154u,"Class not registered"},{0x800401f0u,"CoInitialize has not been called."},
};
typedef struct Text { char *data; size_t used,capacity; int failed; } Text;
static void text_put(Text *t,const char *p,size_t n) {
    if(t->failed)return;
    if(t->used+n+1>t->capacity) {
        size_t next=t->capacity?t->capacity*2:256;while(next<t->used+n+1)next*=2;
        if(next>65536) { t->failed=8;return; }
        char *grown=realloc(t->data,next);if(!grown) { t->failed=8;return; }
        t->data=grown;t->capacity=next;
    }
    memcpy(t->data+t->used,p,n);t->used+=n;t->data[t->used]=0;
}
static int guest_string(uint32_t p,Text *t) {
    if(!p) { text_put(t,"(null)",6);return 1; }
    for(uint32_t i=0;i<65536;i++) { if(!range(p+i,1))return 0;char ch=(char)R8(p+i);if(!ch)return 1;text_put(t,&ch,1); }
    return 0;
}
static uint32_t format_insert(Text *t,uint32_t index,const char *spec,uint32_t args,int array) {
    if(!args)return 87;
    uint32_t list=array?args:(range(args,4)?R32(args):0);
    if(!list || !range(list+4*(index-1),4))return 87;
    uint32_t value=R32(list+4*(index-1));
    size_t n=strlen(spec);char conversion=n?spec[n-1]:'s';
    if(conversion=='s')return guest_string(value,t)?0:87;
    /* Accept only flags/width/precision plus an optional l/h, then one integer/char conversion. */
    size_t i=0;while(spec[i] && strchr("-+ #0",spec[i]))i++;
    while(spec[i]>='0' && spec[i]<='9')i++;
    if(spec[i]=='.') { i++;while(spec[i]>='0' && spec[i]<='9')i++; }
    if(spec[i]=='l' || spec[i]=='h')i++;
    if(i!=n-1 || !strchr("cdiuxX",conversion))return 87;
    char format[48],out[96];size_t keep=0;
    format[keep++]='%';for(size_t k=0;k<n-1;k++)if(spec[k]!='l' && spec[k]!='h' && keep<40)format[keep++]=spec[k];
    format[keep++]=conversion;format[keep]=0;
    int written=conversion=='d'||conversion=='i'?snprintf(out,sizeof out,format,(int)(int32_t)value):
        conversion=='c'?snprintf(out,sizeof out,format,(int)(uint8_t)value):snprintf(out,sizeof out,format,(unsigned)value);
    if(written<0 || (size_t)written>=sizeof out)return 87;
    text_put(t,out,(size_t)written);return 0;
}
static uint32_t format_template(Text *t,const char *source,int ignore,uint32_t args,int array) {
    for(const char *p=source;*p;p++) {
        if(*p!='%' || ignore) { text_put(t,p,1);continue; }
        char next=*++p;
        if(!next)break;
        if(next=='0')return 0;
        if(next=='n') { text_put(t,"\r\n",2);continue; }
        if(next=='r') { text_put(t,"\r",1);continue; }
        if(next=='t') { text_put(t,"\t",1);continue; }
        if(next=='b') { text_put(t," ",1);continue; }
        if(next>='1' && next<='9') {
            uint32_t index=(uint32_t)(next-'0');
            if(p[1]>='0' && p[1]<='9')index=index*10+(uint32_t)(*++p-'0');
            char spec[32]="s";
            if(p[1]=='!') {
                const char *end=strchr(p+2,'!');if(!end || end-(p+2)>=(long)sizeof spec || end==p+2)return 87;
                memcpy(spec,p+2,(size_t)(end-(p+2)));spec[end-(p+2)]=0;p=end;
            }
            uint32_t failure=format_insert(t,index,spec,args,array);if(failure)return failure;continue;
        }
        text_put(t,&next,1); /* %% %. %! and other escaped characters */
    }
    return 0;
}
static void format_message(CPU *c) {
    uint32_t flags=arg(c,0),source=arg(c,1),id=arg(c,2),out=arg(c,4),size=arg(c,5),args=arg(c,6),failure=0;
    int ignore=(flags&0x200u)!=0,allocate=(flags&0x100u)!=0,array=(flags&0x2000u)!=0;
    uint32_t width=flags&0xffu;
    Text t={0};char *source_text=NULL;
    if(flags&0x400u) {
        Text s0={0};if(!guest_string(source,&s0) || !source)failure=87;
        if(!failure)failure=format_template(&t,s0.data?s0.data:"",ignore,args,array);free(s0.data);
    } else if(flags&0x1000u) {
        uint32_t code=(id&0xffff0000u)==0x80070000u?(id&0xffffu):id;const char *text=NULL;
        for(unsigned i=0;i<sizeof system_messages/sizeof system_messages[0];i++)if(system_messages[i].code==code || system_messages[i].code==id)text=system_messages[i].text;
        if(!text)failure=317; /* ERROR_MR_MID_NOT_FOUND */
        else {
            size_t n=strlen(text);source_text=malloc(n+3);
            if(!source_text)failure=8;
            else { memcpy(source_text,text,n);memcpy(source_text+n,width==0xffu?"":"\r\n",width==0xffu?1:3);failure=format_template(&t,source_text,ignore,args,array); }
        }
    } else failure=(flags&0x800u)?1813:87;
    free(source_text);
    if(!failure && t.failed)failure=(uint32_t)t.failed;
    uint32_t length=(uint32_t)t.used;
    if(!failure && !length)failure=317;
    if(!failure) {
        if(allocate) {
            if(!range(out,4))failure=87;
            else {
                uint32_t bytes=length+1<size?size:length+1,memory=movies_local_alloc(bytes);
                if(!memory)failure=8;else { memcpy(HP(memory),t.data,length+1);W32(out,memory); }
            }
        } else if(length+1>size)failure=122;
        else if(!range(out,length+1))failure=87;
        else memcpy(HP(out),t.data,length+1);
    }
    free(t.data);
    if(failure) { movies_set_last_error(failure);ret(c,7,0);return; }
    ret(c,7,length);
}
/* ---- Fatal paths stop the strict runtime with a clear diagnostic. */
static void fatal_app_exit(CPU *c) {
    char text[512];Text t={0};
    if(!guest_string(arg(c,1),&t))text_put(&t,"<invalid message pointer>",25);
    snprintf(text,sizeof text,"FatalAppExitA: %s",t.data?t.data:"");free(t.data);
    fprintf(stderr,"[process] %s\n",text);fflush(NULL);guest_trap(c,R32(c->esp),text);
}
static void debug_break(CPU *c) {
    fprintf(stderr,"[process] DebugBreak caller=%08x\n",R32(c->esp));fflush(NULL);
    guest_trap(c,R32(c->esp),"DebugBreak: breakpoint exception without a debugger");
}
/* No debugger: invoke the registered top-level filter (SetUnhandledExceptionFilter)
 * and return its disposition; with none, EXCEPTION_EXECUTE_HANDLER (1) lets the
 * caller terminate. EXCEPTION_POINTERS fields are logged first.
 * https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-unhandledexceptionfilter */
static void unhandled_exception(CPU *c) {
    uint32_t pointers=arg(c,0),record=range(pointers,8)?R32(pointers):0;
    fprintf(stderr,"[process] UnhandledExceptionFilter caller=%08x code=%08x address=%08x\n",R32(c->esp),
        range(record,16)?R32(record):0,range(record,16)?R32(record+12):0);
    uint32_t filter=movies_unhandled_exception_filter();
    if(!filter) { ret(c,1,1);return; }
    GuestFn fn=lookup_function(filter);
    if(!fn) { guest_trap(c,filter,"untranslated unhandled-exception filter");return; }
    CPU nested=*c;
    if(nested.esp<256 || !range(nested.esp-256,256)) { guest_trap(c,R32(c->esp),"exception filter stack outside guest memory");return; }
    nested.esp-=128;uint32_t before=nested.esp;
    guest_call(&nested,fn,1,&pointers);
    if(nested.esp!=before) { guest_trap(c,filter,"exception filter violated stdcall stack contract");return; }
    ret(c,1,nested.eax);
}
/* Fixed virtual NetBIOS name; host identity is never exposed.
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getcomputernamea */
static void computer_name(CPU *c) {
    static const char name[]="MOVIES-PC";uint32_t out=arg(c,0),size=arg(c,1);
    if(!range(size,4)) { movies_set_last_error(87);ret(c,2,0);return; }
    uint32_t capacity=R32(size);
    if(capacity<sizeof name) { W32(size,sizeof name);movies_set_last_error(111);ret(c,2,0);return; }
    if(!range(out,sizeof name)) { movies_set_last_error(87);ret(c,2,0);return; }
    memcpy(HP(out),name,sizeof name);W32(size,sizeof name-1);ret(c,2,1);
}
ImportHandler *movies_process_handlers(uint32_t *count) {
    static ImportHandler handlers[]={
        {"KERNEL32.DLL!GlobalMemoryStatus",global_memory_status},
        {"KERNEL32.DLL!GetSystemInfo",system_info},
        {"KERNEL32.DLL!GetSystemPowerStatus",system_power_status},
        {"KERNEL32.DLL!OutputDebugStringA",output_debug_string_a},
        {"KERNEL32.DLL!GetCurrentProcess",current_process},
        {"KERNEL32.DLL!GetProcessAffinityMask",get_process_affinity},
        {"KERNEL32.DLL!SetProcessAffinityMask",set_process_affinity},
        {"KERNEL32.DLL!GetStartupInfoA",startup_info},
        {"KERNEL32.DLL!ExitProcess",exit_process},
        {"KERNEL32.DLL!IsProcessorFeaturePresent",processor_feature},
        {"KERNEL32.DLL!MulDiv",multiply_divide},
        {"KERNEL32.DLL!SetConsoleCtrlHandler",console_control},
        {"KERNEL32.DLL!GetStdHandle",get_standard}, {"KERNEL32.DLL!SetStdHandle",set_standard},
        {"KERNEL32.DLL!GetFileType",file_type}, {"KERNEL32.DLL!SetHandleCount",handle_count},
        {"KERNEL32.DLL!GetCommandLineA",command_line}, {"KERNEL32.DLL!GetModuleFileNameA",module_filename},
        {"KERNEL32.DLL!SetEnvironmentVariableA",set_environment_a},
        {"KERNEL32.DLL!GetEnvironmentStrings",environment_a}, {"KERNEL32.DLL!GetEnvironmentStringsA",environment_a},
        {"KERNEL32.DLL!GetEnvironmentStringsW",environment_w},
        {"KERNEL32.DLL!FreeEnvironmentStringsA",free_environment_a}, {"KERNEL32.DLL!FreeEnvironmentStringsW",free_environment_w},
        {"KERNEL32.DLL!GetCurrentProcessId",process_id}, {"KERNEL32.DLL!GetTickCount",tick_count},
        /* OriginalC8B92D: same virtual boot epoch and DWORD wrap, no timer request.
         * https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timegettime */
        {"WINMM.DLL!timeGetTime",tick_count},
        {"KERNEL32.DLL!QueryPerformanceCounter",performance_counter}, {"KERNEL32.DLL!QueryPerformanceFrequency",performance_frequency},
        {"KERNEL32.DLL!GetSystemTimeAsFileTime",system_filetime},
        {"KERNEL32.DLL!CreateProcessA",create_process},{"KERNEL32.DLL!CreatePipe",create_pipe},
        {"KERNEL32.DLL!DeviceIoControl",device_io_control},{"KERNEL32.DLL!TerminateProcess",terminate_process},
        {"KERNEL32.DLL!CreateToolhelp32Snapshot",toolhelp_snapshot},{"KERNEL32.DLL!Module32First",module_first},
        {"KERNEL32.DLL!Module32Next",module_next},{"KERNEL32.DLL!IsBadWritePtr",is_bad_write_ptr},
        {"KERNEL32.DLL!IsBadCodePtr",is_bad_code_ptr},{"KERNEL32.DLL!AllocConsole",console_success},
        {"KERNEL32.DLL!FreeConsole",console_success},{"KERNEL32.DLL!GetConsoleMode",get_console_mode},
        {"KERNEL32.DLL!SetConsoleMode",set_console_mode},{"KERNEL32.DLL!SetConsoleTitleA",set_console_title},
        {"KERNEL32.DLL!FormatMessageA",format_message},{"KERNEL32.DLL!FatalAppExitA",fatal_app_exit},
        {"KERNEL32.DLL!DebugBreak",debug_break},{"KERNEL32.DLL!UnhandledExceptionFilter",unhandled_exception},
        {"KERNEL32.DLL!GetComputerNameA",computer_name},
    };
    *count=sizeof handlers/sizeof handlers[0]; return handlers;
}
