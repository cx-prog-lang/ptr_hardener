#pragma once

#include <stdbool.h>

bool test_char_access_okay();
bool test_int_access_okay();
bool test_oob_pointer_okay();
bool test_char_access_fail();
bool test_int_access_fail();
bool test_cast_access_okay();
bool test_cast_access_fail();
bool test_object_free_okay();
bool test_uaf_fail();
