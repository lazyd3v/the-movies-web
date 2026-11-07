/* Win32 full-fence interlocked contracts (LONG is always 32 bits):
 * https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-interlockedincrement
 * https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-interlockedcompareexchange
 * https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-interlockedexchangeadd
 * Unsupported pointers trap; this does not implement guest exception dispatch.
 */
#include "atomic-api.h"
static uint32_t *target(CPU *c,unsigned argc) {
    if(!g_mem || !c->esp || (uint64_t)c->esp+4*(argc+1)>GUEST_SPACE)
        guest_trap(c,0,"invalid interlocked argument stack");
    uint32_t address=R32(c->esp+4);
    if(!address || (address&3) || (uint64_t)address+4>GUEST_SPACE)
        guest_trap(c,R32(c->esp),"invalid or unaligned interlocked target");
    return (uint32_t *)HP(address);
}
static void ret(CPU *c,unsigned argc,uint32_t value) {
    c->eax=value;c->esp+=4*(argc+1);
}
#ifdef MOVIES_GUEST_THREADS
#define MEMORY_ENTER() guest_lock()
#define MEMORY_LEAVE() guest_unlock()
#else
#define MEMORY_ENTER() ((void)0)
#define MEMORY_LEAVE() ((void)0)
#endif
static void increment(CPU *c) {
    uint32_t *p=target(c,1);
    MEMORY_ENTER();uint32_t value=__atomic_add_fetch(p,1u,__ATOMIC_SEQ_CST);MEMORY_LEAVE();ret(c,1,value);
}
static void decrement(CPU *c) {
    uint32_t *p=target(c,1);
    MEMORY_ENTER();uint32_t value=__atomic_sub_fetch(p,1u,__ATOMIC_SEQ_CST);MEMORY_LEAVE();ret(c,1,value);
}
static void exchange(CPU *c) {
    uint32_t *p=target(c,2), value=R32(c->esp+8);
    MEMORY_ENTER();uint32_t previous=__atomic_exchange_n(p,value,__ATOMIC_SEQ_CST);MEMORY_LEAVE();ret(c,2,previous);
}
static void compare_exchange(CPU *c) {
    uint32_t *p=target(c,3),value=R32(c->esp+8),expected=R32(c->esp+12);
    MEMORY_ENTER();__atomic_compare_exchange_n(p,&expected,value,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    MEMORY_LEAVE();ret(c,3,expected);
}
static void exchange_add(CPU *c) {
    uint32_t *p=target(c,2),value=R32(c->esp+8);
    MEMORY_ENTER();uint32_t previous=__atomic_fetch_add(p,value,__ATOMIC_SEQ_CST);MEMORY_LEAVE();ret(c,2,previous);
}
ImportHandler *movies_atomic_handlers(uint32_t *count) {
    static ImportHandler handlers[]={
        {"KERNEL32.DLL!InterlockedIncrement",increment},
        {"KERNEL32.DLL!InterlockedDecrement",decrement},
        {"KERNEL32.DLL!InterlockedExchange",exchange},
        {"KERNEL32.DLL!InterlockedCompareExchange",compare_exchange},
        {"KERNEL32.DLL!InterlockedExchangeAdd",exchange_add},
    };
    *count=sizeof handlers/sizeof handlers[0];return handlers;
}
