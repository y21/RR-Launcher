/**
 * Ultra cursed.
 */

#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#ifndef STANDALONE
#include "dol.h"
#endif

#ifdef STANDALONE
#define RRC_DOL_SECTION_COUNT 18

struct rrc_dol
{
    u32 section[RRC_DOL_SECTION_COUNT];
    u32 section_addr[RRC_DOL_SECTION_COUNT];
    u32 section_size[RRC_DOL_SECTION_COUNT];

    u32 bss_addr;
    u32 bss_size;
    u32 entry_point;
};
#endif
#include <stdio.h>
void patch_dol(struct rrc_dol *dol, void (*dc_flush_range)(void *, u32))
{
    uint32_t new_sp = 0x808ffe00;
    asm volatile("mr 1, %0" : : "r"(new_sp));

    // First, zero BSS.
    register u64 *bss8 = (u64 *)dol->bss_addr;
    register u8 *start = (u8 *)bss8;
    register u8 *end = start + dol->bss_size;

    // For now, we do 8-byte-at-a-time writes in the main loop, since this usually involves around 1 MB.
    // The WFC patcher uses the asm instruction  dcbz  which can zero up to 32 bytes at once,
    // which we may want to consider using later too.
    // NOTE: bss_size / 8 can have a remainder if the size is not divisible by 8,
    // which is handled by doing one-byte writes for the last few bytes after this main loop
    // (the WFC patcher doesn't even handle that case?).
    for (; (u8 *)bss8 < end; bss8++)
    {
        *bss8 = 0;
    }

    // Zero the last <8 bytes sequentially.
    for (register u8 *bss = (u8 *)bss8; bss < end; bss++)
    {
        *bss = 0;
    }

    dc_flush_range((void *)dol->bss_addr, dol->bss_size);

    // Next, copy the sections.
    for (register u8 section_index = 0; section_index < RRC_DOL_SECTION_COUNT; section_index++)
    {
        register u8 *from = (u8 *)dol + dol->section[section_index];
        register u8 *to = (u8 *)dol->section_addr[section_index];
        register u32 size = dol->section_size[section_index];

        for (register u32 i = 0; i < size; i++)
        {
            to[i] = from[i];
        }

        dc_flush_range((void *)to, size);
    }

    ((void (*)())dol->entry_point)();

    while (1)
    {
    }
}
