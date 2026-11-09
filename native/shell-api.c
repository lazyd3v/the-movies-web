/* https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetspecialfolderpatha
 * https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetfolderpathw
 * https://learn.microsoft.com/en-us/windows/win32/shell/csidl
 * Virtual user folders have real private backing directories. No host Documents.
 */
#define _POSIX_C_SOURCE 200809L
#include "shell-api.h"
#include "startup-api.h"
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#ifndef MOVIES_SHELL_SPACE
#define MOVIES_SHELL_SPACE GUEST_SPACE
#endif
#define PATH_CAP 4096
#define INVALID_ARG 0x80070057u
#define ACCESS_DENIED 0x80070005u
#define PATH_MISSING 0x80070003u
static char root[PATH_CAP];
static const struct { uint32_t id;const char *virtual_path,*relative; } folders[]={
    {5,"C:\\Users\\Player\\Documents","Users/Player/Documents"},
    {0x1a,"C:\\Users\\Player\\AppData\\Roaming","Users/Player/AppData/Roaming"},
    {0x1c,"C:\\Users\\Player\\AppData\\Local","Users/Player/AppData/Local"},
    {0x23,"C:\\ProgramData","ProgramData"},
    {0x28,"C:\\Users\\Player","Users/Player"},
    {0x2e,"C:\\Users\\Public\\Documents","Users/Public/Documents"},
};
const char *movies_shell_virtual_path(uint32_t id) {
    for(unsigned i=0;i<sizeof folders/sizeof folders[0];i++)if(folders[i].id==id)return folders[i].virtual_path;
    return NULL;
}
int movies_shell_backend_path(uint32_t id,char *out,size_t capacity) {
    if(!out || !capacity || !root[0])return 0;
    out[0]=0;
    for(unsigned i=0;i<sizeof folders/sizeof folders[0];i++)if(folders[i].id==id) {
        int n=snprintf(out,capacity,"%s/%s",root,folders[i].relative);
        if(n<0 || (size_t)n>=capacity) { out[0]=0;return 0; } return 1;
    }
    return 0;
}
/* Reject symlinks at every component. Backend is private and single-user;
 * this isn't a general filesystem API or a concurrent untrusted directory walker. */
static uint32_t directory(const char *path,int create) {
    char copy[PATH_CAP];size_t length=strlen(path);
    if(length>=sizeof copy || path[0]!='/')return INVALID_ARG;
    memcpy(copy,path,length+1);
    for(size_t i=1;i<=length;i++)if(copy[i]=='/' || !copy[i]) {
        char saved=copy[i];copy[i]=0;
        const char *part=strrchr(copy,'/');part=part?part+1:copy;
        if(!strcmp(part,".") || !strcmp(part,".."))return INVALID_ARG;
        struct stat st;
        if(lstat(copy,&st)) {
            if(errno!=ENOENT)return ACCESS_DENIED;
            if(!create)return PATH_MISSING;
            if(mkdir(copy,0700) && errno!=EEXIST)return ACCESS_DENIED;
            if(lstat(copy,&st))return ACCESS_DENIED;
        }
        if(!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))return ACCESS_DENIED;
        copy[i]=saved;
    }
    return 0;
}
int movies_shell_init(const char *path) {
    root[0]=0;
    if(!path || path[0]!='/' || strlen(path)>=sizeof root-64 || !strcmp(path,"/"))return 0;
    size_t n=strlen(path);while(n>1 && path[n-1]=='/')n--;
    memcpy(root,path,n);root[n]=0;
    char backend[PATH_CAP];
    /* Windows creates these standard profile directories before applications
     * run. SHGetFolderPath without CREATE must therefore find them initially. */
    const uint32_t profile_ids[]={5,0x1a,0x1c};
    for(unsigned i=0;i<sizeof profile_ids/sizeof profile_ids[0];i++) {
        if(!movies_shell_backend_path(profile_ids[i],backend,sizeof backend) || directory(backend,1)) {
            root[0]=0;return 0;
        }
    }
    return 1;
}
static uint32_t resolve_path(uint32_t csidl,uint32_t token,uint32_t flags,uint32_t out,int wide) {
    uint32_t result=0;
    if(!out || (uint64_t)out+260*(wide?2u:1u)>MOVIES_SHELL_SPACE)result=INVALID_ARG;
    else {
        if(wide)W16(out,0);else W8(out,0);
        uint32_t id=csidl&0xff,options=csidl&~0xffu;
        const char *path=movies_shell_virtual_path(id);
        if(!path || (options&~0xf000u) || ((options&0xc000u)==0xc000u) || flags>1)result=INVALID_ARG;
        else if(token)result=0x80070006u; /* no impersonation/default-user token */
        else if(!root[0])result=0x80004005u;
        else if(!(options&0x4000)) {
            char backend[PATH_CAP];
            if(!movies_shell_backend_path(id,backend,sizeof backend))result=0x80004005u;
            else result=directory(backend,!!(options&0x8000));
        }
        if(!result)for(size_t i=0;i<=strlen(path);i++) {
            if(wide)W16(out+2*i,(unsigned char)path[i]);else W8(out+i,(unsigned char)path[i]);
        }
    }
    return result;
}
static void get_path(CPU *c,int wide) {
    if(!g_mem || !c->esp || (uint64_t)c->esp+24>MOVIES_SHELL_SPACE) {
        guest_trap(c,0,"invalid SHGetFolderPath stack");return;
    }
    c->eax=resolve_path(R32(c->esp+8),R32(c->esp+12),R32(c->esp+16),R32(c->esp+20),wide);
    c->esp+=24;
}
/* BOOL wrapper with distinct four-argument ABI, backed by the same verified
 * private directories. Success preserves LastError; failures expose their
 * corresponding Win32 error, never an HRESULT as a truthy BOOL. */
static void special_path(CPU *c,int wide) {
    if(!g_mem || !c->esp || (uint64_t)c->esp+20>MOVIES_SHELL_SPACE) {
        guest_trap(c,0,"invalid SHGetSpecialFolderPath stack");return;
    }
    uint32_t out=R32(c->esp+8),csidl=R32(c->esp+12),create=R32(c->esp+16);
    uint32_t result=resolve_path(csidl|(create?0x8000u:0),0,0,out,wide);
    if(result) movies_set_last_error((result&0xffff0000u)==0x80070000u?result&0xffffu:31);
    c->eax=result?0:1;c->esp+=20;
}
static void special_a(CPU *c) { special_path(c,0); }
static void special_w(CPU *c) { special_path(c,1); }

/* ShellExecuteA(hwnd, verb, file, parameters, directory, show): the browser build
 * cannot launch host documents/programs. The request is logged and reported as
 * successful (>32) so menu choices such as "view the Readme" do not stop the game.
 * https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutea */
static void ascii_arg(uint32_t p,char *out,size_t cap) { size_t n=0;if(p)while(n+1<cap && (uint64_t)p+n<MOVIES_SHELL_SPACE && R8(p+(uint32_t)n)) { out[n]=(char)R8(p+(uint32_t)n);n++; }out[n]=0; }
static void shell_execute_a(CPU *c) {
    if(!g_mem || !c->esp || (uint64_t)c->esp+28>MOVIES_SHELL_SPACE) { guest_trap(c,0,"invalid ShellExecuteA stack");return; }
    char verb[64],file[260],params[260];ascii_arg(R32(c->esp+8),verb,sizeof verb);ascii_arg(R32(c->esp+12),file,sizeof file);ascii_arg(R32(c->esp+16),params,sizeof params);
    fprintf(stderr,"[shell] ShellExecuteA verb=\"%s\" file=\"%s\" params=\"%s\" caller=%08x: not opened in the browser\n",verb,file,params,R32(c->esp));
    c->eax=42;c->esp+=28;
}
static void get_a(CPU *c) { get_path(c,0); }
static void get_w(CPU *c) { get_path(c,1); }
ImportHandler *movies_shell_handlers(uint32_t *count) {
    static ImportHandler handlers[]={
        {"SHFOLDER.DLL!SHGetFolderPathA",get_a},{"SHFOLDER.DLL!SHGetFolderPathW",get_w},
        {"SHELL32.DLL!SHGetFolderPathA",get_a},{"SHELL32.DLL!SHGetFolderPathW",get_w},
        {"SHELL32.DLL!SHGetSpecialFolderPathA",special_a},{"SHELL32.DLL!SHGetSpecialFolderPathW",special_w},
        {"SHELL32.DLL!ShellExecuteA",shell_execute_a},
    };
    *count=sizeof handlers/sizeof handlers[0];return handlers;
}
