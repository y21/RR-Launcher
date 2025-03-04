/*
    main.c - entry point and key init routines

    Copyright (C) 2025  Retro Rewind Team

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <unistd.h>
#include <wiisocket.h>
#include <ogc/wiilaunch.h>
#include <string.h>

#include "util.h"
#include "shutdown.h"
#include "di.h"
#include "time.h"
#include "loader.h"
#include "dol.h"

/* 100ms */
#define DISKCHECK_DELAY 100000

u32 align_down(u32 num, u32 align_as)
{
    return num & -align_as;
}

u32 align_up(u32 num, u32 align_as)
{
    return (num + align_as - 1) & -align_as;
}

void *wiisocket_init_thread_callback(void *res)
{
    // Note: the void* given to us is an int* that lives in the main function,
    // because we want to assert everything from the main thread rather than asserting in here
    // so that we don't potentially exit(1) from another thread while the main thread is doing some important reading/patching.
    *(int *)res = wiisocket_init();
    rrc_dbg_printf("network initialised with status %d\n", *(int *)res);
    return NULL;
}

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

void video_init()
{
    // Initialise the video system
    VIDEO_Init();

    // Obtain the preferred video mode from the system
    // This will correspond to the settings in the Wii menu
    rmode = VIDEO_GetPreferredMode(NULL);
    // Allocate memory for the display in the uncached region
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    // Initialise the console, required for printf
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    // SYS_STDIO_Report(true);
    // Set up the video registers with the chosen mode
    VIDEO_Configure(rmode);
    // Tell the video hardware where our display memory is
    VIDEO_SetNextFramebuffer(xfb);
    // Make the display visible
    VIDEO_SetBlack(false);
    // Flush the video register changes to the hardware
    VIDEO_Flush();
    // Wait for Video setup to complete
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
}

extern void patch_dol(struct rrc_dol *dol, void (*dc_flush_range)(void *, u32));

int main(int argc, char **argv)
{
    s64 systime_start = gettime();
    // response codes for various library functions
    int res;

    // handles pressing the home button to exit
    rrc_dbg_printf("spawn shutdown background thread\n");
    lwp_t shutdown_thread = rrc_shutdown_spawn();

    // Initializing the network can take fairly long (seconds).
    // It's not really needed right away anyway so we can do it on another thread in parallel to some of the disk reading
    // and join on it later when we actually need it.
    rrc_dbg_printf("spawn network init thread\n");
    int wiisocket_res;
    lwp_t wiisocket_thread;
    res = LWP_CreateThread(&wiisocket_thread, wiisocket_init_thread_callback, &wiisocket_res, NULL, 0, RRC_LWP_PRIO_IDLE);
    RRC_ASSERTEQ(res, RRC_LWP_OK, "LWP_CreateThread for wiisocket init");

    // init video, setup console framebuffer
    video_init();

    // seek to start
    printf("\x1b[0;0H");

    rrc_dbg_printf("init controllers\n");
    res = WPAD_Init();
    RRC_ASSERTEQ(res, WPAD_ERR_NONE, "WPAD_Init");

    rrc_dbg_printf("init disk drive\n");
    int fd = rrc_di_init();
    RRC_ASSERT(fd != 0, "rrc_di_init");

    /*  We should load Mario Kart Wii before doing anything else */
    res = rrc_loader_await_mkw();
    CHECK_EXIT();

    /*  TODO: From this point in the full launcher we will set a timeout of, say, 2 seconds.
        If some button such as A is pressed in that window, initialise the full channel.
        Otherwise, just go ahead and load the game. This saves the user time because on
        most occasions all you want to do is play and not do anything in the settings.

        For now, we're just loading the game. However, we need to do this in stages instead
        of in one big routine (like the WFC launcher). This is because while we're reading
        all of the necessary sections from disc, we're still initalising the network and
        fetching version information in the background thread. This thread will return
        version information as read from the API's text file, so when we join that thread,
        we compare those versions against our local version.txt and then ask the user if they
        want to update (if necessary). This replaces files on the SD card, so once all that
        is done, we can finally read patches from the SD, apply them, and load the game.
        So, all disc reading can be done in advance up to the point we read patch information
        from the SD.
    */

    // We've identified the game. Now find the data partition, which will tell us where the DOL and FST is.
    // This first requires parsing the partition *groups*. Each partition group contains multiple partitions.
    // Data partitions have the id 0.

    struct rrc_di_part_group part_groups[4] __attribute__((aligned(32)));
    res = rrc_di_unencrypted_read(&part_groups, sizeof(part_groups), RRC_DI_PART_GROUPS_OFFSET >> 2);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_unencrypted_read for partition group");

    u32 data_part_offset = UINT32_MAX;
    res = rrc_loader_locate_data_part(&data_part_offset);

    if (data_part_offset == UINT32_MAX)
    {
        RRC_FATAL("no data partition found on disk");
    }
    printf("data partition found at offset %x\n", data_part_offset << 2);
    CHECK_EXIT();

    res = rrc_di_open_partition(data_part_offset);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_open_partition");

    struct rrc_di_data_part_header data_header[3] __attribute__((aligned(32)));
    res = rrc_di_read(&data_header, sizeof(data_header), 0x420 >> 2);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read data partition header");

    printf("DOL offset: %d\n", data_header->dol_offset << 2);
    printf("FST offset: %d\n", data_header->fst_offset << 2);
    printf("FST size: %d\n", data_header->fst_size << 2);

    res = LWP_JoinThread(wiisocket_thread, NULL);
    RRC_ASSERTEQ(res, RRC_LWP_OK, "LWP_JoinThread wiisocket init");
    RRC_ASSERTEQ(wiisocket_res, 0, "wiisocket_init");

    // read the BI2
    u8 bi2[0x2000] __attribute__((aligned(32)));
    res = rrc_di_read(bi2, sizeof(bi2), 0x440 >> 2);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read for bi2");

    // read dol
    struct rrc_dol *dol = (struct rrc_dol *)0x80901000;
    res = rrc_di_read(dol, sizeof(struct rrc_dol), data_header->dol_offset);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read for dol");

    printf("Entrypoint at %x\n", dol->entry_point);
    printf("BSS Addr: %x\n", dol->bss_addr);
    printf("BSS size: %d\n", dol->bss_size);
    for (u32 i = 0; i < RRC_DOL_SECTION_COUNT; i++)
    {
        if (dol->section_size[i] == 0)
        {
            continue;
        }
        printf("%x at %x-%x (%d b)\n", dol->section[i], dol->section_addr[i], dol->section_addr[i] + dol->section_size[i], dol->section_size[i]);
        if ((dol->section_addr[i] < 0x80000000) || (dol->section_addr[i] + dol->section_size[i] > 0x90000000))
        {
            RRC_FATAL("very wrong address: %x", dol->section_addr[i]);
        }

        res = rrc_di_read(
            (void *)(0x80901000 + dol->section[i]),
            dol->section_size[i],
            data_header->dol_offset + (dol->section[i] >> 2));
        RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read section");
    }

    u32 mem1_hi = 0x81800000;
    u32 mem2_hi = *(u32 *)0x80003128;
    printf("mem1 hi: %x\nmem2 hi %x\n", mem1_hi, mem2_hi);

    u32 fst_size = data_header->fst_size << 2;
    u32 fst_dest = align_down(mem1_hi - fst_size, 32);
    if (fst_dest < 0x81700000)
    {
        RRC_FATAL("fst size too large");
    }
    mem1_hi = fst_dest;
    printf("FST at %x, size: %d, aligned: %d\n", fst_dest, fst_size, align_up(fst_size, 32));
    res = rrc_di_read((void *)fst_dest, align_up(fst_size, 32), data_header->fst_offset);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read fst");

    *((u32 *)0x80000038) = fst_dest; // Start of FST

    /////////////// PATCHING //////////////////////

    // Copy BI2
    void *bi2_dest = (void *)(mem1_hi - sizeof(bi2));
    memcpy(bi2_dest, bi2, sizeof(bi2));
    DCStoreRange(bi2_dest, sizeof(bi2));

    printf("stopping shutdown handler...\n");
    rrc_shutting_down = true;
    rrc_shutdown_join(shutdown_thread);
    printf("stopped.\n");

    // Reset the DSP; LibOGC apps like the HBC cannot initialize it properly,
    // but the SDK can
    *(u32 *)0xCD006C00 = 0x00000000;

    *(u32 *)0x80000034 = 0; // Arena High

    *(u32 *)0x800000EC = 0x81800000;    // Dev Debugger Monitor Address
    *(u32 *)0x800000F0 = 0x01800000;    // Simulated Memory Size
    *(u32 *)0x800000F4 = (u32)bi2_dest; // Pointer to data from partition's bi2.bin
    *(u32 *)0x800000F8 = 0x0E7BE2C0;    // Console Bus Speed
    *(u32 *)0x800000FC = 0x2B73A840;    // Console CPU Speed

    *(u32 *)0x80003110 = mem1_hi;              // MEM1 Arena End
    *(u32 *)0x80003124 = 0x90000800;           // Usable MEM2 Start
    *(u32 *)0x80003128 = mem2_hi;              // Usable MEM2 End
    *(u32 *)0x80003180 = *(u32 *)(0x80000000); // Game ID
    *(u32 *)0x80003188 = *(u32 *)(0x80003140); // IOS Version + Revision

    // Check dual layer disc
    if (*(u32 *)((u32)bi2_dest + 0x30) == 0x7ED40000)
    {
        *(u8 *)0x8000319C = 0x81; // Dual layer
    }
    else
    {
        *(u8 *)0x8000319C = 0x80; // Single layer
    }
    DCStoreRange((void *)0x80000000, 0x3400);
    ICInvalidateRange((void *)0x80000000, 0x3400);

#define PATCH_DOL_SIZE 512
    void (*patch_copy)(struct rrc_dol *, void (*)()) = (void (*)(struct rrc_dol *, void (*)()))0x80900000;

    memcpy(patch_copy, patch_dol, PATCH_DOL_SIZE);
    DCFlushRange(patch_copy, PATCH_DOL_SIZE);
    ICInvalidateRange(patch_copy, PATCH_DOL_SIZE);

    void (*flush_range_copy)() = (void *)align_up(0x80900000 + PATCH_DOL_SIZE, 32);
    memcpy(flush_range_copy, DCFlushRange, 52);
    DCFlushRange(flush_range_copy, 64);
    ICInvalidateRange(flush_range_copy, 64);

    __IOS_ShutdownSubsystems();
    for (u32 i = 0; i < 32; i++)
    {
        IOS_CloseAsync(i, 0, 0);
    }

    IRQ_Disable();

    SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);

    (patch_copy)(dol, flush_range_copy);

    s64 systime_end = gettime();
    rrc_dbg_printf("Time taken: %.3f seconds\n", ((f64)diff_msec(systime_start, systime_end)) / 1000.0);

    rrc_shutdown_join(shutdown_thread);

    usleep(10000000);

    return 0;
#undef CHECK_EXIT
}
