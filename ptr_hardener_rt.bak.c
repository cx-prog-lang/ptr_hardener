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

// FIXME: reimplement this in a portable way.
#define STACK_MASK  (0x700000000000)

// Largely, the entry type is divided into 3 classes: NULL, MAP, and BND.
// NULL represents "no entry", MAP represents a nested range map, and BND
// represents a bound information entry. BND is again broken down into 2
// sub-classes: OBJ and PTR. OBJ is the **inbound** information of memory
// objects. PTR is the **out-of-bounds** information of pointers.
enum rngmap_entry_type {
    RNGMAP_ENTRY_NULL = 0,  // null type
    RNGMAP_ENTRY_MAP,       // range map type
    RNGMAP_ENTRY_OBJT,      // object inbound type (tracked objects)
    RNGMAP_ENTRY_OBJU,      // object inbound type (untracked objects)
    RNGMAP_ENTRY_PTR,       // pointer out-of-bounds type (ANYTHING >= THIS VALUE)
};
#define IS_RNGMAP_ENTRY_NULL(x) ((x)->type == RNGMAP_ENTRY_NULL)
#define IS_RNGMAP_ENTRY_MAP(x)  ((x)->type == RNGMAP_ENTRY_MAP)
#define IS_RNGMAP_ENTRY_BND(x)  ((x)->type > RNGMAP_ENTRY_MAP)
#define IS_RNGMAP_ENTRY_OBJ(x)  ((x)->type > RNGMAP_ENTRY_MAP && (x)->type < RNGMAP_ENTRY_PTR)
#define IS_RNGMAP_ENTRY_PTR(x)  ((x)->type >= RNGMAP_ENTRY_PTR)

struct range_info {
    void *base;     // Granule-aligned base address
    size_t len;     // Granule-aligned length (in bytes)
};

struct rngmap_entry {
    union {
        uintptr_t type_or_ptr;
        uintptr_t type;     // For 'enum rngmap_entry_type'. Beyond it,
        void *ptr;          // this will hold the OOB pointer's address.
    }; 
    void *obj;
    struct range_info *rng;
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

static void __ph_print_rngmap_inner(struct rngmap_entry *rngmap, unsigned base_lv, unsigned lv, bool stop, size_t n_entries, bool detailed) {
    assert(rngmap);

    const char *type_str[] = { "NULL", "MAP", "OBJT", "OBJU" };

    __ph_printf("Range map %d entries (level: %d, addr: %p)\n", n_entries, base_lv + lv, rngmap);

    for (int i = 0; i < n_entries; i++) {
        __ph_printf("┌");
        for (int c = 0; c < 24; c++) __ph_printf("─");
        __ph_printf("┐");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        if (!IS_RNGMAP_ENTRY_PTR(&rngmap[i]))
            __ph_printf("type: %18s", type_str[rngmap[i].type]);
        else
            __ph_printf("type: PTR %14p", rngmap[i].ptr);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("obj : %18p", rngmap[i].obj);
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
            if (IS_RNGMAP_ENTRY_BND(&rngmap[i]))
                __ph_printf("-bas: %18p", ((struct range_info *)rngmap[i].rng)->base);
            else
                for (int c = 0; c < 24; c++) __ph_printf(" ");
            __ph_printf("│");
        }
        __ph_printf("\n");
        for (int i = 0; i < n_entries; i++) {
            __ph_printf("│");
            if (IS_RNGMAP_ENTRY_BND(&rngmap[i]))
                __ph_printf("-len: %18d", ((struct range_info *)rngmap[i].rng)->len);
            else
                for (int c = 0; c < 24; c++) __ph_printf(" ");
            __ph_printf("│");
        }
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
                __ph_print_rngmap_inner(rngmap[i].obj, base_lv, lv + 1, stop, RNGMAP_NR_ENTRIES, detailed);
        }
    }
}

static void __ph_print_rngmap() {
    if (!__ph_rngmap) {
        __ph_printf("(no range map)\n");
        return;
    }

    __ph_print_rngmap_inner(__ph_rngmap, 0, 0, true, RNGMAP_NR_ENTRIES, false);
}

static void __ph_print_rngmap_entry(unsigned base_lv, struct rngmap_entry *entry) {
    if (!entry) {
        __ph_printf("(no range map entry)\n");
        return;
    }

    __ph_print_rngmap_inner(entry, base_lv, 0, true, 1, true);
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

// FIXME: optimize this.
static rngmap_index_t __ph_hash_addr(void *addr, unsigned seed) {
    srand(((uintptr_t)addr + seed) % (uintptr_t)UINT_MAX);
    rngmap_index_t hash = rand();
    hash %= RNGMAP_NR_ENTRIES;
    return hash;
}

/*
// FIXME: optimize this.
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
*/

static void __ph_set_rngmap_entry_null(struct rngmap_entry *entry) {
    // entry->type = RNGMAP_ENTRY_NULL; entry->obj = entry->rng = NULL;
    memset(entry, 0, sizeof(struct rngmap_entry));
}

static void __ph_set_rngmap_entry_bnd(struct rngmap_entry *entry, uintptr_t type_or_ptr, void *obj, struct range_info *rng) {
    entry->type_or_ptr = type_or_ptr;
    entry->obj = obj;
    entry->rng = rng;
}

static void __ph_set_rngmap_entry_map(struct rngmap_entry *entry, struct rngmap_entry *rngmap) {
    entry->type = RNGMAP_ENTRY_MAP;
    entry->obj = rngmap;
    entry->rng = NULL;
}

/** Internal functions **/

static struct rngmap_entry *__ph_create_rngmap_entry_inner(struct rngmap_entry *rngmap, struct rngmap_entry evalue, unsigned lv) {
    assert(IS_RNGMAP_ENTRY_BND(&evalue));

    if (lv == UINT_MAX) return NULL;

    rngmap_index_t idx;
    if (IS_RNGMAP_ENTRY_OBJ(&evalue))
        idx = __ph_hash_addr(evalue.obj, lv);
    else if (IS_RNGMAP_ENTRY_PTR(&evalue))
        idx = __ph_hash_addr(evalue.ptr, lv);
    else
        assert(false);

    struct rngmap_entry *entry = &rngmap[idx];
    assert(entry);

    if (IS_RNGMAP_ENTRY_NULL(entry) ||
        (IS_RNGMAP_ENTRY_PTR(entry) && entry->ptr == evalue.ptr)) {
        __ph_set_rngmap_entry_bnd(entry, evalue.type_or_ptr, evalue.obj, evalue.rng);
        __ph_print_rngmap_entry(lv, entry);
        return entry;
    } else if (IS_RNGMAP_ENTRY_MAP(entry)) {
        return __ph_create_rngmap_entry_inner(entry->obj, evalue, lv + 1);
    } else /* BND entry */ {
        assert(!IS_RNGMAP_ENTRY_OBJ(entry) || entry->obj != evalue.obj);

        struct rngmap_entry prev_evalue = *entry;
        void *new_rngmap = __ph_create_rngmap(RNGMAP_NR_ENTRIES);
        if (!new_rngmap) return NULL;

        __ph_set_rngmap_entry_map(entry, new_rngmap);

        struct rngmap_entry *entry1 = __ph_create_rngmap_entry_inner(new_rngmap, prev_evalue, lv + 1);
        if (!entry1) return NULL;

        struct rngmap_entry *entry2 = __ph_create_rngmap_entry_inner(new_rngmap, evalue, lv + 1);
        if (!entry2) return NULL;

        __ph_print_rngmap_entry(lv, entry);

        return entry2;
    }
}

static struct rngmap_entry *__ph_create_rngmap_obj_entry(enum rngmap_entry_type type, void *obj, void *rng) {
    if (!__ph_rngmap) return NULL;
    struct rngmap_entry evalue = { .type = type, .obj = obj, .rng = rng };
    assert(IS_RNGMAP_ENTRY_OBJ(&evalue));
    return __ph_create_rngmap_entry_inner(__ph_rngmap, evalue, 0);
}

static struct rngmap_entry *__ph_create_rngmap_ptr_entry(void *ptr, void *obj, void *rng) {
    if (!__ph_rngmap) return NULL;
    struct rngmap_entry evalue = { .ptr = ptr, .obj = obj, .rng = rng };
    assert(IS_RNGMAP_ENTRY_PTR(&evalue));
    return __ph_create_rngmap_entry_inner(__ph_rngmap, evalue, 0);
}

static bool __ph_create_rngmap_obj_entries(enum rngmap_entry_type type, void *aobj, size_t size) {
    assert((intptr_t)aobj % GRANULE_SIZE == 0);
    assert(IS_RNGMAP_ENTRY_OBJ(&(struct rngmap_entry){ .type = type }));

    if (!__ph_rngmap) 
        __ph_rngmap = __ph_create_rngmap(RNGMAP_NR_ENTRIES);

    for (int goff = 0; goff <= size / GRANULE_SIZE; goff++) {
        void *obj = aobj + (goff * GRANULE_SIZE);
        void *rng = aobj + size;
        void *create_res = __ph_create_rngmap_obj_entry(type, obj, rng);
        if (!create_res) return false;
    }
    
    return true;
}

static struct rngmap_entry *__ph_get_rngmap_obj_entry_inner(struct rngmap_entry *rngmap, void *obj, unsigned lv) {
    if (!rngmap) return NULL;
    struct rngmap_entry *entry = &rngmap[__ph_hash_addr(obj, lv)];

    if (IS_RNGMAP_ENTRY_MAP(entry)) {
        return __ph_get_rngmap_obj_entry_inner(entry->obj, obj, lv + 1);
    } else if (IS_RNGMAP_ENTRY_OBJ(entry)) {
        if (entry->obj == obj) return entry;
        else return NULL;
    } else {
        return NULL;
    }
}

static struct rngmap_entry *__ph_get_rngmap_obj_entry(void *obj) {
    if (!__ph_rngmap) return NULL;
    return __ph_get_rngmap_obj_entry_inner(__ph_rngmap, obj, 0);
}

static struct rngmap_entry *__ph_get_rngmap_ptr_entry_inner(struct rngmap_entry *rngmap, void *ptr, unsigned lv) {
    if (!rngmap) return NULL;
    struct rngmap_entry *entry = &rngmap[__ph_hash_addr(ptr, lv)];

    if (IS_RNGMAP_ENTRY_MAP(entry)) {
        return __ph_get_rngmap_ptr_entry_inner(entry->obj, ptr, lv + 1);
    } else if (IS_RNGMAP_ENTRY_PTR(entry)) {
        if (entry->ptr == ptr) return entry;
        else return NULL;
    } else {
        return NULL;
    }
}

static struct rngmap_entry *__ph_get_rngmap_ptr_entry(void *ptr) {
    if (!__ph_rngmap) return NULL;
    return __ph_get_rngmap_ptr_entry_inner(__ph_rngmap, ptr, 0);
}

static bool __ph_destroy_rngmap_ptr_entry(void *ptr) {
    struct rngmap_entry *entry = __ph_get_rngmap_ptr_entry(ptr);
    if (!entry) return false;
    __ph_set_rngmap_entry_null(entry);
    return true;
}

// ptr: the beginning of a potential pointer address.
// stride: desired increment limit to 'ptr'.
static bool __ph_destroy_rngmap_ptr_entries(void *ptr, size_t stride) {
    // This is when the thing is getting tricky because we should potentially
    // destroy any entry that **may** correspond to a pointer in the deallocated
    // object. As a naive implementation, we simply scan the object and
    // see if there is an entry for the hypothetical pointer.
    for (int off = 0; off <= stride; off++) {
        void *maybe_ptr = (void *)((char *)ptr + off);
        if (__ph_destroy_rngmap_ptr_entry(maybe_ptr))
            off += sizeof(void *) - 1;  // A pity attempt to optimize scanning.
    }

    return true;
}

static bool __ph_destroy_rngmap_obj_entry(void *obj) {
    struct rngmap_entry *entry = __ph_get_rngmap_obj_entry(obj);
    if (!entry) return false;
    __ph_set_rngmap_entry_null(entry);
    return true;
}

static bool __ph_destroy_rngmap_obj_entries(void *aobj) {
    assert((intptr_t)aobj % GRANULE_SIZE == 0);

    // Get the range of the current 'aobj'.
    struct rngmap_entry *entry = __ph_get_rngmap_obj_entry(aobj);
    if (!entry) return true;    // Maybe untracked object. Ignore.
    assert(entry->rng);
    struct range_info rng = *entry->rng;

    // Destroy all associated range map entries with 'aobj'.
    for (int goff = 0; goff < rng.len / GRANULE_SIZE; goff++) {
        void *aobj = rng.base + (goff * GRANULE_SIZE);
        bool destroy_res = __ph_destroy_rngmap_obj_entry(aobj);
        if (!destroy_res) return false;
    }

    // Destroy all entries for the pointers in this object.
    if (rng.len >= sizeof(void *)) { 
        size_t stride = rng.len - sizeof(void *);
        return __ph_destroy_rngmap_ptr_entries(rng.base, rng.len);
    } else {
        return true;
    }
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

    bool init_res = __ph_create_rngmap_obj_entries(RNGMAP_ENTRY_OBJT, aobj, size);
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

    bool destroy_res = __ph_destroy_rngmap_obj_entries(ptr);
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

    bool destroy_res = __ph_destroy_rngmap_obj_entries(aptr);
    if (!destroy_res) return;

    free_impl(ptr);  // TODO: implement quarantine.

    __ph_print_rngmap();
}

__attribute__((weak))
void free_sized(void *ptr) {
    if (!sfree_impl) {
        sfree_impl = dlsym(RTLD_NEXT, "free_sized");
        if (!sfree_impl) return;
    }

    void *aptr = __ph_floor_to_granule_ptr(ptr);

    bool destroy_res = __ph_destroy_rngmap_obj_entries(aptr);
    if (!destroy_res) return;

    sfree_impl(ptr);  // TODO: implement quarantine.

    __ph_print_rngmap();
}

__attribute__((weak))
void free_aligned_sized(void *ptr) {
    if (!asfree_impl) {
        asfree_impl = dlsym(RTLD_NEXT, "free_aligned_sized");
        if (!asfree_impl) return;
    }

    void *aptr = __ph_floor_to_granule_ptr(ptr);

    bool destroy_res = __ph_destroy_rngmap_obj_entries(aptr);
    if (!destroy_res) return;

    asfree_impl(ptr);  // TODO: implement quarantine.

    __ph_print_rngmap();
}

/** Instrumented functions **/

// TODO: obj map (inbound), ptr map (oob)
// TODO: release ptr entries when ptr is released. (heap: free, stack: return) [stack ptr: ptr type or address-taken]
// TODO: stack blob range
// TODO: untracked pointer tolerance
// TODO: untracked entry --> overwritten by tracked entry creation
// TODO: UAF detection: memset and mprotect when free.
// TODO: UAF countermeasure: don't "free" deallocated objects. Always reuse them once it's been allocated.

static void __ph_ptr_move(void *ptr, void *prev, size_t psize, void* next, size_t nsize) {
    __ph_printf("__ph_ptr_move(%p, %p, %d, %p, %d)\n", ptr, prev, psize, next, nsize);
    bool is_oob = false;

    void *aprev = __ph_floor_to_granule_ptr(prev);
    struct rngmap_entry *entry = NULL;
    struct range_info *rng = NULL;

    // For stack pointers, only check if 'next' is still inside the stack.
    // FIXME: reimplement this in a portable way.
    if (((intptr_t)prev & STACK_MASK) == STACK_MASK) {
        if ((((intptr_t)next + nsize - 1) & STACK_MASK) != STACK_MASK) {
            is_oob = true;

            rng = malloc_impl(sizeof(struct range_info));
            rng->base = prev;
            rng->len = 0;
        }
    } else {
        entry = __ph_get_rngmap_obj_entry(aprev);

        if (!entry || entry->type == RNGMAP_ENTRY_OBJU) {
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
    }

    // If 'is_oob', create an 'oob' range map entry.
    if (is_oob) {
        // If the pointer itself doesn't have a storage, raise a signal immediately.
        if (ptr == NULL) raise(SIGUSR1);

        struct rngmap_entry *oob_entry = 
            __ph_create_rngmap_ptr_entry(ptr, next, (void *)rng);
        if (!oob_entry) return;
    } 

    __ph_print_rngmap();
}

static void __ph_ptr_deref(void *ptr) {
    __ph_printf("__ph_ptr_deref(%p)\n", ptr);

    void *obj = *(void **)ptr;
    struct rngmap_entry *entry = __ph_get_rngmap_ptr_entry(ptr);
    if (entry && entry->obj == obj) raise(SIGUSR1);
}
