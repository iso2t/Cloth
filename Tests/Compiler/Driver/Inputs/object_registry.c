// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <assert.h>
#include <cloth/object.h>
#include <stdint.h>

static unsigned          destroyed, disposed;
static const char        base_type, derived_type, other_type;
static const void *const views[] = { &derived_type, &base_type, NULL };

static void destroy (uint64_t self) {
	assert(*(int *)cloth_rt_object_data(self, 1) == 42);
	assert(cloth_rt_object_is(self, &base_type, 1));
	++destroyed;
	// A callback can allocate enough objects to move the registry.
	for (unsigned i = 0; i < 32; ++i) cloth_rt_object_create(1, 1, NULL, NULL, NULL);
}

static void dispose (void *data) {
	assert(*(int *)data == 42);
	++disposed;
}

int main (int argc, char **argv) {
	const uint64_t owner = cloth_rt_object_create(1, 1, NULL, NULL, NULL);
	const uint64_t other = cloth_rt_object_create(1, 1, NULL, NULL, NULL);
	const uint64_t child = cloth_rt_object_create(sizeof(int), 64, views, destroy, dispose);
	int           *field = cloth_rt_object_data(child, 0);
	// The C implementation name and the native ABI name access the same object.
	assert(clothRtObjectData(child, 0) == field);
	assert((uintptr_t)field % 64 == 0 && *field == 0);
	*field       = 42;
	void *buffer = cloth_rt_object_backing(child, 128, 128);
	assert((uintptr_t)buffer % 128 == 0);
	assert(cloth_rt_object_is(child, &base_type, 0) && cloth_rt_object_is(child, &derived_type, 0));
	assert(!cloth_rt_object_is(child, &other_type, 0) && !cloth_rt_object_is(0, &base_type, 0));
	cloth_rt_object_acquire(owner, child);
	if (argc > 1) {
		switch (argv[1][0]) {
		case 'd':
			cloth_rt_object_delete(child, 0);
			break;
		case 'a':
			cloth_rt_object_acquire(other, child);
			break;
		case 'r':
			cloth_rt_object_release(other, child);
			break;
		case 'c':
			cloth_rt_object_acquire(child, owner);
			break;
		case 'm':
			cloth_rt_object_set_cleanup(owner, 1);
			cloth_rt_object_check_mutation(owner);
			break;
		}
	}
	cloth_rt_object_release(owner, child);
	cloth_rt_object_acquire(other, child);
	cloth_rt_object_set_cleanup(other, 1);
	cloth_rt_object_delete(child, other);
	assert(destroyed == 1 && disposed == 1);
	const uint64_t replacement = cloth_rt_object_create(sizeof(int), 64, NULL, NULL, NULL);
	assert((uint32_t)child == (uint32_t)replacement && child != replacement);
	assert(*(int *)cloth_rt_object_data(replacement, 0) == 0);
	if (argc > 1 && argv[1][0] == 's') cloth_rt_object_data(child, 0);
	cloth_rt_object_set_cleanup(other, 0);
	cloth_rt_object_check_mutation(other);
	cloth_rt_object_delete(0, 0);
	cloth_rt_object_delete(replacement, 0);
	cloth_rt_object_delete(other, 0);
	cloth_rt_object_delete(owner, 0);
	return 42;
}
