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

#ifdef __cplusplus
}
#endif
