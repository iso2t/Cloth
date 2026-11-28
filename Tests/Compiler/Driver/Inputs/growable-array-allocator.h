// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <stdlib.h>

void *cloth_test_array_malloc (size_t bytes);
void  cloth_test_array_free (void *memory);

#ifdef CLOTH_TEST_TRACK_ARRAYS
#define malloc cloth_test_array_malloc
#define free   cloth_test_array_free
#elif defined(CLOTH_TEST_FORBID_ALLOCATIONS)
#include <stdio.h>

static inline void *unexpectedAllocation (size_t bytes) {
	fprintf(stderr, "Unexpected allocation of %zu bytes\n", bytes);
	exit(1);
}

#define malloc unexpectedAllocation
#endif
