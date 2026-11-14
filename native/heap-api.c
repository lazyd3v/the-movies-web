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
/* This allocator also serves VirtualAlloc and D3D9 texture storage, so the
 * runtime's separate VirtualAlloc window (GUEST_VALLOC_LO..HI) is otherwise
 * unused. Own it too: in the browser the 256 MiB heap window alone filled up
 * after ~45 min (≈1150 live textures) and operator new threw bad_alloc. */
#if defined(GUEST_VALLOC_LO) && defined(GUEST_VALLOC_HI) && GUEST_VALLOC_LO==GUEST_HEAP_HI
#define MOVIES_HEAP_HI GUEST_VALLOC_HI
#else
#define MOVIES_HEAP_HI GUEST_HEAP_HI
#endif
#endif

typedef struct Block { uint32_t address, capacity, size, owner, pool; struct Block *next, *prev, *hash_next;
    /* Free-extent index: per-pool treap keyed by address, augmented with the
     * largest capacity in each subtree. Finds the lowest-address free extent
     * of a pool with capacity >= n in O(log n), i.e. the same first-fit choice
     * the address-ordered scan made, without walking live blocks. */
    struct Block *left, *right; uint32_t priority, max_capacity, tree_pool; uint8_t in_tree; } Block;
typedef struct Pool { uint32_t id; Block *root; struct Pool *next; } Pool;
static Pool *pools;
static uint32_t treap_seed=0x9e3779b9u;
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
static uint32_t subtree_max(Block *t) { return t?t->max_capacity:0; }
static void update(Block *t) {
    uint32_t m=t->capacity,l=subtree_max(t->left),r=subtree_max(t->right);
    t->max_capacity=m>l?(m>r?m:r):(l>r?l:r);
}
/* split(t,key): left gets addresses < key, right gets >= key. */
static void treap_split(Block *t,uint32_t key,Block **l,Block **r) {
    if(!t) { *l=*r=NULL; return; }
    if(t->address<key) { treap_split(t->right,key,&t->right,r); update(t); *l=t; }
    else { treap_split(t->left,key,l,&t->left); update(t); *r=t; }
}
static Block *treap_merge(Block *a,Block *b) {
    if(!a)return b; if(!b)return a;
    if(a->priority>b->priority) { a->right=treap_merge(a->right,b); update(a); return a; }
    b->left=treap_merge(a,b->left); update(b); return b;
}
static Pool *pool_for(uint32_t id,int create) {
    for(Pool *p=pools;p;p=p->next) if(p->id==id)return p;
    if(!create)return NULL;
    Pool *p=metadata_calloc(1,sizeof *p); if(!p)return NULL;
    p->id=id; p->next=pools; pools=p; return p;
}
static void fi_remove(Block *b) {
    if(!b->in_tree)return;
    Pool *p=pool_for(b->tree_pool,0); b->in_tree=0;
    if(!p)return;
    Block *l,*m,*r; treap_split(p->root,b->address,&l,&m); treap_split(m,b->address+1,&m,&r);
    /* m is exactly b (addresses are unique among free extents). */
    p->root=treap_merge(l,r); b->left=b->right=NULL;
}
static int fi_insert(Block *b) {
    if(b->in_tree || b->owner)return 1;
    Pool *p=pool_for(b->pool,1); if(!p)return 0;
    treap_seed^=treap_seed<<13; treap_seed^=treap_seed>>17; treap_seed^=treap_seed<<5;
    b->priority=treap_seed; b->left=b->right=NULL; update(b);
    Block *l,*r; treap_split(p->root,b->address,&l,&r);
    p->root=treap_merge(treap_merge(l,b),r); b->in_tree=1; b->tree_pool=b->pool; return 1;
}
/* Capacity/address/pool changes of a free extent must re-index it. */
static void fi_refresh(Block *b) { if(b->in_tree) { fi_remove(b); fi_insert(b); } }
static Block *fi_first_fit(uint32_t pool,uint32_t cap) {
    Pool *p=pool_for(pool,0); Block *t=p?p->root:NULL;
    while(t) {
        VISIT();
        if(t->left && t->left->max_capacity>=cap) t=t->left;
        else if(t->capacity>=cap) return t;
        else if(t->right && t->right->max_capacity>=cap) t=t->right;
        else return NULL;
    }
    return NULL;
}
/* Bulk pool/ownership rewrites (HeapDestroy, backend release) rebuild the index. */
static void fi_clear_all(void) {
    for(Block *b=blocks;b;b=b->next) { b->in_tree=0; b->left=b->right=NULL; }
    while(pools) { Pool *n=pools->next; free(pools); pools=n; }
}
static void fi_rebuild(void) { fi_clear_all(); for(Block *b=blocks;b;b=b->next) if(!b->owner)fi_insert(b); }
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
    fi_remove(n);
    b->capacity+=n->capacity;b->next=n->next;
    if(b->next)b->next->prev=b;
    free(n);fi_refresh(b);
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
    index_remove(b);b->owner=0;b->size=0;note_free(b);fi_insert(b);coalesce_at(b);
}
/* Split only after metadata allocation succeeds; failure leaves old block intact. */
static int split(Block *b,uint32_t size) {
    if(size==b->capacity)return 1;
    Block *n=metadata_calloc(1,sizeof *n); if(!n)return 0;
    n->address=b->address+size; n->capacity=b->capacity-size; n->pool=b->pool;
    n->next=b->next; n->prev=b;
    if(n->next)n->next->prev=n;
    int indexed=b->in_tree; if(indexed)fi_remove(b);
    b->next=n; b->capacity=size; note_free(n);
    if(indexed)fi_insert(b);
    fi_insert(n); return 1;
}
static int initialize(void) {
    if(blocks)return 1;
    blocks=metadata_calloc(1,sizeof *blocks); if(!blocks)return 0;
    first_free=blocks;
    blocks->address=MOVIES_HEAP_BASE; blocks->capacity=MOVIES_HEAP_HI-MOVIES_HEAP_BASE;
    return fi_insert(blocks);
}
static Block *reserve(uint32_t pool,uint32_t size) {
    Block *b=fi_first_fit(pool,size);
    if(!b || !split(b,size))return NULL; return b;
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
    fi_remove(b); b->pool=h->id; fi_insert(b);
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
            fi_remove(n);n->address+=need;n->capacity-=need;fi_insert(n);b->capacity+=need;
        } else {
            if(first_free==n)first_free=n->next;
            fi_remove(n);
            b->capacity+=n->capacity;b->next=n->next;
            if(b->next)b->next->prev=b;
            free(n);refresh_free();
        }
    }
    fi_refresh(b);
    return 1;
}
static Block *allocate(Heap *h,uint32_t n) {
    uint32_t cap=aligned(n);
    if(!cap || (h->fixed && n>=0x7fff8) || !index_prepare())return NULL;
    /* Indexed first fit: lowest-address free extent of the heap's own pool, then
     * of the shared pool, whose single capacity suffices. */
    for(unsigned pass=0;pass<(h->fixed?1u:2u);pass++) {
        Block *b=fi_first_fit(pass?0:h->id,cap);
        if(!b)continue;
        if(!split(b,cap))return NULL;
        fi_remove(b);b->pool=h->id;b->owner=h->id;b->size=n;index_insert(b);refresh_free();return b;
    }
    /* Rare fallback: no single extent fits, but adjacent free extents of this
     * heap's pool and the shared pool may be spanned (extend). Address-ordered
     * scan as before. A second pass can start in shared space and span this
     * heap's reserved free extents without stealing another heap. */
    for(unsigned pass=0;pass<(h->fixed?1u:2u);pass++) {
        uint32_t pool=pass?0:h->id;
        for(Block *b=first_free;b;b=b->next) {
            VISIT();
            if(b->owner || b->pool!=pool)continue;
            if(!extend(h,b,cap))continue;
            if(!split(b,cap))return NULL;
            fi_remove(b);b->pool=h->id;b->owner=h->id;b->size=n;index_insert(b);refresh_free();return b;
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
        fi_clear_all();
        for(Block *b=blocks;b;b=b->next) {
            if(b->owner==id) { index_remove(b);b->owner=0; b->size=0; note_free(b); }
            if(b->pool==id)b->pool=0;
        }
        coalesce_all(); fi_rebuild(); ok=1;
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
    fi_clear_all();
    for(Block *extent=blocks;extent;extent=extent->next)if(extent->pool==backend_heap)extent->pool=0;
    backend_heap=0;coalesce_all();fi_rebuild();
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
/* Global, Local, CoTaskMem and Virtual allocations share one private system heap.
 * https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-globalalloc
 * Movable memory is treated as fixed: the handle is the pointer, so Lock returns it
 * and Unlock reports "no longer locked" (0 with ERROR_SUCCESS).
 * https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc
 * Virtual regions are page-aligned, zero-filled, committed on reservation (all
 * guest memory is accessible), and released as a whole by MEM_RELEASE. */
static uint32_t system_heap;
typedef struct { uint32_t base,size,block; } Region;
static Region regions[256];
static unsigned region_count;
static Block *system_alloc(uint32_t n,int zero) {
    if(!system_heap)system_heap=create(0,0);
    Heap *h=system_heap?find_heap(system_heap):NULL;
    Block *b=h?allocate(h,n):NULL;
    if(b && zero)memset(HP(b->address),0,b->size);
    return b;
}
static void global_alloc(CPU *c) {
    uint32_t f=ARG(0),n=ARG(1);
    pthread_mutex_lock(&mutex); Block *b=system_alloc(n,(f&0x40)!=0); uint32_t a=b?b->address:0; pthread_mutex_unlock(&mutex);
    if(!a)movies_set_last_error(8); ret(c,2,a);
}
/* Host-side LocalAlloc(LMEM_ZEROINIT) for APIs that return LocalFree-able memory. */
uint32_t movies_local_alloc(uint32_t bytes) {
    pthread_mutex_lock(&mutex); Block *b=system_alloc(bytes,1); uint32_t a=b?b->address:0; pthread_mutex_unlock(&mutex);
    return a;
}
static void global_free(CPU *c) {
    uint32_t a=ARG(0),ok=1;
    pthread_mutex_lock(&mutex);
    if(a){Block *b=system_heap?find_block(system_heap,a):NULL; if(b)release_block(b); else ok=0;}
    pthread_mutex_unlock(&mutex);
    if(!ok)movies_set_last_error(6); ret(c,1,ok?0:a);
}
static void global_lock(CPU *c) {
    uint32_t a=ARG(0);
    pthread_mutex_lock(&mutex); Block *b=system_heap&&a?find_block(system_heap,a):NULL; pthread_mutex_unlock(&mutex);
    if(!b)movies_set_last_error(6); ret(c,1,b?a:0);
}
static void global_unlock(CPU *c) { movies_set_last_error(0); ret(c,1,0); }
static void global_size(CPU *c) {
    uint32_t a=ARG(0);
    pthread_mutex_lock(&mutex); Block *b=system_heap&&a?find_block(system_heap,a):NULL; uint32_t n=b?b->size:0; pthread_mutex_unlock(&mutex);
    ret(c,1,n);
}
static void cotask_alloc(CPU *c) {
    pthread_mutex_lock(&mutex); Block *b=system_alloc(ARG(0),0); uint32_t a=b?b->address:0; pthread_mutex_unlock(&mutex);
    ret(c,1,a);
}
static void cotask_free(CPU *c) {
    uint32_t a=ARG(0);
    pthread_mutex_lock(&mutex); Block *b=system_heap&&a?find_block(system_heap,a):NULL; if(b)release_block(b); pthread_mutex_unlock(&mutex);
    ret(c,1,0);
}
static Region *find_region(uint32_t a) {
    for(unsigned i=0;i<region_count;i++)if(a>=regions[i].base && a-regions[i].base<regions[i].size)return &regions[i];
    return NULL;
}
static void virtual_alloc(CPU *c) {
    uint32_t a=ARG(0),n=ARG(1),type=ARG(2),result=0,error=8;
    pthread_mutex_lock(&mutex);
    Region *r=a?find_region(a):NULL;
    if(r) {
        /* Commit (or re-reserve) inside an existing region: already accessible.
         * Fresh commits of previously decommitted pages are not re-zeroed. */
        if(a-r->base+(uint64_t)n<=r->size)result=a&~0xfffu; else error=487;
    } else if(a && !(type&0x2000)) error=487; /* commit of an unreserved address */
    else if(!n || n>0x7ffff000u) error=87;
    else if(region_count<sizeof regions/sizeof regions[0]) {
        uint32_t size=(n+0xfffu)&~0xfffu;
        Block *b=system_alloc(size+0x1000u,0);
        if(b) {
            result=(b->address+0xfffu)&~0xfffu; memset(HP(result),0,size);
            regions[region_count++]=(Region){result,size,b->address};
        }
    }
    pthread_mutex_unlock(&mutex);
    if(!result)movies_set_last_error(error); ret(c,4,result);
}
static void virtual_free(CPU *c) {
    uint32_t a=ARG(0),n=ARG(1),type=ARG(2),ok=0;
    pthread_mutex_lock(&mutex);
    Region *r=find_region(a);
    if(r && (type&0x8000) && !n && a==r->base) {
        Block *b=find_block(system_heap,r->block); if(b)release_block(b);
        *r=regions[--region_count]; ok=1;
    } else if(r && (type&0x4000)) ok=1; /* decommit: pages stay accessible */
    pthread_mutex_unlock(&mutex);
    if(!ok)movies_set_last_error(87); ret(c,3,ok);
}
/* MEMORY_BASIC_INFORMATION: BaseAddress, AllocationBase, AllocationProtect,
 * RegionSize, State, Protect, Type (28 bytes). */
static void virtual_query(CPU *c) {
    uint32_t a=ARG(0),out=ARG(1),n=ARG(2);
    if(n<28 || !out){movies_set_last_error(24);ret(c,3,0);return;}
    pthread_mutex_lock(&mutex); Region *r=find_region(a); Region copy=r?*r:(Region){a&~0xfffu,0x1000,0}; pthread_mutex_unlock(&mutex);
    uint32_t page=a&~0xfffu;
    W32(out,page);W32(out+4,copy.base);W32(out+8,4);W32(out+12,copy.base+copy.size-page);
    W32(out+16,0x1000);W32(out+20,4);W32(out+24,r?0x20000:0x1000000);
    ret(c,3,28);
}
static void virtual_protect(CPU *c) {
    uint32_t old=ARG(3);
    if(old)W32(old,4); /* PAGE_READWRITE; protection is not enforced */
    ret(c,4,1);
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
        {"KERNEL32.DLL!GetProcessHeap",process},
        {"KERNEL32.DLL!GlobalAlloc",global_alloc},{"KERNEL32.DLL!GlobalFree",global_free},
        {"KERNEL32.DLL!GlobalLock",global_lock},{"KERNEL32.DLL!GlobalUnlock",global_unlock},
        {"KERNEL32.DLL!GlobalSize",global_size},
        {"KERNEL32.DLL!LocalAlloc",global_alloc},{"KERNEL32.DLL!LocalFree",global_free},
        {"OLE32.DLL!CoTaskMemAlloc",cotask_alloc},{"OLE32.DLL!CoTaskMemFree",cotask_free},
        {"KERNEL32.DLL!VirtualAlloc",virtual_alloc},{"KERNEL32.DLL!VirtualFree",virtual_free},
        {"KERNEL32.DLL!VirtualQuery",virtual_query},{"KERNEL32.DLL!VirtualProtect",virtual_protect}};
    *count=sizeof handlers/sizeof handlers[0]; return handlers;
}
