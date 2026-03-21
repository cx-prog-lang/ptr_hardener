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

#include <stdbool.h>
#include <stdlib.h>

enum rngmap_entry_type {
    RNGMAP_ENTRY_NULL = 0,
    RNGMAP_ENTRY_INB,
    RNGMAP_ENTRY_OOB,
    RNGMAP_ENTRY_TABLE,
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

typedef void *(*malloc_t)(size_t);
typedef void *(*calloc_t)(size_t, size_t);
typedef void *(*aalloc_t)(size_t, size_t);
typedef void *(*realloc_t)(void *, size_t);
typedef void (*free_t)(void *);

// Thanks to __attribute__((weak)), all __ph_rngmap's in different translation
// units would share the same storage space.
void *__ph_rngmap __attribute__((weak));

#define GRANULE_SIZE_LOG2   (3)
#define GRANULE_SIZE        (1 << GRANULE_SIZE_LOG2)

#define RNGMAP_NR_ENTRIES_LOG2  (16)
#define RNGMAP_NR_ENTRIES       (1 << RNGMAP_NR_ENTRIES_LOG2)

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

static bool __ph_init_rngmap_entries(void *aobj, size_t asize, struct allocator ator) {
    if (!__ph_rngmap) 
        __ph_rngmap = __ph_create_rngmap(RNGMAP_NR_ENTRIES, ator);

    // TODO
}

static bool __ph_destroy_rngmap_entries(void *obj) {
    // TODO
}

static size_t __ph_extend_granule_alignable(size_t size) {
    return (GRANULE_SIZE - 1)       // Object alignment slack
        + size                      // Object itself 
        + (GRANULE_SIZE - 1)        // Range information slack
        + sizeof(void *)            // Range information: base address 
        + sizeof(size_t);           // Range information: length in bytes
}

static void *__ph_ceil_to_granule_ptr(void *x) {
    return (void *)((intptr_t)x + (GRANULE_SIZE - 1)) & ~((intptr_t)(GRANULE_SIZE - 1));
}

static size_t __ph_ceil_to_granule_size(size_t x) {
    return (x + (GRANULE_SIZE - 1)) & ~((size_t)(GRANULE_SIZE - 1));
}

static void *__ph_alloc_post(void *aobj, size_t size, struct allocator ator) {
    size_t asize = __ph_ceil_to_granule_size(size);

    void **base = (void **)(aobj + asize);
    *base = aobj;

    size_t *len = (size_t *)(aobj + asize + sizeof(void *));
    *len = asize;

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

    size_t asize = __ph_extend_granule_alignable(size);
    if (asize < size) return NULL;
    void *obj = raw_ator(asize);
    if (!obj) return NULL;
    void *aobj = __ph_ceil_to_granule_ptr(obj);

    return __ph_alloc_post(aobj, size, ator);
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

    return __ph_alloc_post(aobj, num * size, ator);
}

static void *__ph_aalloc(size_t align, size_t size, aalloc_t raw_ator, free_t raw_dtor) {
    static const struct allocator ator = {
        .type = ATOR_AALLOC,
        .ator = (void *)raw_ator,
        .dtor = (void *)raw_dtor
    };

    size_t asize = __ph_extend_granule_alignable(size);
    if (asize < size) return NULL;
    size_t aasize = (asize + align - 1) & ~((size_t)(align - 1));
    void *obj = raw_ator(align, aasize);
    if (!obj) return NULL;
    void *aobj = __ph_ceil_to_granule_ptr(obj);

    return __ph_alloc_post(aobj, size, ator);
}

static void *__ph_realloc(void *ptr, size_t size, realloc_t raw_ator, free_t raw_dtor) {
    static const struct allocator ator = {
        .type = ATOR_realloc,
        .ator = (void *)raw_ator,
        .dtor = (void *)raw_dtor
    };

    bool destroy_res = __ph_destroy_rngmap_entries(ptr);
    if (!destroy_res) return NULL;

    size_t asize = __ph_extend_granule_alignable(size);
    if (asize < size) return NULL;
    void *obj = raw_ator(ptr, asize);
    if (!obj) return NULL;
    void *aobj = __ph_ceil_to_granule_ptr(obj);

    if (aobj != obj)
        memmove(aobj, obj, size);

    return __ph_alloc_post(aobj, size, ator);
}

static void __ph_free(void *ptr, free_t raw_dtor) {
    bool destroy_res = __ph_destroy_rngmap_entries(ptr);
    if (!destroy_res) return;

    raw_dtor(ptr);  // TODO: implement quarantine.
}
