#ifndef RRC_DENSE_HASHSET_H
#define RRC_DENSE_HASHSET_H

#include <types.h>

struct rrc_dense_hashset
{
    // Contiguous array where the keys are stored next to each other. The keys are ordered by hash(key) % len.
    void *data;
    u32 len;
    u32 capacity;
    u32 key_size;
};

void rrc_dense_hashset_init(struct rrc_dense_hashset *set, u32 initial_capacity, u32 key_size);

void *rrc_dense_hashset_insert(struct rrc_dense_hashset *set, void *key);

#endif
