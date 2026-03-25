#include "basic.h"

#include <stdio.h>
#include <stdlib.h>

bool test_char_access_okay() {
    int a;

    void *obj = malloc(sizeof(char));
    void *ptr = obj;
    __ph_ptr_move(&ptr, ptr, sizeof(char), ptr+1, sizeof(char));
    ptr += 1;

    void *obj2 = (void *)0x44444444;
    void *ptr2 = obj2;
    __ph_ptr_move(&ptr, ptr2, sizeof(char), ptr2+1, sizeof(char));
    ptr2 += 1;
    __ph_ptr_move(&ptr2, ptr2, sizeof(char), ptr2+2, sizeof(char));
    __ph_ptr_move(&ptr2, ptr2, sizeof(char), ptr2+2, sizeof(char));
    ptr2 += 2;

    __ph_ptr_move(NULL, &a, sizeof(int), &a+2, sizeof(int));
    __ph_ptr_move(ptr2, &a, sizeof(int), ptr2+5, sizeof(int));

    __ph_ptr_deref(&ptr2);
    *(char *)ptr2;
    return true;
}

bool test_int_access_okay() {
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
