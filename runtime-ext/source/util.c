#include <gctypes.h>

u32 align_down(u32 num, u32 align_as)
{
    return num & -align_as;
}

u32 align_up(u32 num, u32 align_as)
{
    return (num + align_as - 1) & -align_as;
}
