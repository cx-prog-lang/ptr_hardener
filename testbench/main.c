#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "test/basic.h"
#include "test/intermediate.h"
#include "test/advanced.h"

#define TEST(prefix, test_fn) { \
    printf("\033[1m[%s]\033[m %s ... ", (prefix), (#test_fn));   \
    bool result = test_fn();                        \
    if (result) { printf("\033[32mokay\033[m\n"); }               \
    else        { printf("\033[31mfail\033[m\n"); exit(1); }      \
}

int main() {
    // Basic
    //TEST("basic", test_char_access_okay);
    TEST("basic", test_int_access_okay);
    TEST("basic", test_oob_pointer_okay);
    TEST("basic", test_char_access_fail);
    TEST("basic", test_int_access_fail);
    TEST("basic", test_cast_access_okay);
    TEST("basic", test_cast_access_fail);
    TEST("basic", test_object_free_okay);
    TEST("basic", test_uaf_fail);

    /*
    // Intermediate
    TEST("intermediate", test_arr_access_okay);
    TEST("intermediate", test_arr_access_fail);
    TEST("intermediate", test_calloc_access_okay);
    TEST("intermediate", test_calloc_access_fail);
    TEST("intermediate", test_aalloc_access_okay);
    TEST("intermediate", test_aalloc_access_fail);
    TEST("intermediate", test_realloc_access_okay);
    TEST("intermediate", test_realloc_access_fail);
    TEST("intermediate", test_struct_access_okay);
    TEST("intermediate", test_struct_access_fail);
    TEST("intermediate", test_union_access_okay);
    TEST("intermediate", test_union_access_fail);

    // Advanced
    TEST("advanced", test_many_objects_okay);
    TEST("advanced", test_inter_func_okay);
    TEST("advanced", test_inter_func_fail);
    TEST("advanced", test_inter_tu_okay);
    TEST("advanced", test_inter_tu_fail);
    TEST("advanced", test_escaped_okay);
    TEST("advanced", test_escaped_fail);
    TEST("advanced", test_stack_object_okay);
    TEST("advanced", test_global_object_okay);
    TEST("advanced", test_global_object_fail);
    */
    
    return 0;
}
