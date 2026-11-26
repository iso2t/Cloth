// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst
// See Documentation/compiler/driver/Objects.rst

#include <cloth/array.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "runtime.h"

_Static_assert(offsetof(ClothArray, length) == sizeof(size_t) && offsetof(ClothArray, capacity) == 2 * sizeof(size_t) &&
                       offsetof(ClothArray, owner) == 3 * sizeof(size_t) && sizeof(ClothArray) == 4 * sizeof(size_t),
               "Cloth rank-one array descriptor requires four native words");

static size_t capacityLimit (size_t stride, size_t alignment) {
	// LLVM addresses cells using signed pointer offsets. Allow space to align the first cell within the allocation.
	if (!stride || !alignment || (alignment & (alignment - 1)) || alignment - 1 > (size_t)PTRDIFF_MAX) clothRtArrayCapacityOverflow();
	return ((size_t)PTRDIFF_MAX - (alignment - 1)) / stride;
}

static ClothArray allocateBuffer (size_t capacity, size_t stride, size_t alignment) {
	unsigned char *owner = malloc(capacity * stride + (alignment - 1));
	if (!owner) clothRtArrayAllocationFailed();
	const size_t offset = (size_t)(-(uintptr_t)owner & (uintptr_t)(alignment - 1));
	return (ClothArray){ .data = owner + offset, .capacity = capacity, .owner = owner };
}

static void replace (ClothArray *array, ClothArray next) {
	if (array->owner) free(array->owner);
	*array = next;
}

static ClothArray allocateGrownBuffer (const ClothArray *array, size_t minimum, size_t limit, size_t stride, size_t alignment) {
	const size_t current  = array->owner ? array->capacity : array->length;
	size_t       capacity = minimum > array->length ? minimum : array->length;
	if (current < capacity && current <= limit / 2 && current * 2 > capacity) capacity = current * 2;
	return allocateBuffer(capacity, stride, alignment);
}

void clothRtArrayClear (ClothArray *array) {
	if (array->owner)
		array->length = 0;
	else
		*array = (ClothArray){ 0 };
}

void clothRtArrayAssign (ClothArray *array, const void *data, size_t count, size_t stride, size_t alignment) {
	if (count > capacityLimit(stride, alignment)) clothRtArrayCapacityOverflow();
	if (!count) {
		clothRtArrayClear(array);
		return;
	}
	if (array->owner && count <= array->capacity) {
		memmove(array->data, data, count * stride);
		array->length = count;
		return;
	}
	ClothArray next = allocateBuffer(count, stride, alignment);
	memcpy(next.data, data, count * stride);
	next.length = count;
	replace(array, next);
}

void clothRtArrayReserve (ClothArray *array, size_t minimum, size_t stride, size_t alignment) {
	const size_t limit = capacityLimit(stride, alignment);
	if (minimum > limit || array->length > limit) clothRtArrayCapacityOverflow();
	if (minimum <= array->capacity && (array->owner || !array->capacity)) return;
	ClothArray next = allocateGrownBuffer(array, minimum, limit, stride, alignment);
	next.length     = array->length;
	if (next.length) memcpy(next.data, array->data, next.length * stride);
	replace(array, next);
}

void clothRtArrayInsert (ClothArray *array, size_t index, const void *cell, size_t stride, size_t alignment) {
	if (index > array->length) clothRtArrayIndexOutOfBounds();
	const size_t limit = capacityLimit(stride, alignment);
	if (array->length >= limit) clothRtArrayCapacityOverflow();
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
	ClothArray next = allocateGrownBuffer(array, length, limit, stride, alignment);
	// Keep the previous allocation alive until all three pieces have been copied.
	if (prefix) memcpy(next.data, array->data, prefix);
	memcpy(next.data + prefix, cell, stride);
	if (suffix) memcpy(next.data + prefix + stride, array->data + prefix, suffix);
	next.length = length;
	replace(array, next);
}

void clothRtArrayAppend (ClothArray *array, const void *cell, size_t stride, size_t alignment) {
	clothRtArrayInsert(array, array->length, cell, stride, alignment);
}

void clothRtArrayRemoveAt (ClothArray *array, size_t index, void *removed, size_t stride, size_t alignment) {
	if (index >= array->length) clothRtArrayIndexOutOfBounds();
	if (!array->owner) clothRtArrayReserve(array, array->length, stride, alignment);
	unsigned char *at = array->data + index * stride;
	memcpy(removed, at, stride);
	const size_t suffix = (array->length - index - 1) * stride;
	if (suffix) memmove(at, at + stride, suffix);
	--array->length;
}

void clothRtArrayRelease (ClothArray *array) {
	replace(array, (ClothArray){ 0 });
}
