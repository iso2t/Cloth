// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <stddef.h>

#include "abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CLOTH_C_NAME(array) {
	unsigned char *data;
	size_t         length;
	size_t         capacity;
	void          *owner;
} ClothArray;

CLOTH_RT_API(void, clothRtArrayClear, array_clear, ClothArray *array);
CLOTH_RT_API(void, clothRtArrayAssign, array_assign, ClothArray *array, const void *data, size_t count, size_t stride, size_t alignment);
CLOTH_RT_API(void, clothRtArrayReserve, array_reserve, ClothArray *array, size_t minimum, size_t stride, size_t alignment);
CLOTH_RT_API(void, clothRtArrayInsert, array_insert, ClothArray *array, size_t index, const void *cell, size_t stride, size_t alignment);
CLOTH_RT_API(void, clothRtArrayAppend, array_append, ClothArray *array, const void *cell, size_t stride, size_t alignment);
CLOTH_RT_API(void, clothRtArrayRemoveAt, array_remove_at, ClothArray *array, size_t index, void *removed, size_t stride, size_t alignment);
CLOTH_RT_API(void, clothRtArrayRelease, array_release, ClothArray *array);

typedef struct {
	size_t stride;
	size_t alignment;
	void (*copy)(void *destination, const void *source);
	void (*dispose)(void *cell);
} ClothCellOperations;

CLOTH_RT_API(void, clothRtCellsAssign, cells_assign, ClothArray *array, const void *data, size_t count, const ClothCellOperations *cells);
CLOTH_RT_API(void, clothRtCellsClear, cells_clear, ClothArray *array, const ClothCellOperations *cells);
CLOTH_RT_API(void, clothRtCellsRelease, cells_release, ClothArray *array, const ClothCellOperations *cells);
CLOTH_RT_API(void *, clothRtCellBacking, cell_backing, void **owner, size_t bytes, size_t alignment);
CLOTH_RT_API(void, clothRtCellFree, cell_free, void *owner);

// Rectangular descriptors place the inner extents between length and capacity.
CLOTH_RT_API(void, clothRtRowsAssign, rows_assign, void *destination, const void *source, size_t rank, size_t stride, size_t alignment);
CLOTH_RT_API(void, clothRtRowsFromFlat, rows_from_flat, void *destination, const void *source, const void *shape, size_t rank, size_t stride, size_t alignment);
CLOTH_RT_API(void, clothRtRowsToFlat, rows_to_flat, void *destination, const void *source, const void *shape, size_t rank, size_t stride);
CLOTH_RT_API(void, clothRtRowsInsert, rows_insert, void *array, size_t index, const void *row, size_t rank, size_t stride, size_t alignment);
CLOTH_RT_API(void, clothRtRowsSet, rows_set, void *array, size_t index, const void *row, size_t rank, size_t stride, size_t alignment);
CLOTH_RT_API(void, clothRtRowsRemove, rows_remove, void *array, size_t index, void *row, size_t rank);
CLOTH_RT_API(void, clothRtRowsReserve, rows_reserve, void *array, size_t minimum, size_t rank);
CLOTH_RT_API(void, clothRtRowsClear, rows_clear, void *array, size_t rank);
CLOTH_RT_API(void, clothRtRowsRelease, rows_release, void *array, size_t rank);

#ifdef __cplusplus
}
#endif
