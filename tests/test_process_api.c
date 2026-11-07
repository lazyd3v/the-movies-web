#include "process-api.h"
#include "heap-api.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <sys/wait.h>
#endif
uint8_t *g_mem;
static uint32_t error;
static MoviesHeapBudget fixture_budget={0x10000000,0x08000000,0x04000000};
void movies_heap_budget(MoviesHeapBudget*out){*out=fixture_budget;}
static jmp_buf trapped;
void movies_set_last_error(uint32_t value) { error=value; }
void guest_trap(CPU *c,uint32_t at,const char *why) { (void)c;(void)at;(void)why;longjmp(trapped,1); }
static uint32_t call(const char *name,unsigned argc,uint32_t a,uint32_t b,uint32_t d) {
    uint32_t count;ImportHandler *h=movies_process_handlers(&count);CPU c={0};c.esp=0x100;c.ebx=0x12345678;
    W32(c.esp,0xdeadbeef);W32(c.esp+4,a);W32(c.esp+8,b);W32(c.esp+12,d);
    for(unsigned i=0;i<count;i++) if(!strcmp(name,strchr(h[i].name,'!')+1)) {
        h[i].fn(&c);assert(c.esp==0x104+4*argc);assert(c.ebx==0x12345678);return c.eax;
    }
    assert(0);return 0;
}
static uint32_t expected_type(int fd) {
    struct stat st;assert(!fstat(fd,&st));
    if(S_ISCHR(st.st_mode))return 2;
    if(S_ISREG(st.st_mode)||S_ISDIR(st.st_mode)||S_ISBLK(st.st_mode))return 1;
    if(S_ISFIFO(st.st_mode)||S_ISSOCK(st.st_mode))return 3;
    return 0;
}
static void debug_output_tests(void) {
    uint32_t count;ImportHandler *handlers=movies_process_handlers(&count);GuestFn fn=NULL;
    for(unsigned i=0;i<count;i++)if(!strcmp(handlers[i].name,"KERNEL32.DLL!OutputDebugStringA"))fn=handlers[i].fn;
    assert(fn);memcpy(HP(0x4500),"original debug message",23);
    /* Last allocated byte is deliberately not NUL. With no debugger, no string
     * access is needed, even for NULL/out-of-range/unterminated optional input. */
    const uint32_t last=MOVIES_PROCESS_DATA_END+15;W8(last,0xff);
    const uint32_t pointers[]={0,0x4500,last,UINT32_MAX};
    for(unsigned i=0;i<4;i++) {
        CPU c={0};c.esp=0x100;c.eax=0xaabbccdd;c.ebx=42;c.ecx=43;c.edx=44;c.esi=45;c.edi=46;c.cf=1;c.zf=1;
        W32(c.esp,0xabcdef);W32(c.esp+4,pointers[i]);error=789;
        CPU expected=c;expected.esp+=8;fn(&c);
        assert(!memcmp(&c,&expected,sizeof c)&&error==789&&R8(last)==0xff);
    }
}
static void power_tests(void) {
    uint32_t count;ImportHandler *handlers=movies_process_handlers(&count);GuestFn fn=NULL;
    for(unsigned i=0;i<count;i++)if(!strcmp(handlers[i].name,"KERNEL32.DLL!GetSystemPowerStatus"))fn=handlers[i].fn;
    assert(fn);CPU c={0};c.esp=0x100;c.eax=0xdeadbeef;c.ebx=88;c.esi=99;c.cf=1;c.zf=1;
    W32(0x100,0xabcdef);W32(0x104,0x4001);memset(HP(0x4000),0xa5,14);error=123;
    CPU expected=c;expected.esp+=8;expected.eax=1;fn(&c);assert(!memcmp(&c,&expected,sizeof c)&&error==123);
    const uint8_t unknown[12]={255,255,255,0,255,255,255,255,255,255,255,255};
    assert(!memcmp(HP(0x4001),unknown,12)&&R8(0x4000)==0xa5&&R8(0x400d)==0xa5);
    const uint32_t limit=MOVIES_PROCESS_DATA_END+16;
    assert(call("GetSystemPowerStatus",1,limit-12,0,0)==1&&error==123&&!memcmp(HP(limit-12),unknown,12));
    memset(HP(limit-12),0xa5,12);
    const uint32_t invalid[]={0,limit-11,UINT32_MAX};
    for(unsigned i=0;i<3;i++){
        error=321;assert(!call("GetSystemPowerStatus",1,invalid[i],0,0)&&error==87);
        for(unsigned j=0;j<12;j++)assert(R8(limit-12+j)==0xa5);
    }
}
static void system_tests(void) {
    uint32_t count;ImportHandler *handlers=movies_process_handlers(&count);GuestFn fn=NULL;
    for(unsigned i=0;i<count;i++)if(!strcmp(handlers[i].name,"KERNEL32.DLL!GetSystemInfo"))fn=handlers[i].fn;
    assert(fn);CPU c={0};c.esp=0x100;c.eax=0xdeadbeef;c.edx=77;c.ebx=88;c.esi=99;c.cf=1;c.zf=1;
    W32(0x100,0xabcdef);W32(0x104,0x4000);memset(HP(0x3fff),0xa5,38);error=123;
    CPU expected=c;expected.esp+=8;fn(&c);assert(!memcmp(&c,&expected,sizeof c)&&error==123);
    assert(R8(0x3fff)==0xa5&&R8(0x4024)==0xa5);
    assert(R16(0x4000)==0&&R16(0x4002)==0&&R32(0x4004)==4096);
    assert(R32(0x4008)==GUEST_TIB&&R32(0x400c)==(uint32_t)(GUEST_SPACE-1));
    assert(R32(0x4010)==1&&R32(0x4014)==1&&R32(0x4018)==586&&R32(0x401c)==65536);
    assert(R16(0x4020)==6&&R16(0x4022)==0x0803);
    uint8_t bytes[36];memcpy(bytes,HP(0x4000),36);call("GetSystemInfo",1,0x4101,0,0);assert(!memcmp(bytes,HP(0x4101),36));
    const uint32_t limit=MOVIES_PROCESS_DATA_END+16;
    call("GetSystemInfo",1,limit-36,0,0);assert(!memcmp(bytes,HP(limit-36),36));
    const uint32_t invalid[]={0,limit-35,UINT32_MAX};
    for(unsigned i=0;i<3;i++){error=321;if(!setjmp(trapped)){call("GetSystemInfo",1,invalid[i],0,0);assert(0);}assert(error==321);}
    error=123;assert(call("GetCurrentProcess",0,0,0,0)==UINT32_MAX&&error==123);
    assert(call("GetProcessAffinityMask",3,UINT32_MAX,0x4200,0x4204)==1&&error==123&&R32(0x4200)==1&&R32(0x4204)==1);
    assert(call("SetProcessAffinityMask",2,UINT32_MAX,1,0)==1&&error==123);
    const uint32_t masks[]={0,2,3,0x80000000u,UINT32_MAX};
    for(unsigned i=0;i<5;i++){assert(!call("SetProcessAffinityMask",2,UINT32_MAX,masks[i],0)&&error==87);}
    assert(!call("SetProcessAffinityMask",2,0,1,0)&&error==6);
    W32(0x4200,0x12345678);W32(0x4204,0x87654321);
    assert(!call("GetProcessAffinityMask",3,0,0x4200,0x4204)&&error==6);
    assert(!call("GetProcessAffinityMask",3,UINT32_MAX,0x4200,0)&&error==87);
    assert(!call("GetProcessAffinityMask",3,UINT32_MAX,0,0x4204)&&error==87);
    assert(!call("GetProcessAffinityMask",3,UINT32_MAX,0x4200,limit-3)&&error==87);
    assert(R32(0x4200)==0x12345678&&R32(0x4204)==0x87654321);
    error=123;assert(call("GetProcessAffinityMask",3,UINT32_MAX,0x4201,0x4207)==1&&error==123);
    const uint8_t one[4]={1,0,0,0};assert(!memcmp(HP(0x4201),one,4)&&!memcmp(HP(0x4207),one,4));
}
static void memory_tests(void){
 error=123;call("GlobalMemoryStatus",1,0x4000,0,0);assert(error==123&&R32(0x4000)==32&&R32(0x4004)==50&&R32(0x4008)==0x10000000&&R32(0x400c)==0x08000000&&R32(0x4010)==0x10000000&&R32(0x4014)==0x08000000&&R32(0x4018)==0x10000000&&R32(0x401c)==0x04000000);
 fixture_budget.free_bytes=fixture_budget.total;fixture_budget.unreserved=fixture_budget.total;call("GlobalMemoryStatus",1,0x4000,0,0);assert(R32(0x4004)==0);fixture_budget.free_bytes=0;fixture_budget.unreserved=0;call("GlobalMemoryStatus",1,0x4000,0,0);assert(R32(0x4004)==100&&R32(0x400c)==0&&R32(0x401c)==0);
 if(!setjmp(trapped)){call("GlobalMemoryStatus",1,0x2100000,0,0);assert(0);}
}
int main(int argc,char **argv) {
    /* Same actual handler is usable as a linked Node/Wasm child-process probe. */
    if(argc==3 && !strcmp(argv[1],"--exit-code")) {
        char *end;unsigned long code=strtoul(argv[2],&end,0);assert(*end==0 && code<=UINT32_MAX);
        g_mem=calloc(1,0x1000);assert(g_mem);call("ExitProcess",1,(uint32_t)code,0,0);
        assert(!"ExitProcess returned");return 99;
    }
    g_mem=calloc(1,MOVIES_PROCESS_DATA_END+16);assert(g_mem);debug_output_tests();system_tests();power_tests();memory_tests();
    W32(MOVIES_PROCESS_DATA_BASE-4,0x12345678);W32(MOVIES_PROCESS_DATA_END,0x87654321);
    const char *path="C:\\Game Files\\MoviesSE.exe";
    assert(movies_process_init(0x400000,path));
    error=123;
    for(uint32_t feature=0;feature<64;feature++) {
        assert(call("IsProcessorFeaturePresent",1,feature,0,0)==(feature==2 || feature==8));
        assert(error==123);
    }
    assert(!call("IsProcessorFeaturePresent",1,0xffffffffu,0,0));
    const int32_t scaling[][4]={
        {10,96,72,13},{1,1,2,1},{-1,1,2,-1},{1,1,-2,-1},
        {-1,1,-2,1},{1,1,3,0},{-1,1,3,0},{2,1,3,1},
        {INT32_MAX,INT32_MAX,INT32_MAX,INT32_MAX},
        {INT32_MIN,1,1,INT32_MIN},{INT32_MIN,1,INT32_MIN,1},
        {INT32_MIN,-1,1,-1},{INT32_MAX,2,1,-1},
        {INT32_MIN,2,1,-1},{0,INT32_MIN,-1,0},{10,2,0,-1}
    };
    for(unsigned i=0;i<sizeof scaling/sizeof scaling[0];i++) {
        assert((int32_t)call("MulDiv",3,scaling[i][0],scaling[i][1],scaling[i][2])==scaling[i][3]);
        assert(error==123);
    }
    assert(R32(MOVIES_PROCESS_DATA_BASE-4)==0x12345678 && R32(MOVIES_PROCESS_DATA_END)==0x87654321);
    uint32_t command=call("GetCommandLineA",0,0,0,0);
    assert(command>=MOVIES_PROCESS_DATA_BASE && command<MOVIES_PROCESS_DATA_END);
    assert(!strcmp((char *)HP(command),"\"C:\\Game Files\\MoviesSE.exe\""));
    memset(HP(0x1000),0xa5,80);call("GetStartupInfoA",1,0x1000,0,0);
    assert(R32(0x1000)==68 && R32(0x1000+44)==0x100 && R8(0x1000+68)==0xa5);
    assert(R32(0x1000+52)==0 && R16(0x1000+50)==0);
    for(unsigned i=0;i<3;i++) {
        uint32_t handle=call("GetStdHandle",1,(uint32_t)-10-i,0,0);
        assert(handle==MOVIES_STD_INPUT_HANDLE+i);
        assert(R32(0x1000+56+4*i)==handle);
        assert(call("GetFileType",1,handle,0,0)==expected_type((int)i));
    }
    assert(!call("GetFileType",1,0x88888888,0,0) && error==6);
    assert(call("GetStdHandle",1,100,0,0)==0xffffffffu && error==87);
    assert(!call("SetStdHandle",2,(uint32_t)-11,0x1234,0) && error==6);
    assert(call("SetStdHandle",2,(uint32_t)-11,MOVIES_STD_ERROR_HANDLE,0));
    assert(call("GetStdHandle",1,(uint32_t)-11,0,0)==MOVIES_STD_ERROR_HANDLE);
    call("GetStartupInfoA",1,0x1000,0,0);assert(R32(0x1000+60)==MOVIES_STD_OUTPUT_HANDLE);
    assert(call("SetStdHandle",2,(uint32_t)-11,0,0));assert(!call("GetStdHandle",1,(uint32_t)-11,0,0));
    assert(call("SetHandleCount",1,512,0,0)==512);
    uint32_t callbacks[4]={0};int ignore=-1;
    assert(movies_process_console_snapshot(callbacks,4,&ignore)==0 && ignore==0);
    error=123;
    assert(call("SetConsoleCtrlHandler",2,0xAD53A3,1,0) && error==123);
    assert(call("SetConsoleCtrlHandler",2,0x401000,2,0)); /* nonzero BOOL */
    assert(call("SetConsoleCtrlHandler",2,0xAD53A3,1,0)); /* list retains duplicate registrations */
    assert(movies_process_console_snapshot(callbacks,4,&ignore)==3);
    assert(callbacks[0]==0xAD53A3 && callbacks[1]==0x401000 && callbacks[2]==0xAD53A3);
    assert(call("SetConsoleCtrlHandler",2,0xAD53A3,0,0));
    assert(movies_process_console_snapshot(callbacks,1,&ignore)==2 && callbacks[0]==0x401000);
    assert(!call("SetConsoleCtrlHandler",2,0x999999,0,0) && error==87);
    assert(call("SetConsoleCtrlHandler",2,0,1,0));
    assert(movies_process_console_snapshot(NULL,0,&ignore)==2 && ignore==1);
    assert(call("SetConsoleCtrlHandler",2,0,0,0));
    assert(movies_process_console_snapshot(NULL,0,&ignore)==2 && ignore==0);
    for(unsigned i=0;i<100;i++) assert(call("SetConsoleCtrlHandler",2,0x500000+i,1,0));
    assert(movies_process_console_snapshot(callbacks,4,NULL)==102 && callbacks[0]==0x500063);
    assert(movies_process_init(0x400000,path));
    assert(movies_process_console_snapshot(NULL,0,&ignore)==0 && ignore==0);
    assert(!call("SetConsoleCtrlHandler",2,0xAD53A3,0,0) && error==87);
    size_t length=strlen(path);
    error=123;assert(call("GetModuleFileNameA",3,0,0x2000,100)==length);
    assert(!strcmp((char *)HP(0x2000),path) && error==123);
    memset(HP(0x2000),0xa5,100);error=123;
    assert(call("GetModuleFileNameA",3,0x400000,0x2000,5)==5 && error==0);
    assert(!memcmp(HP(0x2000),path,5) && R8(0x2005)==0xa5); /* documented XP truncation */
    error=123;assert(!call("GetModuleFileNameA",3,0,0,0) && error==0);
    assert(!call("GetModuleFileNameA",3,0x9999,0x2000,100) && error==126);
    assert(!call("GetModuleFileNameA",3,0,0,100) && error==87);
    assert(!call("GetModuleFileNameA",3,0,0xfffffff0,100) && error==87);
    uint32_t a=call("GetEnvironmentStringsA",0,0,0,0),w=call("GetEnvironmentStringsW",0,0,0,0);
    assert(a!=w && a>=MOVIES_PROCESS_DATA_BASE && w<MOVIES_PROCESS_DATA_END);
    assert(R16(a)==0 && R32(w)==0);
    assert(!call("FreeEnvironmentStringsA",1,w,0,0) && error==87);
    assert(call("FreeEnvironmentStringsA",1,a,0,0));
    assert(!call("FreeEnvironmentStringsA",1,a,0,0) && error==87);
    assert(call("FreeEnvironmentStringsW",1,w,0,0));
    uint32_t blocks[256];for(unsigned i=0;i<256;i++) { blocks[i]=call("GetEnvironmentStringsW",0,0,0,0);assert(blocks[i]); }
    assert(!call("GetEnvironmentStringsW",0,0,0,0) && error==8);
    for(unsigned i=0;i<256;i++)assert(call("FreeEnvironmentStringsW",1,blocks[i],0,0));
    strcpy((char *)HP(0x4000),"=C:");strcpy((char *)HP(0x4100),"C:\\Games\\TheMovies");
    error=321;assert(call("SetEnvironmentVariableA",2,0x4000,0x4100,0) && error==321);
    uint32_t oldenv=call("GetEnvironmentStringsA",0,0,0,0);
    assert(!strcmp((char *)HP(oldenv),"=C:=C:\\Games\\TheMovies"));
    assert(R8(oldenv+strlen((char *)HP(oldenv))+1)==0);
    strcpy((char *)HP(0x4000),"path");strcpy((char *)HP(0x4100),"first");
    assert(call("SetEnvironmentVariableA",2,0x4000,0x4100,0));
    strcpy((char *)HP(0x4000),"PATH");strcpy((char *)HP(0x4100),"second\x80");
    assert(call("SetEnvironmentVariableA",2,0x4000,0x4100,0));
    a=call("GetEnvironmentStringsA",0,0,0,0);w=call("GetEnvironmentStringsW",0,0,0,0);
    uint32_t next=a+(uint32_t)strlen((char *)HP(a))+1;
    assert(!strcmp((char *)HP(next),"PATH=second\x80") && !R8(next+13));
    unsigned k=0;while(R16(w+k*2))++k;++k;
    const char *expected="PATH=second";
    for(unsigned i=0;i<strlen(expected);i++) assert(R16(w+(k+i)*2)==(uint8_t)expected[i]);
    assert(R16(w+(k+11)*2)==0x20ac && R16(w+(k+12)*2)==0 && R16(w+(k+13)*2)==0);
    assert(!strcmp((char *)HP(oldenv),"=C:=C:\\Games\\TheMovies")); /* independent snapshot */
    assert(!call("FreeEnvironmentStringsA",1,a+8,0,0) && error==87);
    assert(!call("FreeEnvironmentStringsA",1,w,0,0) && error==87);
    assert(call("FreeEnvironmentStringsA",1,oldenv,0,0));
    assert(call("FreeEnvironmentStringsA",1,a,0,0));assert(call("FreeEnvironmentStringsW",1,w,0,0));
    W8(0x4100,0);assert(call("SetEnvironmentVariableA",2,0x4000,0x4100,0));
    a=call("GetEnvironmentStringsA",0,0,0,0);next=a+(uint32_t)strlen((char *)HP(a))+1;
    assert(!strcmp((char *)HP(next),"PATH="));assert(call("FreeEnvironmentStringsA",1,a,0,0));
    assert(call("SetEnvironmentVariableA",2,0x4000,0,0)); /* NULL deletes, empty does not */
    assert(call("SetEnvironmentVariableA",2,0x4000,0,0)); /* missing delete succeeds */
    strcpy((char *)HP(0x4000),"bad=name");assert(!call("SetEnvironmentVariableA",2,0x4000,0x4100,0) && error==87);
    W8(0x4000,0);assert(!call("SetEnvironmentVariableA",2,0x4000,0x4100,0) && error==87);
    assert(!call("SetEnvironmentVariableA",2,0,0x4100,0) && error==87);
    assert(!call("SetEnvironmentVariableA",2,0xfffffff0,0,0) && error==87);
    strcpy((char *)HP(0x4000),"large");memset(HP(0x8000),'x',32767);
    assert(!call("SetEnvironmentVariableA",2,0x4000,0x8000,0) && error==206);
    W8(0x8000+32766,0);assert(call("SetEnvironmentVariableA",2,0x4000,0x8000,0));
    strcpy((char *)HP(0x4200),"large2");assert(!call("SetEnvironmentVariableA",2,0x4200,0x8000,0) && error==8);
    uint32_t snapshots[20];unsigned used=0;
    while(used<20 && (snapshots[used]=call("GetEnvironmentStringsW",0,0,0,0))) ++used;
    assert(used>1 && used<20 && error==8);
    for(unsigned i=0;i<used;i++) assert(call("FreeEnvironmentStringsW",1,snapshots[i],0,0));
    assert(call("SetEnvironmentVariableA",2,0x4000,0,0));
    assert(movies_process_init(0x400000,path));
    a=call("GetEnvironmentStringsA",0,0,0,0);assert(R16(a)==0);assert(call("FreeEnvironmentStringsA",1,a,0,0));
    assert(call("GetCurrentProcessId",0,0,0,0)==1);
    assert(call("QueryPerformanceFrequency",1,0x3000,0,0) && R64(0x3000)==1000000000ull);
    assert(!call("QueryPerformanceCounter",1,0,0,0) && error==87);
    assert(!call("QueryPerformanceFrequency",1,0xfffffffc,0,0) && error==87);
    error=123;uint32_t converted=0xdeadbeef;
    const uint64_t epoch=1234567890123ull;
    assert(movies_process_elapsed_ticks(epoch,epoch,&converted)&&converted==0&&error==123);
    assert(movies_process_elapsed_ticks(epoch+999999,epoch,&converted)&&converted==0);
    assert(movies_process_elapsed_ticks(epoch+1000000,epoch,&converted)&&converted==1);
    assert(movies_process_elapsed_ticks(epoch+0xffffffffull*1000000ull,epoch,&converted)&&converted==0xffffffffu);
    assert(movies_process_elapsed_ticks(epoch+0x100000000ull*1000000ull,epoch,&converted)&&converted==0);
    assert(movies_process_elapsed_ticks(epoch+0x100000001ull*1000000ull+999999,epoch,&converted)&&converted==1);
    converted=0xdeadbeef;assert(!movies_process_elapsed_ticks(epoch-1,epoch,&converted)&&converted==0xdeadbeef&&error==123);
    assert(!movies_process_elapsed_ticks(epoch,epoch,NULL)&&error==123);
    uint32_t count;ImportHandler *all=movies_process_handlers(&count);GuestFn tick=NULL,mm=NULL;
    for(unsigned i=0;i<count;i++){if(!strcmp(all[i].name,"KERNEL32.DLL!GetTickCount"))tick=all[i].fn;if(!strcmp(all[i].name,"WINMM.DLL!timeGetTime"))mm=all[i].fn;}
    assert(tick&&tick==mm); /* identical clock, failure, wrap and stdcall path */
    uint32_t before=call("GetTickCount",0,0,0,0),shared_tick;
    error=123;assert(movies_process_ticks(&shared_tick) && shared_tick>=before && error==123);
    assert(!movies_process_ticks(NULL) && error==123);
    uint32_t multimedia=call("timeGetTime",0,0,0,0);assert(multimedia>=shared_tick&&error==123);
    assert(call("GetTickCount",0,0,0,0)>=multimedia&&error==123);
    assert(call("QueryPerformanceCounter",1,0x3000,0,0));uint64_t qpc=R64(0x3000);
    struct timespec delay={0,20000000};nanosleep(&delay,NULL);
    assert(call("GetTickCount",0,0,0,0)>=before+10);
    assert(call("timeGetTime",0,0,0,0)>=multimedia+10&&error==123);
    assert(call("QueryPerformanceCounter",1,0x3000,0,0) && R64(0x3000)>qpc);
    assert(R64(0x3000)-qpc>=10000000ull);
    time_t utc=time(NULL);call("GetSystemTimeAsFileTime",1,0x3000,0,0);
    uint64_t seconds=R64(0x3000)/10000000ull-11644473600ull;
    assert(seconds>=(uint64_t)utc && seconds<=(uint64_t)time(NULL));
    if(!setjmp(trapped)) { call("GetStartupInfoA",1,0xfffffff0,0,0);assert(0); }
    assert(!movies_process_init(0x400000,"bad\"path") && error==87);
#ifndef __EMSCRIPTEN__
    const uint32_t exits[]={1,0xdeadbe01u};
    for(unsigned i=0;i<2;i++) {
        int pipes[2];assert(!pipe(pipes));fflush(NULL);pid_t child=fork();assert(child>=0);
        if(!child) {
            close(pipes[0]);assert(dup2(pipes[1],STDERR_FILENO)>=0);close(pipes[1]);
            call("ExitProcess",1,exits[i],0,0);_exit(99);
        }
        close(pipes[1]);char diagnostic[256]={0};ssize_t n=read(pipes[0],diagnostic,sizeof diagnostic-1);assert(n>0);close(pipes[0]);
        int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==1);
        char expected[80];snprintf(expected,sizeof expected,"ExitProcess code=%u (0x%08x)",exits[i],exits[i]);assert(strstr(diagnostic,expected));
    }
#endif
    free(g_mem);puts("process ABI tests passed");
}
