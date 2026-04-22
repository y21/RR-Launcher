#include "dense_hashset.h"

#define TODO(x)

void rrc_dense_hashset_init(struct rrc_dense_hashset *set, u32 initial_capacity, u32 key_size)
{
    set->data = malloc(initial_capacity * key_size);
    if (!set->data)
    {
        TODO("handle out of memory");
    }
    set->len = 0;
    set->capacity = initial_capacity;
    set->key_size = key_size;
}

void *rrc_dense_hashset_insert(struct rrc_dense_hashset *set, void *key)
{
}
