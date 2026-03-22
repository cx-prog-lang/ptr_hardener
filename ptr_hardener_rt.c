/*
 * ptr_hardner - instrumentation functions
 * ---------------------------------------
 *
 * Written by Gwangmu Lee <iss300@proton.me>
 *
 * Functions that will be inserted in place of
 * (de)allocallocators and pointer moves/dereferences.
 */

/*
 * Glossary:
 *  - rngmap := RaNGe MAP
 *  - Variables starting with a redundant 'a' := granule-Aligned
 */

#include <assert.h>
#include <dlfcn.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum rngmap_entry_type {
    RNGMAP_ENTRY_NULL = 0,
    RNGMAP_ENTRY_INB,
    RNGMAP_ENTRY_OOB,
    RNGMAP_ENTRY_MAP,
    NR_RNGMAP_ENTRY_TYPES
};

struct rngmap_entry {
    enum rngmap_entry_type type;
    void *tag;
    void *rng;
    void *oob;
};

struct range_info {
    void *base;     // Granule-aligned base address
    size_t len;     // Granule-aligned length (in bytes)
};

#define GRANULE_SIZE        (1 << 5)
#define RNGMAP_NR_ENTRIES   (1 << 8)        // NOTE: originally 1 << 16

// NOTE: sizeof(rngmap_index_t) should agree with 'RNGMAP_NR_ENRIES'.
typedef uint8_t rngmap_index_t;

// Thanks to __attribute__((weak)), all __ph_rngmap's in different translation
// units would share the same storage space.
void *__ph_rngmap __attribute__((weak));

// Actual standard allocators. The default behavior is to initialize them when
// they're used the first time, but they _may_ be updated by a global
// constructor if there is an annotated custom allocator. The custom ones should
// have the same type signature to the allocator that it wants to replace.
// Let's say multiple custom allocators (for the same original allocator) causes
// an undefined behavior, though I think it'll function just fine.
void *(*malloc_impl)(size_t) __attribute__((weak));           // for 'malloc'
void *(*calloc_impl)(size_t, size_t) __attribute__((weak));   // for 'calloc'
void *(*aalloc_impl)(size_t, size_t) __attribute__((weak));   // for 'aligned_alloc'
void *(*realloc_impl)(void *, size_t) __attribute__((weak));  // for 'realloc'
void (*free_impl)(void *) __attribute__((weak));              // for 'free'
void (*sfree_impl)(void *) __attribute__((weak));             // for 'free_sized'
void (*asfree_impl)(void *) __attribute__((weak));            // for 'free_aligned_sized'

/** Internal utilities **/

static void *__ph_ceil_to_granule_ptr(void *x) {
    return (void *)(((intptr_t)x + (GRANULE_SIZE - 1)) & ~(intptr_t)(GRANULE_SIZE - 1));
}

static size_t __ph_ceil_to_granule_size(size_t x) {
    return (x + (GRANULE_SIZE - 1)) & ~(size_t)(GRANULE_SIZE - 1);
}

static void *__ph_floor_to_granule_ptr(void *x) {
    return (void *)((intptr_t)x & ~(intptr_t)(GRANULE_SIZE - 1));
}

static void *__ph_create_rngmap(unsigned n_entries) {
    assert(malloc_impl);
    unsigned size = n_entries * sizeof(struct rngmap_entry);
    void *ret = malloc_impl(size);
    if (!ret) return 0;
    memset(ret, 0, size);
    return ret;
}

static rngmap_index_t __ph_hash_addr(void *addr, unsigned seed) {
    uintptr_t addr_w_seed = (uintptr_t)addr + seed;
    char *_addr_w_seed = (char *)&addr_w_seed;

    rngmap_index_t ret = 0;

    int off = 0;
    for (; off + sizeof(rngmap_index_t) - 1 < sizeof(uintptr_t); off += sizeof(rngmap_index_t)) 
        ret ^= *(rngmap_index_t *)(_addr_w_seed + off);
    for (int i = 0; off < sizeof(uintptr_t); i++, off++)
        ((char *)&ret)[i] ^= *(char *)(_addr_w_seed + off);

    return ret;
}

static struct rngmap_entry *__ph_index_rngmap_entry(void *rngmap, rngmap_index_t idx) {
    return &((struct rngmap_entry *)rngmap)[idx];
}

static void __ph_set_rngmap_entry_null(struct rngmap_entry *entry) {
    // entry->type = RNGMAP_ENTRY_NULL; entry->tag = entry->rng = entry->oob = NULL;
    memset(entry, 0, sizeof(struct rngmap_entry));
}

static void __ph_set_rngmap_entry_bnd(struct rngmap_entry *entry, enum rngmap_entry_type type, void *tag, void *rng) {
    entry->type = type;
    entry->tag = tag;
    entry->rng = rng;
    entry->oob = NULL;
}

static void __ph_set_rngmap_entry_map(struct rngmap_entry *entry, void *rngmap) {
    entry->type = RNGMAP_ENTRY_MAP;
    entry->tag = NULL;
    entry->rng = rngmap;
    entry->oob = NULL;
}

/** Internal functions **/

static struct rngmap_entry *__ph_create_rngmap_entry_inner(void *rngmap, struct rngmap_entry evalue, unsigned lv) {
    assert(evalue.type == RNGMAP_ENTRY_INB || evalue.type == RNGMAP_ENTRY_OOB);

    if (lv == UINT_MAX) return NULL;

    struct rngmap_entry *entry = __ph_index_rngmap_entry(rngmap, __ph_hash_addr(evalue.tag, lv));
    assert(entry);

    switch (entry->type) {
        case RNGMAP_ENTRY_NULL:
            __ph_set_rngmap_entry_bnd(entry, evalue.type, evalue.tag, evalue.rng);
            return entry;
        case RNGMAP_ENTRY_MAP:
            return __ph_create_rngmap_entry_inner(entry->rng, evalue, lv + 1);
        default:
            struct rngmap_entry prev_evalue = *entry;
            void *new_rngmap = __ph_create_rngmap(RNGMAP_NR_ENTRIES);
            if (!new_rngmap) return NULL;

            __ph_set_rngmap_entry_map(entry, new_rngmap);

            struct rngmap_entry *entry1 = __ph_create_rngmap_entry_inner(new_rngmap, prev_evalue, lv + 1);
            if (!entry1) return NULL;

            struct rngmap_entry *entry2 =__ph_create_rngmap_entry_inner(new_rngmap, evalue, lv + 1);
            if (!entry2) return NULL;

            return entry2;
    }
}

static struct rngmap_entry *__ph_create_rngmap_entry(enum rngmap_entry_type type, void *tag, void *rng) {
    if (!__ph_rngmap) return NULL;
    struct rngmap_entry evalue = { .type = type, .tag = tag, .rng = rng };
    return __ph_create_rngmap_entry_inner(__ph_rngmap, evalue, 0);
}

static bool __ph_create_rngmap_entries(void *aobj, size_t size) {
    assert((intptr_t)aobj % GRANULE_SIZE == 0);

    if (!__ph_rngmap) 
        __ph_rngmap = __ph_create_rngmap(RNGMAP_NR_ENTRIES);

    for (int goff = 0; goff < size / GRANULE_SIZE; goff++) {
        void *tag = aobj + (goff * GRANULE_SIZE);
        void *rng = aobj + size;
        void *create_res = __ph_create_rngmap_entry(RNGMAP_ENTRY_INB, tag, rng);
        if (!create_res) return false;
    }
    
    return true;
}

static struct rngmap_entry *__ph_get_rngmap_bnd_entry_inner(void *rngmap, void *obj, unsigned lv) {
    if (!rngmap) return NULL;
    struct rngmap_entry *entry = __ph_index_rngmap_entry(rngmap, __ph_hash_addr(obj, lv));

    switch (entry->type) {
        case RNGMAP_ENTRY_MAP:
            return __ph_get_rngmap_bnd_entry_inner(entry->rng, obj, lv + 1);
        case RNGMAP_ENTRY_NULL:
            return NULL;
        default:
            if (entry->tag == obj) return entry;
            else return NULL;
    }
}

static struct rngmap_entry *__ph_get_rngmap_bnd_entry(void *obj) {
    if (!__ph_rngmap) return NULL;
    return __ph_get_rngmap_bnd_entry_inner(__ph_rngmap, obj, 0);
}

static bool __ph_destroy_rngmap_entry(void *tag) {
    struct rngmap_entry *entry = __ph_get_rngmap_bnd_entry(tag);
    if (!entry) return false;

    struct rngmap_entry *oob_entry = (struct rngmap_entry *)entry->oob;
    __ph_set_rngmap_entry_null(entry);

    while (oob_entry) {
        struct rngmap_entry *next_oob_entry = (struct rngmap_entry *)oob_entry->oob;
        __ph_set_rngmap_entry_null(oob_entry);
        oob_entry = next_oob_entry;
    }
    
    return true;
}

static bool __ph_destroy_rngmap_entries(void *aobj) {
    assert((intptr_t)aobj % GRANULE_SIZE == 0);

    // Get the range of the current 'aobj'.
    struct rngmap_entry *entry = __ph_get_rngmap_bnd_entry(aobj);
    if (!entry) return true;    // Maybe untracked object. Ignore.

    struct range_info *rng = entry->rng;
    assert(rng);

    // Destroy all associated range map entries with 'aobj'.
    for (int goff = 0; goff < rng->len / GRANULE_SIZE; goff++) {
        void *atag = rng->base + (goff * GRANULE_SIZE);
        bool destroy_res = __ph_destroy_rngmap_entry(atag);
        if (!destroy_res) return false;
    }

    return true;
}

static size_t __ph_extend_w_range_info_granule_alignable(size_t size) {
    return (GRANULE_SIZE - 1)          // Object alignment slack
        + size                         // Object itself
        + sizeof(struct range_info);   // Range information
}

static void *__ph_alloc_post(void *aobj, size_t size) {
    assert((intptr_t)aobj % GRANULE_SIZE == 0);

    struct range_info *rng = (struct range_info *)(aobj + size);
    rng->base = aobj;
    rng->len = size;

    bool init_res = __ph_create_rngmap_entries(aobj, size);
    if (!init_res) {
        if (!free_impl) {
            free_impl = dlsym(RTLD_NEXT, "free");
            if (!free_impl) return NULL;
        }

        free_impl(aobj);
        return NULL;
    }

    return aobj;
}

/** Wrapper functions **/

__attribute__((weak))
void *malloc(size_t size) {
    if (!malloc_impl) {
        malloc_impl = dlsym(RTLD_NEXT, "malloc");
        if (!malloc_impl) return NULL;
    }

    size_t aligned_size = __ph_extend_w_range_info_granule_alignable(size);
    if (aligned_size < size) return NULL;
    void *obj = malloc_impl(aligned_size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    return __ph_alloc_post(aobj, size);
}

__attribute__((weak))
void *calloc(size_t num, size_t esize) {
    if (!calloc_impl) {
        calloc_impl = dlsym(RTLD_NEXT, "calloc");
        if (!calloc_impl) return NULL;
    }

    size_t size = num * esize;
    if (size < esize) return NULL;
    size_t aligned_size = __ph_extend_w_range_info_granule_alignable(size);
    if (aligned_size < size) return NULL;
    size_t aligned_num = (aligned_size + esize - 1) / esize;
    void *obj = calloc_impl(aligned_num, esize);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(size);

    return __ph_alloc_post(aobj, asize);
}

__attribute__((weak))
void *aligned_alloc(size_t align, size_t size) {
    if (!aalloc_impl) {
        aalloc_impl = dlsym(RTLD_NEXT, "aligned_alloc");
        if (!aalloc_impl) return NULL;
    }

    size_t aligned_size = __ph_extend_w_range_info_granule_alignable(size);
    if (aligned_size < size) return NULL;
    size_t aligned_aligned_size = (aligned_size + align - 1) & ~((size_t)(align - 1));
    void *obj = aalloc_impl(align, aligned_aligned_size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(size);

    return __ph_alloc_post(aobj, asize);
}

__attribute__((weak))
void *realloc(void *ptr, size_t size) {
    if (!realloc_impl) {
        realloc_impl = dlsym(RTLD_NEXT, "realloc");
        if (!realloc_impl) return NULL;
    }

    bool destroy_res = __ph_destroy_rngmap_entries(ptr);
    if (!destroy_res) return NULL;

    size_t aligned_size = __ph_extend_w_range_info_granule_alignable(size);
    if (aligned_size < size) return NULL;
    void *obj = realloc(ptr, aligned_size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(size);

    if (aobj != obj)
        memmove(aobj, obj, size);

    return __ph_alloc_post(aobj, asize);
}

__attribute__((weak))
void free(void *ptr) {
    if (!free_impl) {
        free_impl = dlsym(RTLD_NEXT, "free");
        if (!free_impl) return;
    }

    void *aptr = __ph_floor_to_granule_ptr(ptr);

    bool destroy_res = __ph_destroy_rngmap_entries(aptr);
    if (!destroy_res) return;

    free_impl(ptr);  // TODO: implement quarantine.
}

__attribute__((weak))
void free_sized(void *ptr) {
    if (!sfree_impl) {
        sfree_impl = dlsym(RTLD_NEXT, "free_sized");
        if (!sfree_impl) return;
    }

    void *aptr = __ph_floor_to_granule_ptr(ptr);

    bool destroy_res = __ph_destroy_rngmap_entries(aptr);
    if (!destroy_res) return;

    sfree_impl(ptr);  // TODO: implement quarantine.
}

__attribute__((weak))
void free_aligned_sized(void *ptr) {
    if (!asfree_impl) {
        asfree_impl = dlsym(RTLD_NEXT, "free_aligned_sized");
        if (!asfree_impl) return;
    }

    void *aptr = __ph_floor_to_granule_ptr(ptr);

    bool destroy_res = __ph_destroy_rngmap_entries(aptr);
    if (!destroy_res) return;

    asfree_impl(ptr);  // TODO: implement quarantine.
}

/** Instrumented functions **/

static void __ph_ptr_move(void *prev, void* next, size_t size) {
    void *aprev = __ph_floor_to_granule_ptr(prev);
    struct rngmap_entry *entry = __ph_get_rngmap_bnd_entry(aprev);
    if (!entry) return;

    struct range_info *rng = entry->rng;
    assert(rng);

    // If 'next' is out of bounds, create an 'oob' range map entry.
    if (!(rng->base <= next && next + size <= rng->base + rng->len)) {
        struct rngmap_entry *oob_entry = __ph_create_rngmap_entry(RNGMAP_ENTRY_OOB, next, (void *)rng);
        if (!oob_entry) return;

        // Chain 'oob_entry' to this inbound entry.
        while (entry->oob)
            entry = entry->oob;
        entry->oob = (void *)oob_entry;
    } 
}

static void __ph_ptr_deref(void *ptr) {
    struct rngmap_entry *entry = __ph_get_rngmap_bnd_entry(ptr);
    if (entry && entry->type == RNGMAP_ENTRY_OOB)
        raise(SIGUSR1);
}
