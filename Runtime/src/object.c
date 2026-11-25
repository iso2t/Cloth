// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/objects/Ownership.rst

#include <cloth/object.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

struct backing {
	struct backing *next;
};

enum lifetime { VACANT, LIVE, DESTROYING };

struct object_slot {
	uint32_t           generation;
	uint32_t           next_free;
	enum lifetime      state;
	uint64_t           owner;
	uint8_t            cleaning;
	void              *storage;
	struct backing    *blocks;
	const void *const *types;
	cloth_destructor   destroy;
	cloth_dispose      dispose;
};

static struct object_slot *slots;
static size_t              count, capacity;
static uint32_t            free_head;
static atomic_flag         registry_lock = ATOMIC_FLAG_INIT;

static void lock_registry (void) {
	while (atomic_flag_test_and_set_explicit(&registry_lock, memory_order_acquire)) {
	}
}

static void unlock_registry (void) {
	atomic_flag_clear_explicit(&registry_lock, memory_order_release);
}

static _Noreturn void fail_locked (const char *reason) {
	unlock_registry();
	fflush(stdout);
	fprintf(stderr, "Cloth runtime error: %s\n", reason);
	exit(1);
}

static void free_storage (struct object_slot *slot) {
	if (slot->dispose && slot->storage) slot->dispose(slot->storage);
	while (slot->blocks) {
		struct backing *block = slot->blocks;
		slot->blocks          = block->next;
		free(block);
	}
}

static void shutdown_registry (void) {
	// Process shutdown releases native storage. Cloth destructor effects run at explicit deletion.
	for (size_t i = 0; i < count; ++i) free_storage(&slots[i]);
	free(slots);
	slots = NULL;
	count = capacity = 0;
	free_head        = 0;
}

static struct object_slot *lookup (uint64_t handle, int direct_self) {
	const uint32_t index = (uint32_t)handle;
	if (!index || index > count) fail_locked("access to a deleted class instance");
	struct object_slot *slot = &slots[index - 1];
	if (slot->generation != (uint32_t)(handle >> 32) || slot->state == VACANT) fail_locked("access to a deleted class instance");
	if (slot->state == DESTROYING && !direct_self) fail_locked("access to a class instance during destruction");
	return slot;
}

static void *allocate_block (struct object_slot *slot, size_t bytes, size_t alignment) {
	if (!bytes) bytes = 1;
	if (!alignment || (alignment & (alignment - 1)) || alignment - 1 > SIZE_MAX - sizeof(struct backing) ||
	    bytes > SIZE_MAX - sizeof(struct backing) - (alignment - 1))
		fail_locked("class storage exceeds addressable memory");
	struct backing *block = calloc(1, sizeof(*block) + bytes + alignment - 1);
	if (!block) fail_locked("unable to allocate class storage");
	block->next           = slot->blocks;
	slot->blocks          = block;
	const uintptr_t start = (uintptr_t)(block + 1);
	return (unsigned char *)(block + 1) + ((-start) & (alignment - 1));
}

static uint32_t reserve_slot (void) {
	if (free_head) {
		const uint32_t index = free_head;
		free_head            = slots[index - 1].next_free;
		return index;
	}
	const size_t limit = SIZE_MAX / sizeof(*slots) < UINT32_MAX ? SIZE_MAX / sizeof(*slots) : UINT32_MAX;
	if (count == limit) fail_locked("object identity space exhausted");
	if (count == capacity) {
		const size_t        next  = !capacity ? 16 : capacity > limit / 2 ? limit : capacity * 2;
		struct object_slot *grown = realloc(slots, next * sizeof(*slots));
		if (!grown) fail_locked("unable to allocate class storage");
		slots    = grown;
		capacity = next;
		if (!count && atexit(shutdown_registry)) fail_locked("unable to register class storage cleanup");
	}
	slots[count] = (struct object_slot){ .generation = 1 };
	return (uint32_t)++count;
}

uint64_t cloth_rt_object_create (size_t bytes, size_t alignment, const void *const *types, cloth_destructor destroy, cloth_dispose dispose) {
	lock_registry();
	const uint32_t      index = reserve_slot();
	struct object_slot *slot  = &slots[index - 1];
	slot->state               = LIVE;
	slot->types               = types;
	slot->destroy             = destroy;
	slot->dispose             = dispose;
	slot->storage             = allocate_block(slot, bytes, alignment);
	const uint64_t handle     = ((uint64_t)slot->generation << 32) | index;
	unlock_registry();
	return handle;
}

uint64_t cloth_rt_new_identity (void) {
	return cloth_rt_object_create(0, 1, NULL, NULL, NULL);
}

void *cloth_rt_object_data (uint64_t handle, uint8_t direct_self) {
	lock_registry();
	void *storage = lookup(handle, direct_self)->storage;
	unlock_registry();
	return storage;
}

void *cloth_rt_object_backing (uint64_t handle, size_t bytes, size_t alignment) {
	lock_registry();
	void *storage = allocate_block(lookup(handle, 0), bytes, alignment);
	unlock_registry();
	return storage;
}

uint8_t cloth_rt_object_is (uint64_t handle, const void *type, uint8_t direct_self) {
	if (!handle) return 0;
	lock_registry();
	const void *const *view = lookup(handle, direct_self)->types;
	while (view && *view && *view != type) ++view;
	const uint8_t found = view && *view;
	unlock_registry();
	return found;
}

void cloth_rt_object_delete (uint64_t handle, uint64_t authority) {
	if (!handle) return;
	lock_registry();
	struct object_slot *slot = lookup(handle, 0);
	if (authority) lookup(authority, 1);
	if (slot->owner != authority) fail_locked("only the owning list can delete its object");
	slot->state              = DESTROYING;
	cloth_destructor destroy = slot->destroy;
	unlock_registry();
	if (destroy) destroy(handle);
	// A destructor may allocate another instance and relocate the slot table.
	lock_registry();
	slot = lookup(handle, 1);
	free_storage(slot);
	const uint32_t generation = slot->generation;
	*slot                     = (struct object_slot){ .generation = generation };
	if (generation != UINT32_MAX) {
		++slot->generation;
		slot->next_free = free_head;
		free_head       = (uint32_t)handle;
	}
	unlock_registry();
}

static struct object_slot *writable_list (uint64_t owner) {
	struct object_slot *list = lookup(owner, 1);
	if (list->cleaning) fail_locked("cannot mutate an owning list during cleanup");
	return list;
}

void cloth_rt_object_acquire (uint64_t owner, uint64_t child) {
	lock_registry();
	writable_list(owner);
	struct object_slot *item = lookup(child, 0);
	if (item->owner) fail_locked("class instance already belongs to an owning list");
	for (uint64_t ancestor = owner; ancestor; ancestor = lookup(ancestor, 1)->owner) {
		if (ancestor == child) fail_locked("owning lists cannot form an ownership cycle");
	}
	item->owner = owner;
	unlock_registry();
}

void cloth_rt_object_release (uint64_t owner, uint64_t child) {
	lock_registry();
	writable_list(owner);
	struct object_slot *item = lookup(child, 0);
	if (item->owner != owner) fail_locked("only the owning list can release its object");
	item->owner = 0;
	unlock_registry();
}

void cloth_rt_object_check_mutation (uint64_t owner) {
	lock_registry();
	writable_list(owner);
	unlock_registry();
}

void cloth_rt_object_set_cleanup (uint64_t owner, uint8_t active) {
	lock_registry();
	struct object_slot *list = lookup(owner, 1);
	if (active && list->cleaning) fail_locked("cannot mutate an owning list during cleanup");
	list->cleaning = active != 0;
	unlock_registry();
}
