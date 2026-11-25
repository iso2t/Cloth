// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <stdlib.h>

void *cloth_test_array_malloc (size_t bytes);
void  cloth_test_array_free (void *memory);

#ifdef CLOTH_TEST_TRACK_ARRAYS
#define malloc cloth_test_array_malloc
#define free   cloth_test_array_free
#endif
