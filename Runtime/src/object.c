// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// See Documentation/language/objects/Ownership.rst

#include <cloth/object.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Backing {
	struct Backing *next;
};

enum Lifetime { Vacant, Live, Destroying, BoxedNumber };

struct NumberBox {
	struct NumberBox      *next;
	const ClothNumberType *type;
	uint64_t               handle;
	unsigned char          value[];
};

// Ordinary object handles have a nonzero generation and a nonzero slot.
// The 2^32 handles from 1 through 2^32 encode every signed int without allocating.
static const uint64_t    LAST_INLINE_INT = UINT64_C(1) << 32;
static struct NumberBox *numberBuckets[1021];

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
	memset(numberBuckets, 0, sizeof(numberBuckets));
	slots = NULL;
	count = capacity = 0;
	freeHead         = 0;
}

static struct ObjectSlot *lookup (const uint64_t handle, const int directSelf) {
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
	if (!alignment || alignment & alignment - 1 || alignment - 1 > SIZE_MAX - sizeof(struct Backing) ||
	    bytes > SIZE_MAX - sizeof(struct Backing) - (alignment - 1))
		failLocked("class storage exceeds addressable memory");
	struct Backing *block = calloc(1, sizeof(*block) + bytes + alignment - 1);
	if (!block) failLocked("unable to allocate class storage");
	block->next           = *blocks;
	*blocks               = block;
	const uintptr_t start = (uintptr_t)(block + 1);
	return (unsigned char *)(block + 1) + (-start & alignment - 1);
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

uint64_t clothRtObjectCreate (const size_t bytes, const size_t alignment, const void *const *types, const ClothDestructor destroy, const ClothDispose dispose) {
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

void *clothRtObjectData (const uint64_t handle, const uint8_t directSelf) {
	lockRegistry();
	void *storage = lookup(handle, directSelf)->storage;
	unlockRegistry();
	return storage;
}

void clothRtObjectCheckBorrow (const uint64_t owner, const uint8_t directSelf) {
	if (owner) (void)clothRtObjectData(owner, directSelf);
}

void *clothRtObjectBacking (const uint64_t handle, const size_t bytes, const size_t alignment) {
	lockRegistry();
	void *storage = allocateBlock(&lookup(handle, 0)->blocks, bytes, alignment);
	unlockRegistry();
	return storage;
}

uint8_t clothRtObjectIs (const uint64_t handle, const void *type, const uint8_t directSelf) {
	if (!handle) return 0;
	lockRegistry();
	const void *const *view = lookup(handle, directSelf)->types;
	while (view && *view && *view != type) ++view;
	const uint8_t found = view && *view;
	unlockRegistry();
	return found;
}

uint64_t clothRtNumberBox (const ClothNumberType *type, const void *value, const uint8_t present) {
	if (!present) return 0;
	lockRegistry();
	if (type->bytes > SIZE_MAX - sizeof(struct NumberBox)) failLocked("numeric value exceeds addressable memory");
	const unsigned char *bytes = value;
	uint32_t             hash  = (uint32_t)(uintptr_t)type->views[0];
	for (size_t i = 0; i < type->bytes; ++i) hash = (hash ^ bytes[i]) * UINT32_C(16777619);
	struct NumberBox **bucket = &numberBuckets[hash % (sizeof(numberBuckets) / sizeof(*numberBuckets))];
	for (const struct NumberBox *box = *bucket; box; box = box->next) {
		if (box->type == type && !memcmp(box->value, value, type->bytes)) {
			const uint64_t handle = box->handle;
			unlockRegistry();
			return handle;
		}
	}
	const uint32_t     index  = reserveSlot();
	struct ObjectSlot *slot   = &slots[index - 1];
	struct NumberBox  *box    = allocateBlock(&slot->blocks, sizeof(*box) + type->bytes, _Alignof(struct NumberBox));
	const uint64_t     handle = ((uint64_t)slot->generation << 32) | index;
	*box                      = (struct NumberBox){ .next = *bucket, .type = type, .handle = handle };
	memcpy(box->value, value, type->bytes);
	*bucket       = box;
	slot->state   = BoxedNumber;
	slot->types   = type->views;
	slot->storage = box;
	unlockRegistry();
	return handle;
}

uint8_t clothRtNumberIs (const uint64_t value, const void *type, const uint8_t matchesInt, const uint8_t directSelf) {
	if (value <= LAST_INLINE_INT) return value && matchesInt;
	return clothRtObjectIs(value, type, directSelf);
}

uint8_t clothRtNumberEqual (const uint64_t left, const uint64_t right, const uint8_t leftSelf, const uint8_t rightSelf) {
	if (!left || !right || (left <= LAST_INLINE_INT && right <= LAST_INLINE_INT)) return left == right;
	lockRegistry();
	const struct ObjectSlot *a = left > LAST_INLINE_INT ? lookup(left, leftSelf) : NULL;
	const struct ObjectSlot *b = right > LAST_INLINE_INT ? lookup(right, rightSelf) : NULL;
	const struct NumberBox  *x = a && a->state == BoxedNumber ? a->storage : NULL;
	const struct NumberBox  *y = b && b->state == BoxedNumber ? b->storage : NULL;
	unlockRegistry();
	// Floating equality handles signed zeros and NaNs. Interned integer handles already compare by exact type and value.
	if (x && y && x->type == y->type && x->type->equal) return x->type->equal(x->value, y->value);
	return left == right;
}

void clothRtObjectDelete (const uint64_t handle, const uint64_t authority) {
	if (!handle) return;
	lockRegistry();
	struct ObjectSlot *slot = lookup(handle, 0);
	if (slot->state == BoxedNumber) failLocked("cannot delete a boxed number");
	if (authority) lookup(authority, 1);
	if (slot->owner != authority) failLocked("only the owning list can delete its object");
	slot->state                   = Destroying;
	const ClothDestructor destroy = slot->destroy;
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

static struct ObjectSlot *writableList (const uint64_t owner) {
	struct ObjectSlot *list = lookup(owner, 1);
	if (list->cleaning) failLocked("cannot mutate an owning list during cleanup");
	return list;
}

void clothRtObjectAcquire (const uint64_t owner, const uint64_t child) {
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

void clothRtObjectRelease (const uint64_t owner, const uint64_t child) {
	lockRegistry();
	writableList(owner);
	struct ObjectSlot *item = lookup(child, 0);
	if (item->owner != owner) failLocked("only the owning list can release its object");
	item->owner = 0;
	unlockRegistry();
}

void clothRtObjectCheckMutation (const uint64_t owner) {
	lockRegistry();
	writableList(owner);
	unlockRegistry();
}

void clothRtObjectSetCleanup (const uint64_t owner, const uint8_t active) {
	lockRegistry();
	struct ObjectSlot *list = lookup(owner, 1);
	if (active && list->cleaning) failLocked("cannot mutate an owning list during cleanup");
	list->cleaning = active != 0;
	unlockRegistry();
}

void clothRtObjectConfigure (const uint64_t self, const ClothDispatch *dispatch) {
	lockRegistry();
	struct ObjectSlot *slot = lookup(self, 0);
	slot->dispatch          = dispatch;
	slot->active            = dispatch->initial;
	unlockRegistry();
}

ClothMethod clothRtObjectMethod (const uint64_t self, const size_t index, const uint8_t directSelf) {
	lockRegistry();
	const struct ObjectSlot *slot   = lookup(self, directSelf);
	ClothMethod              method = slot->dispatch->methods[index];
	if (!method && slot->active) method = slot->active->methods[index];
	if (!method) failLocked("method is unavailable in the active state");
	unlockRegistry();
	return method;
}

uint8_t clothRtObjectBeginInitial (const uint64_t self, const void *concreteType) {
	lockRegistry();
	const struct ObjectSlot *slot  = lookup(self, 0);
	const uint8_t            begin = slot->types[0] == concreteType && !slot->entered;
	unlockRegistry();
	return begin;
}

const void *clothRtObjectState (const uint64_t self, const uint8_t directSelf) {
	if (!self) return NULL;
	lockRegistry();
	const struct ObjectSlot *slot = lookup(self, directSelf);
	const void              *tag  = slot->active ? slot->active->tag : NULL;
	unlockRegistry();
	return tag;
}

void *clothRtObjectEnter (const uint64_t self, const void *tag, const size_t bytes, const size_t alignment, const ClothDispose dispose) {
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

void *clothRtObjectPayload (const uint64_t self, const void *tag) {
	lockRegistry();
	const struct ObjectSlot *slot = lookup(self, 1);
	if (!slot->active || slot->active->tag != tag || !slot->entered) failLocked("state payload is unavailable");
	void *payload = slot->payload;
	unlockRegistry();
	return payload;
}
