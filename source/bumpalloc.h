#ifndef RRC_BUMPALLOC_H
#define RRC_BUMPALLOC_H

#include <types.h>

struct rrc_bump_id
{
    u32 offset;
};

/**
 * A very simple bump allocator for fast allocation avoiding fragmentation where everything is deallocated at once.
 * One way in which this differs from typical implementations is this implementation will return opaque indices instead of pointers.
 * There are a few reasons for this:
 * - We can have a contiguous block of memory that can be reallocated because no address stability is guaranteed
 * - It can be very easily memcpy'd into MEM1 later and indices can still be easily mapped to addresses
 */
struct rrc_bump_alloc
{
    void *data;
    u32 capacity;
    u32 len;
};

void rrc_bump_alloc_init(struct rrc_bump_alloc *alloc, u32 initial_capacity);

void rrc_bump_alloc_destroy(struct rrc_bump_alloc *alloc);

struct rrc_bump_id rrc_bump_alloc(struct rrc_bump_alloc *alloc, const void *value, u32 size, u32 align);

void *rrc_bump_get(struct rrc_bump_alloc *alloc, struct rrc_bump_id id);

#endif
