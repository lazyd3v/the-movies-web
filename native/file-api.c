/* Synchronous virtual Win32 disk files. Contracts:
 * https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew
 * https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfilepointer
 * https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile
 * ASCII path subset, no device/UNC paths, no guest asynchronous I/O. The original
 * tree is immutable; game-relative mutations materialize in a private overlay.
 */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "file-api.h"
#include "process-api.h"
#include "startup-api.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#ifndef __EMSCRIPTEN__
#include <sys/statvfs.h>
#endif
#define CAP 4096
#define SLOTS 512
#define BAD 0xffffffffu
#define READ 0x80000000u
#define WRITE 0x40000000u
static char game[CAP],userdata[CAP],cwd[CAP]="C:\\Games\\TheMovies";
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static struct File { int fd;uint32_t handle,access,share,inheritable;char key[CAP]; } files[SLOTS];
static uint32_t next_handle=0x09000000;
static int initialized;
/* Logical contents of private regular files, including sparse gaps and copied
 * assets. Directory metadata and immutable original assets are outside quota.
 * Browser storage is volatile MEMFS, not persistent browser storage or reserved RAM. */
static uint64_t quota_limit=1024ull*1024*1024,quota_used; /* saves + exported movies; persisted to OPFS */
static int quota_dirty;
void movies_file_set_quota(uint64_t bytes) { pthread_mutex_lock(&lock);quota_limit=bytes;pthread_mutex_unlock(&lock); }

static int (*close_resolver)(uint32_t);
void movies_file_set_close_resolver(int (*resolver)(uint32_t)) { close_resolver=resolver; }
/* These directories are the namespace connecting the configured mounts, never
 * host directories. Only explicitly listed ancestors exist virtually. */
static int virtual_dir(const char *key) {
    return !strcasecmp(key,"C:\\") || !strcasecmp(key,"C:\\Games") ||
           !strcasecmp(key,"C:\\Users") || !strcasecmp(key,"C:\\Users\\Public") ||
           !strcasecmp(key,"C:\\Windows") || !strcasecmp(key,"C:\\Windows\\System32");
}
static void virtual_stat(struct stat *st) { memset(st,0,sizeof *st);st->st_mode=S_IFDIR|0700; }

/* Presence metadata for actual registered Wasm implementations. No Windows PE
 * bytes exist here: size zero is explicit metadata, and CreateFile is denied. */
#define BUILTIN_LIMIT 32
static struct Builtin { char name[64],required_export[64]; } builtins[BUILTIN_LIMIT];
static unsigned builtin_count;
int movies_file_register_builtin_module(const char *dll,const char *required_export) {
    if(!dll || !required_export || strlen(dll)<5 || strlen(dll)>=64 || strlen(required_export)>=64 ||
       strcasecmp(dll+strlen(dll)-4,".dll"))return 0;
    for(const unsigned char *p=(const unsigned char *)dll;*p;p++)if(!isalnum(*p) && *p!='.' && *p!='_' && *p!='-')return 0;
    if(!movies_startup_module_available(dll,required_export))return 0;
    pthread_mutex_lock(&lock);
    for(unsigned i=0;i<builtin_count;i++)if(!strcasecmp(builtins[i].name,dll)) {
        int same=!strcmp(builtins[i].required_export,required_export);pthread_mutex_unlock(&lock);return same;
    }
    if(builtin_count==BUILTIN_LIMIT) { pthread_mutex_unlock(&lock);return 0; }
    struct Builtin *b=&builtins[builtin_count++];
    for(size_t i=0;i<=strlen(dll);i++)b->name[i]=(char)tolower((unsigned char)dll[i]);
    strcpy(b->required_export,required_export);pthread_mutex_unlock(&lock);return 1;
}
static int builtin_stat(const char *key,struct stat *st) {
    const char *base="C:\\Windows\\System32\\";size_t n=strlen(base);
    if(strncasecmp(key,base,n))return 0;
    for(unsigned i=0;i<builtin_count;i++)if(!strcasecmp(key+n,builtins[i].name) &&
       movies_startup_module_available(builtins[i].name,builtins[i].required_export)) {
        memset(st,0,sizeof *st);st->st_mode=S_IFREG|0444;st->st_nlink=1;return 1;
    }
    return 0;
}
static uint32_t file_attributes(const struct stat *st) {
    return S_ISDIR(st->st_mode)?0x10:0x20|((st->st_mode&S_IWUSR)?0:1);
}
static void reset_searches(void);
static uint32_t error_code(void) {
    switch(errno) {case ENOENT:return 2;case ENOTDIR:return 3;case EEXIST:return 183;
    case EACCES:case EPERM:case ELOOP:return 5;case EMFILE:case ENFILE:return 4;
    case ENOSPC:return 112;case ENAMETOOLONG:return 206;case EINVAL:return 87;
    default:return 31;}
}
static uint32_t scan_usage(const char *path,uint64_t *total,unsigned depth) {
    if(depth>64)return 5;
    DIR *dir=opendir(path);if(!dir)return error_code();
    uint32_t e=0;struct dirent *entry;
    for(;;) {
        errno=0;entry=readdir(dir);if(!entry) { if(errno)e=error_code();break; }
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        char child[CAP];if(snprintf(child,sizeof child,"%s/%s",path,entry->d_name)>=(int)sizeof child) { e=206;break; }
        struct stat st;if(lstat(child,&st)) { e=error_code();break; }
        if(S_ISDIR(st.st_mode))e=scan_usage(child,total,depth+1);
        else if(S_ISREG(st.st_mode) && st.st_nlink==1 && st.st_size>=0) {
            if((uint64_t)st.st_size>UINT64_MAX-*total)e=8;else *total+=(uint64_t)st.st_size;
        } else e=5; /* No symlinks, special files or hard-link aliases in private storage. */
        if(e)break;
    }
    closedir(dir);return e;
}
static uint32_t refresh_usage(void) {
    uint64_t total=0;uint32_t e=scan_usage(userdata,&total,0);
    if(!e) { quota_used=total;quota_dirty=0; }else quota_dirty=1;
    return e;
}
static uint64_t quota_free(void) { return quota_used>=quota_limit?0:quota_limit-quota_used; }
static void account_size(uint64_t before,uint64_t after) {
    if(before>quota_used || after>UINT64_MAX-(quota_used-before)) { quota_dirty=1;return; }
    quota_used=quota_used-before+after;
}
static int range(uint32_t p,uint32_t n) { return p && (uint64_t)p+n<=GUEST_SPACE; }
static uint32_t arg(CPU *c,unsigned i) { return R32(c->esp+4*(i+1)); }
static void ret(CPU *c,unsigned n,uint32_t value) { c->eax=value;c->esp+=4*(n+1); }
static void fail(CPU *c,unsigned n,uint32_t value,uint32_t error) { movies_set_last_error(error);ret(c,n,value); }
static uint32_t guest_path(uint32_t p,int wide,char *out) {
    for(unsigned i=0;i<260;i++) {
        if(!range(p,(i+1)*(wide?2u:1u)))return 87;
        uint32_t ch=wide?R16(p+2*i):R8(p+i);
        if(ch>127)return 50; /* unsupported character set, never truncate */
        out[i]=(char)ch;if(!ch)return i?0:123;
    }
    return 206;
}
/* Opt-in metadata only: never log buffers or contents. Sanitize guest strings
 * so filenames cannot inject new log lines; preserve host errno as well. */
static int log_files_enabled(void) {
    const char *value=getenv("PORT_LOG_FILES");return value && !strcmp(value,"1");
}
static void log_path(const char *api,uint32_t address,int wide,uint32_t handle,uint32_t error) {
    if(!log_files_enabled())return;
    int saved=errno;char path[CAP];
    uint32_t path_error=guest_path(address,wide,path);
    if(path_error)snprintf(path,sizeof path,"<unavailable: error %u>",path_error);
    for(char *p=path;*p;p++)if((unsigned char)*p<32 || *p=='"')*p='?';
    fprintf(stderr,"[file] %s path=\"%s\" result=%08x error=%u\n",api,path,handle,error);
    errno=saved;
}
static void log_read(uint32_t handle,uint32_t requested,uint32_t actual,uint32_t error) {
    if(!log_files_enabled())return;
    int saved=errno;
    fprintf(stderr,"[file] ReadFile handle=%08x requested=%u actual=%u error=%u\n",handle,requested,actual,error);
    errno=saved;
}
static int prefix(const char *s,const char *p) { size_t n=strlen(p);return !strncasecmp(s,p,n) && (!s[n] || s[n]=='\\'); }
static uint32_t normalize(const char *input,char *out) {
    char full[CAP];
    if(input[0] && input[1]==':') {
        if((input[0]!='C' && input[0]!='c') || (input[2]!='\\' && input[2]!='/'))return 123;
        if(strlen(input)>=CAP)return 206;
        strcpy(full,input);
    } else {
        if(input[0]=='/' || input[0]=='\\')return 123;
        if(snprintf(full,sizeof full,"%s\\%s",cwd,input)>=(int)sizeof full)return 206;
    }
    for(char *p=full;*p;p++)if(*p=='/')*p='\\';
    strcpy(out,"C:");size_t n=2;
    char *save=NULL;
    for(char *part=strtok_r(full+3,"\\",&save);part;part=strtok_r(NULL,"\\",&save)) {
        if(!strcmp(part,"."))continue;
        if(!strcmp(part,".."))return 5;
        size_t length=strlen(part);
        if(!length || part[length-1]=='.' || part[length-1]==' ' || strpbrk(part,":*?\"<>|"))return 123;
        for(size_t i=0;i<length;i++)if((unsigned char)part[i]<32)return 123;
        char stem[CAP];strcpy(stem,part);char *dot=strchr(stem,'.');if(dot)*dot=0;
        if(!strcasecmp(stem,"CON") || !strcasecmp(stem,"NUL") || !strcasecmp(stem,"AUX") || !strcasecmp(stem,"PRN") ||
           (strlen(stem)==4 && (!strncasecmp(stem,"COM",3)||!strncasecmp(stem,"LPT",3)) && stem[3]>='1' && stem[3]<='9'))return 50;
        if(n+length+2>=CAP)return 206;
        out[n++]='\\';memcpy(out+n,part,length+1);n+=length;
    }
    if(n==2) { out[n++]='\\';out[n]=0; }
    if(n>=260)return 206;
    if(virtual_dir(out) || prefix(out,"C:\\Windows"))return 0;
    if(!prefix(out,"C:\\Games\\TheMovies") && !prefix(out,"C:\\Users\\Player") &&
       !prefix(out,"C:\\Users\\Public\\Documents") && !prefix(out,"C:\\ProgramData"))return 5;
    return 0;
}
/* Resolve every existing component case-insensitively; no symlink traversal.
 * Missing suffixes are returned for later creation with parents checked separately. */
static uint32_t case_path(const char *base,const char *relative,char *out,int *exists) {
    if(strlen(base)>=CAP || strlen(relative)>=CAP)return 206;
    strcpy(out,base);*exists=1;
    char rest[CAP];strcpy(rest,relative);char *save=NULL;
    for(char *part=strtok_r(rest,"/",&save);part;part=strtok_r(NULL,"/",&save)) {
        char chosen[CAP];strcpy(chosen,part);
        if(*exists) {
            DIR *dir=opendir(out);if(!dir)return error_code();
            struct dirent *entry;int matches=0;
            while((entry=readdir(dir)))if(!strcasecmp(entry->d_name,part)) {
                if(++matches>1) { closedir(dir);return 123; }
                strcpy(chosen,entry->d_name);
            }
            closedir(dir);if(!matches)*exists=0;
        }
        size_t n=strlen(out),m=strlen(chosen);if(n+m+2>CAP)return 206;
        out[n++]='/';memcpy(out+n,chosen,m+1);
        if(*exists) { struct stat st;if(lstat(out,&st))return error_code();if(S_ISLNK(st.st_mode))return 5; }
    }
    return 0;
}
static uint32_t paths(const char *key,char *original,char *overlay,int *original_exists,int *overlay_exists) {
    const char *relative;char rel[CAP],base[CAP];uint32_t e;
    if(prefix(key,"C:\\Windows"))return 2; /* Synthetic read-only system namespace; no host files. */
    *original_exists=0;original[0]=0;
    if(prefix(key,"C:\\Games\\TheMovies")) {
        relative=key+strlen("C:\\Games\\TheMovies");if(*relative)relative++;
        strcpy(rel,relative);for(char *p=rel;*p;p++)if(*p=='\\')*p='/';
        if((e=case_path(game,rel,original,original_exists)))return e;
        if(snprintf(base,sizeof base,"%s/game-overlay",userdata)>=(int)sizeof base)return 206;
    } else {
        strcpy(rel,key+3);for(char *p=rel;*p;p++)if(*p=='\\')*p='/';strcpy(base,userdata);
    }
    return case_path(base,rel,overlay,overlay_exists);
}
static uint32_t lookup(const char *key,char *out,struct stat *st) {
    if(virtual_dir(key)) { out[0]=0;virtual_stat(st);return 0; }
    if(builtin_stat(key,st)) { out[0]=0;return 0; }
    char original[CAP],overlay[CAP];int oe,we;uint32_t e=paths(key,original,overlay,&oe,&we);
    if(e)return e;if(!oe && !we)return 2;
    strcpy(out,we?overlay:original);if(lstat(out,st))return error_code();return 0;
}
static uint32_t parent_exists(const char *key) {
    char parent[CAP],path[CAP];strcpy(parent,key);char *p=strrchr(parent,'\\');if(!p)return 5;
    if(p==parent+2)p[1]=0;else *p=0;
    struct stat st;uint32_t e=lookup(parent,path,&st);if(e)return e==2?3:e;
    return S_ISDIR(st.st_mode)?0:3;
}
static uint32_t mkdir_parents(char *path) {
    for(char *p=path+strlen(userdata)+1;*p;p++)if(*p=='/') {
        *p=0;struct stat st;
        if(lstat(path,&st)) { if(errno!=ENOENT || mkdir(path,0700)) { *p='/';return error_code(); } }
        else if(!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) { *p='/';return 5; }
        *p='/';
    }
    return 0;
}
static struct File *find(uint32_t h) { if(!h)return NULL;for(unsigned i=0;i<SLOTS;i++)if(files[i].handle==h)return &files[i];return NULL; }
static int file_type_resolver(uint32_t h,uint32_t *type) {
    pthread_mutex_lock(&lock);int valid=find(h)!=NULL;pthread_mutex_unlock(&lock);if(valid)*type=1;return valid;
}
static uint32_t initialize_temp(const char *base) {
    char target[CAP];int exists;uint32_t e=case_path(base,"Users/Player/AppData/Local/Temp",target,&exists);
    if(e)return e;
    for(char *p=target+strlen(base)+1;;p++)if(!*p || *p=='/') {
        char saved=*p;*p=0;struct stat st;
        if(lstat(target,&st)) {
            if(errno!=ENOENT || mkdir(target,0700)) { *p=saved;return error_code(); }
        } else if(!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) { *p=saved;return 5; }
        *p=saved;if(!saved)break;
    }
    return 0;
}
int movies_file_init(const char *game_root,const char *userdata_root) {
    if(!game_root || !userdata_root)return 0;
    char a[CAP],b[CAP];struct stat st;
    if(!realpath(game_root,a) || !realpath(userdata_root,b))return 0;
    size_t an=strlen(a),bn=strlen(b);
    if((!strncmp(a,b,bn) && (!a[bn]||a[bn]=='/')) || (!strncmp(b,a,an) && (!b[an]||b[an]=='/')))return 0;
    if(stat(a,&st)||!S_ISDIR(st.st_mode)||stat(b,&st)||!S_ISDIR(st.st_mode))return 0;
    char overlay[CAP];if(snprintf(overlay,sizeof overlay,"%s/game-overlay",b)>=(int)sizeof overlay)return 0;
    if(mkdir(overlay,0700) && errno!=EEXIST)return 0;
    if(lstat(overlay,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return 0;
    if(initialize_temp(b))return 0;
    uint64_t initial_usage=0;if(scan_usage(b,&initial_usage,0))return 0;
    pthread_mutex_lock(&lock);
    for(unsigned i=0;i<SLOTS;i++)if(files[i].handle) { close(files[i].fd);files[i].handle=0; }
    reset_searches();
    strcpy(game,a);strcpy(userdata,b);strcpy(cwd,"C:\\Games\\TheMovies");quota_used=initial_usage;quota_dirty=0;initialized=1;
    pthread_mutex_unlock(&lock);movies_process_set_file_resolver(file_type_resolver);return 1;
}
static uint32_t copy_file(const char *from,const char *to) {
    int in=open(from,O_RDONLY|O_NOFOLLOW);if(in<0)return error_code();
    struct stat st;
    if(fstat(in,&st)) { uint32_t e=error_code();close(in);return e; }
    if(st.st_size<0 || !S_ISREG(st.st_mode)) { close(in);return 5; }
    if((uint64_t)st.st_size>quota_free()) { close(in);return 112; }
    int out=open(to,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(out<0) { uint32_t e=error_code();close(in);return e; }
    uint32_t error=0;uint64_t copied=0;char buf[65536];ssize_t n;
    while((n=read(in,buf,sizeof buf))>0) {
        if((uint64_t)n>quota_free()-copied) { error=112;goto done; }
        for(ssize_t at=0;at<n;) {
            ssize_t wrote=write(out,buf+at,(size_t)(n-at));
            if(wrote<=0) { error=wrote<0?error_code():112;goto done; }
            at+=wrote;copied+=(uint64_t)wrote;
        }
    }
    if(n<0)error=error_code();
    done:close(in);if(close(out) && !error)error=error_code();
    if(error) { if(unlink(to))quota_dirty=1; }else quota_used+=copied;
    return error;
}
/* SECURITY_ATTRIBUTES is 12 bytes in the 32-bit guest. A NULL descriptor
 * uses the private backend's default permissions. Inheritance is stored as
 * guest handle metadata; this does not enable guest process creation. */
static uint32_t security_attributes(uint32_t address,uint32_t *inheritable) {
    *inheritable=0;
    if(!address)return 0;
    if(!range(address,12) || R32(address)!=12)return 87;
    if(R32(address+4))return 50;
    *inheritable=R32(address+8)!=0;return 0;
}
static uint32_t create_impl(CPU *c,int wide,uint32_t *error) {
    char input[CAP],key[CAP],original[CAP],overlay[CAP];uint32_t e=guest_path(arg(c,0),wide,input);
    if(e) { *error=e;return BAD; }
    uint32_t access=arg(c,1),share=arg(c,2),security=arg(c,3),creation=arg(c,4),flags=arg(c,5),template=arg(c,6);
    uint32_t inheritable;
    if((e=security_attributes(security,&inheritable))) { *error=e;return BAD; }
    if((access&~(READ|WRITE|4u)) || (access&4u && access&WRITE) || (share&~7u) || template || (flags&~0x180000a0u)) { *error=50;return BAD; }
    if(creation<1 || creation>5) { *error=87;return BAD; }
    if((creation==2 || creation==5) && !(access&WRITE)) { *error=5;return BAD; }
    if((e=normalize(input,key))) { *error=e;return BAD; }
    if(virtual_dir(key) || prefix(key,"C:\\Windows")) { *error=5;return BAD; }
    struct File *slot=NULL;
    unsigned wanted=((access&READ)?1:0)|((access&(WRITE|4))?2:0);
    for(unsigned i=0;i<SLOTS;i++) {
        if(!files[i].handle) { if(!slot)slot=&files[i];continue; }
        if(!strcasecmp(files[i].key,key)) {
            unsigned prior=((files[i].access&READ)?1:0)|((files[i].access&(WRITE|4))?2:0);
            if((wanted&~files[i].share) || (prior&~share)) { *error=32;return BAD; }
        }
    }
    if(!slot || next_handle==BAD) { *error=4;return BAD; }
    int oe,we;if((e=paths(key,original,overlay,&oe,&we))) { *error=e;return BAD; }
    int exists=oe||we;uint64_t previous_overlay_size=0;
    if(exists) { struct stat st;if(lstat(we?overlay:original,&st)) { *error=error_code();return BAD; }if(!S_ISREG(st.st_mode) || (we && st.st_nlink!=1) || st.st_size<0) { *error=5;return BAD; }if(we)previous_overlay_size=(uint64_t)st.st_size; }
    if(creation==1 && exists) { *error=80;return BAD; }
    if((creation==3 || creation==5) && !exists) { *error=2;return BAD; }
    if(!exists && (e=parent_exists(key))) { *error=e;return BAD; }
    int mutable=(access&(WRITE|4)) || !exists || creation==2 || creation==5;
    const char *selected=we?overlay:original;
    if(mutable) {
        if(quota_dirty && (e=refresh_usage())) { *error=e;return BAD; }
        if((e=mkdir_parents(overlay))) { *error=e;return BAD; }
        if(oe && !we && creation!=2 && creation!=5 && (e=copy_file(original,overlay))) { *error=e;return BAD; }
        selected=overlay;
    }
    int oflags=(access&READ)?((access&(WRITE|4))?O_RDWR:O_RDONLY):((access&(WRITE|4))?O_WRONLY:O_RDONLY);
    if(access&4)oflags|=O_APPEND;
    if(!exists || creation==2 || creation==5)oflags|=O_CREAT;
    if(creation==1)oflags|=O_EXCL;
    if(creation==2 || creation==5)oflags|=O_TRUNC;
    int fd=open(selected,oflags|O_NOFOLLOW,0600);if(fd<0) { *error=error_code();return BAD; }
    if(we && (creation==2 || creation==5))account_size(previous_overlay_size,0);
    /* Readers opened before first copy-on-write must observe the same virtual
     * file after materialization, while retaining their independent offsets. */
    if(mutable && oe && !we)for(unsigned i=0;i<SLOTS;i++)if(files[i].handle && !strcasecmp(files[i].key,key)) {
        off_t offset=lseek(files[i].fd,0,SEEK_CUR);
        int replacement=open(overlay,O_RDONLY|O_NOFOLLOW);
        if(offset<0 || replacement<0 || lseek(replacement,offset,SEEK_SET)<0) {
            *error=error_code();if(replacement>=0)close(replacement);close(fd);return BAD;
        }
        close(files[i].fd);files[i].fd=replacement;
    }
    slot->fd=fd;slot->handle=next_handle++;slot->access=access;slot->share=share;slot->inheritable=inheritable;strcpy(slot->key,key);
    *error=((creation==2 || creation==4) && exists)?183:0;
    return slot->handle;
}
static void create(CPU *c,int wide) {
    pthread_mutex_lock(&lock);uint32_t e=0,h=BAD;
    if(!initialized)e=21;else h=create_impl(c,wide,&e);
    pthread_mutex_unlock(&lock);
    if(log_files_enabled()) {
        int saved=errno;uint32_t inherit=0,security_error=security_attributes(arg(c,3),&inherit);
        fprintf(stderr,"[file] CreateFile parameters access=%08x share=%08x security=%08x disposition=%u flags=%08x template=%08x inherit=%u security_error=%u\n",
                arg(c,1),arg(c,2),arg(c,3),arg(c,4),arg(c,5),arg(c,6),inherit,security_error);errno=saved;
    }
    log_path(wide?"CreateFileW":"CreateFileA",arg(c,0),wide,h,e);movies_set_last_error(e);ret(c,7,h);
}
static void create_a(CPU *c) { create(c,0); }static void create_w(CPU *c) { create(c,1); }
static void io(CPU *c,int writing) {
    uint32_t h=arg(c,0),buffer=arg(c,1),n=arg(c,2),out=arg(c,3),overlapped=arg(c,4);
    if(!range(out,4) || (n && !range(buffer,n))) { if(!writing)log_read(h,n,0,87);fail(c,5,0,87);return; }W32(out,0);
    if(overlapped) { if(!writing)log_read(h,n,0,50);fail(c,5,0,50);return; }
    pthread_mutex_lock(&lock);struct File *f=find(h);uint32_t e=0;ssize_t got=-1;
    if(!f)e=6;
    else if(!(f->access&(writing?(WRITE|4):READ)))e=5;
    else {
        struct stat before;
        if(writing && quota_dirty)e=refresh_usage();
        if(writing && !e) {
            if(fstat(f->fd,&before))e=error_code();
            else if(!S_ISREG(before.st_mode) || before.st_nlink!=1 || before.st_size<0)e=5;
            else {
                off_t position=(f->access&4)?before.st_size:lseek(f->fd,0,SEEK_CUR);
                if(position<0)e=error_code();
                else {
                    uint64_t end=n?(uint64_t)position+n:(uint64_t)before.st_size;
                    uint64_t growth=end>(uint64_t)before.st_size?end-(uint64_t)before.st_size:0;
                    if(growth>quota_free())e=112;
                }
            }
        }
        if(!e) {
            do { got=writing?write(f->fd,HP(buffer),n):read(f->fd,HP(buffer),n); }while(got<0 && errno==EINTR);
            if(got<0)e=error_code();
            if(writing) {
                struct stat after;
                if(fstat(f->fd,&after) || after.st_size<0) { quota_dirty=1;if(!e)e=31; }
                else account_size((uint64_t)before.st_size,(uint64_t)after.st_size);
            }
        }
    }
    pthread_mutex_unlock(&lock);
    if(!writing)log_read(h,n,e?0:(uint32_t)got,e);
    if(e)fail(c,5,0,e);else { W32(out,(uint32_t)got);ret(c,5,1); }
}
static void read_file(CPU *c) { io(c,0); }static void write_file(CPU *c) { io(c,1); }
static void close_file(CPU *c) {
    pthread_mutex_lock(&lock);struct File *f=find(arg(c,0));uint32_t e=0;
    if(!f)e=6;else { if(close(f->fd))e=error_code();f->handle=0; }
    pthread_mutex_unlock(&lock);
    if(e==6 && close_resolver && close_resolver(arg(c,0)))e=0;
    if(e)fail(c,1,0,e);else ret(c,1,1);
}
static void seek_file(CPU *c) {
    uint32_t high=arg(c,2),method=arg(c,3);
    if((high && !range(high,4)) || method>2) { fail(c,4,BAD,87);return; }
    int64_t distance=high?(int64_t)(((uint64_t)R32(high)<<32)|arg(c,1)):(int32_t)arg(c,1);
    pthread_mutex_lock(&lock);struct File *f=find(arg(c,0));uint32_t e=0;off_t position=-1;
    if(!f)e=6;else {
        int64_t base=0;struct stat st;
        if(method==1) { base=lseek(f->fd,0,SEEK_CUR);if(base<0)e=error_code(); }
        if(method==2) { if(fstat(f->fd,&st))e=error_code();else base=st.st_size; }
        if(!e && ((distance<0 && distance < -base) || (distance>0 && base>INT64_MAX-distance)))e=131;
        if(!e) { position=lseek(f->fd,base+distance,SEEK_SET);if(position<0)e=error_code(); }
    }
    pthread_mutex_unlock(&lock);
    if(e)fail(c,4,BAD,e);else { if(high)W32(high,(uint32_t)((uint64_t)position>>32));movies_set_last_error(0);ret(c,4,(uint32_t)position); }
}
static void size_file(CPU *c) {
    uint32_t high=arg(c,1);if(high && !range(high,4)) { fail(c,2,BAD,87);return; }
    pthread_mutex_lock(&lock);struct File *f=find(arg(c,0));struct stat st;uint32_t e=0;
    if(!f)e=6;else if(fstat(f->fd,&st))e=error_code();
    pthread_mutex_unlock(&lock);if(e)fail(c,2,BAD,e);else { if(high)W32(high,(uint32_t)((uint64_t)st.st_size>>32));movies_set_last_error(0);ret(c,2,(uint32_t)st.st_size); }
}
/* GetFileTime: FILETIME = 100 ns intervals since 1601-01-01 (Unix epoch offset
 * 11644473600 s). Creation/access/write all come from the host stat times. */
static void put_filetime(uint32_t out,time_t t,long nsec) {
    if(!out)return;uint64_t v=((uint64_t)(t<0?0:t)+11644473600ull)*10000000ull+(uint64_t)(nsec/100);W32(out,(uint32_t)v);W32(out+4,(uint32_t)(v>>32));
}
static void file_time(CPU *c) {
    uint32_t created=arg(c,1),accessed=arg(c,2),written=arg(c,3);
    if((created && !range(created,8)) || (accessed && !range(accessed,8)) || (written && !range(written,8))) { fail(c,4,0,998);return; }
    pthread_mutex_lock(&lock);struct File *f=find(arg(c,0));struct stat st;uint32_t e=0;
    if(!f)e=6;else if(fstat(f->fd,&st))e=error_code();
    pthread_mutex_unlock(&lock);if(e) { fail(c,4,0,e);return; }
#ifdef __APPLE__
    put_filetime(created,st.st_mtimespec.tv_sec,st.st_mtimespec.tv_nsec);put_filetime(accessed,st.st_atimespec.tv_sec,st.st_atimespec.tv_nsec);put_filetime(written,st.st_mtimespec.tv_sec,st.st_mtimespec.tv_nsec);
#else
    put_filetime(created,st.st_mtim.tv_sec,st.st_mtim.tv_nsec);put_filetime(accessed,st.st_atim.tv_sec,st.st_atim.tv_nsec);put_filetime(written,st.st_mtim.tv_sec,st.st_mtim.tv_nsec);
#endif
    movies_set_last_error(0);ret(c,4,1);
}
static void attributes(CPU *c,int wide) {
    char input[CAP],key[CAP],path[CAP];struct stat st;uint32_t e=guest_path(arg(c,0),wide,input);
    pthread_mutex_lock(&lock);if(!e)e=initialized?normalize(input,key):21;if(!e)e=lookup(key,path,&st);pthread_mutex_unlock(&lock);
    if(e)fail(c,1,BAD,e);else ret(c,1,file_attributes(&st));
}
static void attrs_a(CPU *c) { attributes(c,0); }static void attrs_w(CPU *c) { attributes(c,1); }
static void make_directory(CPU *c,int wide) {
    char input[CAP],key[CAP],original[CAP],overlay[CAP];uint32_t e=guest_path(arg(c,0),wide,input);
    pthread_mutex_lock(&lock);if(!e && arg(c,1))e=50;if(!e)e=initialized?normalize(input,key):21;
    int oe,we;if(!e && virtual_dir(key))e=183;
    if(!e && prefix(key,"C:\\Windows"))e=5;
    if(!e)e=paths(key,original,overlay,&oe,&we);if(!e && (oe||we))e=183;
    if(!e)e=parent_exists(key);if(!e)e=mkdir_parents(overlay);if(!e && mkdir(overlay,0700))e=error_code();
    pthread_mutex_unlock(&lock);if(e)fail(c,2,0,e);else ret(c,2,1);
}
static void mkdir_a(CPU *c) { make_directory(c,0); }static void mkdir_w(CPU *c) { make_directory(c,1); }
static void get_cwd(CPU *c) {
    uint32_t n=arg(c,0),out=arg(c,1);pthread_mutex_lock(&lock);size_t length=strlen(cwd);
    if(n<=length) { pthread_mutex_unlock(&lock);ret(c,2,(uint32_t)length+1);return; }
    if(!range(out,(uint32_t)length+1)) { pthread_mutex_unlock(&lock);fail(c,2,0,87);return; }
    memcpy(HP(out),cwd,length+1);pthread_mutex_unlock(&lock);ret(c,2,(uint32_t)length);
}
static void set_cwd(CPU *c) {
    char input[CAP],key[CAP],path[CAP];struct stat st;uint32_t e=guest_path(arg(c,0),0,input);
    pthread_mutex_lock(&lock);if(!e)e=initialized?normalize(input,key):21;if(!e)e=lookup(key,path,&st);
    if(!e && !S_ISDIR(st.st_mode))e=267;if(!e)strcpy(cwd,key);pthread_mutex_unlock(&lock);
    if(e)fail(c,1,0,e);else ret(c,1,1);
}
/* Snapshot directory searches merge overlay entries ahead of original entries.
 * The backend has no 8.3 aliases; cAlternateFileName therefore remains empty.
 * https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfilea
 * https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-win32_find_dataa */
#define SEARCH_SLOTS 128
struct Found { char name[260];struct stat st;int builtin;struct Found *next; };
static struct Search { uint32_t handle;struct Found *head,*next; } searches[SEARCH_SLOTS];
static uint32_t next_search=0x0a000000;
static void clear_search(struct Search *s) {
    struct Found *p=s->head;while(p) { struct Found *next=p->next;free(p);p=next; }
    memset(s,0,sizeof *s);
}
static void reset_searches(void) { for(unsigned i=0;i<SEARCH_SLOTS;i++)clear_search(&searches[i]); }
static int fold(int ch) { return ch>='A' && ch<='Z'?ch+32:ch; }
static int match_at(const char *pattern,const char *name,unsigned p,unsigned n,signed char memo[261][261]) {
    if(memo[p][n]>=0)return memo[p][n];
    int matched;
    if(!pattern[p])matched=!name[n];
    else if(pattern[p]=='*') {
        int can_consume=name[n] && !(pattern[p+1]=='.' && name[n]=='.' && !strchr(name+n+1,'.'));
        matched=match_at(pattern,name,p+1,n,memo) || (can_consume && match_at(pattern,name,p,n+1,memo));
        /* Windows "name.*" also matches an extensionless name. */
        if(!matched && pattern[p+1]=='.' && pattern[p+2]=='*' && !pattern[p+3] && !name[n])matched=1;
    } else if(pattern[p]=='?') {
        if(!name[n] || name[n]=='.') { unsigned q=p;while(pattern[q]=='?')q++;matched=match_at(pattern,name,q,n,memo); }
        else matched=match_at(pattern,name,p+1,n+1,memo);
    } else if(pattern[p]=='.' && !name[n]) {
        matched=!pattern[p+1] || (pattern[p+1]=='*' && !pattern[p+2]);
    } else matched=name[n] && fold((unsigned char)pattern[p])==fold((unsigned char)name[n]) && match_at(pattern,name,p+1,n+1,memo);
    memo[p][n]=(signed char)!!matched;return matched;
}
static int matches(const char *pattern,const char *name) {
    signed char memo[261][261];memset(memo,-1,sizeof memo);
    return match_at(pattern,name,0,0,memo);
}
static uint32_t collect_directory(struct Search *s,const char *directory,const char *pattern,unsigned *count) {
    DIR *dir=opendir(directory);if(!dir)return error_code();
    struct dirent *entry;uint32_t e=0;
    while((entry=readdir(dir))) {
        if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
        if(strlen(entry->d_name)>=260) { e=206;break; }
        int ascii=1;for(const unsigned char *p=(const unsigned char *)entry->d_name;*p;p++)if(*p>127)ascii=0;
        if(!ascii) { e=50;break; }
        if(!matches(pattern,entry->d_name))continue;
        int duplicate=0;for(struct Found *f=s->head;f;f=f->next)if(!strcasecmp(f->name,entry->d_name)) { duplicate=1;break; }
        if(duplicate)continue;
        char full[CAP];if(snprintf(full,sizeof full,"%s/%s",directory,entry->d_name)>=(int)sizeof full) { e=206;break; }
        struct stat st;if(lstat(full,&st)) { e=error_code();break; }
        if(S_ISLNK(st.st_mode) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) { e=50;break; }
        if(++*count>65536) { e=8;break; }
        struct Found *f=calloc(1,sizeof *f);if(!f) { e=8;break; }
        strcpy(f->name,entry->d_name);f->st=st;f->next=s->head;s->head=f;
    }
    closedir(dir);return e;
}
static uint64_t filetime(time_t seconds) {
    if(seconds < (time_t)-11644473600ll)return 0;
    return ((uint64_t)((int64_t)seconds+11644473600ll))*10000000ull;
}
static void found_data(uint32_t out,int wide,const struct Found *f) {
    memset(HP(out),0,wide?592:320);
    W32(out,file_attributes(&f->st));
if(!f->builtin) {
#ifdef __APPLE__
        W64(out+4,filetime(f->st.st_birthtimespec.tv_sec));
#endif
        W64(out+12,filetime(f->st.st_atime));W64(out+20,filetime(f->st.st_mtime));
    }
    uint64_t size=S_ISREG(f->st.st_mode)?(uint64_t)f->st.st_size:0;
    W32(out+28,(uint32_t)(size>>32));W32(out+32,(uint32_t)size);
    for(unsigned i=0;i<=strlen(f->name);i++) {
        if(wide)W16(out+44+2*i,(unsigned char)f->name[i]);else W8(out+44+i,(unsigned char)f->name[i]);
    }
}
static uint32_t search_impl(const char *input,uint32_t out,int wide,uint32_t *e) {
    char parent[CAP],pattern[260],key[CAP],original[CAP],overlay[CAP],resolved[CAP];
    strcpy(parent,input);for(char *p=parent;*p;p++)if(*p=='/')*p='\\';
    char *last=strrchr(parent,'\\');
    const char *pattern_start=last?last+1:parent;
    if(strlen(pattern_start)>=sizeof pattern) { *e=206;return BAD; }
    if(last) { strcpy(pattern,last+1);*last=0;if(!strcmp(parent,"C:") || !strcmp(parent,"c:"))strcat(parent,"\\"); }
    else { strcpy(pattern,parent);strcpy(parent,"."); }
    if(!pattern[0]) { *e=123;return BAD; }
    if(strpbrk(pattern,":\"<>|")) { *e=123;return BAD; }
    if(!strcmp(pattern,"..")) { *e=5;return BAD; }
    if((*e=normalize(parent,key)))return BAD;
    struct stat st;if((*e=lookup(key,resolved,&st))) { if(*e==2)*e=3;return BAD; }
    if(!S_ISDIR(st.st_mode)) { *e=3;return BAD; }
    int oe=0,we=0;if(!virtual_dir(key) && (*e=paths(key,original,overlay,&oe,&we)))return BAD;
    struct Search *s=NULL;for(unsigned i=0;i<SEARCH_SLOTS;i++)if(!searches[i].handle) { s=&searches[i];break; }
    if(!s || next_search==0x0affffff) { *e=4;return BAD; }
    unsigned count=0;
    if(virtual_dir(key)) {
        const char *children[4]={NULL,NULL,NULL,NULL};
        if(!strcasecmp(key,"C:\\")) { children[0]="Games";children[1]="Users";children[2]="ProgramData";children[3]="Windows"; }
        else if(!strcasecmp(key,"C:\\Games"))children[0]="TheMovies";
        else if(!strcasecmp(key,"C:\\Users")) { children[0]="Player";children[1]="Public"; }
        else if(!strcasecmp(key,"C:\\Users\\Public"))children[0]="Documents";
        else if(!strcasecmp(key,"C:\\Windows"))children[0]="System32";
        for(unsigned i=0;i<4 && !*e;i++)if(children[i] && matches(pattern,children[i])) {
            char child[CAP];
            snprintf(child,sizeof child,"%s%s%s",key,key[strlen(key)-1]=='\\'?"":"\\",children[i]);
            struct stat child_stat;uint32_t child_error=lookup(child,resolved,&child_stat);
            if(child_error==2 || child_error==3)continue;
            if(child_error) { *e=child_error;break; }
            struct Found *f=calloc(1,sizeof *f);if(!f) { *e=8;break; }
            strcpy(f->name,children[i]);f->st=child_stat;f->next=s->head;s->head=f;count++;
        }
    }
    if(!*e && !strcasecmp(key,"C:\\Windows\\System32")) {
        for(unsigned i=0;i<builtin_count;i++)if(matches(pattern,builtins[i].name)) {
            char child[CAP];snprintf(child,sizeof child,"C:\\Windows\\System32\\%s",builtins[i].name);
            struct stat module_stat;if(!builtin_stat(child,&module_stat))continue;
            struct Found *f=calloc(1,sizeof *f);if(!f) { *e=8;break; }
            strcpy(f->name,builtins[i].name);f->st=module_stat;f->builtin=1;f->next=s->head;s->head=f;count++;
        }
    }
    if(!*e && we)*e=collect_directory(s,overlay,pattern,&count);
    if(!*e && oe)*e=collect_directory(s,original,pattern,&count);
    if(*e || !count) { if(!*e)*e=2;clear_search(s);return BAD; }
    s->handle=next_search++;s->next=s->head->next;found_data(out,wide,s->head);return s->handle;
}
static void first_file(CPU *c,int wide) {
    char input[CAP];uint32_t e=guest_path(arg(c,0),wide,input),out=arg(c,1),h=BAD;
    if(!e && !range(out,wide?592:320))e=87;
    pthread_mutex_lock(&lock);if(!e && !initialized)e=21;if(!e)h=search_impl(input,out,wide,&e);pthread_mutex_unlock(&lock);
    log_path(wide?"FindFirstFileW":"FindFirstFileA",arg(c,0),wide,h,e);
    if(e)fail(c,2,BAD,e);else ret(c,2,h);
}
static void first_a(CPU *c) { first_file(c,0); }static void first_w(CPU *c) { first_file(c,1); }
static struct Search *find_search(uint32_t h) {
    if(!h)return NULL;for(unsigned i=0;i<SEARCH_SLOTS;i++)if(searches[i].handle==h)return &searches[i];return NULL;
}
static void next_file(CPU *c,int wide) {
    uint32_t out=arg(c,1);if(!range(out,wide?592:320)) { fail(c,2,0,87);return; }
    pthread_mutex_lock(&lock);struct Search *s=find_search(arg(c,0));uint32_t e=0;
    if(!s)e=6;else if(!s->next)e=18;else { found_data(out,wide,s->next);s->next=s->next->next; }
    pthread_mutex_unlock(&lock);if(e)fail(c,2,0,e);else ret(c,2,1);
}
static void next_a(CPU *c) { next_file(c,0); }static void next_w(CPU *c) { next_file(c,1); }
static void close_search(CPU *c) {
    pthread_mutex_lock(&lock);struct Search *s=find_search(arg(c,0));uint32_t ok=s!=NULL;if(s)clear_search(s);pthread_mutex_unlock(&lock);
    if(ok)ret(c,1,1);else fail(c,1,0,6);
}

/* Private virtual volume capacity, not host disk size. All outputs optional.
 * https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdiskfreespaceexw */
static void disk_space(CPU *c,int wide) {
    uint32_t outputs[3]={arg(c,1),arg(c,2),arg(c,3)};
    for(unsigned i=0;i<3;i++)if(outputs[i] && !range(outputs[i],8)) { fail(c,4,0,87);return; }
    char input[CAP],key[CAP],resolved[CAP];uint32_t e=0;
    if(arg(c,0))e=guest_path(arg(c,0),wide,input);else strcpy(input,"C:\\");
    pthread_mutex_lock(&lock);
    if(!e)e=initialized?normalize(input,key):21;
    struct stat st;if(!e)e=lookup(key,resolved,&st);
    if(e==2)e=3;
    if(!e && !S_ISDIR(st.st_mode))e=267;
    if(!e)e=refresh_usage();
    uint64_t available=quota_free(),free_total=available;
#ifndef __EMSCRIPTEN__
    if(!e) {
        struct statvfs volume;
        if(statvfs(userdata,&volume))e=error_code();
        else {
            uint64_t unit=volume.f_frsize?volume.f_frsize:volume.f_bsize;
            uint64_t host_available=unit && volume.f_bavail>UINT64_MAX/unit?UINT64_MAX:unit*volume.f_bavail;
            uint64_t host_free=unit && volume.f_bfree>UINT64_MAX/unit?UINT64_MAX:unit*volume.f_bfree;
            if(host_available<available)available=host_available;
            if(host_free<free_total)free_total=host_free;
        }
    }
#endif
    if(!e) { if(outputs[0])W64(outputs[0],available);if(outputs[1])W64(outputs[1],quota_limit);if(outputs[2])W64(outputs[2],free_total); }
    if(log_files_enabled() && !e)fprintf(stderr,"[file] private logical storage quota=%llu used=%llu available=%llu bytes\n",(unsigned long long)quota_limit,(unsigned long long)quota_used,(unsigned long long)available);
    pthread_mutex_unlock(&lock);if(e)fail(c,4,0,e);else ret(c,4,1);
}
static void disk_a(CPU *c) { disk_space(c,0); }static void disk_w(CPU *c) { disk_space(c,1); }
/* Private guest temp policy. No host environment/home/temp directory is used.
 * https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-gettemppatha
 * https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-gettempfilenamea */
static const char temp_path[]="C:\\Users\\Player\\AppData\\Local\\Temp\\";
static uint32_t temp_sequence;
static void output_path(uint32_t out,int wide,const char *path) {
    for(unsigned i=0;i<=strlen(path);i++)if(wide)W16(out+2*i,(unsigned char)path[i]);else W8(out+i,(unsigned char)path[i]);
}
static void get_temp_path(CPU *c,int wide) {
    uint32_t n=arg(c,0),out=arg(c,1),length=(uint32_t)strlen(temp_path);
    if(!initialized) { fail(c,2,0,21);return; }
    if(n<=length) { ret(c,2,length+1);return; }
    if(!range(out,(length+1)*(wide?2u:1u))) { fail(c,2,0,87);return; }
    output_path(out,wide,temp_path);ret(c,2,length);
}
static void temp_path_a(CPU *c) { get_temp_path(c,0); }static void temp_path_w(CPU *c) { get_temp_path(c,1); }
static uint32_t temp_name_impl(const char *directory,const char *prefix,uint32_t unique,char *output,uint32_t *error) {
    char key[CAP],resolved[CAP];struct stat st;
    if(strlen(directory)>246) { *error=111;return 0; }
    if((*error=normalize(directory,key)))return 0;
    if(strlen(key)>246) { *error=111;return 0; }
    /* Nonzero unique only constructs a name: it neither checks nor creates it. */
    if(unique) { snprintf(output,260,"%s%s%s%04X.TMP",key,key[strlen(key)-1]=='\\'?"":"\\",prefix,unique&0xffff);return unique; }
    if((*error=lookup(key,resolved,&st))) { if(*error==2)*error=3;return 0; }
    if(!S_ISDIR(st.st_mode)) { *error=267;return 0; }
    if(quota_dirty && (*error=refresh_usage()))return 0;
    if(!temp_sequence) {
        struct timespec ts;if(clock_gettime(CLOCK_REALTIME,&ts)) { *error=31;return 0; }
        temp_sequence=(uint32_t)((uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000)&0xffff;
        if(!temp_sequence)temp_sequence=1;
    }
    for(unsigned attempt=0;attempt<65535;attempt++) {
        uint32_t id=temp_sequence;temp_sequence=temp_sequence==65535?1:temp_sequence+1;
        snprintf(output,260,"%s%s%s%04X.TMP",key,key[strlen(key)-1]=='\\'?"":"\\",prefix,id);
        char original[CAP],overlay[CAP],candidate[CAP];int oe,we;
        if((*error=normalize(output,candidate)))return 0;
        if((*error=paths(candidate,original,overlay,&oe,&we)))return 0;
        if(oe || we)continue;
        if((*error=mkdir_parents(overlay)))return 0;
        int fd=open(overlay,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        if(fd<0) { if(errno==EEXIST)continue;*error=error_code();return 0; }
        if(close(fd)) { *error=error_code();if(unlink(overlay))quota_dirty=1;return 0; }
        /* Empty file has zero logical size even if the quota is exhausted. */
        return id;
    }
    *error=80;return 0;
}
static void get_temp_name(CPU *c,int wide) {
    char directory[CAP],prefix[4]={0},output[260];uint32_t e=guest_path(arg(c,0),wide,directory),out=arg(c,3),result=0;
    if(!e && !range(out,260*(wide?2u:1u)))e=87;
    uint32_t p=arg(c,1);
    for(unsigned i=0;!e && i<3;i++) {
        if(!range(p,(i+1)*(wide?2u:1u))) { e=87;break; }
        uint32_t ch=wide?R16(p+2*i):R8(p+i);
        if(!ch)break;
        if(ch>127) { e=50;break; }
        if(ch<32 || strchr("\\/:*?\"<>|",(int)ch)) { e=123;break; }
        prefix[i]=(char)ch;
    }
    pthread_mutex_lock(&lock);if(!e && !initialized)e=21;
    if(!e)result=temp_name_impl(directory,prefix,arg(c,2),output,&e);
    if(!e)output_path(out,wide,output);
    pthread_mutex_unlock(&lock);
    log_path(wide?(arg(c,2)?"GetTempFileNameW(derived)":"GetTempFileNameW(created)"):(arg(c,2)?"GetTempFileNameA(derived)":"GetTempFileNameA(created)"),e?arg(c,0):out,wide,result,e);
    if(e)fail(c,4,0,e);else ret(c,4,result);
}
/* Only private files without immutable originals may be deleted. No tombstones
 * are implemented: deleting a COW replacement would otherwise reveal original
 * data again. Open FILE_SHARE_DELETE handles require deferred deletion; report
 * that unsupported operation explicitly rather than unlinking a live inode.
 * https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-deletefilea */
static uint32_t delete_impl(const char *input) {
    char key[CAP],original[CAP],overlay[CAP];uint32_t e=normalize(input,key);
    if(e)return e;if(virtual_dir(key))return 5;
    struct stat module_stat;if(builtin_stat(key,&module_stat))return 5;
    int oe,we;if((e=paths(key,original,overlay,&oe,&we)))return e;
    if(!oe && !we) { e=parent_exists(key);return e?e:2; }
    if(oe)return 5; /* immutable original, with or without a private replacement */
    struct stat st;if(lstat(overlay,&st))return error_code();
    if(!S_ISREG(st.st_mode) || st.st_nlink!=1 || st.st_size<0 || !(st.st_mode&S_IWUSR))return 5;
    int open_handles=0;
    for(unsigned i=0;i<SLOTS;i++)if(files[i].handle && !strcasecmp(files[i].key,key)) {
        if(!(files[i].share&4))return 32;
        open_handles=1;
    }
    if(open_handles)return 50;
    if(quota_dirty && (e=refresh_usage()))return e;
    if(unlink(overlay))return error_code();
    account_size((uint64_t)st.st_size,0);return 0;
}
static void delete_file(CPU *c,int wide) {
    char input[CAP];uint32_t e=guest_path(arg(c,0),wide,input);
    pthread_mutex_lock(&lock);if(!e)e=initialized?delete_impl(input):21;pthread_mutex_unlock(&lock);
    log_path(wide?"DeleteFileW":"DeleteFileA",arg(c,0),wide,e?0:1,e);
    if(e)fail(c,1,0,e);else ret(c,1,1);
}
static void delete_a(CPU *c) { delete_file(c,0); }static void delete_w(CPU *c) { delete_file(c,1); }
static void temp_name_a(CPU *c) { get_temp_name(c,0); }static void temp_name_w(CPU *c) { get_temp_name(c,1); }

/* Handle-level helpers. https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setendoffile
 * Locks are advisory and single-process here; times are host-managed. */
static void flush_file(CPU *c) {
    pthread_mutex_lock(&lock);struct File *f=find(arg(c,0));uint32_t e=!f?6:(!(f->access&(WRITE|4))?5:0);pthread_mutex_unlock(&lock);
    if(e)fail(c,1,0,e);else ret(c,1,1);
}
static void end_of_file(CPU *c) {
    pthread_mutex_lock(&lock);struct File *f=find(arg(c,0));uint32_t e=0;
    if(!f)e=6;else if(!(f->access&WRITE))e=5;
    else {
        struct stat st;off_t position=lseek(f->fd,0,SEEK_CUR);
        if(position<0 || fstat(f->fd,&st))e=error_code();
        else if(position>st.st_size && (uint64_t)(position-st.st_size)>quota_free())e=112;
        else if(ftruncate(f->fd,position))e=error_code();
        else account_size((uint64_t)st.st_size,(uint64_t)position);
    }
    pthread_mutex_unlock(&lock);if(e)fail(c,1,0,e);else ret(c,1,1);
}
static void valid_handle(CPU *c,unsigned n) {
    pthread_mutex_lock(&lock);int ok=find(arg(c,0))!=NULL;pthread_mutex_unlock(&lock);
    if(!ok)fail(c,n,0,6);else ret(c,n,1);
}
static void set_file_time(CPU *c) { valid_handle(c,4); }
static void lock_file_ex(CPU *c) { valid_handle(c,6); }
static void unlock_file_ex(CPU *c) { valid_handle(c,5); }
/* Attributes are not stored: existence is checked, read-only/hidden flags ignored. */
static void set_attributes(CPU *c,int wide) {
    char input[CAP],key[CAP],path[CAP];struct stat st;uint32_t e=guest_path(arg(c,0),wide,input);
    pthread_mutex_lock(&lock);if(!e)e=initialized?normalize(input,key):21;if(!e)e=lookup(key,path,&st);pthread_mutex_unlock(&lock);
    if(e)fail(c,2,0,e);else ret(c,2,1);
}
static void set_attrs_a(CPU *c) { set_attributes(c,0); }static void set_attrs_w(CPU *c) { set_attributes(c,1); }
/* Destination always lands in the private overlay; originals stay immutable, so a
 * move whose source is an original asset is refused (ERROR_ACCESS_DENIED). */
static uint32_t destination(const char *input,char *key,char *overlay) {
    char original[CAP];int oe,we;uint32_t e=normalize(input,key);
    if(!e && (virtual_dir(key) || prefix(key,"C:\\Windows")))e=5;
    if(!e)e=paths(key,original,overlay,&oe,&we);
    if(!e && (oe||we))e=183;
    if(!e)e=parent_exists(key);
    if(!e)e=mkdir_parents(overlay);
    return e;
}
static uint32_t open_on(const char *key) {
    for(unsigned i=0;i<SLOTS;i++)if(files[i].handle && !strcasecmp(files[i].key,key))return 1;
    return 0;
}
static void move_file(CPU *c,int wide) {
    char from[CAP],to[CAP],fkey[CAP],tkey[CAP],original[CAP],overlay[CAP],target[CAP];int oe,we;
    uint32_t e=guest_path(arg(c,0),wide,from);if(!e)e=guest_path(arg(c,1),wide,to);
    pthread_mutex_lock(&lock);
    if(!e)e=initialized?normalize(from,fkey):21;
    if(!e)e=paths(fkey,original,overlay,&oe,&we);
    if(!e && !oe && !we)e=2;
    if(!e && oe)e=5;
    if(!e && open_on(fkey))e=32;
    if(!e)e=destination(to,tkey,target);
    if(!e && rename(overlay,target))e=error_code();
    pthread_mutex_unlock(&lock);
    log_path(wide?"MoveFileW":"MoveFileA",arg(c,0),wide,e?0:1,e);
    if(e)fail(c,2,0,e);else ret(c,2,1);
}
static void move_a(CPU *c) { move_file(c,0); }static void move_w(CPU *c) { move_file(c,1); }
static void copy_file_a(CPU *c) {
    char from[CAP],to[CAP],fkey[CAP],tkey[CAP],source[CAP],original[CAP],target[CAP];struct stat st;int oe,we;
    uint32_t e=guest_path(arg(c,0),0,from);if(!e)e=guest_path(arg(c,1),0,to);
    pthread_mutex_lock(&lock);
    if(!e)e=initialized?normalize(from,fkey):21;
    if(!e)e=lookup(fkey,source,&st);
    if(!e && (!source[0] || !S_ISREG(st.st_mode)))e=5;
    if(!e)e=normalize(to,tkey);
    if(!e && (virtual_dir(tkey) || prefix(tkey,"C:\\Windows")))e=5;
    if(!e)e=paths(tkey,original,target,&oe,&we);
    if(!e && (oe||we) && arg(c,2))e=80;
    if(!e && oe && !we)e=5;
    if(!e && we) { if(open_on(tkey))e=32; else { struct stat old;if(!lstat(target,&old)){ if(unlink(target))e=error_code(); else account_size((uint64_t)old.st_size,0);} } }
    if(!e)e=parent_exists(tkey);
    if(!e)e=mkdir_parents(target);
    if(!e && quota_dirty)e=refresh_usage();
    if(!e)e=copy_file(source,target);
    pthread_mutex_unlock(&lock);
    if(e)fail(c,3,0,e);else ret(c,3,1);
}
static void remove_directory(CPU *c,int wide) {
    char input[CAP],key[CAP],original[CAP],overlay[CAP];int oe,we;uint32_t e=guest_path(arg(c,0),wide,input);
    pthread_mutex_lock(&lock);
    if(!e)e=initialized?normalize(input,key):21;
    if(!e && virtual_dir(key))e=5;
    if(!e)e=paths(key,original,overlay,&oe,&we);
    if(!e && !oe && !we)e=2;
    if(!e && oe)e=5;
    if(!e && rmdir(overlay))e=errno==ENOTEMPTY||errno==EEXIST?145:error_code();
    pthread_mutex_unlock(&lock);if(e)fail(c,1,0,e);else ret(c,1,1);
}
static void rmdir_a(CPU *c) { remove_directory(c,0); }static void rmdir_w(CPU *c) { remove_directory(c,1); }
static void get_cwd_w(CPU *c) {
    uint32_t n=arg(c,0),out=arg(c,1);pthread_mutex_lock(&lock);size_t length=strlen(cwd);
    if(n<=length) { pthread_mutex_unlock(&lock);ret(c,2,(uint32_t)length+1);return; }
    if(!range(out,2*((uint32_t)length+1))) { pthread_mutex_unlock(&lock);fail(c,2,0,87);return; }
    for(size_t i=0;i<=length;i++)W16(out+2*(uint32_t)i,(uint8_t)cwd[i]);
    pthread_mutex_unlock(&lock);ret(c,2,(uint32_t)length);
}
/* Only the virtual C: drive exists (DRIVE_FIXED = 3, DRIVE_NO_ROOT_DIR = 1). */
static void drive_type(CPU *c,int wide) {
    uint32_t p=arg(c,0),first=p&&range(p,2)?(wide?R16(p):R8(p)):0;
    ret(c,1,!p || first=='C' || first=='c' || first=='\\' ? 3 : 1);
}
static void drive_a(CPU *c) { drive_type(c,0); }static void drive_w(CPU *c) { drive_type(c,1); }

/* Host-side whole-file read of a guest path (relative to the guest current
 * directory), for host services such as movie playback. Caller frees *data. */
int movies_file_read_all(const char *guest_path,uint8_t **data,size_t *size,size_t limit) {
    char key[CAP],path[CAP];struct stat st;uint32_t e;
    *data=NULL;*size=0;
    pthread_mutex_lock(&lock);
    e=initialized?normalize(guest_path,key):21;
    if(!e)e=lookup(key,path,&st);
    pthread_mutex_unlock(&lock);
    if(e || !path[0] || !S_ISREG(st.st_mode) || st.st_size<=0 || (size_t)st.st_size>limit)return 0;
    int fd=open(path,O_RDONLY);if(fd<0)return 0;
    uint8_t *buffer=malloc((size_t)st.st_size);size_t got=0;
    while(buffer && got<(size_t)st.st_size) { ssize_t n=read(fd,buffer+got,(size_t)st.st_size-got);if(n<=0)break;got+=(size_t)n; }
    close(fd);
    if(!buffer || got!=(size_t)st.st_size) { free(buffer);return 0; }
    *data=buffer;*size=got;return 1;
}

/* Lexical Windows path resolution is deliberately separate from normalize(),
 * which authorizes backend I/O. A returned path does not grant host access.
 * https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfullpathnamea */
static int separator(char ch) { return ch=='\\' || ch=='/'; }
static int drive_letter(char ch) { return (ch>='A' && ch<='Z') || (ch>='a' && ch<='z'); }
static uint32_t full_path_lexical(const char *input,char *output) {
    char joined[CAP];const char *suffix=input;size_t root_length;
    if(separator(input[0]) && separator(input[1])) {
        if((input[2]=='?' || input[2]=='.') && separator(input[3]))return 50;
        strcpy(joined,input);
    } else if(input[1]==':') {
        if(!drive_letter(input[0]))return 123;
        char drive=(char)(input[0]>='a' && input[0]<='z'?input[0]-32:input[0]);
        suffix=input+2;
        if(separator(*suffix))snprintf(joined,sizeof joined,"%c:%s",drive,suffix);
        else if(drive=='C')snprintf(joined,sizeof joined,"%s%s%s",cwd,*suffix?"\\":"",suffix);
        else snprintf(joined,sizeof joined,"%c:\\%s",drive,suffix); /* no per-drive CWD assigned on other virtual drives */
    } else if(separator(input[0]))snprintf(joined,sizeof joined,"C:%s",input);
    else if(snprintf(joined,sizeof joined,"%s\\%s",cwd,input)>=(int)sizeof joined)return 206;
    for(char *p=joined;*p;p++)if(*p=='/')*p='\\';
    size_t length=strlen(joined);while(length && joined[length-1]==' ')joined[--length]=0;
    int trailing=length && joined[length-1]=='\\';
    const char *cursor=joined;
    if(joined[0]=='\\' && joined[1]=='\\') {
        /* Keep the UNC server/share root opaque and never resolve above it. */
        const char *server=joined+2,*slash=strchr(server,'\\');
        if(!slash || slash==server)return 123;
        const char *share=slash+1,*end=strchr(share,'\\');if(!end)end=joined+length;
        if(end==share)return 123;
        root_length=(size_t)(end-joined);memcpy(output,joined,root_length);output[root_length]=0;
        cursor=end;if(*cursor)cursor++;
    } else {
        output[0]=joined[0];output[1]=':';output[2]='\\';output[3]=0;root_length=3;cursor=joined+3;
    }
    size_t n=root_length,components[CAP/2],depth=0;
    while(*cursor) {
        while(*cursor=='\\')cursor++;if(!*cursor)break;
        const char *end=strchr(cursor,'\\');if(!end)end=cursor+strlen(cursor);
        size_t part_length=(size_t)(end-cursor);
        if(part_length==1 && cursor[0]=='.') { cursor=end;continue; }
        if(part_length==2 && cursor[0]=='.' && cursor[1]=='.') {
            if(depth)n=components[--depth];output[n]=0;cursor=end;continue;
        }
        while(part_length && (cursor[part_length-1]==' ' || cursor[part_length-1]=='.'))part_length--;
        if(!part_length) { cursor=end;continue; }
        if(n+part_length+2>=CAP || depth>=CAP/2)return 206;
        components[depth++]=n;
        if(output[n-1]!='\\')output[n++]='\\';
        memcpy(output+n,cursor,part_length);n+=part_length;output[n]=0;cursor=end;
    }
    if(trailing && output[n-1]!='\\') { output[n++]='\\';output[n]=0; }
    return 0;
}
static void full_path(CPU *c,int wide) {
    char input[CAP],output[CAP];uint32_t e=guest_path(arg(c,0),wide,input),capacity=arg(c,1),out=arg(c,2),part=arg(c,3);
    pthread_mutex_lock(&lock);if(!e)e=initialized?full_path_lexical(input,output):21;pthread_mutex_unlock(&lock);
    if(e) { fail(c,4,0,e);return; }
    uint32_t length=(uint32_t)strlen(output);
    if(capacity<=length) { ret(c,4,length+1);return; }
    if(!range(out,(length+1)*(wide?2u:1u)) || (part && !range(part,4))) { fail(c,4,0,87);return; }
    output_path(out,wide,output);
    if(part) {
        const char *last=strrchr(output,'\\');
        int unc_root=output[0]=='\\' && output[1]=='\\' && last==strchr(output+2,'\\');
        W32(part,!unc_root && last && last[1]?out+(uint32_t)(last+1-output)*(wide?2u:1u):0);
    }
    ret(c,4,length);
}
static void full_path_a(CPU *c) { full_path(c,0); }static void full_path_w(CPU *c) { full_path(c,1); }

ImportHandler *movies_file_handlers(uint32_t *count) {
    static ImportHandler h[]={
        {"KERNEL32.DLL!CreateFileA",create_a},{"KERNEL32.DLL!CreateFileW",create_w},
        {"KERNEL32.DLL!ReadFile",read_file},{"KERNEL32.DLL!WriteFile",write_file},
        {"KERNEL32.DLL!CloseHandle",close_file},{"KERNEL32.DLL!SetFilePointer",seek_file},
        {"KERNEL32.DLL!GetFileSize",size_file},{"KERNEL32.DLL!GetFileTime",file_time},{"KERNEL32.DLL!GetFileAttributesA",attrs_a},
        {"KERNEL32.DLL!GetFileAttributesW",attrs_w},{"KERNEL32.DLL!CreateDirectoryA",mkdir_a},
        {"KERNEL32.DLL!CreateDirectoryW",mkdir_w},{"KERNEL32.DLL!GetCurrentDirectoryA",get_cwd},
        {"KERNEL32.DLL!SetCurrentDirectoryA",set_cwd},
        {"KERNEL32.DLL!FindFirstFileA",first_a},{"KERNEL32.DLL!FindFirstFileW",first_w},
        {"KERNEL32.DLL!FindNextFileA",next_a},{"KERNEL32.DLL!FindNextFileW",next_w},
        {"KERNEL32.DLL!FindClose",close_search},
        {"KERNEL32.DLL!GetDiskFreeSpaceExA",disk_a},{"KERNEL32.DLL!GetDiskFreeSpaceExW",disk_w},
        {"KERNEL32.DLL!GetTempPathA",temp_path_a},{"KERNEL32.DLL!GetTempPathW",temp_path_w},
        {"KERNEL32.DLL!GetTempFileNameA",temp_name_a},{"KERNEL32.DLL!GetTempFileNameW",temp_name_w},
        {"KERNEL32.DLL!FlushFileBuffers",flush_file},{"KERNEL32.DLL!SetEndOfFile",end_of_file},
        {"KERNEL32.DLL!SetFileTime",set_file_time},{"KERNEL32.DLL!LockFileEx",lock_file_ex},{"KERNEL32.DLL!UnlockFileEx",unlock_file_ex},
        {"KERNEL32.DLL!SetFileAttributesA",set_attrs_a},{"KERNEL32.DLL!SetFileAttributesW",set_attrs_w},
        {"KERNEL32.DLL!MoveFileA",move_a},{"KERNEL32.DLL!MoveFileW",move_w},{"KERNEL32.DLL!CopyFileA",copy_file_a},
        {"KERNEL32.DLL!RemoveDirectoryA",rmdir_a},{"KERNEL32.DLL!RemoveDirectoryW",rmdir_w},
        {"KERNEL32.DLL!GetCurrentDirectoryW",get_cwd_w},{"KERNEL32.DLL!GetDriveTypeA",drive_a},{"KERNEL32.DLL!GetDriveTypeW",drive_w},
        {"KERNEL32.DLL!DeleteFileA",delete_a},{"KERNEL32.DLL!DeleteFileW",delete_w},
        {"KERNEL32.DLL!GetFullPathNameA",full_path_a},{"KERNEL32.DLL!GetFullPathNameW",full_path_w},
    };*count=sizeof h/sizeof h[0];return h;
}
