#ifndef RRC_RUNTIME_EXT_UTIL
#define RRC_RUNTIME_EXT_UTIL

#include <gctypes.h>

void OS_Report(const char *, ...);
void OS_Fatal(u32 *, u32 *, const char *);


#define FATAL(msg)                   \
    do                               \
    {                                \
        u32 fg = 0xFFFFFFFF, bg = 0; \
        OS_Fatal(&fg, &bg, msg);     \
        while (1)                    \
            ;                        \
    } while (0);

u32 align_down(u32 num, u32 align_as);
u32 align_up(u32 num, u32 align_as);

#endif
