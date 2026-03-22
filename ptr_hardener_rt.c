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
 *  - ator   := AllocaTOR
 *  - dtor   := DeallocaTOR
 */

#include <assert.h>
#include <limits.h>
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
#define RNGMAP_NR_ENTRIES   (1 << 16)

typedef uint16_t rngmap_index_t;

// Thanks to __attribute__((weak)), all __ph_rngmap's in different translation
// units would share the same storage space.
void *__ph_rngmap __attribute__((weak));

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

// Allocate a granule-aligned object (using 'aligned_alloc').
// Every allocation should ultimately use this.
static void *__ph_alloc(size_t size) {
    return malloc(size);
}

// Every deallocation should ultimately use this.
static void __ph_dealloc(void *ptr) {
    free(ptr);
}

static void *__ph_create_rngmap(unsigned n_entries) {
    unsigned size = n_entries * sizeof(struct rngmap_entry);
    void *ret = __ph_alloc(GRANULE_SIZE);
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

static void __ph_init_rngmap_entry_null(struct rngmap_entry *entry) {
    // entry->type = RNGMAP_ENTRY_NULL; entry->tag = entry->rng = entry->oob = NULL;
    memset(entry, 0, sizeof(struct rngmap_entry));
}

static void __ph_init_rngmap_entry_inb(struct rngmap_entry *entry, void *tag, void *rng) {
    entry->type = RNGMAP_ENTRY_INB;
    entry->tag = tag;
    entry->rng = rng;
    entry->oob = NULL;
}

static void __ph_init_rngmap_entry_oob(struct rngmap_entry *entry, void *tag, void *rng) {
    entry->type = RNGMAP_ENTRY_OOB;
    entry->tag = tag;
    entry->rng = rng;
    entry->oob = NULL;
}

static void __ph_init_rngmap_entry_map(struct rngmap_entry *entry, void *rngmap) {
    entry->type = RNGMAP_ENTRY_MAP;
    entry->tag = NULL;
    entry->rng = rngmap;
    entry->oob = NULL;
}

/** Internal functions **/

static bool __ph_init_rngmap_entry(void *rngmap, void *tag, void *rng, unsigned lv) {
    if (lv == UINT_MAX) return false;

    struct rngmap_entry *entry = __ph_index_rngmap_entry(rngmap, __ph_hash_addr(tag, lv));
    assert(entry);

    switch (entry->type) {
        case RNGMAP_ENTRY_NULL:
            __ph_init_rngmap_entry_inb(entry, tag, rng);
            break;
        case RNGMAP_ENTRY_MAP:
            bool init_res = __ph_init_rngmap_entry(entry->rng, tag, rng, lv + 1);
            if (!init_res) return false;
            break;
        default:
            struct rngmap_entry prev_entry = *entry;
            void *new_rngmap = __ph_create_rngmap(RNGMAP_NR_ENTRIES);
            if (!new_rngmap) return false;

            __ph_init_rngmap_entry_map(entry, new_rngmap);

            bool init1_res = __ph_init_rngmap_entry(new_rngmap, prev_entry.tag, prev_entry.rng, lv + 1);
            if (!init1_res) return false;

            bool init2_res = __ph_init_rngmap_entry(new_rngmap, tag, rng, lv + 1);
            if (!init2_res) return false;
            break;
    }

    return true;
}

static bool __ph_init_rngmap_entries(void *aobj, size_t size) {
    assert(aobj == __ph_ceil_to_granule_ptr(aobj));

    if (!__ph_rngmap) 
        __ph_rngmap = __ph_create_rngmap(RNGMAP_NR_ENTRIES);

    for (int goff = 0; goff < size / GRANULE_SIZE; goff++) {
        void *tag = aobj + (goff * GRANULE_SIZE);
        void *rng = aobj + size;
        bool init_res = __ph_init_rngmap_entry(__ph_rngmap, tag, rng, 0);
        if (!init_res) return false;
    }
    
    return true;
}

static struct rngmap_entry *__ph_get_rngmap_bnd_entry_inner(void *rngmap, void *aobj, unsigned lv) {
    if (!rngmap) return NULL;
    struct rngmap_entry *entry = __ph_index_rngmap_entry(rngmap, __ph_hash_addr(aobj, lv));

    switch (entry->type) {
        case RNGMAP_ENTRY_MAP:
            return __ph_get_rngmap_bnd_entry_inner(entry->rng, aobj, lv + 1);
        case RNGMAP_ENTRY_NULL:
            return NULL;
        default:
            if (entry->tag == aobj) return entry;
            else return NULL;
    }
}

static struct rngmap_entry *__ph_get_rngmap_bnd_entry(void *aobj) {
    if (!__ph_rngmap) return NULL;
    return __ph_get_rngmap_bnd_entry_inner(__ph_rngmap, aobj, 0);
}

static bool __ph_destroy_rngmap_entry(void *tag) {
    struct rngmap_entry *entry = __ph_get_rngmap_bnd_entry(tag);
    if (!entry) return false;

    void *cur_oob = entry->oob;
    __ph_init_rngmap_entry_null(entry);

    while (cur_oob) {        
        struct rngmap_entry *oob_entry = __ph_get_rngmap_bnd_entry(cur_oob);
        if (!oob_entry) return false;

        assert(oob_entry->type == RNGMAP_ENTRY_OOB);

        cur_oob = oob_entry->oob;
        __ph_init_rngmap_entry_null(oob_entry);
    }
    
    return true;
}

static bool __ph_destroy_rngmap_entries(void *aobj) {
    assert(aobj == __ph_floor_to_granule_ptr(aobj));

    // Get the range of the current 'aobj'.
    struct rngmap_entry *entry = __ph_get_rngmap_bnd_entry(aobj);
    if (!entry) return true;    // Maybe untracked object. Ignore.

    struct range_info *rng = entry->rng;
    if (!rng) return false;

    // Destroy all associated range map entries with 'aobj'.
    for (int goff = 0; goff < rng->len / GRANULE_SIZE; goff++) {
        void *tag = rng->base + (goff * GRANULE_SIZE);
        bool destroy_res = __ph_destroy_rngmap_entry(tag);
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
    assert(aobj == __ph_ceil_to_granule_ptr(aobj));

    struct range_info *rng = (struct range_info *)(aobj + size);
    rng->base = aobj;
    rng->len = size;

    bool init_res = __ph_init_rngmap_entries(aobj, size);
    if (!init_res) {
        __ph_dealloc(aobj);
        return NULL;
    }

    return aobj;
}

/** Wrappers and instrumented functions **/

static void *__ph_malloc(size_t size) {
    size_t aligned_size = __ph_extend_w_range_info_granule_alignable(size);
    if (aligned_size < size) return NULL;
    void *obj = __ph_alloc(aligned_size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    return __ph_alloc_post(aobj, size);
}

/*
static void *__ph_calloc(size_t num, size_t esize) {
    size_t size = num * esize;
    if (size < esize) return NULL;
    size_t aligned_size = __ph_extend_granule_alignable(size);
    if (aligned_size < size) return NULL;
    void *obj = __ph_alloc(aligned_size);
    if (!obj) return NULL;

    memset(obj, 0, aligned_size);

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(size);

    return __ph_alloc_post(aobj, asize);
}

static void *__ph_aalloc(size_t align, size_t size) {
    size_t aligned_size = __ph_extend_granule_alignable(size);
    if (aligned_size < size) return NULL;
    size_t aligned_aligned_size = (aligned_size + align - 1) & ~((size_t)(align - 1));
    void *obj = __ph_alloc(align, aligned_aligned_size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(size);

    return __ph_alloc_post(aobj, asize);
}

static void *__ph_realloc(void *ptr, size_t size) {
    bool destroy_res = __ph_destroy_rngmap_entries(ptr);
    if (!destroy_res) return NULL;

    size_t aligned_size = __ph_extend_granule_alignable(size);
    if (aligned_size < size) return NULL;
    void *obj = realloc(ptr, aligned_size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(size);

    if (aobj != obj)
        memmove(aobj, obj, size);

    return __ph_alloc_post(aobj, asize);
}
*/

static void __ph_free(void *ptr) {
    void *aptr = __ph_floor_to_granule_ptr(ptr);

    bool destroy_res = __ph_destroy_rngmap_entries(aptr);
    if (!destroy_res) return;

    __ph_dealloc(ptr);  // TODO: implement quarantine.
}

static void __ph_ptr_move(void *prev, void* next, size_t size) {
    
}

static void __ph_ptr_deref(void *ptr) {
    // TODO: check oob entry.
}
