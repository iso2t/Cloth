// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_RUNTIME_OBJECT_H
#define CLOTH_RUNTIME_OBJECT_H

#include <stddef.h>
#include <stdint.h>

#include "abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ClothDestructor)(uint64_t self);
typedef void (*ClothDispose)(void *storage);
typedef void (*ClothMethod)(void);
typedef void (*ClothResourceOperation)(void **resource, void *argument);

// Keep the C names used by existing clients of this header.
typedef ClothDestructor        CLOTH_C_NAME(destructor);
typedef ClothDispose           CLOTH_C_NAME(dispose);
typedef ClothMethod            CLOTH_C_NAME(method);
typedef ClothResourceOperation CLOTH_C_NAME(resource_operation);

typedef struct CLOTH_C_NAME(state) {
	const void        *tag;
	const ClothMethod *methods;
} ClothState;

typedef struct CLOTH_C_NAME(dispatch) {
	const ClothMethod *methods;
	const ClothState  *states;
	const ClothState  *initial;
} ClothDispatch;

CLOTH_RT_API(void, clothRtObjectResource, object_resource, const uint64_t handle, const ClothResourceOperation operation, void *argument,
             const ClothDispose dispose);
CLOTH_RT_API(uint64_t, clothRtObjectCreate, object_create, size_t bytes, size_t alignment, const void *const *types, ClothDestructor destroy,
             ClothDispose dispose);
CLOTH_RT_API(uint64_t, clothRtNewIdentity, new_identity, void);
CLOTH_RT_API(void *, clothRtObjectData, object_data, uint64_t handle, uint8_t directSelf);
CLOTH_RT_API(void, clothRtObjectCheckBorrow, object_check_borrow, uint64_t owner, uint8_t directSelf);
CLOTH_RT_API(void *, clothRtObjectBacking, object_backing, uint64_t handle, size_t bytes, size_t alignment);
CLOTH_RT_API(uint8_t, clothRtObjectIs, object_is, uint64_t handle, const void *type, uint8_t directSelf);
CLOTH_RT_API(void, clothRtObjectDelete, object_delete, uint64_t handle, uint64_t authority);
CLOTH_RT_API(void, clothRtObjectAcquire, object_acquire, uint64_t owner, uint64_t child);
CLOTH_RT_API(void, clothRtObjectRelease, object_release, uint64_t owner, uint64_t child);
CLOTH_RT_API(void, clothRtObjectCheckMutation, object_check_mutation, uint64_t owner);
CLOTH_RT_API(void, clothRtObjectSetCleanup, object_set_cleanup, uint64_t owner, uint8_t active);
CLOTH_RT_API(void, clothRtObjectConfigure, object_configure, uint64_t self, const ClothDispatch *dispatch);
CLOTH_RT_API(ClothMethod, clothRtObjectMethod, object_method, uint64_t self, size_t index, uint8_t directSelf);
CLOTH_RT_API(uint8_t, clothRtObjectBeginInitial, object_begin_initial, uint64_t self, const void *concreteType);
CLOTH_RT_API(const void *, clothRtObjectState, object_state, uint64_t self, uint8_t directSelf);
CLOTH_RT_API(void *, clothRtObjectEnter, object_enter, uint64_t self, const void *tag, size_t bytes, size_t alignment, ClothDispose dispose);
CLOTH_RT_API(void *, clothRtObjectPayload, object_payload, uint64_t self, const void *tag);

#ifdef __cplusplus
}
#endif

#endif
