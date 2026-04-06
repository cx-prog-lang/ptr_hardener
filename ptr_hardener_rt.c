/*
 * ptr_hardner - instrumentation functions
 * ---------------------------------------
 *
 * Written by Gwangmu Lee <iss300@proton.me>
 *
 * Functions that will be inserted in place of
 * (de)allocallocators and pointer assignments
 * and dereferences.
 */

/*
 * Glossary:
 *  - objmap := Object Map
 *  - ptrmap := Pointer Map
 *  - Variables starting with a redundant 'a' := granule-aligned
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

/** Type/constant declarations **/

// Pointer map entry: represents a pointer's range information
// and the object asociated with this pointer. Some conventions here:
//  - tag == 0: null entry. (=no pointer is occupying this entry)
//  - next == -1 && prev == -1: map entry. ('base' = nested map)
//  - len == 0: invalid pointer. (=dereference banned)
//  - base == 0: "loose" pointer. (=can dereference anywhere)
// Non-null 'next' and 'prev' are assumed valid.
struct ptrmap_entry {
    void *base;                     // Range: base address.
    size_t len;                     // Range: length (in bytes).
    struct ptrmap_entry *next;      // Associated pointers: next ptr.
    struct ptrmap_entry *prev;      // Associated pointers: previous ptr.
    void *tag;                      // Hash tag: the address of this ptr.
};

#define __PH_PTRMAP_ENTRY_SET_NULL(entry)   ((entry)->tag = NULL)
#define __PH_PTRMAP_ENTRY_IS_NULL(entry)    ((entry)->tag == NULL)

#define __PH_PTRMAP_ENTRY_SET_MAP(entry, map) do {          \
    (entry)->next == (struct ptrmap_entry *)~(uintptr_t)0;  \
    (entry)->prev == (struct ptrmap_entry *)~(uintptr_t)0;  \
    (entry)->base == (void *)(map); } while(0)
#define __PH_PTRMAP_ENTRY_IS_MAP(entry)                         \
    ((entry)->next == (struct ptrmap_entry *)~(uintptr_t)0 &&   \
     (entry)->prev == (struct ptrmap_entry *)~(uintptr_t)0)
#define __PH_PTRMAP_ENTRY_GET_MAP(entry)        \
    (assert(__PH_PTRMAP_ENTRY_IS_MAP(entry)),   \
     (struct ptrmap_entry *)((entry)->base));

#define __PH_PTRMAP_ENTRY_IS_RANGE(entry) \
    (!__PH_PTRMAP_ENTRY_IS_NULL(entry) && !__PH_PTRMAP_ENTRY_IS_MAP(entry))

#define __PH_PTRMAP_ENTRY_SET_RANGE_LOOSE(entry)  ((entry)->base = NULL)
#define __PH_PTRMAP_ENTRY_IS_RANGE_LOOSE(entry)   ((entry)->base == NULL)

#define __PH_PTRMAP_ENTRY_SET_RANGE_INVAL(entry)  ((entry)->len = 0)
#define __PH_PTRMAP_ENTRY_IS_RANGE_INVAL(entry)   ((entry)->len == 0)

#define __PH_PTRMAP_NR_ENTRIES  (1 << 3)    // NOTE: reduced for debugging. Original "1 << 16".

typedef uint64_t ptrmap_index_t;

// Object map entry: represents an object's range information
// and the pointers associated with this object. Some conventions here:
//  - base == 0: null entry. (=no object is occupying this entry)
//  - base == 1: map entry. ('ptr' = nested map)
//  - base == -1: untracked object. (=can be overwritten by other allocs)
// 'len' is always taken at surface value. Non-null 'ptr' is assumed valid.
struct objmap_entry {
    void *base;                     // Range: base address. (also hash tag)
    size_t len;                     // Range: length (in bytes).
    struct ptrmap_entry *ptr;       // Associated pointers: first ptr.
};

#define __PH_OBJMAP_ENTRY_SET_NULL(entry)   ((entry)->base = NULL)
#define __PH_OBJMAP_ENTRY_IS_NULL(entry)    ((entry)->base == NULL)

#define __PH_OBJMAP_ENTRY_SET_MAP(entry)   ((entry)->base = (void *)1)
#define __PH_OBJMAP_ENTRY_IS_MAP(entry)    ((entry)->base == (void *)1)
#define __PH_OBJMAP_ENTRY_GET_MAP(entry)        \
    (assert(__PH_OBJMAP_ENTRY_GET_MAP(entry)),  \
     (struct objmap_entry *)((entry)->ptr))

#define __PH_OBJMAP_ENTRY_IS_RANGE(entry)   \
    (!__PH_OBJMAP_ENTRY_IS_NULL(entry) && !__PH_OBJMAP_ENTRY_IS_MAP(entry))

#define __PH_OBJMAP_ENTRY_SET_RANGE_UNTRA(entry)  ((entry)->base = (void *)~(uintptr_t)0)
#define __PH_OBJMAP_ENTRY_IS_RANGE_UNTRA(entry)   ((entry)->base == (void *)~(uintptr_t)0)

#define __PH_OBJMAP_NR_ENTRIES  (1 << 3)    // NOTE: reduced for debugging. Original "1 << 16".

typedef uint64_t objmap_index_t;

// Granule: the minimum unit of byte alignment. 
#define GRANULE_SIZE  (1 << 5)

#define __PH_GRANULE_CEIL(x)     \
    (((intptr_t)(x) + (GRANULE_SIZE - 1)) & ~(intptr_t)(GRANULE_SIZE - 1))

#define __PH_GRANULE_FLOOR(x)    \
    ((intptr_t)(x) & ~(intptr_t)(GRANULE_SIZE - 1))

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

/** Generai utilities **/

static char *__ph_printf_convert(uint64_t num, int base, int *len) { 
    // Based on https://stackoverflow.com/questions/1735236/how-to-write-my-own-printf-in-c
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

static void __ph_printf(char* format, ...) {
    // Based on https://stackoverflow.com/questions/1735236/how-to-write-my-own-printf-in-c
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

static rngmap_index_t __ph_hash_addr(void *addr, unsigned seed) {
    // FIXME: optimize this with a number sequence visible to this TU.
    srand(((uintptr_t)addr + seed) % (uintptr_t)UINT_MAX);
    rngmap_index_t hash = rand();
    hash %= RNGMAP_NR_ENTRIES;
    return hash;
}

/** Pointer map **/

// Global pointer map: the entry point of the pointer/object map. On a hash
// collision, the collided entries are moved to a nested map. Thanks to
// __attribute__((weak)), all global variables in different translation units
// would share the same storage space.
void *__ph_ptrmap __attribute__((weak));

static void __ph_ptrmap_print_inner(struct ptrmap_entry *ptrmap, unsigned base_lv, unsigned lv, bool stop, size_t n_entries) {
    assert(ptrmap);

    char *type = NULL;
    if (__PH_PTRMAP_ENTRY_IS_NULL(ptrmap))
        type = "NULL";
    else if (__PH_PTRMAP_ENTRY_IS_MAP(ptrmap))
        type = "MAP";
    else if (__PH_PTRMAP_ENTRY_IS_RANGE(ptrmap))
        type = "RANGE";
    else
        assert(false);

    __ph_printf("Pointer map %d entries (level: %d, addr: %p)\n", n_entries, base_lv + lv, ptrmap);

    for (int i = 0; i < n_entries; i++) {
        __ph_printf("┌");
        for (int c = 0; c < 24; c++) __ph_printf("─");
        __ph_printf("┐");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("type: %18s", type);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("base: %18p", ptrmap[i].base);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("len : %18d", ptrmap[i].len);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("next: %18p", ptrmap[i].next);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("prev: %18p", ptrmap[i].prev);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("tag : %18p", ptrmap[i].tag);
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
            if (__PH_PTRMAP_ENTRY_IS_MAP(ptrmap))
                __ph_ptrmap_print_inner(ptrmap[i].base, base_lv, lv + 1, stop, __PH_PTRMAP_NR_ENTRIES);
        }
    }
}

static void __ph_ptrmap_print() {
    if (!__ph_ptrmap) {
        __ph_printf("(no range map)\n");
        return;
    }

    __ph_ptrmap_print_inner(__ph_ptrmap, 0, 0, true, __PH_PTRMAP_NR_ENTRIES);
}

static void __ph_ptrmap_entry_print(unsigned base_lv, struct ptrmap_entry *entry) {
    if (!entry) {
        __ph_printf("(no range map entry)\n");
        return;
    }

    __ph_ptrmap_print_inner(entry, base_lv, 0, true, 1);
}

static bool __ph_ptrmap_entry_insert_to_list(struct ptrmap_entry *this, struct ptrmap_entry *prev) {
    assert(this && prev);
    this->next = prev->next;
    this->prev = prev;
    prev->next = this;
}

static bool __ph_ptrmap_entry_remove_from_list(struct ptrmap_entry *this) {
    assert(this);
    this->prev->next = this->next;
    this->next->prev = this->prev;
    this->prev = this->next = NULL;
}

static void *__ph_ptrmap_create(unsigned n_entries) {
    assert(malloc_impl);
    unsigned size = n_entries * sizeof(struct ptrmap_entry);
    void *ret = malloc_impl(size);
    if (!ret) return 0;
    memset(ret, 0, size);
    return ret;
}

static struct ptrmap_entry *__ph_ptrmap_entry_create_inner(struct ptrmap_entry *ptrmap, struct ptrmap_entry evalue, unsigned lv) {
    assert(!__PH_PTRMAP_ENTRY_IS_NULL(&evalue));

    if (lv == UINT_MAX) return NULL;

    ptrmap_index_t idx = __ph_hash_addr(evalue.tag, lv);
    assert(0 <= idx && idx < __PH_PTRMAP_NR_ENTRIES);
    struct ptrmap_entry *entry = &ptrmap[idx];

    if (__PH_PTRMAP_ENTRY_IS_NULL(entry) || 
        (__PH_PTRMAP_ENTRY_IS_RANGE(entry) && entry->tag == evalue.tag)) {
        __PH_PTRMAP_ENTRY_SET_RANGE(entry, evalue.tag, evalue.base, evalue.len);
        __ph_ptrmap_print_entry(lv, entry);
        return entry;
    } else if (__PH_PTRMAP_ENTRY_IS_MAP(entry)) {
        return __ph_ptrmap_entry_create_inner(entry->tag, evalue, lv + 1);
    } else /* range entry */ {
        assert(__PH_PTRMAP_ENTRY_IS_RANGE(entry) && entry->tag != evalue.tag);

        struct ptrmap_entry prev_evalue = *entry;
        void *new_ptrmap = __ph_create_ptrmap(__PH_PTRMAP_NR_ENTRIES);
        if (!new_ptrmap) return NULL;

        __PH_PTRMAP_ENTRY_SET_MAP(entry, new_ptrmap);

        struct ptrmap_entry *entry1 = __ph_ptrmap_entry_create_inner(new_ptrmap, prev_evalue, lv + 1);
        if (!entry1) return NULL;

        struct ptrmap_entry *entry2 = __ph_ptrmap_entry_create_inner(new_ptrmap, evalue, lv + 1);
        if (!entry2) return NULL;

        __ph_ptrmap_print_entry(lv, entry);

        return entry2;
    }
}

static struct rngmap_entry *__ph_rngmap_entry_create(struct *ptrmap_entry prev, void *tag, void *base, size_t len) {
    if (!__ph_ptrmap) return NULL;
    struct ptrmap_entry evalue = { .base = base, .len = len, .tag = tag };
    assert(__PH_PTRMAP_ENTRY_IS_RANGE(&evalue));
    struct ptrmap_entry *ret = __ph_ptrmap_entry_create_inner(__ph_rngmap, evalue, 0);
    if (!ret) return NULL;
    __ph_ptrmap_entry_insert_to_list(ret, prev);
    return ret;
}

/** Object map **/

// Global object map: roughly the same as the global pointer map.
void *__ph_objmap __attribute__((weak));

static void __ph_objmap_print_inner(struct objmap_entry *objmap, unsigned base_lv, unsigned lv, bool stop, size_t n_entries) {
    assert(objmap);

    char *type = NULL;
    if (__PH_OBJMAP_ENTRY_IS_NULL(objmap))
        type = "NULL";
    else if (__PH_OBJMAP_ENTRY_IS_MAP(objmap))
        type = "MAP";
    else if (__PH_OBJMAP_ENTRY_IS_RANGE(objmap))
        type = "RANGE";
    else
        assert(false);

    __ph_printf("Pointer map %d entries (level: %d, addr: %p)\n", n_entries, base_lv + lv, objmap);

    for (int i = 0; i < n_entries; i++) {
        __ph_printf("┌");
        for (int c = 0; c < 24; c++) __ph_printf("─");
        __ph_printf("┐");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("type: %18s", type);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("base: %18p", objmap[i].base);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("len : %18d", objmap[i].len);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("ptr : %18p", objmap[i].ptr);
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
            if (__PH_OBJMAP_ENTRY_IS_MAP(objmap))
                __ph_objmap_print_inner(objmap[i].ptr, base_lv, lv + 1, stop, __PH_OBJMAP_NR_ENTRIES);
        }
    }
}

static void __ph_objmap_print() {
    if (!__ph_objmap) {
        __ph_printf("(no range map)\n");
        return;
    }

    __ph_objmap_print_inner(__ph_objmap, 0, 0, true, __PH_OBJMAP_NR_ENTRIES);
}

static void __ph_objmap_entry_print(unsigned base_lv, struct objmap_entry *entry) {
    if (!entry) {
        __ph_printf("(no range map entry)\n");
        return;
    }

    __ph_objmap_print_inner(entry, base_lv, 0, true, 1);
}

static void *__ph_objmap_create(unsigned n_entries) {
    assert(malloc_impl);
    unsigned size = n_entries * sizeof(struct objmap_entry);
    void *ret = malloc_impl(size);
    if (!ret) return 0;
    memset(ret, 0, size);
    return ret;
}
