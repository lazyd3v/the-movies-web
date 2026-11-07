#define _DARWIN_C_SOURCE
#include "file-api.h"
#include "process-api.h"
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
uint8_t *g_mem;
static uint32_t error;
static int builtin_registered;
int movies_startup_module_available(const char *dll,const char *export) {
    return builtin_registered && !strcasecmp(dll,"test.dll") && !strcmp(export,"ActualFactory");
}
static unsigned extra_closed;
static int close_extra(uint32_t handle) { if(handle!=0x0b000001 || extra_closed)return 0;extra_closed++;return 1; }
static int (*resolver)(uint32_t,uint32_t *);
void movies_set_last_error(uint32_t e) { error=e; }
void movies_process_set_file_resolver(int (*fn)(uint32_t,uint32_t *)) { resolver=fn; }
void guest_trap(CPU *c,uint32_t at,const char *why) { (void)c;(void)at;(void)why;abort(); }
static uint32_t call(const char *name,unsigned n,...) {
    CPU c={0};c.esp=0x100;c.ebx=0x12345;c.esi=0x54321;
    W32(c.esp,0xabcdef);va_list args;va_start(args,n);
    for(unsigned i=0;i<n;i++)W32(c.esp+4+4*i,va_arg(args,uint32_t));va_end(args);
    uint32_t count;ImportHandler *h=movies_file_handlers(&count);
    for(unsigned i=0;i<count;i++)if(!strcmp(h[i].name+13,name)) {
        h[i].fn(&c);assert(c.esp==0x104+4*n && c.ebx==0x12345 && c.esi==0x54321);return c.eax;
    }
    assert(0);return 0;
}
static uint32_t path(const char *text,int wide) {
    for(unsigned i=0;i<=strlen(text);i++)if(wide)W16(0x1000+2*i,(unsigned char)text[i]);else W8(0x1000+i,(unsigned char)text[i]);return 0x1000;
}
static uint32_t open_guest(const char *p,uint32_t access,uint32_t share,uint32_t creation,int wide) {
    return call(wide?"CreateFileW":"CreateFileA",7,path(p,wide),access,share,0u,creation,0x80u,0u);
}
static void host_write(const char *path,const char *data) {
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);assert(fd>=0);assert(write(fd,data,strlen(data))==(ssize_t)strlen(data));assert(!close(fd));
}
static void host_read(const char *path,const char *expected) {
    char data[64]={0};int fd=open(path,O_RDONLY);assert(fd>=0);assert(read(fd,data,sizeof data)==(ssize_t)strlen(expected));assert(!strcmp(data,expected));assert(!close(fd));
}
int main(void) {
    g_mem=calloc(1,0x10000);assert(g_mem);
    char temp[]="/private/tmp/movies-file-XXXXXX";assert(mkdtemp(temp));char game[4096],user[4096],file[4096];
    snprintf(game,sizeof game,"%s/game",temp);snprintf(user,sizeof user,"%s/user",temp);assert(!mkdir(game,0700)&&!mkdir(user,0700));
    snprintf(file,sizeof file,"%s/Data",game);assert(!mkdir(file,0700));
    snprintf(file,sizeof file,"%s/Data/Asset.DAT",game);host_write(file,"original");
    assert(movies_file_init(game,user));uint32_t type=0;assert(resolver && !resolver(0,&type));
    const char *ancestors[]={"C:\\","c:\\Games","C:\\Users","C:\\Users\\Public"};
    for(unsigned i=0;i<4;i++)assert(call("GetFileAttributesA",1,path(ancestors[i],0))==0x10);
    uint32_t ancestor=call("FindFirstFileA",2,path("c:\\Games",0),0x4000u);
    assert(ancestor!=0xffffffff && !strcmp((char *)HP(0x4000+44),"Games") && R32(0x4000)==0x10);
    assert(call("FindClose",1,ancestor));
    ancestor=call("FindFirstFileA",2,path("c:\\Games\\TheMovies",0),0x4000u);
    assert(ancestor!=0xffffffff && !strcmp((char *)HP(0x4000+44),"TheMovies"));assert(call("FindClose",1,ancestor));
    ancestor=call("FindFirstFileA",2,path("C:\\*",0),0x4000u);assert(ancestor!=0xffffffff);
    unsigned roots=0;do { const char *name=(char *)HP(0x4000+44);assert(!strcmp(name,"Games") || !strcmp(name,"Users") || !strcmp(name,"Windows"));roots++; }while(call("FindNextFileA",2,ancestor,0x4000u));
    assert(roots==3 && error==18);assert(call("FindClose",1,ancestor));
    assert(call("FindFirstFileA",2,path("C:\\NoSuchRoot",0),0x4000u)==0xffffffff && error==2);
    assert(call("GetFileAttributesA",1,path("C:\\Windows",0))==0x10);
    assert(call("GetFileAttributesW",1,path("C:\\Windows\\System32",1))==0x10);
    ancestor=call("FindFirstFileA",2,path("C:\\Windows\\*",0),0x4000u);assert(ancestor!=0xffffffff && !strcmp((char *)HP(0x4000+44),"System32"));assert(call("FindClose",1,ancestor));
    assert(call("FindFirstFileA",2,path("C:\\Windows\\System32\\*",0),0x4000u)==0xffffffff && error==2);
    assert(call("GetFileAttributesA",1,path("C:\\Windows\\System32\\wmvcore.dll",0))==0xffffffff && error==2);
    assert(open_guest("C:\\Windows\\System32\\new.dll",0x40000000u,3,2,0)==0xffffffff && error==5);
    assert(!call("CreateDirectoryA",2,path("C:\\Windows\\NewDir",0),0u)&&error==5);
    snprintf(file,sizeof file,"%s/Windows",user);assert(access(file,F_OK));snprintf(file,sizeof file,"%s/Data/Asset.DAT",game);

    assert(call("GetFileAttributesA",1,path("C:\\ProgramData",0))==0xffffffff && error==2);
    assert(call("CreateDirectoryA",2,path("C:\\ProgramData",0),0u));
    assert(call("GetFileAttributesA",1,path("C:\\ProgramData",0))==0x10);
    assert(!call("CreateDirectoryA",2,path("C:\\ArbitraryRoot",0),0u) && error==5);
    snprintf(file,sizeof file,"%s/ArbitraryRoot",user);assert(access(file,F_OK));
    snprintf(file,sizeof file,"%s/ProgramData",user);assert(!rmdir(file));
    snprintf(file,sizeof file,"%s/Data/Asset.DAT",game);

    assert(call("FindFirstFileA",2,path("C:\\Users\\Public\\Documents",0),0x4000u)==0xffffffff && error==2);
    assert(call("SetCurrentDirectoryA",1,path("C:\\",0)));
    assert(call("GetCurrentDirectoryA",2,260u,0x2000u)==3 && !strcmp((char *)HP(0x2000),"C:\\"));
    assert(call("SetCurrentDirectoryA",1,path("Games\\TheMovies",0)));
    movies_file_set_close_resolver(close_extra);assert(call("CloseHandle",1,0x0b000001u) && extra_closed==1);
    assert(!call("CloseHandle",1,0x0b000001u) && error==6);movies_file_set_close_resolver(NULL);

    /* Original CRT passes a 12-byte default descriptor with bInheritHandle=1. */
    W32(0x5000,12);W32(0x5004,0);W32(0x5008,1);
    uint32_t inherited=call("CreateFileA",7,path("data/asset.dat",0),0x80000000u,3u,0x5000u,3u,0x08000080u,0u);
    assert(inherited!=0xffffffff && error==0);
    assert(call("ReadFile",5,inherited,0x2000u,8u,0x3000u,0u) && R32(0x3000)==8 && !memcmp(HP(0x2000),"original",8));
    assert(call("CloseHandle",1,inherited));
    W32(0x5008,0);inherited=call("CreateFileW",7,path("data/asset.dat",1),0x80000000u,3u,0x5000u,3u,0x10000080u,0u);
    assert(inherited!=0xffffffff && call("CloseHandle",1,inherited));
    W32(0x5000,8);assert(call("CreateFileA",7,path("data/asset.dat",0),0x80000000u,3u,0x5000u,3u,0x80u,0u)==0xffffffff && error==87);
    W32(0x5000,12);W32(0x5004,0x6000);assert(call("CreateFileA",7,path("data/asset.dat",0),0x80000000u,3u,0x5000u,3u,0x80u,0u)==0xffffffff && error==50);
    assert(call("CreateFileA",7,path("data/asset.dat",0),0x80000000u,3u,0xfffffffcu,3u,0x80u,0u)==0xffffffff && error==87);
    uint32_t h=open_guest("data/asset.dat",0x80000000u,3,3,0);assert(h!=0xffffffff && resolver(h,&type) && type==1);
    assert(call("ReadFile",5,h,0x2000u,4u,0x3000u,0u) && R32(0x3000)==4 && !memcmp(HP(0x2000),"orig",4));
    uint32_t w=open_guest("C:\\Games\\TheMovies\\DATA\\ASSET.dat",0xc0000000u,3,3,1);assert(w!=0xffffffff);
    memcpy(HP(0x2000),"CHANGED!",8);assert(call("WriteFile",5,w,0x2000u,8u,0x3000u,0u));
    assert(call("ReadFile",5,h,0x2000u,4u,0x3000u,0u) && !memcmp(HP(0x2000),"GED!",4));
    assert(call("CloseHandle",1,w) && call("CloseHandle",1,h) && !resolver(h,&type));
    host_read(file,"original");
    h=open_guest("data/asset.dat",0x80000000u,0,3,0);assert(h!=0xffffffff);
    assert(open_guest("data/asset.dat",0x80000000u,3,3,0)==0xffffffff && error==32);
    assert(!call("WriteFile",5,h,0x2000u,1u,0x3000u,0u)&&error==5);
    assert(!call("ReadFile",5,h,0x2000u,1u,0x3000u,1u)&&error==50);
    assert(call("GetFileSize",2,h,0x3000u)==8 && !R32(0x3000));
    assert(call("SetFilePointer",4,h,0xfffffff0u,0u,0u)==0xffffffff && error==131);
    assert(call("CloseHandle",1,h));assert(!call("CloseHandle",1,h)&&error==6);
    h=open_guest("data/asset.dat",4,3,3,0);assert(h!=0xffffffff);
    assert(call("SetFilePointer",4,h,0u,0u,0u)==0);W8(0x2000,'+');assert(call("WriteFile",5,h,0x2000u,1u,0x3000u,0u));assert(call("GetFileSize",2,h,0u)==9);assert(call("CloseHandle",1,h));
    assert(open_guest("data/asset.dat",0x40000000u,3,1,0)==0xffffffff && error==80);
    h=open_guest("data/asset.dat",0x40000000u,3,2,0);assert(h!=0xffffffff && error==183);assert(!call("GetFileSize",2,h,0u));assert(call("CloseHandle",1,h));host_read(file,"original");
    assert(open_guest("../game/Data/Asset.DAT",0x40000000u,3,2,0)==0xffffffff && error==5);
    assert(open_guest("missing/new.dat",0x40000000u,3,2,0)==0xffffffff && error==3);
    assert(call("CreateDirectoryW",2,path("data/NewFolder",1),0u));
    assert(call("GetFileAttributesA",1,path("DATA/newfolder",0))==0x10);
    assert(!call("CreateDirectoryA",2,path("data/newfolder",0),0u)&&error==183);
    assert(call("SetCurrentDirectoryA",1,path("data/NewFolder",0)));
    assert(call("GetCurrentDirectoryA",2,0u,0u)==strlen("C:\\Games\\TheMovies\\data\\NewFolder")+1);
    assert(call("GetCurrentDirectoryA",2,260u,0x2000u));assert(!strcmp((char *)HP(0x2000),"C:\\Games\\TheMovies\\data\\NewFolder"));
    movies_file_set_quota(8ull*1024*1024*1024); /* Preserve sparse >4GiB regression. */
    h=open_guest("save.bin",0xc0000000u,3,4,0);assert(h!=0xffffffff && error==0);
    W32(0x3000,1);assert(call("SetFilePointer",4,h,3u,0x3000u,0u)==3 && R32(0x3000)==1);
    W8(0x2000,'x');assert(call("WriteFile",5,h,0x2000u,1u,0x3004u,0u));assert(call("GetFileSize",2,h,0x3000u)==4 && R32(0x3000)==1);assert(call("CloseHandle",1,h));
    snprintf(file,sizeof file,"%s/Users",user);assert(!mkdir(file,0700)||errno==EEXIST);snprintf(file,sizeof file,"%s/Users/Player",user);assert(!mkdir(file,0700)||errno==EEXIST);snprintf(file,sizeof file,"%s/Users/Player/Documents",user);assert(!mkdir(file,0700));
    h=open_guest("c:/users/player/documents/Save.ini",0x40000000u,3,1,1);assert(h!=0xffffffff);assert(call("CloseHandle",1,h));
    assert(call("GetFileAttributesW",1,path("C:\\Users\\Player\\Documents\\save.INI",1))==0x20);
    snprintf(file,sizeof file,"%s/Data/OnlyOriginal.bin",game);host_write(file,"fixture");
    snprintf(file,sizeof file,"%s/Data/LICENSE",game);host_write(file,"license");
    uint32_t search=call("FindFirstFileA",2,path("C:\\Games\\TheMovies\\Data\\*.DaT",0),0x4000u);
    assert(search!=0xffffffff && !strcasecmp((char *)HP(0x4000+44),"asset.dat"));
    assert(R32(0x4000)==0x20 && !R32(0x4000+32) && !R32(0x4000+28)); /* overlay size, not original */
    assert(R64(0x4000+20)>116444736000000000ull && !R8(0x4000+304));
    assert(!call("CloseHandle",1,search) && error==6);
    assert(!call("FindNextFileA",2,search,0x4000u) && error==18);
    assert(!call("FindNextFileA",2,search,0x4000u) && error==18);
    assert(call("FindClose",1,search));assert(!call("FindClose",1,search) && error==6);
    search=call("FindFirstFileA",2,path("C:\\Games\\TheMovies\\Data\\*.*",0),0x4000u);assert(search!=0xffffffff);
    unsigned assets=0,dirs=0,originals=0,licenses=0,total=0;
    do {
        const char *name=(char *)HP(0x4000+44);total++;
        if(!strcasecmp(name,"asset.dat"))assets++;
        if(!strcasecmp(name,"NewFolder")) { dirs++;assert(R32(0x4000)==0x10); }
        if(!strcasecmp(name,"OnlyOriginal.bin"))originals++;
        if(!strcasecmp(name,"LICENSE"))licenses++;
    }while(call("FindNextFileA",2,search,0x4000u));
    assert(error==18 && total==4 && assets==1 && dirs==1 && originals==1 && licenses==1);assert(call("FindClose",1,search));
    search=call("FindFirstFileW",2,path("C:\\Games\\TheMovies\\Data\\NewFolder\\save.b?n",1),0x4000u);assert(search!=0xffffffff);
    const char *found="save.bin";for(unsigned i=0;i<=strlen(found);i++)assert(R16(0x4000+44+2*i)==(unsigned char)found[i]);
    assert(R32(0x4000+28)==1 && R32(0x4000+32)==4);assert(!call("FindNextFileW",2,search,0x4000u)&&error==18);assert(call("FindClose",1,search));
    assert(call("FindFirstFileA",2,path("C:\\Games\\TheMovies\\Data\\absent.*",0),0x4000u)==0xffffffff && error==2);
    assert(call("FindFirstFileA",2,path("C:\\Games\\TheMovies\\missing\\*",0),0x4000u)==0xffffffff && error==3);
    assert(call("FindFirstFileA",2,path("C:\\Games\\TheMovies\\Data\\",0),0x4000u)==0xffffffff && error==123);
    assert(call("FindFirstFileA",2,path("C:\\Games\\TheMovies\\Data\\..\\*",0),0x4000u)==0xffffffff && error==5);
    assert(call("FindFirstFileW",2,path("*",1),0xffffff00u)==0xffffffff && error==87);
    assert(!call("FindNextFileA",2,0u,0x4000u)&&error==6);
    memset(HP(0x1000),'x',260);W8(0x1000+260,0);
    assert(call("FindFirstFileA",2,0x1000u,0x4000u)==0xffffffff && error==206);
    search=call("FindFirstFileA",2,path("C:\\Games\\TheMovies\\Data\\*.",0),0x4000u);assert(search!=0xffffffff);
    total=0;do { assert(!strchr((char *)HP(0x4000+44),'.'));total++; }while(call("FindNextFileA",2,search,0x4000u));
    assert(total==2 && error==18);assert(call("FindClose",1,search));
    snprintf(file,sizeof file,"%s/Data/escape",game);assert(!symlink("/private/tmp",file));
    assert(open_guest("C:\\Games\\TheMovies\\Data\\escape\\oops",0x40000000u,3,2,0)==0xffffffff && error==5);assert(!unlink(file));
    path("C:\\Users\\Player\\Documents\\bad",1);W16(0x1000,0x100);assert(call("CreateFileW",7,0x1000u,0x40000000u,3u,0u,2u,0x80u,0u)==0xffffffff && error==50);
    assert(!movies_file_init(game,game));
    char quota_root[4096];snprintf(quota_root,sizeof quota_root,"%s/quota",temp);assert(!mkdir(quota_root,0700));
    movies_file_set_quota(16);assert(movies_file_init(game,quota_root));
    assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0x6008u,0x6010u));
    assert(R64(0x6000)==16 && R64(0x6008)==16 && R64(0x6010)==16);
    assert(call("GetDiskFreeSpaceExW",4,path("C:\\Games\\TheMovies\\Data",1),0u,0u,0u));
    movies_file_set_quota(0x100000002ull);assert(call("GetDiskFreeSpaceExW",4,0u,0u,0x6008u,0u));assert(R64(0x6008)==0x100000002ull);movies_file_set_quota(16);
    W64(0x6000,0x123456789ull);assert(!call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0xfffffffcu)&&error==87);assert(R64(0x6000)==0x123456789ull);
    assert(!call("GetDiskFreeSpaceExA",4,path("data/missing",0),0u,0u,0u)&&error==3);
    assert(!call("GetDiskFreeSpaceExA",4,path("data/asset.dat",0),0u,0u,0u)&&error==267);
    h=open_guest("data/asset.dat",0x80000000u,3,3,0);assert(h!=0xffffffff);assert(call("CloseHandle",1,h));
    assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&R64(0x6000)==16); /* original bytes excluded */
    h=open_guest("data/asset.dat",0xc0000000u,3,3,0);assert(h!=0xffffffff);
    assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&R64(0x6000)==8); /* full COW charged */
    memset(HP(0x2000),'q',32);assert(call("WriteFile",5,h,0x2000u,8u,0x3000u,0u)&&R32(0x3000)==8);
    assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&R64(0x6000)==8);
    assert(call("WriteFile",5,h,0x2000u,8u,0x3000u,0u));
    assert(!call("WriteFile",5,h,0x2000u,1u,0x3000u,0u)&&error==112 && !R32(0x3000));
    assert(call("GetFileSize",2,h,0u)==16);assert(call("SetFilePointer",4,h,0u,0u,0u)==0);
    assert(call("WriteFile",5,h,0x2000u,1u,0x3000u,0u));assert(call("CloseHandle",1,h));
    assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&R64(0x6000)==0); /* close does not refund */
    assert(open_guest("data/OnlyOriginal.bin",0x40000000u,3,3,0)==0xffffffff && error==112);
    snprintf(file,sizeof file,"%s/game-overlay/data/OnlyOriginal.bin",quota_root);assert(access(file,F_OK));
    h=open_guest("data/asset.dat",0x40000000u,3,5,0);assert(h!=0xffffffff);assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&R64(0x6000)==16);
    assert(call("SetFilePointer",4,h,20u,0u,0u)==20);assert(call("WriteFile",5,h,0u,0u,0x3000u,0u));assert(call("GetFileSize",2,h,0u)==0);
    assert(call("SetFilePointer",4,h,15u,0u,0u)==15);assert(call("WriteFile",5,h,0x2000u,1u,0x3000u,0u));assert(call("GetFileSize",2,h,0u)==16);assert(call("CloseHandle",1,h));
    h=open_guest("data/asset.dat",4u,3,3,0);assert(h!=0xffffffff);assert(call("SetFilePointer",4,h,0u,0u,0u)==0);assert(!call("WriteFile",5,h,0x2000u,1u,0x3000u,0u)&&error==112);assert(call("CloseHandle",1,h));
    h=open_guest("data/asset.dat",0x40000000u,3,2,0);assert(h!=0xffffffff && call("CloseHandle",1,h));
    /* POSIX can complete only part of a write; account actual resulting size. */
    struct rlimit old_limit,limited;assert(!getrlimit(RLIMIT_FSIZE,&old_limit));limited=old_limit;limited.rlim_cur=10;
    h=open_guest("data/partial.bin",0x40000000u,3,1,0);assert(h!=0xffffffff);
    void (*previous_signal)(int)=signal(SIGXFSZ,SIG_IGN);assert(previous_signal!=SIG_ERR);assert(!setrlimit(RLIMIT_FSIZE,&limited));
    uint32_t partial_ok=call("WriteFile",5,h,0x2000u,16u,0x3000u,0u);uint32_t partial_bytes=R32(0x3000);
    assert(!setrlimit(RLIMIT_FSIZE,&old_limit));signal(SIGXFSZ,previous_signal);
    assert(partial_ok && partial_bytes==10);assert(call("CloseHandle",1,h));
    assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&R64(0x6000)==6);
    /* Existing data survives lowering the limit; free space saturates at zero. */
    movies_file_set_quota(4);assert(movies_file_init(game,quota_root));
    assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0x6008u,0u)&&R64(0x6000)==0 && R64(0x6008)==4);
    h=open_guest("data/partial.bin",0x80000000u,3,3,0);assert(h!=0xffffffff);assert(call("ReadFile",5,h,0x2000u,16u,0x3000u,0u)&&R32(0x3000)==10);assert(call("CloseHandle",1,h));
    snprintf(file,sizeof file,"%s/unsafe",quota_root);assert(!symlink("/private/tmp",file));
    assert(!call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&error==5);assert(!movies_file_init(game,quota_root));assert(!unlink(file));
    h=open_guest("data/partial.bin",0x40000000u,3,2,0);assert(h!=0xffffffff && call("CloseHandle",1,h));
    assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&R64(0x6000)==4);
    char linked_source[4096];snprintf(linked_source,sizeof linked_source,"%s/game-overlay/data/partial.bin",quota_root);
    snprintf(file,sizeof file,"%s/alias",quota_root);assert(!link(linked_source,file));
    assert(!movies_file_init(game,quota_root));assert(!call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&error==5);assert(!unlink(file));
    assert(movies_file_init(game,quota_root));
    const char *temp_virtual="C:\\Users\\Player\\AppData\\Local\\Temp\\";
    uint32_t temp_length=(uint32_t)strlen(temp_virtual);
    assert(call("GetTempPathA",2,0u,0u)==temp_length+1);
    W8(0x7000,0xaa);assert(call("GetTempPathA",2,temp_length,0x7000u)==temp_length+1 && R8(0x7000)==0xaa);
    assert(call("GetTempPathA",2,temp_length+1,0x7000u)==temp_length && !strcmp((char *)HP(0x7000),temp_virtual));
    assert(call("GetTempPathW",2,temp_length+1,0x7000u)==temp_length);
    for(unsigned i=0;i<=temp_length;i++)assert(R16(0x7000+2*i)==(unsigned char)temp_virtual[i]);
    assert(!call("GetTempPathW",2,260u,0xfffffff0u)&&error==87);
    memcpy(HP(0x1200),"abcdef",7);
    uint32_t unique=call("GetTempFileNameA",4,path(temp_virtual,0),0x1200u,0x12340001u,0x7000u);
    assert(unique==0x12340001u && strstr((char *)HP(0x7000),"abc0001.TMP"));
    char generated[260];strcpy(generated,(char *)HP(0x7000));
    assert(call("GetFileAttributesA",1,path(generated,0))==0xffffffff && error==2); /* nonzero only derives */
    unique=call("GetTempFileNameA",4,path(temp_virtual,0),0x1200u,0u,0x7000u);assert(unique && unique<=65535);
    strcpy(generated,(char *)HP(0x7000));assert(call("GetFileAttributesA",1,path(generated,0))==0x20);
    h=open_guest(generated,0x80000000u,3,3,0);assert(h!=0xffffffff && !call("GetFileSize",2,h,0u));assert(call("CloseHandle",1,h));
    uint32_t second=call("GetTempFileNameA",4,path(temp_virtual,0),0x1200u,0u,0x7000u);assert(second && second!=unique && strcmp(generated,(char *)HP(0x7000)));
    W16(0x1200,'x');W16(0x1202,'y');W16(0x1204,'z');W16(0x1206,0);
    assert(call("GetTempFileNameW",4,path(temp_virtual,1),0x1200u,0x10000u,0x7000u)==0x10000u);
    const char *suffix="xyz0000.TMP";for(unsigned i=0;i<=strlen(suffix);i++)assert(R16(0x7000+2*(temp_length+i))==(unsigned char)suffix[i]);
    assert(!call("GetTempFileNameW",4,0u,0x1200u,0u,0x7000u)&&error==87);
    assert(!call("GetTempFileNameW",4,path(temp_virtual,1),0x1200u,0u,0xffffff00u)&&error==87);
    assert(!call("GetTempFileNameW",4,path("C:\\",1),0x1200u,0u,0x7000u)&&error==5);
    assert(!call("GetTempFileNameW",4,path("C:\\Users\\Player\\absent",1),0x1200u,0u,0x7000u)&&error==3);
    assert(call("GetTempFileNameW",4,path("C:\\Users\\Player\\absent",1),0x1200u,123u,0x7000u)==123); /* derive only, no stat */
    char too_long[248];memset(too_long,'a',247);too_long[247]=0;
    assert(!call("GetTempFileNameA",4,path(too_long,0),0x1200u,1u,0x7000u)&&error==111);
    assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&R64(0x6000)==4); /* empty temps charge zero */
    h=open_guest(generated,0xc0000000u,3u,3,0);assert(h!=0xffffffff);
    memcpy(HP(0x2000),"temp",4);assert(call("WriteFile",5,h,0x2000u,4u,0x3000u,0u));
    assert(!call("DeleteFileA",1,path(generated,0))&&error==32);assert(call("CloseHandle",1,h));
    h=open_guest(generated,0x80000000u,7u,3,0);assert(h!=0xffffffff);
    assert(!call("DeleteFileA",1,path(generated,0))&&error==50); /* deferred-delete unsupported, not sharing violation */
    assert(call("ReadFile",5,h,0x2000u,4u,0x3000u,0u)&&R32(0x3000)==4);assert(call("CloseHandle",1,h));
    assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&R64(0x6000)==0);
    assert(call("DeleteFileW",1,path(generated,1)));
    assert(call("GetDiskFreeSpaceExA",4,0u,0x6000u,0u,0u)&&R64(0x6000)==4);
    assert(!call("DeleteFileA",1,path(generated,0))&&error==2);
    assert(!call("DeleteFileA",1,path(temp_virtual,0))&&error==5);
    assert(!call("DeleteFileA",1,path("data/OnlyOriginal.bin",0))&&error==5);
    assert(!call("DeleteFileA",1,path("data/asset.dat",0))&&error==5); /* original plus COW remains immutable */
    assert(!call("DeleteFileA",1,path("data/absent/missing",0))&&error==3);
    assert(!call("DeleteFileW",1,0xffffffffu)&&error==87);
    snprintf(file,sizeof file,"%s/game-overlay/data/partial.bin",quota_root);assert(!chmod(file,0400));
    assert(!call("DeleteFileA",1,path("data/partial.bin",0))&&error==5);assert(!chmod(file,0600));
    assert(call("DeleteFileA",1,path("data/partial.bin",0)));
    const struct { const char *input,*expected; } full_cases[]={
        {"data/./sub/../asset.dat","C:\\Games\\TheMovies\\data\\asset.dat"},
        {"C:relative.txt","C:\\Games\\TheMovies\\relative.txt"},
        {"C:","C:\\Games\\TheMovies"},
        {"\\Users\\Player\\Documents\\","C:\\Users\\Player\\Documents\\"},
        {"../../../outside.txt","C:\\outside.txt"},
        {"D:relative.txt","D:\\relative.txt"},
        {"d:/somewhere/../file","D:\\file"},
        {"\\\\server\\share\\a\\..\\..\\file","\\\\server\\share\\file"},
        {"C:\\","C:\\"},
    };
    for(unsigned i=0;i<sizeof full_cases/sizeof full_cases[0];i++) {
        uint32_t expected_length=(uint32_t)strlen(full_cases[i].expected);
        assert(call("GetFullPathNameA",4,path(full_cases[i].input,0),0u,0u,0u)==expected_length+1);
        W8(0x7000,0xaa);W32(0x7800,0xabcdef);
        assert(call("GetFullPathNameA",4,path(full_cases[i].input,0),expected_length,0x7000u,0x7800u)==expected_length+1 && R8(0x7000)==0xaa && R32(0x7800)==0xabcdef);
        assert(call("GetFullPathNameA",4,path(full_cases[i].input,0),260u,0x7000u,0x7800u)==expected_length);
        assert(!strcmp((char *)HP(0x7000),full_cases[i].expected));
        const char *last=strrchr(full_cases[i].expected,'\\');uint32_t offset=last && last[1]?(uint32_t)(last+1-full_cases[i].expected):0;
        assert(R32(0x7800)==(offset?0x7000+offset:0));
        assert(call("GetFullPathNameW",4,path(full_cases[i].input,1),260u,0x7000u,0x7800u)==expected_length);
        for(unsigned j=0;j<=expected_length;j++)assert(R16(0x7000+2*j)==(unsigned char)full_cases[i].expected[j]);
        assert(R32(0x7800)==(offset?0x7000+2*offset:0));
    }
    assert(call("GetFullPathNameA",4,path("\\\\server\\share",0),260u,0x7000u,0x7800u)==14);
    assert(!strcmp((char *)HP(0x7000),"\\\\server\\share") && !R32(0x7800));
    assert(call("SetCurrentDirectoryA",1,path(temp_virtual,0)));
    assert(call("GetFullPathNameA",4,path("C:next.tmp",0),260u,0x7000u,0u));
    char cwd_full[260];snprintf(cwd_full,sizeof cwd_full,"%snext.tmp",temp_virtual);assert(!strcmp((char *)HP(0x7000),cwd_full));
    assert(call("SetCurrentDirectoryA",1,path("C:\\Games\\TheMovies",0)));
    assert(call("GetFullPathNameA",4,path("../../../outside.txt",0),260u,0x7000u,0u));
    assert(open_guest((char *)HP(0x7000),0x40000000u,3,2,0)==0xffffffff && error==5); /* lexical result grants no backend access */
    assert(!call("GetFullPathNameA",4,path("\\\\?\\C:\\foo",0),260u,0x7000u,0u)&&error==50);
    assert(!call("GetFullPathNameA",4,0u,260u,0x7000u,0u)&&error==87);
    assert(!call("GetFullPathNameW",4,path("a",1),260u,0xffffffffu,0u)&&error==87);
    W8(0x7000,0xaa);assert(!call("GetFullPathNameA",4,path("a",0),260u,0x7000u,0xffffffffu)&&error==87 && R8(0x7000)==0xaa);
    assert(!movies_file_register_builtin_module("test.dll","ActualFactory"));
    assert(!movies_file_register_builtin_module("dinput8.dll","DirectInput8Create"));
    assert(call("GetFileAttributesA",1,path("C:\\Windows\\System32\\dinput8.dll",0))==0xffffffff && error==2);
    builtin_registered=1;
    assert(!movies_file_register_builtin_module("test.dll","WrongExport"));
    assert(!movies_file_register_builtin_module("../test.dll","ActualFactory"));
    assert(movies_file_register_builtin_module("TEST.dll","ActualFactory"));
    assert(movies_file_register_builtin_module("test.DLL","ActualFactory"));
    assert(call("GetFileAttributesA",1,path("c:/windows/system32/TEST.dll",0))==0x21);
    assert(call("GetFileAttributesW",1,path("C:\\Windows\\System32\\test.dll",1))==0x21);
    uint32_t builtin_search=call("FindFirstFileA",2,path("C:\\Windows\\System32\\TEST.DLL",0),0x4000u);
    assert(builtin_search!=0xffffffff && !strcmp((char *)HP(0x4000+44),"test.dll"));
    assert(R32(0x4000)==0x21 && !R32(0x4000+28) && !R32(0x4000+32));assert(!R64(0x4000+4) && !R64(0x4000+12) && !R64(0x4000+20));
    assert(!call("FindNextFileA",2,builtin_search,0x4000u)&&error==18);assert(call("FindClose",1,builtin_search));
    builtin_search=call("FindFirstFileW",2,path("C:\\Windows\\System32\\*.dll",1),0x4000u);assert(builtin_search!=0xffffffff);
    assert(R16(0x4000+44)=='t' && R32(0x4000)==0x21);assert(call("FindClose",1,builtin_search));
    assert(open_guest("C:\\Windows\\System32\\test.dll",0x80000000u,3,3,0)==0xffffffff && error==5);
    assert(open_guest("C:\\Windows\\System32\\test.dll",0x40000000u,3,2,0)==0xffffffff && error==5);
    assert(!call("DeleteFileW",1,path("C:\\Windows\\System32\\test.dll",1))&&error==5);
    assert(call("GetFileAttributesA",1,path("C:\\Windows\\System32\\dinput8.dll",0))==0xffffffff && error==2);
    snprintf(file,sizeof file,"%s/Windows",quota_root);assert(access(file,F_OK));
    builtin_registered=0;assert(call("GetFileAttributesA",1,path("C:\\Windows\\System32\\test.dll",0))==0xffffffff && error==2);
    assert(call("FindFirstFileA",2,path("C:\\Windows\\System32\\*.dll",0),0x4000u)==0xffffffff && error==2);
    movies_file_set_quota(256ull*1024*1024);
    /* Only this uniquely-created test tree is removed; no original assets involved. */
    char command[8192];snprintf(command,sizeof command,"/bin/rm -rf '%s'",temp);assert(!system(command));
    free(g_mem);puts("file ABI, overlay, sharing, path and 64-bit seek tests passed");
}
