// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_RUNTIME_OBJECT_H
#define CLOTH_RUNTIME_OBJECT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cloth_destructor)(uint64_t self);
typedef void (*cloth_dispose)(void *storage);
typedef void (*cloth_method)(void);

struct cloth_state {
	const void         *tag;
	const cloth_method *methods;
};

struct cloth_dispatch {
	const cloth_method       *methods;
	const struct cloth_state *states;
	const struct cloth_state *initial;
};

uint64_t cloth_rt_new_identity (void);
uint64_t cloth_rt_object_create (size_t bytes, size_t alignment, const void *const *types, cloth_destructor destroy, cloth_dispose dispose);
void    *cloth_rt_object_data (uint64_t handle, uint8_t direct_self);
void     cloth_rt_object_check_borrow (uint64_t owner, uint8_t direct_self);
void    *cloth_rt_object_backing (uint64_t handle, size_t bytes, size_t alignment);
uint8_t  cloth_rt_object_is (uint64_t handle, const void *type, uint8_t direct_self);
void     cloth_rt_object_delete (uint64_t handle, uint64_t authority);
void     cloth_rt_object_acquire (uint64_t owner, uint64_t child);
void     cloth_rt_object_release (uint64_t owner, uint64_t child);
void     cloth_rt_object_check_mutation (uint64_t owner);
void     cloth_rt_object_set_cleanup (uint64_t owner, uint8_t active);
void         cloth_rt_object_configure (uint64_t self, const struct cloth_dispatch *dispatch);
cloth_method cloth_rt_object_method (uint64_t self, size_t slot, uint8_t direct_self);
uint8_t      cloth_rt_object_begin_initial (uint64_t self, const void *concrete_type);
const void  *cloth_rt_object_state (uint64_t self, uint8_t direct_self);
void        *cloth_rt_object_enter (uint64_t self, const void *tag, size_t bytes, size_t alignment, cloth_dispose dispose);
void        *cloth_rt_object_payload (uint64_t self, const void *tag);

#ifdef __cplusplus
}
#endif

#endif
