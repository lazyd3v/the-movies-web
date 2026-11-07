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
static void exit_process(CPU *c) {
    uint32_t code=arg(c,0);
    fprintf(stderr,"[process] ExitProcess code=%u (0x%08x)\n",code,code);
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
 * Address bounds describe the runtime reservation, not the 256MiB browser heap.
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
    };
    *count=sizeof handlers/sizeof handlers[0]; return handlers;
}
