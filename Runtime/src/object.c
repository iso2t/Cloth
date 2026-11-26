// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/objects/Ownership.rst

#include <cloth/object.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct Backing {
	struct Backing *next;
};

enum Lifetime { Vacant, Live, Destroying };

struct ObjectSlot {
	uint32_t             generation;
	uint32_t             nextFree;
	enum Lifetime        state;
	uint64_t             owner;
	uint8_t              cleaning;
	void                *storage;
	struct Backing      *blocks;
	const void *const   *types;
	ClothDestructor      destroy;
	ClothDispose         dispose;
	void                *resource;
	ClothDispose         disposeResource;
	const ClothDispatch *dispatch;
	const ClothState    *active;
	struct Backing      *payloadBlock;
	void                *payload;
	ClothDispose         disposePayload;
	uint8_t              entered;
};

static struct ObjectSlot *slots;
static size_t             count, capacity;
static uint32_t           freeHead;
static atomic_flag        registryLock = ATOMIC_FLAG_INIT;

static void lockRegistry (void) {
	while (atomic_flag_test_and_set_explicit(&registryLock, memory_order_acquire)) {
	}
}

static void unlockRegistry (void) {
	atomic_flag_clear_explicit(&registryLock, memory_order_release);
}

static _Noreturn void failLocked (const char *reason) {
	unlockRegistry();
	fflush(stdout);
	fprintf(stderr, "Cloth runtime error: %s\n", reason);
	exit(1);
}

static void freePayload (struct ObjectSlot *slot) {
	if (slot->disposePayload && slot->payload) slot->disposePayload(slot->payload);
	free(slot->payloadBlock);
	slot->payloadBlock   = NULL;
	slot->payload        = NULL;
	slot->disposePayload = NULL;
}

static void freeStorage (struct ObjectSlot *slot) {
	if (slot->resource) slot->disposeResource(slot->resource);
	freePayload(slot);
	if (slot->dispose && slot->storage) slot->dispose(slot->storage);
	while (slot->blocks) {
		struct Backing *block = slot->blocks;
		slot->blocks          = block->next;
		free(block);
	}
}

static void shutdownRegistry (void) {
	// Process shutdown releases native storage. Cloth destructor effects run at explicit deletion.
	for (size_t i = 0; i < count; ++i) freeStorage(&slots[i]);
	free(slots);
	slots = NULL;
	count = capacity = 0;
	freeHead         = 0;
}

static struct ObjectSlot *lookup (uint64_t handle, int directSelf) {
	const uint32_t index = (uint32_t)handle;
	if (!index || index > count) failLocked("access to a deleted class instance");
	struct ObjectSlot *slot = &slots[index - 1];
	if (slot->generation != (uint32_t)(handle >> 32) || slot->state == Vacant) failLocked("access to a deleted class instance");
	if (slot->state == Destroying && !directSelf) failLocked("access to a class instance during destruction");
	return slot;
}

void clothRtObjectResource (const uint64_t handle, const ClothResourceOperation operation, void *argument, const ClothDispose dispose) {
	lockRegistry();
	struct ObjectSlot *slot = lookup(handle, 0);
	slot->disposeResource   = dispose;
	operation(&slot->resource, argument);
	unlockRegistry();
}

static void *allocateBlock (struct Backing **blocks, size_t bytes, const size_t alignment) {
	if (!bytes) bytes = 1;
	if (!alignment || (alignment & (alignment - 1)) || alignment - 1 > SIZE_MAX - sizeof(struct Backing) ||
	    bytes > SIZE_MAX - sizeof(struct Backing) - (alignment - 1))
		failLocked("class storage exceeds addressable memory");
	struct Backing *block = calloc(1, sizeof(*block) + bytes + alignment - 1);
	if (!block) failLocked("unable to allocate class storage");
	block->next           = *blocks;
	*blocks               = block;
	const uintptr_t start = (uintptr_t)(block + 1);
	return (unsigned char *)(block + 1) + ((-start) & (alignment - 1));
}

static uint32_t reserveSlot (void) {
	if (freeHead) {
		const uint32_t index = freeHead;
		freeHead             = slots[index - 1].nextFree;
		return index;
	}
	const size_t limit = SIZE_MAX / sizeof(*slots) < UINT32_MAX ? SIZE_MAX / sizeof(*slots) : UINT32_MAX;
	if (count == limit) failLocked("object identity space exhausted");
	if (count == capacity) {
		const size_t       next  = !capacity ? 16 : capacity > limit / 2 ? limit : capacity * 2;
		struct ObjectSlot *grown = realloc(slots, next * sizeof(*slots));
		if (!grown) failLocked("unable to allocate class storage");
		slots    = grown;
		capacity = next;
		if (!count && atexit(shutdownRegistry)) failLocked("unable to register class storage cleanup");
	}
	slots[count] = (struct ObjectSlot){ .generation = 1 };
	return (uint32_t)++count;
}

uint64_t clothRtObjectCreate (size_t bytes, size_t alignment, const void *const *types, ClothDestructor destroy, ClothDispose dispose) {
	lockRegistry();
	const uint32_t     index = reserveSlot();
	struct ObjectSlot *slot  = &slots[index - 1];
	slot->state              = Live;
	slot->types              = types;
	slot->destroy            = destroy;
	slot->dispose            = dispose;
	slot->storage            = allocateBlock(&slot->blocks, bytes, alignment);
	const uint64_t handle    = ((uint64_t)slot->generation << 32) | index;
	unlockRegistry();
	return handle;
}

uint64_t clothRtNewIdentity (void) {
	return clothRtObjectCreate(0, 1, NULL, NULL, NULL);
}

void *clothRtObjectData (uint64_t handle, uint8_t directSelf) {
	lockRegistry();
	void *storage = lookup(handle, directSelf)->storage;
	unlockRegistry();
	return storage;
}

void clothRtObjectCheckBorrow (uint64_t owner, uint8_t directSelf) {
	if (owner) (void)clothRtObjectData(owner, directSelf);
}

void *clothRtObjectBacking (uint64_t handle, size_t bytes, size_t alignment) {
	lockRegistry();
	void *storage = allocateBlock(&lookup(handle, 0)->blocks, bytes, alignment);
	unlockRegistry();
	return storage;
}

uint8_t clothRtObjectIs (uint64_t handle, const void *type, uint8_t directSelf) {
	if (!handle) return 0;
	lockRegistry();
	const void *const *view = lookup(handle, directSelf)->types;
	while (view && *view && *view != type) ++view;
	const uint8_t found = view && *view;
	unlockRegistry();
	return found;
}

void clothRtObjectDelete (uint64_t handle, uint64_t authority) {
	if (!handle) return;
	lockRegistry();
	struct ObjectSlot *slot = lookup(handle, 0);
	if (authority) lookup(authority, 1);
	if (slot->owner != authority) failLocked("only the owning list can delete its object");
	slot->state             = Destroying;
	ClothDestructor destroy = slot->destroy;
	unlockRegistry();
	if (destroy) destroy(handle);
	// A destructor may allocate another instance and relocate the slot table.
	lockRegistry();
	slot = lookup(handle, 1);
	freeStorage(slot);
	const uint32_t generation = slot->generation;
	*slot                     = (struct ObjectSlot){ .generation = generation };
	if (generation != UINT32_MAX) {
		++slot->generation;
		slot->nextFree = freeHead;
		freeHead       = (uint32_t)handle;
	}
	unlockRegistry();
}

static struct ObjectSlot *writableList (uint64_t owner) {
	struct ObjectSlot *list = lookup(owner, 1);
	if (list->cleaning) failLocked("cannot mutate an owning list during cleanup");
	return list;
}

void clothRtObjectAcquire (uint64_t owner, uint64_t child) {
	lockRegistry();
	writableList(owner);
	struct ObjectSlot *item = lookup(child, 0);
	if (item->owner) failLocked("class instance already belongs to an owning list");
	for (uint64_t ancestor = owner; ancestor; ancestor = lookup(ancestor, 1)->owner) {
		if (ancestor == child) failLocked("owning lists cannot form an ownership cycle");
	}
	item->owner = owner;
	unlockRegistry();
}

void clothRtObjectRelease (uint64_t owner, uint64_t child) {
	lockRegistry();
	writableList(owner);
	struct ObjectSlot *item = lookup(child, 0);
	if (item->owner != owner) failLocked("only the owning list can release its object");
	item->owner = 0;
	unlockRegistry();
}

void clothRtObjectCheckMutation (uint64_t owner) {
	lockRegistry();
	writableList(owner);
	unlockRegistry();
}

void clothRtObjectSetCleanup (uint64_t owner, uint8_t active) {
	lockRegistry();
	struct ObjectSlot *list = lookup(owner, 1);
	if (active && list->cleaning) failLocked("cannot mutate an owning list during cleanup");
	list->cleaning = active != 0;
	unlockRegistry();
}

void clothRtObjectConfigure (uint64_t self, const ClothDispatch *dispatch) {
	lockRegistry();
	struct ObjectSlot *slot = lookup(self, 0);
	slot->dispatch          = dispatch;
	slot->active            = dispatch->initial;
	unlockRegistry();
}

ClothMethod clothRtObjectMethod (uint64_t self, size_t index, uint8_t directSelf) {
	lockRegistry();
	struct ObjectSlot *slot   = lookup(self, directSelf);
	ClothMethod        method = slot->dispatch->methods[index];
	if (!method && slot->active) method = slot->active->methods[index];
	if (!method) failLocked("method is unavailable in the active state");
	unlockRegistry();
	return method;
}

uint8_t clothRtObjectBeginInitial (uint64_t self, const void *concreteType) {
	lockRegistry();
	struct ObjectSlot *slot  = lookup(self, 0);
	const uint8_t      begin = slot->types[0] == concreteType && !slot->entered;
	unlockRegistry();
	return begin;
}

const void *clothRtObjectState (uint64_t self, uint8_t directSelf) {
	if (!self) return NULL;
	lockRegistry();
	struct ObjectSlot *slot = lookup(self, directSelf);
	const void        *tag  = slot->active ? slot->active->tag : NULL;
	unlockRegistry();
	return tag;
}

void *clothRtObjectEnter (uint64_t self, const void *tag, size_t bytes, size_t alignment, ClothDispose dispose) {
	lockRegistry();
	struct ObjectSlot *slot  = lookup(self, 0);
	const ClothState  *state = slot->dispatch->states;
	while (state->tag && state->tag != tag) ++state;
	if (!state->tag) failLocked("state is unavailable on this class");
	freePayload(slot);
	slot->active         = state;
	slot->entered        = 1;
	slot->disposePayload = dispose;
	void *payload        = bytes ? allocateBlock(&slot->payloadBlock, bytes, alignment) : NULL;
	slot->payload        = payload;
	unlockRegistry();
	return payload;
}

void *clothRtObjectPayload (uint64_t self, const void *tag) {
	lockRegistry();
	struct ObjectSlot *slot = lookup(self, 1);
	if (!slot->active || slot->active->tag != tag || !slot->entered) failLocked("state payload is unavailable");
	void *payload = slot->payload;
	unlockRegistry();
	return payload;
}
