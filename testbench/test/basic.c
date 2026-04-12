#include "basic.h"

#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>

bool test_char_access_okay() {
    int a;

    void *obj = malloc(1);
    struct ptrmap_entry *obj_pent = __ph_lvalue_ptr_update_from_obj(&obj, obj);

    void *ptr = obj + 1;
    struct ptrmap_entry *ptr_pent = __ph_lvalue_ptr_update_from_ptrent(&ptr, obj_pent);

    //__ph_lvalue_ptr_deref(&ptr, ptr, sizeof(char));
    //*(char *)ptr;

    //free(obj);

    //__ph_lvalue_ptr_deref(&obj, obj, sizeof(char));
    //*(char *)obj;

    void *obj2 = (void *)0x44444444 + 1000;
    struct ptrmap_entry *obj2_pent = __ph_lvalue_ptr_update_from_base(&obj2, (void *)0x44444444);
    //void *ptr2 = obj2;
    
    __ph_ptr_deref(obj2_pent, obj2, sizeof(char));
    *(char *)obj2;

    /*
    //__ph_lvalue_ptr_move(&ptr, ptr2, sizeof(char), ptr2+1, sizeof(char));
    ptr2 += 1;
    //__ph_lvalue_ptr_move(&ptr2, ptr2, sizeof(char), ptr2+2, sizeof(char));
    //__ph_lvalue_ptr_move(&ptr2, ptr2, sizeof(char), ptr2+2, sizeof(char));
    ptr2 += 2;

    //__ph_lvalue_ptr_move(NULL, &a, sizeof(int), &a+2, sizeof(int));
    //__ph_lvalue_ptr_move(&ptr2, &a, sizeof(int), ptr2+5, sizeof(int));
    ptr2 += 5;

    __ph_lvalue_ptr_deref(&ptr2, ptr2, sizeof(char));
    *(char *)ptr2;
    */
    return true;
}

bool test_int_access_okay() {
    /*
    struct Test {
        int x;
        char *p;
        char y;
        char *q;
        char *r;
    };

    struct Test *test = malloc(sizeof(struct Test));
    char *buf = malloc(10);
    test->p = buf;
    test->q = buf;
    test->r = buf;

    //__ph_lvalue_ptr_move(&test->p, test->p, sizeof(char), test->p + 20, sizeof(char));
    test->p += 20;

    //__ph_lvalue_ptr_deref(&test->p);
    // *test->p;

    free(test);
    */
    return true;
}

bool test_oob_pointer_okay() {
    return true;
}

bool test_char_access_fail() {
    return true;
}

bool test_int_access_fail() {
    return true;
}

bool test_cast_access_okay() {
    return true;
}

bool test_cast_access_fail() {
    return true;
}

bool test_object_free_okay() {
    return true;
}

bool test_uaf_fail() {
    return true;
}
