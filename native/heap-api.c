/* Guest Win32 heap subset. Reference semantics:
 * https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapalloc
 * .../nf-heapapi-heaprealloc, heapcreate, heapfree, heapsize.
 * 8-byte x86 alignment; host-only metadata; all heaps reserve initial rounded pages; growable heaps acquire extra extents.
 * Exception-generating/executable heaps trap pending guest SEH/code support.
 * NO_SERIALIZE is accepted; host metadata remains synchronized for safety.
 */
#include "heap-api.h"
#include <pthread.h>
#include <stdlib.h>
#ifdef MOVIES_HEAP_TEST_ALLOCATE
extern void *movies_heap_test_calloc(size_t count,size_t size);
#define metadata_calloc movies_heap_test_calloc
#else
#define metadata_calloc calloc
#endif
#ifndef MOVIES_HEAP_BASE
#define MOVIES_HEAP_BASE GUEST_HEAP_BASE
#endif
#ifndef MOVIES_HEAP_HI
#define MOVIES_HEAP_HI GUEST_HEAP_HI
#endif

typedef struct Block { uint32_t address, capacity, size, owner, pool; struct Block *next, *prev, *hash_next; } Block;
typedef struct Heap { uint32_t id, fixed; struct Heap *next; } Heap;
static Block *blocks;
/* Earliest free extent. Every earlier block is live, so skipping that prefix
 * preserves exact first-fit ordering while avoiding quadratic append workloads.
 * The list stays address-sorted: split inserts immediately after its parent;
 * extend/coalesce only absorb adjacent nodes; destroy never reorders nodes.
 * Fragmented suffix allocation search remains linear; live lookup is hashed. */
static Block *first_free;
#ifdef MOVIES_HEAP_PROFILE
static uint64_t allocation_visits, lookup_visits, coalesce_visits;
uint64_t movies_heap_lookup_visits(void) { return lookup_visits; }
uint64_t movies_heap_coalesce_visits(void) { return coalesce_visits; }
uint64_t movies_heap_allocation_visits(void) { return allocation_visits; }
void movies_heap_reset_allocation_visits(void) { allocation_visits=lookup_visits=coalesce_visits=0; }
#define VISIT() (++allocation_visits)
#define LOOKUP_VISIT() (++lookup_visits)
#define COALESCE_VISIT() (++coalesce_visits)
#else
#define VISIT() ((void)0)
#define LOOKUP_VISIT() ((void)0)
#define COALESCE_VISIT() ((void)0)
#endif
static void refresh_free(void) {
    while(first_free && first_free->owner)first_free=first_free->next;
}
static void note_free(Block *b) {
    if(!first_free || b->address<first_free->address)first_free=b;
}
static Heap *heaps;
static Block **address_index;
static size_t address_capacity, address_count;
static uint32_t next_id = 0x70000000, process_heap, backend_heap, backend_allocations;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#define ARG(n) R32(c->esp + 4 + 4*(n))
static void ret(CPU *c, unsigned n, uint32_t value) { c->eax=value; c->esp+=4*(n+1); }
static int flags(CPU *c, uint32_t value, uint32_t allowed) {
    if (value & ~allowed) { guest_trap(c,R32(c->esp),"heap flags unsupported (SEH/executable heaps unavailable)"); return 0; }
    return 1;
}
static Heap *find_heap(uint32_t id) { for(Heap *h=heaps;h;h=h->next) if(h->id==id)return h; return NULL; }
/* Hash only live allocations, so splitting/moving free extents never requires
 * rekeying. Allocate growth metadata before changing guest allocation state. */
static size_t address_bucket(uint32_t address,size_t capacity) {
    uint32_t key=address>>3;
    key^=key>>16;key*=0x7feb352du;key^=key>>15;
    return key&(capacity-1);
}
static int index_prepare(void) {
    if(address_count<address_capacity)return 1;
    size_t capacity=address_capacity?address_capacity*2:256;
    if(capacity<address_capacity || capacity>SIZE_MAX/sizeof(Block *))return 0;
    Block **next=metadata_calloc(capacity,sizeof *next);if(!next)return 0;
    for(size_t i=0;i<address_capacity;i++) {
        Block *b=address_index[i];
        while(b) {
            Block *following=b->hash_next;
            size_t bucket=address_bucket(b->address,capacity);
            b->hash_next=next[bucket];next[bucket]=b;b=following;
        }
    }
    free(address_index);address_index=next;address_capacity=capacity;return 1;
}
static void index_insert(Block *b) {
    size_t bucket=address_bucket(b->address,address_capacity);
    b->hash_next=address_index[bucket];address_index[bucket]=b;address_count++;
}
static void index_remove(Block *b) {
    Block **p=&address_index[address_bucket(b->address,address_capacity)];
    while(*p && *p!=b)p=&(*p)->hash_next;
    if(*p) { *p=b->hash_next;b->hash_next=NULL;address_count--; }
}
static Block *find_block(uint32_t id,uint32_t address) {
    if(!address_capacity)return NULL;
    for(Block *b=address_index[address_bucket(address,address_capacity)];b;b=b->hash_next) {
        LOOKUP_VISIT();
        if(b->owner==id && b->address==address)return b;
    }
    return NULL;
}
static Heap *find_guest_heap(uint32_t id) { return id==backend_heap?NULL:find_heap(id); }
static int mergeable(Block *left,Block *right) {
    COALESCE_VISIT();
    return left && right && !left->owner && !right->owner && left->pool==right->pool &&
        left->address+left->capacity==right->address;
}
static void merge_next(Block *b) {
    Block *n=b->next;
    if(first_free==n)first_free=b;
    b->capacity+=n->capacity;b->next=n->next;
    if(b->next)b->next->prev=b;
    free(n);
}
static void coalesce_at(Block *b) {
    if(mergeable(b->prev,b)) { b=b->prev;merge_next(b); }
    while(mergeable(b,b->next))merge_next(b);
}
/* HeapDestroy changes multiple pool identities; a single bulk pass is needed. */
static void coalesce_all(void) {
    for(Block *b=blocks;b && b->next;) {
        if(mergeable(b,b->next))merge_next(b);else b=b->next;
    }
}
static void release_block(Block *b) {
    index_remove(b);b->owner=0;b->size=0;note_free(b);coalesce_at(b);
}
/* Split only after metadata allocation succeeds; failure leaves old block intact. */
static int split(Block *b,uint32_t size) {
    if(size==b->capacity)return 1;
    Block *n=metadata_calloc(1,sizeof *n); if(!n)return 0;
    n->address=b->address+size; n->capacity=b->capacity-size; n->pool=b->pool;
    n->next=b->next; n->prev=b;
    if(n->next)n->next->prev=n;
    b->next=n; b->capacity=size; note_free(n); return 1;
}
static int initialize(void) {
    if(blocks)return 1;
    blocks=metadata_calloc(1,sizeof *blocks); if(!blocks)return 0;
    first_free=blocks;
    blocks->address=MOVIES_HEAP_BASE; blocks->capacity=MOVIES_HEAP_HI-MOVIES_HEAP_BASE;
    return 1;
}
static Block *reserve(uint32_t pool,uint32_t size) {
    for(Block *b=first_free;b;b=b->next) if(!b->owner && b->pool==pool && b->capacity>=size) {
        if(!split(b,size))return NULL; return b;
    }
    return NULL;
}
static uint32_t create(uint32_t initial,uint32_t maximum) {
    uint64_t rounded=((uint64_t)maximum+4095)&~4095ull;
    uint64_t committed=((uint64_t)(initial?initial:1)+4095)&~4095ull;
    if(!initialize() || committed>MOVIES_HEAP_HI-MOVIES_HEAP_BASE ||
       rounded>MOVIES_HEAP_HI-MOVIES_HEAP_BASE || (maximum && committed>rounded)) return 0;
    Heap *h=metadata_calloc(1,sizeof *h); if(!h)return 0;
    Block *b=reserve(0,(uint32_t)(maximum?rounded:committed));
    if(!b) { free(h); return 0; }
    h->id=next_id++; h->fixed=!!maximum; h->next=heaps; heaps=h;
    b->pool=h->id;
    return h->id;
}
static uint32_t aligned(uint32_t n) { return n>0xfffffff8u ? 0 : ((n?n:1)+7)&~7u; }
/* Extend only through free extents belonging to this heap or the shared pool.
 * Preflight the entire span so failure preserves allocation and reservations.
 * A partial final extent keeps its original pool and remains independently free. */
static int extend(Heap *h,Block *b,uint32_t cap) {
    uint64_t total=b->capacity;
    for(Block *n=b->next;total<cap;n=n->next) {
        if(!n || n->owner || (n->pool!=h->id && (h->fixed || n->pool)))return 0;
        total+=n->capacity;
    }
    while(b->capacity<cap) {
        Block *n=b->next;
        uint32_t need=cap-b->capacity;
        if(n->capacity>need) {
            n->address+=need;n->capacity-=need;b->capacity+=need;
        } else {
            if(first_free==n)first_free=n->next;
            b->capacity+=n->capacity;b->next=n->next;
            if(b->next)b->next->prev=b;
            free(n);refresh_free();
        }
    }
    return 1;
}
static Block *allocate(Heap *h,uint32_t n) {
    uint32_t cap=aligned(n);
    if(!cap || (h->fixed && n>=0x7fff8) || !index_prepare())return NULL;
    /* Reuse reserved extents first. A second pass can start in shared space
     * and span this heap's reserved free extents without stealing another heap. */
    for(unsigned pass=0;pass<(h->fixed?1u:2u);pass++) {
        uint32_t pool=pass?0:h->id;
        for(Block *b=first_free;b;b=b->next) {
            VISIT();
            if(b->owner || b->pool!=pool)continue;
            if(!extend(h,b,cap))continue;
            if(!split(b,cap))return NULL;
            b->pool=h->id;b->owner=h->id;b->size=n;index_insert(b);refresh_free();return b;
        }
    }
    return NULL;
}
static void heap_create(CPU *c) {
    if(!flags(c,ARG(0),1))return;
    pthread_mutex_lock(&mutex); uint32_t h=create(ARG(1),ARG(2)); pthread_mutex_unlock(&mutex);
    if(!h)movies_set_last_error(8); ret(c,3,h);
}
static void process(CPU *c) {
    pthread_mutex_lock(&mutex);
    if(!process_heap)process_heap=create(0,0);
    uint32_t h=process_heap; pthread_mutex_unlock(&mutex);
    if(!h)movies_set_last_error(8); ret(c,0,h);
}
static void destroy(CPU *c) {
    uint32_t id=ARG(0),ok=0;
    pthread_mutex_lock(&mutex);
    Heap **p=&heaps;
    while(*p && (*p)->id!=id)p=&(*p)->next;
    if(*p && id!=process_heap && id!=backend_heap) {
        Heap *h=*p; *p=h->next; free(h);
        for(Block *b=blocks;b;b=b->next) {
            if(b->owner==id) { index_remove(b);b->owner=0; b->size=0; note_free(b); }
            if(b->pool==id)b->pool=0;
        }
        coalesce_all(); ok=1;
    }
    pthread_mutex_unlock(&mutex);
    if(!ok)movies_set_last_error(6); ret(c,1,ok);
}
static void alloc_api(CPU *c) {
    uint32_t f=ARG(1); if(!flags(c,f,1|8))return;
    pthread_mutex_lock(&mutex);
    Heap *h=find_guest_heap(ARG(0)); Block *b=h?allocate(h,ARG(2)):NULL;
    uint32_t address=b?b->address:0;
    if(b && (f&8))memset(HP(address),0,b->size);
    pthread_mutex_unlock(&mutex);
    /* HeapAlloc/HeapReAlloc do not set last-error on allocation failure. */
    ret(c,3,address);
}
static void free_api(CPU *c) {
    if(!flags(c,ARG(1),1))return;
    pthread_mutex_lock(&mutex);
    Heap *h=find_guest_heap(ARG(0)); Block *b=h?find_block(h->id,ARG(2)):NULL;
    uint32_t ok=h && (!ARG(2) || b);
    if(b)release_block(b);
    uint32_t failure=h?87:6;
    pthread_mutex_unlock(&mutex);
    if(!ok)movies_set_last_error(failure); ret(c,3,ok);
}
static void size_api(CPU *c) {
    if(!flags(c,ARG(1),1))return;
    pthread_mutex_lock(&mutex);
    Block *b=find_guest_heap(ARG(0))?find_block(ARG(0),ARG(2)):NULL;
    uint32_t size=b?b->size:0xffffffffu;
    pthread_mutex_unlock(&mutex); ret(c,3,size);
}
static void realloc_api(CPU *c) {
    uint32_t f=ARG(1),n=ARG(3),cap=aligned(n),result=0;
    if(!flags(c,f,1|8|16))return;
    pthread_mutex_lock(&mutex);
    Heap *h=find_guest_heap(ARG(0)); Block *b=h?find_block(h->id,ARG(2)):NULL;
    if(b && cap && !(h->fixed && n>=0x7fff8)) {
        uint32_t old=b->size;
        if(cap>b->capacity)(void)extend(h,b,cap);
        if(cap<=b->capacity) {
            /* Failed shrink metadata allocation simply keeps spare capacity. */
            (void)split(b,cap); b->size=n; result=b->address;
            if((f&8) && n>old)memset(HP(result+old),0,n-old);
            if(b->next && !b->next->owner)coalesce_at(b->next);
        } else if(!(f&16)) {
            Block *replacement=allocate(h,n);
            if(replacement) {
                result=replacement->address; memcpy(HP(result),HP(b->address),old<n?old:n);
                if((f&8) && n>old)memset(HP(result+old),0,n-old);
                release_block(b);
            }
        }
    }
    pthread_mutex_unlock(&mutex); ret(c,4,result);
}

/* Private graphics/media resource storage inside the one existing guest heap
 * arena. Never expose this owner to guest Heap* calls. Returns zero on OOM,
 * has no last-error side effects, and preserves the allocator's lock/index rules. */
static void release_backend_heap(void) {
    if(!backend_heap || backend_allocations)return;
    Heap **p=&heaps;while(*p && (*p)->id!=backend_heap)p=&(*p)->next;
    if(*p){Heap *h=*p;*p=h->next;free(h);}
    for(Block *extent=blocks;extent;extent=extent->next)if(extent->pool==backend_heap)extent->pool=0;
    backend_heap=0;coalesce_all();
}
uint32_t movies_backend_alloc(uint32_t bytes) {
    if(!bytes || bytes>(32u<<20))return 0;
    pthread_mutex_lock(&mutex);
    if(!backend_heap)backend_heap=create(0,0);
    Heap *h=backend_heap?find_heap(backend_heap):NULL;
    Block *b=h?allocate(h,bytes):NULL;uint32_t result=b?b->address:0;
    if(b){memset(HP(result),0,bytes);backend_allocations++;}
    else release_backend_heap();
    pthread_mutex_unlock(&mutex);return result;
}
int movies_backend_free(uint32_t address) {
    pthread_mutex_lock(&mutex);
    Block *b=backend_heap?find_block(backend_heap,address):NULL;
    int ok=b!=NULL;
    if(b) {
        release_block(b);backend_allocations--;
        if(!backend_allocations) {
            release_backend_heap();
        }
    }
    pthread_mutex_unlock(&mutex);return ok;
}
void movies_heap_budget(MoviesHeapBudget *out) {
    MoviesHeapBudget b={MOVIES_HEAP_HI-MOVIES_HEAP_BASE,0,0};
    pthread_mutex_lock(&mutex);
    if(!blocks){b.free_bytes=b.total;b.unreserved=b.total;}
    else for(Block *p=blocks;p;p=p->next)if(!p->owner){b.free_bytes+=p->capacity;if(!p->pool)b.unreserved+=p->capacity;}
    pthread_mutex_unlock(&mutex);*out=b;
}
ImportHandler *movies_heap_handlers(uint32_t *count) {
    static ImportHandler handlers[]={
        {"KERNEL32.DLL!HeapCreate",heap_create},{"KERNEL32.DLL!HeapDestroy",destroy},
        {"KERNEL32.DLL!HeapAlloc",alloc_api},{"KERNEL32.DLL!HeapFree",free_api},
        {"KERNEL32.DLL!HeapReAlloc",realloc_api},{"KERNEL32.DLL!HeapSize",size_api},
        {"KERNEL32.DLL!GetProcessHeap",process}};
    *count=sizeof handlers/sizeof handlers[0]; return handlers;
}
