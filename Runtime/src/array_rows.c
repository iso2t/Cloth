// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/compiler/storage/Aggregates.rst

#include <cloth/array.h>
#include <runtime.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
	size_t  count;
	uint8_t known;
} Extent;

_Static_assert(sizeof(Extent) == 2 * sizeof(size_t), "LLVM array extent layout");

static size_t descriptorSize (size_t rank) {
	return sizeof(ClothArray) + (rank - 1) * sizeof(Extent);
}

// The prefix and suffix match the rank-one runtime header. Copy them separately
// so the rank-one allocation routines can also manage a buffer of row descriptors.
static ClothArray header (const void *descriptor, size_t rank) {
	const unsigned char *bytes = descriptor;
	ClothArray           array;
	memcpy(&array, bytes, offsetof(ClothArray, capacity));
	memcpy((unsigned char *)&array + offsetof(ClothArray, capacity), bytes + descriptorSize(rank) - 2 * sizeof(size_t), 2 * sizeof(size_t));
	return array;
}

static void storeHeader (void *descriptor, size_t rank, ClothArray array) {
	unsigned char *bytes = descriptor;
	memcpy(bytes, &array, offsetof(ClothArray, capacity));
	memcpy(bytes + descriptorSize(rank) - 2 * sizeof(size_t), (const unsigned char *)&array + offsetof(ClothArray, capacity), 2 * sizeof(size_t));
}

static Extent *extents (void *descriptor) {
	return (Extent *)((unsigned char *)descriptor + offsetof(ClothArray, capacity));
}

static const Extent *sourceExtents (const void *descriptor) {
	return (const Extent *)((const unsigned char *)descriptor + offsetof(ClothArray, capacity));
}

static void mergeExtent (Extent *destination, Extent incoming) {
	if (!incoming.known) return;
	if (destination->known && destination->count != incoming.count) clothRtArrayLengthMismatch();
	*destination = incoming;
}

static void mergeRowShape (void *array, const void *row, size_t rank) {
	Extent *shape = extents(array);
	mergeExtent(shape, (Extent){ .count = header(row, rank - 1).length, .known = 1 });
	for (size_t i = 1; i < rank - 1; ++i) mergeExtent(shape + i, sourceExtents(row)[i - 1]);
}

static void releaseChildren (ClothArray array, size_t rank) {
	if (!array.owner || rank == 1) return;
	const size_t step = descriptorSize(rank - 1);
	for (size_t i = 0; i < array.length; ++i) clothRtRowsRelease(array.data + i * step, rank - 1);
}

void clothRtRowsRelease (void *descriptor, size_t rank) {
	ClothArray array = header(descriptor, rank);
	releaseChildren(array, rank);
	clothRtArrayRelease(&array);
	memset(descriptor, 0, descriptorSize(rank));
}

static ClothArray copyBuffer (const void *source, size_t rank, size_t stride, size_t alignment) {
	const ClothArray original = header(source, rank);
	ClothArray       copy     = { 0 };
	if (rank == 1) {
		clothRtArrayAssign(&copy, original.data, original.length, stride, alignment);
		return copy;
	}
	const size_t step = descriptorSize(rank - 1);
	clothRtArrayAssign(&copy, original.data, original.length, step, _Alignof(ClothArray));
	for (size_t i = 0; i < copy.length; ++i) {
		unsigned char *row = copy.data + i * step;
		storeHeader(row, rank - 1, copyBuffer(original.data + i * step, rank - 1, stride, alignment));
	}
	return copy;
}

void clothRtRowsAssign (void *destination, const void *source, size_t rank, size_t stride, size_t alignment) {
	if (rank == 1) {
		const ClothArray row = header(source, rank);
		clothRtArrayAssign(destination, row.data, row.length, stride, alignment);
		return;
	}
	for (size_t i = 0; i < rank - 1; ++i) mergeExtent(extents(destination) + i, sourceExtents(source)[i]);
	// Finish reading the source before freeing any possibly aliased destination rows.
	const ClothArray copy     = copyBuffer(source, rank, stride, alignment);
	ClothArray       previous = header(destination, rank);
	releaseChildren(previous, rank);
	clothRtArrayRelease(&previous);
	storeHeader(destination, rank, copy);
}

static const unsigned char *unpackFlat (void *destination, const unsigned char *source, size_t count, const Extent *shape, size_t rank, size_t stride,
                                        size_t alignment) {
	ClothArray array = { 0 };
	if (rank == 1) {
		clothRtArrayAssign(&array, source, count, stride, alignment);
		storeHeader(destination, rank, array);
		return count ? source + count * stride : source;
	}
	memcpy(extents(destination), shape, (rank - 1) * sizeof(Extent));
	const size_t step = descriptorSize(rank - 1);
	clothRtArrayReserve(&array, count, step, _Alignof(ClothArray));
	array.length = count;
	for (size_t i = 0; i < count; ++i) source = unpackFlat(array.data + i * step, source, shape->count, shape + 1, rank - 1, stride, alignment);
	storeHeader(destination, rank, array);
	return source;
}

void clothRtRowsFromFlat (void *destination, const void *source, const void *shape, size_t rank, size_t stride, size_t alignment) {
	clothRtRowsRelease(destination, rank);
	unpackFlat(destination, source, header(shape, rank).length, sourceExtents(shape), rank, stride, alignment);
}

static unsigned char *packFlat (unsigned char *destination, const void *source, size_t count, const Extent *shape, size_t rank, size_t stride) {
	const ClothArray array = header(source, rank);
	if (array.length != count) clothRtArrayLengthMismatch();
	if (rank == 1) {
		if (!array.length) return destination;
		memcpy(destination, array.data, array.length * stride);
		return destination + array.length * stride;
	}
	for (size_t i = 0; i < rank - 1; ++i) {
		const Extent stored = sourceExtents(source)[i];
		if (stored.known && stored.count != shape[i].count) clothRtArrayLengthMismatch();
	}
	const size_t step = descriptorSize(rank - 1);
	for (size_t i = 0; i < array.length; ++i) destination = packFlat(destination, array.data + i * step, shape->count, shape + 1, rank - 1, stride);
	return destination;
}

void clothRtRowsToFlat (void *destination, const void *source, const void *shape, size_t rank, size_t stride) {
	packFlat(destination, source, header(shape, rank).length, sourceExtents(shape), rank, stride);
}

void clothRtRowsInsert (void *descriptor, size_t index, const void *row, size_t rank, size_t stride, size_t alignment) {
	ClothArray array = header(descriptor, rank);
	if (index > array.length) clothRtArrayIndexOutOfBounds();
	mergeRowShape(descriptor, row, rank);
	const ClothArray copy = copyBuffer(row, rank - 1, stride, alignment);
	const size_t     step = descriptorSize(rank - 1);
	clothRtArrayInsert(&array, index, row, step, _Alignof(ClothArray));
	storeHeader(array.data + index * step, rank - 1, copy);
	storeHeader(descriptor, rank, array);
}

void clothRtRowsSet (void *descriptor, size_t index, const void *row, size_t rank, size_t stride, size_t alignment) {
	const ClothArray array = header(descriptor, rank);
	if (index >= array.length) clothRtArrayIndexOutOfBounds();
	mergeRowShape(descriptor, row, rank);
	clothRtRowsAssign(array.data + index * descriptorSize(rank - 1), row, rank - 1, stride, alignment);
}

void clothRtRowsRemove (void *descriptor, size_t index, void *row, size_t rank) {
	ClothArray array = header(descriptor, rank);
	clothRtArrayRemoveAt(&array, index, row, descriptorSize(rank - 1), _Alignof(ClothArray));
	storeHeader(descriptor, rank, array);
}

void clothRtRowsReserve (void *descriptor, size_t minimum, size_t rank) {
	ClothArray array = header(descriptor, rank);
	clothRtArrayReserve(&array, minimum, descriptorSize(rank - 1), _Alignof(ClothArray));
	storeHeader(descriptor, rank, array);
}

void clothRtRowsClear (void *descriptor, size_t rank) {
	ClothArray array = header(descriptor, rank);
	releaseChildren(array, rank);
	clothRtArrayClear(&array);
	storeHeader(descriptor, rank, array);
}
