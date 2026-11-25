// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <growable-array-allocator.h>

static size_t live_allocations;
static int    registered;

static void check_at_exit (void) {
	if (live_allocations) {
		fprintf(stderr, "Growable array allocations still live at exit: %zu\n", live_allocations);
		_Exit(2);
	}
	puts("Growable array allocations balanced");
}

void *cloth_test_array_malloc (size_t bytes) {
	if (getenv("CLOTH_TEST_FAIL_ARRAY_ALLOCATION")) return NULL;
	void *memory = malloc(bytes);
	if (memory) {
		if (!registered) {
			if (atexit(check_at_exit) != 0) abort();
			registered = 1;
		}
		++live_allocations;
	}
	return memory;
}

void cloth_test_array_free (void *memory) {
	if (memory) --live_allocations;
	free(memory);
}

// The lifetime fixture uses println to inspect its in-scope array allocations.
void cloth_rt_println_i32 (int32_t expected) {
	if (live_allocations != (size_t)expected) {
		fprintf(stderr, "Expected %d live growable array allocations, found %zu\n", (int)expected, live_allocations);
		_Exit(2);
	}
}
