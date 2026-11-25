// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst
// See Documentation/compiler/driver/Objects.rst

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct cloth_array {
	unsigned char *data;
	size_t         length;
	size_t         capacity;
	void          *owner;
};

_Static_assert(offsetof(struct cloth_array, length) == sizeof(size_t) && offsetof(struct cloth_array, capacity) == 2 * sizeof(size_t) &&
                       offsetof(struct cloth_array, owner) == 3 * sizeof(size_t) && sizeof(struct cloth_array) == 4 * sizeof(size_t),
               "Cloth rank-one array descriptor requires four native words");

_Noreturn void cloth_rt_array_capacity_overflow (void);
_Noreturn void cloth_rt_array_allocation_failed (void);
_Noreturn void cloth_rt_array_index_out_of_bounds (void);

static size_t capacity_limit (size_t stride, size_t alignment) {
	// LLVM addresses cells using signed pointer offsets. Allow space to align the first cell within the allocation.
	if (!stride || !alignment || (alignment & (alignment - 1)) || alignment - 1 > (size_t)PTRDIFF_MAX) cloth_rt_array_capacity_overflow();
	return ((size_t)PTRDIFF_MAX - (alignment - 1)) / stride;
}

static struct cloth_array allocate_buffer (size_t capacity, size_t stride, size_t alignment) {
	unsigned char *owner = malloc(capacity * stride + (alignment - 1));
	if (!owner) cloth_rt_array_allocation_failed();
	const size_t offset = (size_t)(-(uintptr_t)owner & (uintptr_t)(alignment - 1));
	return (struct cloth_array){ .data = owner + offset, .capacity = capacity, .owner = owner };
}

static void replace (struct cloth_array *array, struct cloth_array next) {
	if (array->owner) free(array->owner);
	*array = next;
}

static struct cloth_array allocate_grown_buffer (const struct cloth_array *array, size_t minimum, size_t limit, size_t stride, size_t alignment) {
	const size_t current  = array->owner ? array->capacity : array->length;
	size_t       capacity = minimum > array->length ? minimum : array->length;
	if (current < capacity && current <= limit / 2 && current * 2 > capacity) capacity = current * 2;
	return allocate_buffer(capacity, stride, alignment);
}

void cloth_rt_array_clear (struct cloth_array *array) {
	if (array->owner)
		array->length = 0;
	else
		*array = (struct cloth_array){ 0 };
}

void cloth_rt_array_assign (struct cloth_array *array, const void *data, size_t count, size_t stride, size_t alignment) {
	if (count > capacity_limit(stride, alignment)) cloth_rt_array_capacity_overflow();
	if (!count) {
		cloth_rt_array_clear(array);
		return;
	}
	if (array->owner && count <= array->capacity) {
		memmove(array->data, data, count * stride);
		array->length = count;
		return;
	}
	struct cloth_array next = allocate_buffer(count, stride, alignment);
	memcpy(next.data, data, count * stride);
	next.length = count;
	replace(array, next);
}

void cloth_rt_array_reserve (struct cloth_array *array, size_t minimum, size_t stride, size_t alignment) {
	const size_t limit = capacity_limit(stride, alignment);
	if (minimum > limit || array->length > limit) cloth_rt_array_capacity_overflow();
	if (minimum <= array->capacity && (array->owner || !array->capacity)) return;
	struct cloth_array next = allocate_grown_buffer(array, minimum, limit, stride, alignment);
	next.length             = array->length;
	if (next.length) memcpy(next.data, array->data, next.length * stride);
	replace(array, next);
}

void cloth_rt_array_insert (struct cloth_array *array, size_t index, const void *cell, size_t stride, size_t alignment) {
	if (index > array->length) cloth_rt_array_index_out_of_bounds();
	const size_t limit = capacity_limit(stride, alignment);
	if (array->length >= limit) cloth_rt_array_capacity_overflow();
	const size_t length = array->length + 1;
	const size_t prefix = index * stride;
	const size_t suffix = (array->length - index) * stride;
	if (array->owner && length <= array->capacity) {
		unsigned char *at = array->data + prefix;
		if (suffix) {
			// A source cell in the shifted suffix will be one slot farther along after the move.
			if ((uintptr_t)cell - (uintptr_t)at < suffix) cell = (const unsigned char *)cell + stride;
			memmove(at + stride, at, suffix);
		}
		memmove(at, cell, stride);
		array->length = length;
		return;
	}
	struct cloth_array next = allocate_grown_buffer(array, length, limit, stride, alignment);
	// Keep the previous allocation alive until all three pieces have been copied.
	if (prefix) memcpy(next.data, array->data, prefix);
	memcpy(next.data + prefix, cell, stride);
	if (suffix) memcpy(next.data + prefix + stride, array->data + prefix, suffix);
	next.length = length;
	replace(array, next);
}

void cloth_rt_array_append (struct cloth_array *array, const void *cell, size_t stride, size_t alignment) {
	cloth_rt_array_insert(array, array->length, cell, stride, alignment);
}

void cloth_rt_array_remove_at (struct cloth_array *array, size_t index, void *removed, size_t stride, size_t alignment) {
	if (index >= array->length) cloth_rt_array_index_out_of_bounds();
	if (!array->owner) cloth_rt_array_reserve(array, array->length, stride, alignment);
	unsigned char *at = array->data + index * stride;
	memcpy(removed, at, stride);
	const size_t suffix = (array->length - index - 1) * stride;
	if (suffix) memmove(at, at + stride, suffix);
	--array->length;
}

void cloth_rt_array_release (struct cloth_array *array) {
	replace(array, (struct cloth_array){ 0 });
}
