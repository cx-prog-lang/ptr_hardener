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
#include <stdlib.h>

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

enum allocator_type {
    ATOR_MALLOC,    // malloc-equivalent allocallocator
    ATOR_CALLOC,    // calloc-equivalent allocallocator
    ATOR_AALLOC,    // aligned_alloc-equivalent allocallocator
    ATOR_REALLOC,   // realloc-equivalent allocallocator
    NR_ATOR_TYPES
};

struct allocator {
    enum allocator_type type;
    void *ator;
    void *dtor;
};

struct range_info {
    void *base;     // Granule-aligned base address
    size_t len;     // Granule-aligned length (in bytes)
};

typedef void *(*malloc_t)(size_t);
typedef void *(*calloc_t)(size_t, size_t);
typedef void *(*aalloc_t)(size_t, size_t);
typedef void *(*realloc_t)(void *, size_t);
typedef void (*free_t)(void *);

#define GRANULE_SIZE        (1 << 3)
#define RNGMAP_NR_ENTRIES   (1 << 16)

typedef uint16_t rngmap_index_t;

// Thanks to __attribute__((weak)), all __ph_rngmap's in different translation
// units would share the same storage space.
void *__ph_rngmap __attribute__((weak));

/** Internal functions **/

// Allocate a memory like malloc, agnostic to the underlying allocator (which
// is assumed to behave the standard counterpart).
static void *__ph_allocate(size_t size, struct allocator ator) {
    void *ret = 0;
    switch (ator.type) {
        case ATOR_MALLOC:
            ret = ((malloc_t)ator.ator)(size);
            break;
        case ATOR_CALLOC:
            ret = ((calloc_t)ator.ator)(1, size);
            break;
        case ATOR_AALLOC:
            ret = ((aalloc_t)ator.ator)(4, (size + 4) & ~(size_t)0x11);
            break;
        case ATOR_REALLOC:
            ret = ((realloc_t)ator.ator)(NULL, size);
            break;
    }
    return ret;
}

static void *__ph_create_rngmap(unsigned n_entries, struct allocator ator) {
    unsigned size = n_entries * sizeof(struct rngmap_entry);
    void *ret = __ph_allocate(size, ator);
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
    // entry->type = RNGMAP_TYPE_NULL; entry->tag = entry->rng = entry->oob = NULL;
    memset(entry, 0, sizeof(struct rngmap_entry));
}

static void __ph_init_rngmap_entry_inb(struct rngmap_entry *entry, void *atag, void *arng) {
    entry->type = RNGMAP_TYPE_INB;
    entry->tag = atag;
    entry->rng = arng;
    entry->oob = NULL;
}

static void __ph_init_rngmap_entry_oob(struct rngmap_entry *entry, void *atag, void *arng) {
    entry->type = RNGMAP_TYPE_OOB;
    entry->tag = atag;
    entry->rng = arng;
    entry->oob = NULL;
}

static void __ph_init_rngmap_entry_map(struct rngmap_entry *entry, void *rngmap) {
    entry->type = RNGMAP_TYPE_MAP;
    entry->tag = NULL;
    entry->rng = rngmap;
    entry->oob = NULL;
}

static bool __ph_init_rngmap_entry(void *rngmap, void *atag, void *arng, unsigned lv, struct allocator ator) {
    if (lv == UINT_MAX) return false;

    struct rngmap_entry *entry = __ph_index_rngmap_entry(rngmap, __ph_hash_addr(atag, lv));
    assert(entry);

    switch (entry->type) {
        case RNGMAP_ENTRY_NULL:
            __ph_init_rngmap_entry_inb(entry, atag, arng);
            break;
        case RNGMAP_ENTRY_MAP:
            bool init_res = __ph_init_rngmap_entry(entry->rng, atag, arng, lv + 1, ator);
            if (!init_res) return false;
            break;
        default:
            struct rngmap_entry prev_entry = *entry;
            void *new_rngmap = __ph_create_rngmap(RNGMAP_NR_ENTRIES, ator);
            if (!new_rngmap) return false;

            __ph_init_rngmap_entry_map(entry, new_rngmap);

            bool init1_res = __ph_init_rngmap_entry(new_rngmap, prev_entry->tag, prev_entry->rng, lv + 1, ator);
            if (!init1_res) return false;

            bool init2_res = __ph_init_rngmap_entry(new_rngmap, atag, arng, lv + 1, ator);
            if (!init2_res) return false;
            break;
    }

    return true;
}

static bool __ph_init_rngmap_entries(void *aobj, size_t asize, struct allocator ator) {
    assert(aobj == __ph_ceil_to_granule_ptr(aobj));
    assert(asize % GRANULE_SIZE == 0);

    if (!__ph_rngmap) 
        __ph_rngmap = __ph_create_rngmap(RNGMAP_NR_ENTRIES, ator);

    for (int goff = 0; goff < asize / GRANULE_SIZE; goff++) {
        void *atag = aobj + (goff * GRANULE_SIZE);
        void *arng = aobj + asize;
        bool init_res = __ph_init_rngmap_entry(__ph_rngmap, atag, arng, 0, ator);
        if (!init_res) return false;
    }
    
    return true;
}

static void *__ph_floor_to_granule_ptr(void *x) {
    return (void *)((intptr_t)x & ~(intptr_t)(GRANULE_SIZE - 1));
}

static struct rngmap_entry *__ph_get_rngmap_bnd_entry(void *rngmap, void *aobj, unsigned lv) {
    if (!rngmap) return NULL;
    struct rngmap_entry *entry = __ph_index_rngmap_entry(rngmap, __ph_hash_addr(atag, lv));

    switch (entry->type) {
        case RNGMAP_ENTRY_MAP:
            return __ph_get_rngmap_entry(entry->rng, aobj, lv + 1);
        case RNGMAP_ENTRY_NULL:
            return NULL;
        default:
            if (entry->tag == aobj) return entry;
            else return NULL;
    }
}

static bool __ph_destroy_rngmap_entry(void *rngmap, void *atag) {
    struct rngmap_entry *entry = __ph_get_rngmap_bnd_entry(rngmap, atag, 0);
    if (!entry) return false;

    void *cur_oob = entry->oob;
    __ph_init_rngmap_entry_null(entry);

    while (cur_oob) {        
        struct rngmap_entry *oob_entry = __ph_get_rngmap_bnd_entry(rngmap, cur_oob, 0);
        if (!oob_entry) return false;

        assert(oob_entry->type == RNGMAP_ENTRY_OOB);

        cur_oob = oob_entry->oob;
        __ph_init_rngmap_entry_null(oob_entry);
    }
    
    return true;
}

static bool __ph_destroy_rngmap_entries(void *aobj) {
    assert(aobj == __ph_floor_to_granule_ptr(aobj));

    struct rngmap_entry *entry = __ph_get_rngmap_bnd_entry(__ph_rngmap, aobj, 0);
    if (!entry) return true;    // Maybe untracked object. Ignore.

    struct range_info *rng = entry->rng;
    if (!rng) return false;

    for (int goff = 0; goff < rng->len / GRANULE_SIZE; goff++) {
        void *atag = rng->base + (goff * GRANULE_SIZE);
        bool destroy_res = __ph_destroy_rngmap_entry(__ph_rngmap, atag);
        if (!destroy_res) return false;
    }

    return true;
}

static void *__ph_ceil_to_granule_ptr(void *x) {
    return (void *)(((intptr_t)x + (GRANULE_SIZE - 1)) & ~(intptr_t)(GRANULE_SIZE - 1));
}

static size_t __ph_ceil_to_granule_size(size_t x) {
    return (x + (GRANULE_SIZE - 1)) & ~(size_t)(GRANULE_SIZE - 1);
}

static size_t __ph_extend_granule_alignable(size_t size) {
    return (GRANULE_SIZE - 1)               // Object alignment slack
        + __ph_ceil_to_granule_size(size)   // Object itself with paddings
        + sizeof(struct range_info);        // Range information
}

static void *__ph_alloc_post(void *aobj, size_t asize, struct allocator ator) {
    assert(aobj == __ph_ceil_to_granule_ptr(aobj));
    assert(asize == __ph_ceil_to_granule_size(asize));

    struct range_info *rng = (struct range_info *)(aobj + asize);
    rng->base = aobj;
    rng->len = asize;

    bool init_res = __ph_init_rngmap_entries(aobj, asize, ator);
    if (!init_res) {
        if (raw_dtor) raw_dtor(aobj);
        return NULL;
    }

    return aobj;
}

/** Wrappers and instrumented functions **/

static void *__ph_malloc(size_t size, malloc_t raw_ator, free_t raw_dtor) {
    static const struct allocator ator = { 
        .type = ATOR_MALLOC, 
        .ator = (void *)raw_ator, 
        .dtor = (void *)raw_dtor 
    };

    size_t alloc_size = __ph_extend_granule_alignable(size);
    if (alloc_size < size) return NULL;
    void *obj = raw_ator(alloc_size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(size);

    return __ph_alloc_post(aobj, asize, ator);
}

static void *__ph_calloc(size_t num, size_t size, calloc_t raw_ator, free_t raw_dtor) {
    static const struct allocator ator = {
        .type = ATOR_CALLOC,
        .ator = (void *)raw_ator,
        .dtor = (void *)raw_dtor
    };

    size_t byte_size = num * size;
    if (byte_size < size) return NULL;
    size_t aligned_byte_size = __ph_extend_granule_alignable(byte_size);
    if (aligned_byte_size < byte_size) return NULL;
    size_t anum = (aligned_byte_size + size - 1) / size;
    void *obj = raw_ator(anum, size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(byte_size);

    return __ph_alloc_post(aobj, asize, ator);
}

static void *__ph_aalloc(size_t align, size_t size, aalloc_t raw_ator, free_t raw_dtor) {
    static const struct allocator ator = {
        .type = ATOR_AALLOC,
        .ator = (void *)raw_ator,
        .dtor = (void *)raw_dtor
    };

    size_t alloc_size = __ph_extend_granule_alignable(size);
    if (alloc_size < size) return NULL;
    size_t aligned_alloc_size = (alloc_size + align - 1) & ~((size_t)(align - 1));
    void *obj = raw_ator(align, aligned_alloc_size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(size);

    return __ph_alloc_post(aobj, asize, ator);
}

static void *__ph_realloc(void *ptr, size_t size, realloc_t raw_ator, free_t raw_dtor) {
    static const struct allocator ator = {
        .type = ATOR_realloc,
        .ator = (void *)raw_ator,
        .dtor = (void *)raw_dtor
    };

    bool destroy_res = __ph_destroy_rngmap_entries(ptr);
    if (!destroy_res) return NULL;

    size_t alloc_size = __ph_extend_granule_alignable(size);
    if (alloc_size < size) return NULL;
    void *obj = raw_ator(ptr, alloc_size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(size);

    if (aobj != obj)
        memmove(aobj, obj, size);

    return __ph_alloc_post(aobj, asize, ator);
}

static void __ph_free(void *ptr, free_t raw_dtor) {
    void *aptr = __ph_floor_to_granule_ptr(ptr);

    bool destroy_res = __ph_destroy_rngmap_entries(aptr);
    if (!destroy_res) return;

    raw_dtor(ptr);  // TODO: implement quarantine.
}

static void __ph_ptr_move(void *prev, void* next) {
    // TODO: check boundary.
    // TODO: if failed, create oob.
}

static void __ph_ptr_deref(void *ptr) {
    // TODO: check oob entry.
}
