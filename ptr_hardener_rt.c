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

#include <stdarg.h>     // NOTE: debugging
#include <unistd.h>     // NOTE: debugging

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
#define RNGMAP_NR_ENTRIES   (1 << 3)        // NOTE: originally 1<<16.

typedef uint64_t rngmap_index_t;

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

/** Debug functions **/

// Based on https://stackoverflow.com/questions/1735236/how-to-write-my-own-printf-in-c
static char *__ph_printf_convert(uint64_t num, int base, int *len) { 
    static char repr[]= "0123456789abcdef";
    static char buffer[50]; 
    char *ptr; 
    int _len = 0;

    ptr = &buffer[49]; 
    *ptr = '\0'; 

    do { 
        *--ptr = repr[num % base]; 
        num /= base; 
        _len++;
    } while(num != 0); 

    *len = _len;
    return ptr; 
}

// Based on https://stackoverflow.com/questions/1735236/how-to-write-my-own-printf-in-c
static void __ph_printf(char* format, ...) {
    char *traverse;
    uint64_t i;
    char *s;
    int len;

    va_list arg;
    va_start(arg, format);

    for (traverse = format; *traverse != '\0'; traverse++) {
        while (*traverse != '%') {
            if( *traverse == '\0') return;
            write(STDOUT_FILENO, traverse, 1);
            traverse++;
        }
        traverse++;

        int pad = 0;
        while (*traverse >= '0' && *traverse <= '9') {
            pad = (pad * 10) + (*traverse - '0');
            traverse++;
        }

        len = 0;
        switch (*traverse) {
            case 'c': 
                i = va_arg(arg, int);
                write(STDOUT_FILENO, &i, 1);
                len++;
                break;
            case 'd':
                i = va_arg(arg, int);
                if (i < 0) {
                    i = -i;
                    write(STDOUT_FILENO, "-", 1);
                    len++;
                }
                int dsize;
                char *dconv = __ph_printf_convert(i, 10, &dsize);
                write(STDOUT_FILENO, dconv, dsize);
                len += dsize;
                break;
            case 's':
                len = 0;
                s = va_arg(arg, char *);
                for (; *s != '\0'; s++) {
                    write(STDOUT_FILENO, s, 1);
                    len++;
                }
                break;
            case 'x': 
                i = va_arg(arg, unsigned int);
                int xsize;
                char *xconv = __ph_printf_convert(i, 16, &xsize);
                write(STDOUT_FILENO, xconv, xsize);
                len += xsize;
                break;
            case 'p': 
                i = va_arg(arg, uintptr_t);
                int psize;
                char *pconv = __ph_printf_convert(i, 16, &psize);
                write(STDOUT_FILENO, "0x", 2);
                write(STDOUT_FILENO, pconv, psize);
                len += psize + 2;
                break;
        }
        while (len < pad) {
            write(STDOUT_FILENO, " ", 1);
            len++;
        }
    }

    va_end(arg);
}

static void __ph_print_rngmap_inner(void *_rngmap, unsigned lv, bool stop, size_t n_entries, bool detailed) {
    assert(_rngmap);

    const char *type_str[] = { "NULL", "INB", "OOB", "MAP" };
    struct rngmap_entry *rngmap = (struct rngmap_entry *)_rngmap;

    __ph_printf("Range map %d entries (lv: %d, addr: %p)\n", n_entries, lv, rngmap);

    for (int i = 0; i < n_entries; i++) {
        __ph_printf("┌");
        for (int c = 0; c < 24; c++) __ph_printf("─");
        __ph_printf("┐");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("type: %18s", type_str[rngmap[i].type]);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("tag : %18p", rngmap[i].tag);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("rng : %18p", rngmap[i].rng);
        __ph_printf("│");
    }
    if (detailed) {
        __ph_printf("\n");
        for (int i = 0; i < n_entries; i++) {
            __ph_printf("│");
            if (rngmap[i].type == RNGMAP_ENTRY_INB || rngmap[i].type == RNGMAP_ENTRY_OOB)
                __ph_printf("-bas: %18p", ((struct range_info *)rngmap[i].rng)->base);
            else
                for (int c = 0; c < 24; c++) __ph_printf(" ");
            __ph_printf("│");
        }
        __ph_printf("\n");
        for (int i = 0; i < n_entries; i++) {
            __ph_printf("│");
            if (rngmap[i].type == RNGMAP_ENTRY_INB || rngmap[i].type == RNGMAP_ENTRY_OOB)
                __ph_printf("-len: %18d", ((struct range_info *)rngmap[i].rng)->len);
            else
                for (int c = 0; c < 24; c++) __ph_printf(" ");
            __ph_printf("│");
        }
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("oob : %18p", rngmap[i].oob);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("└");
        for (int c = 0; c < 24; c++) __ph_printf("─");
        __ph_printf("┘");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf(" ");
        __ph_printf("%24d", i);
        __ph_printf(" ");
    }
    __ph_printf("\n");
    __ph_printf("\n");

    if (!stop) {
        for (int i = 0; i < n_entries; i++) {
            if (rngmap[i].type == RNGMAP_ENTRY_MAP)
                __ph_print_rngmap_inner(rngmap[i].rng, lv + 1, stop, RNGMAP_NR_ENTRIES, detailed);
        }
    }
}

static void __ph_print_rngmap() {
    if (!__ph_rngmap) {
        __ph_printf("(no range map)\n");
        return;
    }

    __ph_print_rngmap_inner(__ph_rngmap, 0, true, RNGMAP_NR_ENTRIES, false);
}

static void __ph_print_rngmap_entry(struct rngmap_entry *entry) {
    if (!entry) {
        __ph_printf("(no range map entry)\n");
        return;
    }

    __ph_print_rngmap_inner(entry, 0, true, 1, true);
}

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

    const uint64_t n_entries_m1 = RNGMAP_NR_ENTRIES - 1;
    size_t idx_unit_size = 1;
    for (; idx_unit_size < sizeof(rngmap_index_t); idx_unit_size++) {
        if (n_entries_m1 == (n_entries_m1 & ~(~(uint64_t)0 << (idx_unit_size * 8))))
            break;
    }

    rngmap_index_t ret = 0;

    int off = 0;
    for (; off + idx_unit_size - 1 < sizeof(uintptr_t); off += idx_unit_size) 
        ret ^= *(rngmap_index_t *)(_addr_w_seed + off);
    for (int i = 0; off < sizeof(uintptr_t); i++, off++)
        ((char *)&ret)[i] ^= *(char *)(_addr_w_seed + off);

    const uint64_t n_entries = RNGMAP_NR_ENTRIES;
    if (n_entries < (1 << (idx_unit_size * 8)))
        ret %= n_entries;

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
            __ph_print_rngmap_entry(entry);
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

            __ph_print_rngmap_entry(entry);

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

    for (int goff = 0; goff <= size / GRANULE_SIZE; goff++) {
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
    __ph_printf("malloc(%d)\n", size);

    size_t aligned_size = __ph_extend_w_range_info_granule_alignable(size);
    if (aligned_size < size) return NULL;
    void *obj = malloc_impl(aligned_size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    if (size == 1024) return aobj;      // NOTE: God awful stopgap (from libc) 

    void *ret = __ph_alloc_post(aobj, size);

    __ph_print_rngmap();

    return ret;
}

__attribute__((weak))
void *calloc(size_t num, size_t esize) {
    if (!calloc_impl) {
        calloc_impl = dlsym(RTLD_NEXT, "calloc");
        if (!calloc_impl) return NULL;
    }
    __ph_printf("calloc(%d, %d)\n", num, esize);

    size_t size = num * esize;
    if (size < esize) return NULL;
    size_t aligned_size = __ph_extend_w_range_info_granule_alignable(size);
    if (aligned_size < size) return NULL;
    size_t aligned_num = (aligned_size + esize - 1) / esize;
    void *obj = calloc_impl(aligned_num, esize);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(size);
    void *ret = __ph_alloc_post(aobj, asize);

    __ph_print_rngmap();

    return ret;
}

__attribute__((weak))
void *aligned_alloc(size_t align, size_t size) {
    if (!aalloc_impl) {
        aalloc_impl = dlsym(RTLD_NEXT, "aligned_alloc");
        if (!aalloc_impl) return NULL;
    }
    __ph_printf("aligned_alloc(%d, %d)\n", align, size);

    size_t aligned_size = __ph_extend_w_range_info_granule_alignable(size);
    if (aligned_size < size) return NULL;
    size_t aligned_aligned_size = (aligned_size + align - 1) & ~((size_t)(align - 1));
    void *obj = aalloc_impl(align, aligned_aligned_size);
    if (!obj) return NULL;

    void *aobj = __ph_ceil_to_granule_ptr(obj);
    size_t asize = __ph_ceil_to_granule_size(size);
    void *ret = __ph_alloc_post(aobj, asize);

    __ph_print_rngmap();

    return ret;
}

__attribute__((weak))
void *realloc(void *ptr, size_t size) {
    if (!realloc_impl) {
        realloc_impl = dlsym(RTLD_NEXT, "realloc");
        if (!realloc_impl) return NULL;
    }
    __ph_printf("realloc(%p, %d)\n", ptr, size);

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

    void *ret = __ph_alloc_post(aobj, asize);

    __ph_print_rngmap();

    return ret;
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

static void __ph_ptr_move(void *prev, size_t psize, void* next, size_t nsize) {
    __ph_printf("__ph_ptr_move(%p, %p, %d)\n", prev, next, nsize);
    bool is_oob = false;

    void *aprev = __ph_floor_to_granule_ptr(prev);
    struct rngmap_entry *entry = __ph_get_rngmap_bnd_entry(aprev);
    struct range_info *rng;

    if (!entry) {
        // Moving or casting an untracked pointer is a straight-up oob.
        if (prev != next || psize != nsize) {
            is_oob = true;
            
            rng = malloc_impl(sizeof(struct range_info));
            rng->base = prev;
            rng->len = 0;
        }
    } else {
        rng = entry->rng;
        assert(rng);

        // If 'next' is out of bounds, it's oob.
        is_oob = !(rng->base <= next && next + nsize <= rng->base + rng->len);
    }

    // If 'is_oob', create an 'oob' range map entry.
    if (is_oob) {
        struct rngmap_entry *oob_entry = __ph_create_rngmap_entry(RNGMAP_ENTRY_OOB, next, (void *)rng);
        if (!oob_entry) return;

        // Chain 'oob_entry' to this inbound entry.
        // FIXME: the 'oob_entry' from an untracked pointer will pile up uncleaned no matter what.
        // This will become a problem if some of them turn into **inbound** by future allocations.
        // IDEA: make a new range map entry type 'inb_untracked' to keep track of inbound untracked objects.
        // When a further 'legitimate' allocation make it 'inb', overwrite this entry and clear
        // associated 'oob' entries.
        if (entry) {
            while (entry->oob)
                entry = entry->oob;
            entry->oob = (void *)oob_entry;
        }
    } 

    __ph_print_rngmap();
}

static void __ph_ptr_deref(void *ptr) {
    __ph_printf("__ph_ptr_deref(%p)\n", ptr);

    struct rngmap_entry *entry = __ph_get_rngmap_bnd_entry(ptr);
    if (entry && entry->type == RNGMAP_ENTRY_OOB)
        raise(SIGUSR1);
}
