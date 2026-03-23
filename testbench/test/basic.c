#include "basic.h"

#include <stdio.h>
#include <stdlib.h>

bool test_char_access_okay() {
    void *obj = malloc(sizeof(char));
    void *ptr = obj;
    ptr += 1;
    __ph_ptr_move(obj, sizeof(char), obj+1, sizeof(char));
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
