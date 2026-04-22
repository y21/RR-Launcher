#ifndef SHARED_PATHTREE_H
#define SHARED_PATHTREE_H

#include "types.h"

struct rrc_path_component
{
    const char *component;
    struct rrc_path_component *parent;
    u32 hash;
};

#endif
