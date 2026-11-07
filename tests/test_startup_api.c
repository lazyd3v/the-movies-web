#include "startup-api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static uint8_t memory[131072];
uint8_t *g_mem = memory;
static int traps;
static jmp_buf trap_target;
void guest_trap(CPU *cpu, uint32_t at, const char *why) { (void)cpu; (void)at; (void)why; traps++; longjmp(trap_target, 1); }
void guest_bounds_fail(uint32_t at, int size) { (void)at; (void)size; abort(); }
static CPU cpu;
static ImportHandler *handlers;
static uint32_t count;
static uint32_t dynamic_calls;
static int test_export_enabled=1;
static void test_export(CPU *c) { c->eax=123;c->esp+=4; }
GuestFn find_import_handler(const char *name) {
    if(test_export_enabled && (!strcmp(name,"TEST.DLL!Example") || !strcmp(name,"TEST.DLL!Second")))return test_export;
    for (uint32_t i = 0; i < count; i++) if (!strcmp(handlers[i].name, name)) return handlers[i].fn;
    return NULL;
}
uint32_t add_dynamic_import(const char *name, GuestFn fn) {
    assert(fn == find_import_handler(name));
    dynamic_calls++;
    return 0xf0001000+(dynamic_calls-1)*16;
}
static uint32_t call(const char *name, uint32_t argument, uint32_t argc) {
    cpu.esp = 128;
    cpu.ebx = 0x12345678;
    W32(128, 0xdeadbeef); W32(132, argument);
    for (uint32_t i = 0; i < count; i++) {
        if (!strcmp(handlers[i].name, name)) {
            handlers[i].fn(&cpu);
            assert(cpu.ebx == 0x12345678 && R32(128) == 0xdeadbeef);
            if (!traps) assert(cpu.esp == 132 + argc * 4);
            return cpu.eax;
        }
    }
    abort();
}
int main(void) {
    handlers = movies_startup_handlers(0x400000, &count);
    for (uint32_t size = 148; size <= 156; size += 8) {
        memset(memory + 512, 0xa5, 164); W32(512, size);
        assert(call("KERNEL32.DLL!GetVersionExA", 512, 1) == 1);
        assert(R32(512) == size && R32(516) == 5 && R32(520) == 1);
        assert(R32(524) == 2600 && R32(528) == 2);
        assert(!strcmp((char *)HP(532), "Service Pack 3"));
        assert(R8(512 + size) == 0xa5);
        if (size == 156) assert(R16(660) == 3 && R8(666) == 1);
    }
    call("KERNEL32.DLL!SetLastError",123,1);
    assert(movies_get_last_error()==123);
    uint32_t packed=call("KERNEL32.DLL!GetVersion",0,0);
    assert(packed==0x0a280105u);
    assert((packed&255)==R32(516) && ((packed>>8)&255)==R32(520));
    assert((packed>>16)==R32(524) && !(packed&0x80000000u) && R32(528)==2);
    assert(call("KERNEL32.DLL!GetLastError",0,0)==123);
    memset(memory + 512, 0xa5, 164); W32(512, 147);
    assert(call("KERNEL32.DLL!GetVersionExA", 512, 1) == 0);
    assert(R32(516) == 0xa5a5a5a5);
    assert(call("KERNEL32.DLL!GetLastError", 0, 0) == 87);
    call("KERNEL32.DLL!SetLastError", 123, 1);
    assert(call("KERNEL32.DLL!GetLastError", 0, 0) == 123);
    assert(call("KERNEL32.DLL!GetModuleHandleA", 0, 1) == 0x400000);
    strcpy((char *)HP(512), "missing.dll");
    assert(call("KERNEL32.DLL!GetModuleHandleA", 512, 1) == 0);
    assert(call("KERNEL32.DLL!GetLastError", 0, 0) == 126);
    strcpy((char *)HP(512), "C:\\Windows\\System32\\KeRnEl32.DlL");
    uint32_t module = call("KERNEL32.DLL!GetModuleHandleA", 512, 1);
    assert(module != 0 && module != 0x400000);
    strcpy((char *)HP(65536), "FlsAlloc"); W32(136, 65536);
    assert(call("KERNEL32.DLL!GetProcAddress", module, 2) == 0);
    assert(call("KERNEL32.DLL!GetLastError", 0, 0) == 127);
    strcpy((char *)HP(65536), "GetLastError");
    assert(call("KERNEL32.DLL!GetProcAddress", module, 2) == 0xf0001000);
    assert(call("KERNEL32.DLL!GetProcAddress", module, 2) == 0xf0001000);
    assert(dynamic_calls == 1 && traps == 0);
    strcpy((char *)HP(512),"imagehlp.dll");
    assert(!call("KERNEL32.DLL!LoadLibraryA",512,1));assert(call("KERNEL32.DLL!GetLastError",0,0)==126);
    assert(!call("KERNEL32.DLL!LoadLibraryA",0,1));assert(call("KERNEL32.DLL!GetLastError",0,0)==87);
    strcpy((char *)HP(512),"kernel32");call("KERNEL32.DLL!SetLastError",123,1);
    assert(call("KERNEL32.DLL!LoadLibraryA",512,1)==module);assert(call("KERNEL32.DLL!GetLastError",0,0)==123);
    const char *kernel_w="C:/Windows/System32/KERNEL32.dll";
    for(unsigned i=0;i<=strlen(kernel_w);i++)W16(512+2*i,(unsigned char)kernel_w[i]);
    assert(call("KERNEL32.DLL!LoadLibraryW",512,1)==module);assert(call("KERNEL32.DLL!GetModuleHandleW",512,1)==module);
    assert(call("KERNEL32.DLL!FreeLibrary",module,1));assert(call("KERNEL32.DLL!FreeLibrary",module,1));
    assert(call("KERNEL32.DLL!GetModuleHandleW",512,1)==module); /* core is pinned */
    assert(!call("KERNEL32.DLL!FreeLibrary",0,1));assert(call("KERNEL32.DLL!GetLastError",0,0)==6);
    strcpy((char *)HP(512),"C:\\Elsewhere\\kernel32.dll");assert(!call("KERNEL32.DLL!LoadLibraryA",512,1));assert(call("KERNEL32.DLL!GetLastError",0,0)==126);
    strcpy((char *)HP(512),"kernel32.");assert(!call("KERNEL32.DLL!LoadLibraryA",512,1)); /* trailing period disables default DLL suffix */
    assert(!movies_startup_register_module("missing.dll","NoExport"));
    assert(!movies_startup_module_available("test.dll","Example"));
    assert(movies_startup_register_module("test.dll","Example"));
    assert(movies_startup_module_available("TeSt.DLL","Example"));
    assert(!movies_startup_module_available("test.dll","Second"));
    assert(!movies_startup_register_module("test.dll","Second"));
    assert(!movies_startup_module_available("dinput8.dll","DirectInput8Create"));
    test_export_enabled=0;assert(!movies_startup_module_available("test.dll","Example"));test_export_enabled=1;

    strcpy((char *)HP(512),"TEST.dll");assert(!call("KERNEL32.DLL!GetModuleHandleA",512,1));
    uint32_t test_module=call("KERNEL32.DLL!LoadLibraryA",512,1);assert(test_module && test_module!=module);
    assert(call("KERNEL32.DLL!LoadLibraryA",512,1)==test_module);
    strcpy((char *)HP(65536),"Example");W32(136,65536);
    assert(call("KERNEL32.DLL!GetProcAddress",test_module,2)==0xf0001010);
    strcpy((char *)HP(65536),"example");assert(!call("KERNEL32.DLL!GetProcAddress",test_module,2));assert(call("KERNEL32.DLL!GetLastError",0,0)==127);
    assert(call("KERNEL32.DLL!FreeLibrary",test_module,1));assert(call("KERNEL32.DLL!GetModuleHandleA",512,1)==test_module);
    assert(call("KERNEL32.DLL!FreeLibrary",test_module,1));assert(!call("KERNEL32.DLL!GetModuleHandleA",512,1));
    strcpy((char *)HP(65536),"Example");assert(!call("KERNEL32.DLL!GetProcAddress",test_module,2));
    assert(!call("KERNEL32.DLL!FreeLibrary",test_module,1));assert(call("KERNEL32.DLL!LoadLibraryA",512,1)==test_module);
    assert(call("KERNEL32.DLL!GetProcAddress",test_module,2)==0xf0001010 && dynamic_calls==2);
    assert(call("KERNEL32.DLL!FreeLibrary",test_module,1));
    memset(HP(512),'x',260);assert(!call("KERNEL32.DLL!LoadLibraryA",512,1));assert(call("KERNEL32.DLL!GetLastError",0,0)==126);
    W16(512,0x1234);W16(514,0);assert(!call("KERNEL32.DLL!LoadLibraryW",512,1));assert(call("KERNEL32.DLL!GetLastError",0,0)==50);
    assert(!call("KERNEL32.DLL!LoadLibraryW",0xffffffffu,1));assert(call("KERNEL32.DLL!GetLastError",0,0)==87);
    const char *directory_apis[]={"GetSystemDirectoryA","GetSystemDirectoryW","GetWindowsDirectoryA","GetWindowsDirectoryW"};
    for(unsigned k=0;k<4;k++) {
        char api[96];snprintf(api,sizeof api,"KERNEL32.DLL!%s",directory_apis[k]);
        const char *expected=k<2?"C:\\Windows\\System32":"C:\\Windows";uint32_t length=(uint32_t)strlen(expected);
        W32(136,0);assert(call(api,0,2)==length+1);
        W8(512,0xaa);W32(136,length);assert(call(api,512,2)==length+1 && R8(512)==0xaa);
        W32(136,length+1);call("KERNEL32.DLL!SetLastError",123,1);assert(call(api,512,2)==length);assert(call("KERNEL32.DLL!GetLastError",0,0)==123);
        for(unsigned j=0;j<=length;j++)assert((k&1?R16(512+2*j):R8(512+j))==(unsigned char)expected[j]);
        W32(136,260);assert(!call(api,0xffffffffu,2));assert(call("KERNEL32.DLL!GetLastError",0,0)==87);
    }
    assert(call("KERNEL32.DLL!SetUnhandledExceptionFilter", 0xada268, 1) == 0);
    assert(movies_unhandled_exception_filter() == 0xada268);
    assert(call("KERNEL32.DLL!SetUnhandledExceptionFilter", 0, 1) == 0xada268);
    assert(movies_unhandled_exception_filter() == 0);
    puts("startup ABI tests passed");
}
