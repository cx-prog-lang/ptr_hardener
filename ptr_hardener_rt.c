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
 *  - mapent := (Generic) Map Entry
 *  - Variables starting with a redundant 'a' := granule-aligned
 *  - Pointers starting with a redundant 'ee' := Assignee (lhs) 
 *  - Pointers starting with a redundant 'er' := Assigner (rhs)
 */

#include <assert.h>
#include <dlfcn.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdarg.h> // NOTE: debugging
#include <unistd.h> // NOTE: debugging

/** Type/constant declarations **/

// Generic map entry: range information and associated pointers.
//  - tag == NULL:  null entry (=no object)
//  - tag == 1:     map entry ('next' = nested map)
//  - otherwise:    range entry (=object's range)
#define COMMON_MAP_ENTRY                                                       \
    struct {                                                                   \
        void *tag;  /* Hash tag: the address of this granule. */ \
        void *base; /* Range: base address. */                                 \
        size_t len; /* Range: length (in bytes). */                            \
        void *next; /* Assoc. ptrs: next entry. */                             \
    }

#define __PH_MAPENT_SET_NULL(entry) ((entry)->tag = NULL) 
#define __PH_MAPENT_IS_NULL(entry) ((entry)->tag == NULL)

#define __PH_MAPENT_SET_MAP(entry, map) ((entry)->tag = (void *)1)
#define __PH_MAPENT_IS_MAP(entry) ((entry)->tag == (void *)1)
#define __PH_MAPENT_GET_MAP(entry)                                             \
    (assert(__PH_MAPENT_IS_MAP(entry), (entry)->next)

#define __PH_MAPENT_IS_RANGE(entry)                                            \
    (!__PH_MAPENT_IS_NULL(entry) && !__PH_MAPENT_IS_MAP(entry))

// Object map entry
struct objmap_entry {
    COMMON_MAP_ENTRY;
};

// Pointer map entry
struct ptrmap_entry {
    COMMON_MAP_ENTRY;
    void *prev; /* Assoc. ptrs: previous entry. */
};

// NOTE: reduced for debugging. Original "1 << 16".
#define __PH_OBJMAP_NR_ENTRIES (1 << 3)
#define __PH_PTRMAP_NR_ENTRIES (1 << 3)

// Granule: the minimum unit of byte alignment.
#define __PH_GRANULE_SIZE (1 << 5)
#define __PH_GRANULE_MASK (__PH_GRANULE_SIZE - 1)

#define __PH_GRANULE_CEIL(x)                                                   \
    (((intptr_t)(x) + __PH_GRANULE_MASK) & ~(intptr_t)__PH_GRANULE_MASK)
#define __PH_GRANULE_FLOOR(x)                                                  \
    ((intptr_t)(x) & ~(intptr_t)(__PH_GRANULE_SIZE - 1))

// Actual standard allocators. The default behavior is to initialize them when
// they're used the first time, but they _may_ be updated by a global
// constructor if there is an annotated custom allocator. The custom ones should
// have the same type signature to the allocator that it wants to replace.
// Let's say multiple custom allocators (for the same original allocator) causes
// an undefined behavior, though I think it'll function just fine.
void *(*malloc_impl)(size_t) __attribute__((weak));          // 'malloc'
void *(*calloc_impl)(size_t, size_t) __attribute__((weak));  // 'calloc'
void *(*aalloc_impl)(size_t, size_t) __attribute__((weak));  // 'aligned_alloc'
void *(*realloc_impl)(void *, size_t) __attribute__((weak)); // 'realloc'
void (*free_impl)(void *) __attribute__((weak));             // 'free'
void (*sfree_impl)(void *) __attribute__((weak));            // 'free_sized'
void (*asfree_impl)(void *) __attribute__((weak)); // 'free_aligned_sized'

/** Debug utilities **/

static char *__ph_printf_convert(uint64_t num, int base, int *len) {
    // Based on:
    // https://stackoverflow.com/questions/1735236/how-to-write-my-own-printf-in-c
    static char repr[] = "0123456789abcdef";
    static char buffer[50];
    char *ptr;
    int _len = 0;

    ptr = &buffer[49];
    *ptr = '\0';

    do {
        *--ptr = repr[num % base];
        num /= base;
        _len++;
    } while (num != 0);

    *len = _len;
    return ptr;
}

static void __ph_printf(char *format, ...) {
    // Based on:
    // https://stackoverflow.com/questions/1735236/how-to-write-my-own-printf-in-c
    char *traverse;
    uint64_t i;
    char *s;
    int len;

    va_list arg;
    va_start(arg, format);

    for (traverse = format; *traverse != '\0'; traverse++) {
        while (*traverse != '%') {
            if (*traverse == '\0')
                return;
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

static void __ph_ptrmap_print_inner(struct ptrmap_entry *ptrmap,
                                    unsigned base_lv, unsigned lv, bool stop,
                                    size_t n_entries) {
    // FIXME: merge this function to objmap's.
    assert(ptrmap);

    char *type = NULL;
    if (__PH_MAPENT_IS_NULL(ptrmap))
        type = "NULL";
    else if (__PH_MAPENT_IS_MAP(ptrmap))
        type = "MAP";
    else if (__PH_MAPENT_IS_RANGE(ptrmap))
        type = "RANGE";
    else
        assert(false);

    __ph_printf("Pointer map %d entries (level: %d, addr: %p)\n", n_entries,
                base_lv + lv, ptrmap);

    for (int i = 0; i < n_entries; i++) {
        __ph_printf("┌");
        for (int c = 0; c < 24; c++)
            __ph_printf("─");
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
        for (int c = 0; c < 24; c++)
            __ph_printf("─");
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
            if (__PH_MAPENT_IS_MAP(ptrmap))
                __ph_ptrmap_print_inner(ptrmap[i].base, base_lv, lv + 1, stop,
                                        __PH_PTRMAP_NR_ENTRIES);
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

static void __ph_ptrmap_entry_print(unsigned base_lv,
                                    struct ptrmap_entry *entry) {
    if (!entry) {
        __ph_printf("(no range map entry)\n");
        return;
    }

    __ph_ptrmap_print_inner(entry, base_lv, 0, true, 1);
}

static void __ph_objmap_print_inner(struct objmap_entry *objmap,
                                    unsigned base_lv, unsigned lv, bool stop,
                                    size_t n_entries) {
    // FIXME: merge this function to ptrmap's.
    assert(objmap);

    char *type = NULL;
    if (__PH_MAPENT_IS_NULL(objmap))
        type = "NULL";
    else if (__PH_MAPENT_IS_MAP(objmap))
        type = "MAP";
    else if (__PH_MAPENT_IS_RANGE(objmap))
        type = "RANGE";
    else
        assert(false);

    __ph_printf("Pointer map %d entries (level: %d, addr: %p)\n", n_entries,
                base_lv + lv, objmap);

    for (int i = 0; i < n_entries; i++) {
        __ph_printf("┌");
        for (int c = 0; c < 24; c++)
            __ph_printf("─");
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
        __ph_printf("next: %18p", objmap[i].next);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("└");
        for (int c = 0; c < 24; c++)
            __ph_printf("─");
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
            if (__PH_MAPENT_IS_MAP(objmap))
                __ph_objmap_print_inner(objmap[i].base, base_lv, lv + 1, stop,
                                        __PH_OBJMAP_NR_ENTRIES);
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

static void __ph_objmap_entry_print(unsigned base_lv,
                                    struct objmap_entry *entry) {
    if (!entry) {
        __ph_printf("(no range map entry)\n");
        return;
    }

    __ph_objmap_print_inner(entry, base_lv, 0, true, 1);
}

/** General utilities **/

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

static void *__ph_ptrmap_create(unsigned n_entries) {
    assert(malloc_impl);
    unsigned size = n_entries * sizeof(struct ptrmap_entry);
    void *ret = malloc_impl(size);
    if (!ret)
        return 0;
    memset(ret, 0, size);
    return ret;
}

static struct ptrmap_entry *__ph_ptrmap_entry_insert_to_list(struct ptrmap_entry *this,
                                             struct ptrmap_entry *prev) {
    if (!this) return NULL;
    if (!prev) return this;
    this->next = prev->next;
    this->prev = prev;
    prev->next = this;
    return this;
}

static struct ptrmap_entry *__ph_ptrmap_entry_remove_from_list(struct ptrmap_entry *this) {
    if (!this) return NULL;
    this->prev->next = this->next;
    this->next->prev = this->prev;
    this->prev = this->next = NULL;
    return this;
}

static struct ptrmap_entry *__ph_ptrmap_update_entry(
    struct ptrmap_entry *ptrmap, struct ptrmap_entry evalue, unsigned lv) {
    assert(!__PH_MAPENT_IS_NULL(&evalue));

    if (lv == UINT_MAX)
        return NULL;

    ptrmap_index_t idx = __ph_hash_addr(evalue.tag, lv);
    assert(0 <= idx && idx < __PH_PTRMAP_NR_ENTRIES);
    struct ptrmap_entry *entry = &ptrmap[idx];

    if (__PH_MAPENT_IS_NULL(entry) ||
        (__PH_MAPENT_IS_RANGE(entry) && entry->tag == evalue.tag)) {
        if (__PH_MAPENT_IS_RANGE(entry) && entry->tag == evalue.tag) { 
            if (!__ph_ptrmap_entry_remove_from_list(entry))
                return NULL;
        }
        entry->base = evalue.base;
        entry->len = evalue.len;
        __ph_ptrmap_print_entry(lv, entry);
        return entry;
    } else if (__PH_MAPENT_IS_MAP(entry)) {
        return __ph_ptrmap_update_entry(__PH_MAPENT_GET_MAP(entry), evalue,
                                                lv + 1);
    } else /* range entry */ {
        assert(__PH_MAPENT_IS_RANGE(entry));
        assert(entry->tag != evalue.tag);

        struct ptrmap_entry prev_evalue = *entry;
        void *new_ptrmap = __ph_create_ptrmap(__PH_ptrmap_NR_ENTRIES);
        if (!new_ptrmap)
            return NULL;

        __PH_MAPENT_SET_MAP(entry, new_ptrmap);

        struct ptrmap_entry *entry1 = __ph_ptrmap_update_entry(
            new_ptrmap, prev_evalue, lv + 1);
        if (!entry1)
            return NULL;

        struct ptrmap_entry *entry2 =
            __ph_ptrmap_update_entry(new_ptrmap, evalue, lv + 1);
        if (!entry2)
            return NULL;

        __ph_ptrmap_print_entry(lv, entry);

        return entry2;
    }
}

static struct ptrmap_entry *__ph_ptrmap_update_entry_from_ertag(void *tag, struct ptrmap_entry *prev) {
    if (!__ph_ptrmap) return NULL;
    struct ptrmap_entry evalue = {.tag = tag, .base = prev->base, .len = prev->len};
    struct ptrmap_entry *ret = __ph_ptrmap_update_entry(__ph_ptrmap, evalue, 0);
    return __ph_ptrmap_entry_insert_to_list(ret, prev);
}

static struct ptrmap_entry *__ph_ptrmap_update_entry_from_obj(void *tag, struct objmap_entry *obj) {
    if (!__ph_ptrmap) return NULL;
    struct ptrmap_entry evalue = {.tag = tag, .base = obj->base, .len = obj->len};
    struct ptrmap_entry *ret = __ph_ptrmap_update_entry(__ph_ptrmap, evalue, 0);
    return __ph_ptrmap_entry_insert_to_list(ret, (struct ptrmap_entry *)obj);
}

static struct ptrmap_entry *__ph_ptrmap_update_entry_loose(void *tag) {
    if (!__ph_ptrmap) return NULL;
    struct ptrmap_entry evalue = {.tag = tag, .base = NULL, .len = SIZE_MAX};
    struct ptrmap_entry *ret = __ph_ptrmap_update_entry(__ph_ptrmap, evalue, 0);
    return ret;
}

static struct ptrmap_entry *__ph_ptrmap_get_entry_inner(struct ptrmap_entry *ptrmap, void *tag, unsigned lv) {
    if (!ptrmap) return NULL;
    struct ptrmap_entry *entry = &ptrmap[__ph_hash_addr(tag, lv)];

    if (__PH_MAPENT_IS_MAP(entry)) {
        return __ph_ptrmap_get_entry_inner(__PH_MAPENT_GET_MAP(entry), tag, lv + 1);
    } else if (entry->tag == tag) {
        return entry;
    } else {
        return NULL;
    }
}

static struct ptrmap_entry *__ph_ptrmap_get_entry(void *tag) {
    if (!__ph_ptrmap) return NULL;
    return __ph_get_ptrmap_ptr_entry_inner(__ph_ptrmap, tag, 0);
}

/** Object map **/

// Global object map: roughly the same as the global pointer map.
void *__ph_objmap __attribute__((weak));

static void *__ph_objmap_create(unsigned n_entries) {
    assert(malloc_impl);
    unsigned size = n_entries * sizeof(struct objmap_entry);
    void *ret = malloc_impl(size);
    if (!ret)
        return 0;
    memset(ret, 0, size);
    return ret;
}

static struct objmap_entry *__ph_objmap_create_entry(
    struct objmap_entry *objmap, struct objmap_entry evalue, unsigned lv) {
    assert(!__PH_MAPENT_IS_NULL(&evalue));

    if (lv == UINT_MAX)
        return NULL;

    objmap_index_t idx = __ph_hash_addr(evalue.tag, lv);
    assert(0 <= idx && idx < __PH_OBJMAP_NR_ENTRIES);
    struct objmap_entry *entry = &objmap[idx];

    if (__PH_MAPENT_IS_NULL(entry) ||
        entry->base = evalue.base;
        entry->len = evalue.len;
        entry->tag = evalue.tag;
        __ph_objmap_print_entry(lv, entry);
        return entry;
    } else if (__PH_MAPENT_IS_MAP(entry)) {
        return __ph_objmap_create_entry(__PH_MAPENT_GET_MAP(entry), evalue,
                                                lv + 1);
    } else /* range entry */ {
        assert(__PH_MAPENT_IS_RANGE(entry));
        assert(entry->tag != evalue.tag);

        struct objmap_entry prev_evalue = *entry;
        void *new_objmap = __ph_create_objmap(__PH_OBJMAP_NR_ENTRIES);
        if (!new_objmap)
            return NULL;

        __PH_MAPENT_SET_MAP(entry, new_objmap);

        struct objmap_entry *entry1 = __ph_objmap_create_entry(
            new_objmap, prev_evalue, lv + 1);
        if (!entry1)
            return NULL;

        struct objmap_entry *entry2 =
            __ph_objmap_create_entry(new_objmap, evalue, lv + 1);
        if (!entry2)
            return NULL;

        __ph_objmap_print_entry(lv, entry);

        return entry2;
    }
}

static bool __ph_objmap_create_entries(void *base, size_t size) {
    if (!__ph_objmap) 
        __ph_objmap = __ph_create_objmap(__PH_OBJMAP_NR_ENTRIES);

    for (int goff = 0; goff <= size / __PH_GRANULE_SIZE; goff++) {
        void *_tag = base + (goff * __PH_GRANULE_SIZE);
        struct objmap_entry evalue = { .tag = tag, .base = base, .len = len };
        void *create_res = __ph_objmap_create_entry(__ph_objmap, evalue, 0);
        if (!create_res) return false;
    }
    
    return true;
}

static struct objmap_entry *__ph_objmap_get_entry_inner(struct objmap_entry *objmap, void *aobj, unsigned lv) {
    if (!objmap) return NULL;
    struct objmap_entry *entry = &objmap[__ph_hash_addr(aobj, lv)];

    if (__PH_MAPENT_IS_MAP(entry)) {
        return __ph_objmap_get_entry_inner(__PH_MAPENT_GET_MAP(entry), aobj, lv + 1);
    } else if (entry->tag == aobj) {
        return entry;
    } else {
        return NULL;
    }
}

// Return: the corresponding object to 'obj'.
// Note: 'obj' doesn't need to be granule-aligned!
static struct objmap_entry *__ph_objmap_get_entry(void *obj) {
    if (!__ph_objmap) return NULL;
    void *aobj = (void *)__PH_GRANULE_FLOOR(obj);
    return __ph_objmap_get_entry_inner(__ph_objmap, aobj, 0);
}

static bool __ph_objmap_destroy_entries(void *tag) {
    // Get the range of the current 'tag'.
    struct objmap_entry *entry = __ph_objmap_get_entry(tag);
    if (!entry) return true;    // Maybe untracked object. Ignore.

    for (int goff = 0; goff < entry->len / __PH_GRANULE_SIZE; goff++) {
        void *aobj = entry->base + (goff * __PH_GRANULE_SIZE);
        struct objmapoent *oent = __ph_objmap_getoent(aobj);
        if (!oent) continue;

        // Invalidate all pointer entries associated with this object.
        struct ptrmap_entry *pent = oent->next;
        for (; pent != NULL; pent = pent->next) 
            pent->len = 0;

        // Destroy all object map entries associated with 'tag'.
        __PH_MAPENT_SET_NULL(oent);
    }
}

/** Interface **/

static void __ph_ptr_update(void *eetag, void *eeval, void *ertag) {
    if (!__ph_ptrmap) 
        __ph_ptrmap = __ph_ptrmap_create(__PH_PTRMAP_NR_ENTRIES);

    // Case 1: from tracked pointer.
    // Copy range information from the assigner pointer.
    struct ptrmap_entry *erent = (ertag) ? __ph_ptrent_get_entry(ertag) : NULL;
    if (erent) {
        __ph_ptrmap_update_entry_from_ertag(eetag, erent);
        return;
    }

    // Case 2: from tracked object.
    // Copy range information from the object.
    struct objmap_entry *oent = (eeval) ? __ph_objent_get_entry(eeval) : NULL;
    if (oent) {
        __ph_ptrmap_update_entry_from_obj(eetag, oent);
        return;
    }

    // Case 3: from untracked pointer & object.
    // Make it a loose pointer.
    __ph_ptrmap_update_entry_loose(eetag);
}
