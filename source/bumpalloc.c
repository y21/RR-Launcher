#include <stdlib.h>
#include <string.h>

#include "bumpalloc.h"

#define TODO(x)

void rrc_bump_alloc_init(struct rrc_bump_alloc *alloc, u32 initial_capacity)
{
    alloc->data = malloc(initial_capacity);
    if (!alloc->data)
    {
        TODO("handle out of memory");
    }

    alloc->capacity = initial_capacity;
    alloc->len = 0;
}

void rrc_bump_alloc_destroy(struct rrc_bump_alloc *alloc)
{
    free(alloc->data);
}

#include <stdio.h>

struct rrc_bump_id rrc_bump_alloc(struct rrc_bump_alloc *alloc, const void *value, u32 size, u32 align)
{
    TODO("handle alignment")

    if (alloc->len + size > alloc->capacity)
    {
        // Reallocate.
        u32 new_capacity = alloc->capacity * 2;
        alloc->data = realloc(alloc->data, new_capacity);
        printf("[BUMP] realloc! %d -> %d (%p)\n", alloc->capacity, new_capacity, alloc->data);
        if (!alloc->data)
        {
            TODO("handle out of memory");
        }
        alloc->capacity = new_capacity;
    }

    memcpy((char *)alloc->data + alloc->len, value, size);
    u32 old_len = alloc->len;
    alloc->len += size;

    return (struct rrc_bump_id){
        .offset = old_len,
    };
}

void *rrc_bump_get(struct rrc_bump_alloc *alloc, struct rrc_bump_id id)
{
    return (char *)alloc->data + id.offset;
}
