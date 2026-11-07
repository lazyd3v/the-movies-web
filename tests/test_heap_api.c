/* Build with native/heap-api.c, runtime include path, -pthread
 * -DMOVIES_HEAP_BASE=0x1000 -DMOVIES_HEAP_HI=0x11000. */
#include "heap-api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
uint8_t *g_mem;
static uint32_t error;
static jmp_buf trap;
#ifdef MOVIES_HEAP_TEST_ALLOCATE
static int fail_metadata_after=-1;
void *movies_heap_test_calloc(size_t count,size_t size) {
    if(fail_metadata_after==0)return NULL;
    if(fail_metadata_after>0)fail_metadata_after--;
    return calloc(count,size);
}
#endif

void movies_set_last_error(uint32_t e) { error=e; }
void guest_trap(CPU *c,uint32_t address,const char *message) {
    (void)c;(void)address;(void)message; longjmp(trap,1);
}
static uint32_t call(const char *name,uint32_t a,uint32_t b,uint32_t d,uint32_t e) {
    uint32_t n; ImportHandler *handlers=movies_heap_handlers(&n);
    CPU c={0}; c.esp=0x100;
    W32(c.esp,0xdeadbeef); W32(c.esp+4,a); W32(c.esp+8,b); W32(c.esp+12,d); W32(c.esp+16,e);
    for(uint32_t i=0;i<n;i++) if(!strcmp(handlers[i].name+13,name)) {
        handlers[i].fn(&c);
        unsigned argc=!strcmp(name,"GetProcessHeap")?0:
            (!strcmp(name,"HeapDestroy")?1:(!strcmp(name,"HeapReAlloc")?4:3));
        assert(c.esp==0x100+4*(argc+1)); return c.eax;
    }
    assert(0); return 0;
}
#ifdef MOVIES_HEAP_PROFILE
uint64_t movies_heap_allocation_visits(void);
uint64_t movies_heap_lookup_visits(void);
uint64_t movies_heap_coalesce_visits(void);
void movies_heap_reset_allocation_visits(void);
#endif
int main(void) {
    g_mem=malloc(0x12000); assert(g_mem); memset(g_mem,0xa5,0x12000);
#ifdef MOVIES_HEAP_TEST_ALLOCATE
    uint32_t rollback=call("HeapCreate",0,4096,0,0),live[256];assert(rollback);
    for(unsigned i=0;i<256;i++) { live[i]=call("HeapAlloc",rollback,0,8,0);assert(live[i]); }
    memset(HP(live[0]),0x42,8);
    /* The 257th live entry grows the hash; failed allocation preserves every
     * live pointer and both guest reservation and original realloc contents. */
    fail_metadata_after=0;error=543;
    assert(!call("HeapAlloc",rollback,0,8,0) && error==543);
    assert(!call("HeapReAlloc",rollback,0,live[0],64) && error==543);
    assert(call("HeapSize",rollback,0,live[0],0)==8 && R8(live[0])==0x42);
    fail_metadata_after=-1;
    uint32_t moved_rollback=call("HeapReAlloc",rollback,0,live[0],64);assert(moved_rollback);
    for(unsigned i=0;i<8;i++)assert(R8(moved_rollback+i)==0x42);
    for(unsigned i=1;i<256;i++)assert(call("HeapSize",rollback,0,live[i],0)==8);
    assert(call("HeapDestroy",rollback,0,0,0));
#endif
    /* Even zero initialSize reserves one page, so the 17th heap cannot fit. */
    uint32_t reservations[16];
    for(unsigned i=0;i<16;i++) {
        reservations[i]=call("HeapCreate",0,0,0,0);assert(reservations[i]);
    }
    error=0;assert(!call("HeapCreate",0,0,0,0) && error==8);
    /* Heap A cannot steal B's free reserved page. Failed realloc preserves A. */
    uint32_t first=call("HeapAlloc",reservations[0],0,4096,0);assert(first==0x1000);
    memset(HP(first),0x55,4096);
    assert(!call("HeapReAlloc",reservations[0],16,first,4097));
    assert(call("HeapSize",reservations[0],0,first,0)==4096 && R8(first)==0x55);
    assert(!call("HeapAlloc",reservations[0],0,1,0));
    for(unsigned i=0;i<16;i++)assert(call("HeapDestroy",reservations[i],0,0,0));
    /* Round initial commitment upward and retain it despite shared exhaustion. */
    uint32_t rounded=call("HeapCreate",0,4097,0,0);assert(rounded);
    uint32_t remainder=call("HeapCreate",0,57344,0,0);assert(remainder);
    assert(!call("HeapCreate",0,1,0,0));
    uint32_t rounded_block=call("HeapAlloc",rounded,0,8192,0);assert(rounded_block==0x1000);
    assert(call("HeapFree",rounded,0,rounded_block,0));
    assert(!call("HeapCreate",0,1,0,0)); /* free preserves the heap reservation */
    assert(call("HeapDestroy",remainder,0,0,0));
    /* In-place growth can consume newly shared space, then reuse it on free. */
    rounded_block=call("HeapAlloc",rounded,0,8192,0);
    assert(call("HeapReAlloc",rounded,16,rounded_block,12288)==rounded_block);
    assert(call("HeapFree",rounded,0,rounded_block,0));
    assert(call("HeapAlloc",rounded,0,12288,0)==rounded_block);
    assert(call("HeapDestroy",rounded,0,0,0));
    uint32_t h=call("HeapCreate",0,4096,0,0),other=call("HeapCreate",0,0,0,0);
    assert(h && other && h!=other);
    uint32_t a=call("HeapAlloc",h,8,17,0); assert(a==0x1000 && !(a&7));
    for(unsigned i=0;i<17;i++)assert(R8(a+i)==0);
    assert(call("HeapSize",h,0,a,0)==17);
    error=123; assert(call("HeapSize",other,0,a,0)==0xffffffff && error==123);
    assert(!call("HeapFree",other,0,a,0)); assert(call("HeapSize",h,0,a,0)==17);
    memset(HP(a),0x33,17);
    uint32_t blocker=call("HeapAlloc",h,0,64,0); assert(blocker);
    error=123; assert(!call("HeapReAlloc",h,16,a,128)); assert(error==123);
    assert(call("HeapSize",h,0,a,0)==17 && R8(a)==0x33);
    uint32_t moved=call("HeapReAlloc",h,8,a,128); assert(moved && moved!=a);
    for(unsigned i=0;i<128;i++)assert(R8(moved+i)==(i<17?0x33:0));
    assert(call("HeapReAlloc",h,16,moved,8)==moved);
    assert(call("HeapSize",h,0,moved,0)==8);
    assert(call("HeapFree",h,0,blocker,0)); assert(call("HeapFree",h,0,moved,0));
    assert(!call("HeapFree",h,0,moved,0)); assert(call("HeapFree",h,0,0,0));
    a=call("HeapAlloc",h,0,0,0); assert(a==0x1000); assert(call("HeapSize",h,0,a,0)==0);
    assert(call("HeapDestroy",h,0,0,0)); assert(!call("HeapAlloc",h,0,8,0));
    uint32_t fixed=call("HeapCreate",0,1,4096,0); assert(fixed);
    a=call("HeapAlloc",fixed,0,4096,0); assert(a==0x1000);
    assert(!call("HeapAlloc",fixed,0,1,0)); assert(call("HeapFree",fixed,0,a,0));
    assert(!call("HeapAlloc",fixed,0,0x7fff8,0));
    assert(call("HeapDestroy",fixed,0,0,0));
    a=call("HeapAlloc",other,0,65536,0); assert(a==0x1000);
    error=999; assert(!call("HeapAlloc",other,0,1,0) && error==999);
    assert(!call("HeapReAlloc",other,0,a,0xffffffff) && error==999);
    assert(call("HeapSize",other,0,a,0)==65536);
    assert(call("HeapFree",other,0,a,0));
    a=call("HeapAlloc",other,0,8,0); assert(a==0x1000);
    assert(call("HeapReAlloc",other,16|8,a,4096)==a);
    assert(call("HeapDestroy",other,0,0,0));
    /* Many simultaneously live small blocks must not rescan their live prefix.
     * Freed holes still use exactly the same address-ordered first-fit policy. */
    uint32_t many=call("HeapCreate",0,4096,0,0),addresses[4000];assert(many);
#ifdef MOVIES_HEAP_PROFILE
    movies_heap_reset_allocation_visits();
#endif
    for(unsigned i=0;i<4000;i++) { addresses[i]=call("HeapAlloc",many,0,8,0);assert(addresses[i]); }
#ifdef MOVIES_HEAP_PROFILE
    assert(movies_heap_allocation_visits()<=8000);
#endif
    assert(call("HeapFree",many,0,addresses[31],0));
    assert(call("HeapFree",many,0,addresses[3],0));
    assert(call("HeapFree",many,0,addresses[1000],0));
    assert(call("HeapAlloc",many,0,8,0)==addresses[3]);
    assert(call("HeapAlloc",many,0,8,0)==addresses[31]);
    assert(call("HeapAlloc",many,0,8,0)==addresses[1000]);
#ifdef MOVIES_HEAP_PROFILE
    movies_heap_reset_allocation_visits();
#endif
    for(unsigned i=0;i<4000;i++)assert(call("HeapSize",many,0,addresses[i],0)==8);
#ifdef MOVIES_HEAP_PROFILE
    assert(movies_heap_lookup_visits()<16000);
    movies_heap_reset_allocation_visits();
#endif
    /* A permutation frees fragmented holes and forces both-neighbor merges. */
    for(unsigned i=0;i<4000;i++) {
        unsigned j=(i*1543u)%4000;
        assert(call("HeapFree",many,0,addresses[j],0));
        assert(call("HeapSize",many,0,addresses[j],0)==0xffffffffu);
    }
#ifdef MOVIES_HEAP_PROFILE
    assert(movies_heap_lookup_visits()<24000);
    assert(movies_heap_coalesce_visits()<=12000);
#endif
    assert(call("HeapAlloc",many,0,32000,0)==addresses[0]);
    assert(call("HeapDestroy",many,0,0,0));
    /* A split of a live block exposes an earlier hole than the cursor. */
    many=call("HeapCreate",0,4096,0,0);assert(many);
    uint32_t shrink=call("HeapAlloc",many,0,1024,0);
    assert(call("HeapAlloc",many,0,1024,0));
    assert(call("HeapReAlloc",many,16,shrink,8)==shrink);
    assert(call("HeapAlloc",many,0,16,0)==shrink+8);
    assert(call("HeapDestroy",many,0,0,0));
    /* Deterministic fragmented realloc stress checks contents and exact ownership
     * while address-index entries are inserted, removed and recycled. */
    uint32_t stress=call("HeapCreate",0,4096,0,0),ptrs[128]={0},sizes[128]={0};
    uint32_t random=0x12345678;
    for(unsigned iteration=0;iteration<6000;iteration++) {
        random=random*1664525u+1013904223u;unsigned slot=(random>>16)%128;
        if(ptrs[slot]) {
            assert(call("HeapSize",stress,0,ptrs[slot],0)==sizes[slot]);
            for(unsigned j=0;j<sizes[slot];j++)assert(R8(ptrs[slot]+j)==(uint8_t)(slot+j));
        }
        if(ptrs[slot] && (random&3)==0) {
            assert(call("HeapFree",stress,0,ptrs[slot],0));ptrs[slot]=0;
        } else {
            unsigned wanted=((random>>8)%96)+1;
            uint32_t changed=ptrs[slot]?call("HeapReAlloc",stress,0,ptrs[slot],wanted):call("HeapAlloc",stress,0,wanted,0);
            assert(changed);
            if(ptrs[slot])for(unsigned j=0;j<(sizes[slot]<wanted?sizes[slot]:wanted);j++)assert(R8(changed+j)==(uint8_t)(slot+j));
            ptrs[slot]=changed;sizes[slot]=wanted;
            for(unsigned j=0;j<wanted;j++)W8(changed+j,(uint8_t)(slot+j));
        }
    }
    assert(call("HeapDestroy",stress,0,0,0));
    uint32_t p=call("GetProcessHeap",0,0,0,0); assert(p==call("GetProcessHeap",0,0,0,0));
    assert(!call("HeapDestroy",p,0,0,0));
    if(!setjmp(trap)) { call("HeapAlloc",p,4,8,0); assert(0); }
    if(!setjmp(trap)) { call("HeapCreate",0x40000,0,0,0); assert(0); }
    if(!setjmp(trap)) { call("HeapFree",p,8,0,0); assert(0); }
    /* Traps must not strand the allocator mutex. */
    a=call("HeapAlloc",p,1,8,0); assert(a); assert(call("HeapFree",p,1,a,0));
    MoviesHeapBudget before,after;movies_heap_budget(&before);assert(before.total==0x10000&&before.free_bytes<=before.total&&before.unreserved<=before.free_bytes);
    uint32_t backend=movies_backend_alloc(1024);assert(backend && !(backend&7));movies_heap_budget(&after);assert(after.free_bytes==before.free_bytes-1024&&after.unreserved<=before.unreserved);
    for(unsigned i=0;i<1024;i++)assert(R8(backend+i)==0);
    assert(call("HeapSize",p,0,backend,0)==0xffffffffu);
    assert(!call("HeapFree",p,0,backend,0));assert(!movies_backend_free(a));
    assert(!movies_backend_alloc(0));assert(!movies_backend_alloc((32u<<20)+1));
    assert(!movies_backend_alloc(0x100000)); /* exceeds this test's physical arena */
    assert(movies_backend_free(backend));movies_heap_budget(&after);assert(after.free_bytes==before.free_bytes);assert(!movies_backend_free(backend));
    uint32_t reused=movies_backend_alloc(1024);assert(reused);assert(movies_backend_free(reused));
    for(unsigned i=0x11000;i<0x12000;i++)assert(g_mem[i]==0xa5);
    free(g_mem); puts("heap API tests passed");
}
