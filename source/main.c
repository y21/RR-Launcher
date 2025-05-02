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
#include <fat.h>
#include <curl/curl.h>
#include <mxml.h>
#include <sys/statvfs.h>
#include <errno.h>

#include "util.h"
#include "di.h"
#include "time.h"
#include "loader.h"
#include "dol.h"
#include "console.h"
#include "settings.h"
#include "update/versionsfile.h"
#include "update/update.h"
#include "prompt.h"
#include "gui.h"
#include "res.h"
#include "riivo.h"
#include "settingsfile.h"
#include <setjmp.h>
#include <ogc/isfs.h>

/* 100ms */
#define DISKCHECK_DELAY 100000

void *wiisocket_init_thread_callback(void *res)
{
    // Note: the void* given to us is an int* that lives in the main function,
    // because we want to assert everything from the main thread rather than asserting in here
    // so that we don't potentially exit(1) from another thread while the main thread is doing some important reading/patching.
    *(int *)res = wiisocket_init();
    return NULL;
}

void patching_stuff(u32 *mem1);

int main(int argc, char **argv)
{
    IOS_ReloadIOS(80);

    u32 mem1_hi = 0x81700000; // reserved for patch helpers
    u32 mem2_hi = *(u32 *)0x80003128;

    s64 systime_start = gettime();
    // response codes for various library functions
    int res;

    void *xfb;
    // init video, setup console framebuffer
    rrc_gui_xfb_alloc(&xfb, true);
    rrc_gui_display_con(xfb, true);
    rrc_gui_display_banner(xfb);

    rrc_dbg_printf("Initialising SD card");
    RRC_ASSERTEQ(fatInitDefault(), true, "fatInitDefault()");
    // force filesystem root
    chdir("../..");

    {
        FILE *patch_file = fopen("patch-helpers.dol", "r");
        RRC_ASSERT(patch_file != NULL, "patch file");
        struct rrc_dol patch_dol __attribute__((aligned(32)));

        int read = fread((void *)&patch_dol, sizeof(patch_dol), 1, patch_file);
        SYS_Report("read = %d\n", read);

        for (int i = 0; i < patch_dol.bss_size; i++)
            *(u8 *)(patch_dol.bss_addr + i) = 0;

        DCInvalidateRange((void *)patch_dol.bss_addr, align_up(patch_dol.bss_size, 32));
        DCFlushRange((void *)patch_dol.bss_addr, align_up(patch_dol.bss_size, 32));
        ICInvalidateRange((void *)patch_dol.bss_addr, align_up(patch_dol.bss_size, 32));

        for (int i = 0; i < RRC_DOL_SECTION_COUNT; i++)
        {
            u32 sec = patch_dol.section[i];
            u32 sec_addr = patch_dol.section_addr[i];
            u32 sec_size = patch_dol.section_size[i];
            if (sec_addr == 0)
                continue;

            fseek(patch_file, sec, SEEK_SET);
            SYS_Report("Section %d at %x with size %d\n", sec, sec_addr, sec_size);
            read = fread((void *)sec_addr, sec_size, 1, patch_file);
            DCInvalidateRange((void *)align_down(sec_addr, 32), align_up(sec_size + 32, 32));
            DCFlushRange((void *)align_down(sec_addr, 32), align_up(sec_size + 32, 32));
            ICInvalidateRange((void *)align_down(sec_addr, 32), align_up(sec_size + 32, 32));
            RRC_ASSERTEQ(read, 1, "fully read section");
        }
    }

    SYS_Report("testing stuff\n");

    rrc_con_update("Initialise controllers", 0);

    rrc_dbg_printf("init controllers\n");
    res = WPAD_Init();
    RRC_ASSERTEQ(res, WPAD_ERR_NONE, "WPAD_Init");

    rrc_con_update("Spawn background threads", 5);

    // Initializing the network can take fairly long (seconds).
    // It's not really needed right away anyway so we can do it on another thread in parallel to some of the disk reading
    // and join on it later when we actually need it.
    rrc_dbg_printf("spawn network init thread\n");
    int wiisocket_res;
    lwp_t wiisocket_thread;
    res = LWP_CreateThread(&wiisocket_thread, wiisocket_init_thread_callback, &wiisocket_res, NULL, 0, RRC_LWP_PRIO_IDLE);
    RRC_ASSERTEQ(res, RRC_LWP_OK, "LWP_CreateThread for wiisocket init");

    rrc_con_update("Initialise DVD", 10);

    rrc_dbg_printf("init disk drive\n");
    int fd = rrc_di_init();
    RRC_ASSERT(fd != 0, "rrc_di_init");

    rrc_con_update("Initialise DVD: Check for Mario Kart Wii", 12);
    /*  We should load Mario Kart Wii before doing anything else */
    res = rrc_loader_await_mkw(xfb);
    if (res == RRC_RES_SHUTDOWN_INTERRUPT)
    {
        exit(0);
    }

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

    rrc_con_update("Initialise DVD: Load Data Partition", 15);
    struct rrc_di_part_group part_groups[4] __attribute__((aligned(32)));
    res = rrc_di_unencrypted_read(&part_groups, sizeof(part_groups), RRC_DI_PART_GROUPS_OFFSET >> 2);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_unencrypted_read for partition group");

    u32 data_part_offset = UINT32_MAX;
    res = rrc_loader_locate_data_part(&data_part_offset);

    if (data_part_offset == UINT32_MAX)
    {
        RRC_FATAL("no data partition found on disk");
    }
    rrc_dbg_printf("data partition found at offset %x\n", data_part_offset << 2);

    rrc_con_update("Initialise DVD: Read Data Partition", 17);

    res = rrc_di_open_partition(data_part_offset);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_open_partition");

    struct rrc_di_data_part_header data_header[3] __attribute__((aligned(32)));
    res = rrc_di_read(&data_header, sizeof(data_header), 0x420 >> 2);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read data partition header");

    rrc_dbg_printf("DOL offset: %d\n", data_header->dol_offset << 2);
    rrc_dbg_printf("FST offset: %d\n", data_header->fst_offset << 2);
    rrc_dbg_printf("FST size: %d\n", data_header->fst_size << 2);

    rrc_con_update("Await Network", 20);

    res = LWP_JoinThread(wiisocket_thread, NULL);
    RRC_ASSERTEQ(res, RRC_LWP_OK, "LWP_JoinThread wiisocket init");
    RRC_ASSERTEQ(wiisocket_res, 0, "wiisocket_init");

    //     struct rrc_settingsfile stored_settings;
    //     RRC_ASSERTEQ(rrc_settingsfile_parse(&stored_settings), RRC_SETTINGSFILE_OK, "failed to parse settingsfile");

    //     // Check for updates if the user enabled that setting.
    //     if (stored_settings.auto_update)
    //     {
    //         int update_count;
    //         rrc_update_do_updates(xfb, &update_count);
    //     }

    // #define INTERRUPT_TIME 3000000 /* 3 seconds */
    //     rrc_con_clear(true);

    //     rrc_con_print_text_centered(_RRC_ACTION_ROW, "Press A to launch, or press + to load settings.");
    //     rrc_con_print_text_centered(_RRC_ACTION_ROW + 1, "Auto-launching in 3 seconds...");

    //     for (int i = 0; i < INTERRUPT_TIME / RRC_WPAD_LOOP_TIMEOUT; i++)
    //     {
    //         WPAD_ScanPads();

    //         int pressed = WPAD_ButtonsDown(0);
    //         if (pressed & RRC_WPAD_HOME_MASK)
    //         {
    //             return 0;
    //         }

    //         if (pressed & RRC_WPAD_A_MASK)
    //         {
    //             break;
    //         }

    //         if (pressed & RRC_WPAD_PLUS_MASK)
    //         {
    //             switch (rrc_settings_display(xfb, &stored_settings))
    //             {
    //             case RRC_SETTINGS_LAUNCH:
    //                 goto interrupt_loop_end;
    //             case RRC_SETTINGS_EXIT:
    //                 return 0;
    //             }
    //         }

    //         usleep(RRC_WPAD_LOOP_TIMEOUT);
    //     }
    // interrupt_loop_end:

    rrc_con_clear(true);

    rrc_con_update("Initialise DVD: Read Game DOL", 25);

    // read dol
    struct rrc_dol *dol = (struct rrc_dol *)0x80901000;
    res = rrc_di_read(dol, sizeof(struct rrc_dol), data_header->dol_offset);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read for dol");

    SYS_Report("Entrypoint at %x\n", dol->entry_point);
    rrc_dbg_printf("BSS Addr: %x\n", dol->bss_addr);
    rrc_dbg_printf("BSS size: %d\n", dol->bss_size);
    for (u32 i = 0; i < RRC_DOL_SECTION_COUNT; i++)
    {
        if (dol->section_size[i] == 0)
        {
            continue;
        }
        SYS_Report("%x at %x-%x (%d b)\n", dol->section[i], dol->section_addr[i], dol->section_addr[i] + dol->section_size[i], dol->section_size[i]);
        if ((dol->section_addr[i] < 0x80000000) || (dol->section_addr[i] + dol->section_size[i] > 0x90000000))
        {
            RRC_FATAL("Invalid section address: %x", dol->section_addr[i]);
        }

        // See patch.c comment for why we first copy them to `dol + dol->section[i]` rather than to `section_addr[i]` directly.
        res = rrc_di_read(
            (void *)((u32)dol + dol->section[i]),
            dol->section_size[i],
            data_header->dol_offset + (dol->section[i] >> 2));
        RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read section");
    }

    // Branches to Loader.pul, taken from xml
    RRC_ASSERTEQ(dol->section_addr[1], 0x800072c0, "a");
    RRC_ASSERTEQ(dol->section_addr[1] + dol->section_size[1], 0x80244de0, "b");
    u32 *v = (u32 *)((u32)dol + dol->section[1] + 2339800);
    SYS_Report("%x\n", *v);
    // <memory offset="0x80242698" value="4BDC1968" original="4e800020" />
    *v = 0x4BDC1968;
    DCFlushRange((void *)v, 32);
    SYS_Report("%x\n", *v);
    // <memory offset = "0x8000A3F4" value = "4bff9c0c" original = "4e800020" /><!--RMCP REL-->
    v = (u32 *)((u32)dol + dol->section[1] + 12596);
    *v = 0x4bff9c0c;
    DCFlushRange((void *)v, 32);
    SYS_Report("%x\n", *v);

    // Patch convert_to_entry
    struct function_patch_entry
    {
        u32 addr;
        u32 backjmp_to_original[4];
        // Only this may change when changing dol
        u32 jmp_to_custom[4];
    };

    struct function_patch_entry entries[] = {
        // DVD::ConvertPathToEntrynum
        {.addr = 0x8015df4c, .backjmp_to_original = {0x3d208015, 0x6129df5c, 0x7d2903a6, 0x4e800420}, .jmp_to_custom = {0x3d208171, 0x6129e000, 0x7d2903a6, 0x4e800420}},
        // DVD::FastOpen
        {.addr = 0x8015e254, .backjmp_to_original = {0x3d208015, 0x6129e264, 0x7d2903a6, 0x4e800420}, .jmp_to_custom = {0x3d208171, 0x6129e800, 0x7d2903a6, 0x4e800420}},
        // DVD::Open
        {.addr = 0, .backjmp_to_original = {0x3d208015, 0x6129e2cc, 0x7d2903a6, 0x4e800420}, .jmp_to_custom = {}},
        // DVD::ReadPrio
        {.addr = 0x8015e834, .backjmp_to_original = {0x3d208015, 0x6129e844, 0x7d2903a6, 0x4e800420}, .jmp_to_custom = {0x3d208171, 0x6129ec00, 0x7d2903a6, 0x4e800420}},
        // DVD::Close
        {.addr = 0x8015e568, .backjmp_to_original = {0x3d208015, 0x6129e578, 0x7d2903a6, 0x4e800420}, .jmp_to_custom = {0x3d208171, 0x6129f000, 0x7d2903a6, 0x4e800420}},
    };

    for (u32 i = 0; i < RRC_DOL_SECTION_COUNT; i++)
    {
        u32 section_addr = dol->section_addr[i];
        u32 section_size = dol->section_size[i];
        u32 section_offset = dol->section[i];
        for (int j = 0; j < sizeof(entries) / sizeof(struct function_patch_entry); j++)
        {
            struct function_patch_entry entry = entries[j];
            if (entry.addr == 0)
                continue;
            u32 target_addr = entry.addr;

            if (target_addr >= section_addr && target_addr <= section_addr + section_size)
            {
                u32 addr_section_offset = target_addr - section_addr;
                u32 *virt_target = (void *)((u32)dol + section_offset + addr_section_offset);

                // 32 bytes (4 instructions for the backjmp + 4 overwritten instructions restored) per patched function
                u32 hooked_addr = 0x93400000 + (j * 32);

                memcpy((void *)hooked_addr, virt_target, 16);
                memcpy((void *)(hooked_addr + 16), entry.backjmp_to_original, 16);
                DCInvalidateRange((void *)hooked_addr, 32);
                ICInvalidateRange((void *)hooked_addr, 32);

                memcpy(virt_target, entry.jmp_to_custom, 16);
                DCInvalidateRange((void *)virt_target, 32);
                ICInvalidateRange((void *)virt_target, 32);
            }
        }
    }

    RRC_ASSERTEQ(dol->section_addr[0], 0x80004000, "addr");
    u8 *pul_dest = (u8 *)((u32)dol + dol->section[0]);

    FILE *loader_pul_file = fopen("RetroRewind6/Binaries/Loader.pul", "r");
    RRC_ASSERT(loader_pul_file != NULL, "failed to open");
    int read = 0;
    while ((read = fread(pul_dest, 1, 4096, loader_pul_file)))
    {
        pul_dest += read;
    }
    patching_stuff(&mem1_hi);

    rrc_con_update("Initialise DVD: Read Filesystem Table", 50);

    rrc_dbg_printf("mem1 hi: %x, mem2 hi %x\n", mem1_hi, mem2_hi);

    u32 fst_size = data_header->fst_size << 2;
    u32 fst_dest = align_down(mem1_hi - fst_size, 32);
    // TODO: why is this check a thing???
    // if (fst_dest < 0x81700000)
    // {
    //     RRC_FATAL("fst size too large");
    // }
    mem1_hi = fst_dest;
    rrc_dbg_printf("FST at %x, size: %d, aligned: %d\n", fst_dest, fst_size, align_up(fst_size, 32));
    res = rrc_di_read((void *)fst_dest, align_up(fst_size, 32), data_header->fst_offset);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read fst");

    *((u32 *)0x80000038) = fst_dest; // start of FST

    // read BI2
    mem1_hi = align_down(mem1_hi - RRC_BI2_SIZE, 32);
    void *bi2 = (void *)(mem1_hi);
    res = rrc_di_read(bi2, RRC_BI2_SIZE, 0x440 >> 2);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_read for bi2");
    DCStoreRange(bi2, RRC_BI2_SIZE);

    rrc_con_update("Prepare For Patching", 60);

    // start shutting down background threads to boot the game

    WPAD_Shutdown();

    s64 systime_end = gettime();
    rrc_dbg_printf("time taken: %.3f seconds\n", ((f64)diff_msec(systime_start, systime_end)) / 1000.0);

    mem2_hi = 0x93400000;
    rrc_loader_load(dol, bi2, mem1_hi, mem2_hi);

    return 0;
}

char *bump_alloc_string(u32 *mem1, const char *src)
{
    *mem1 -= strlen(src) + 1;
    char *dest = (char *)*mem1;
    strcpy(dest, src);
    return dest;
}

void patching_stuff(u32 *mem1)
{
#define MAX_PATCHES 1000

    *mem1 -= sizeof(struct rrc_riivo_disc_replacement) * MAX_PATCHES;
    *mem1 -= sizeof(struct rrc_riivo_disc);
    struct rrc_riivo_disc *riivo_disc = (void *)*mem1;

    // Read the XML to extract all possible options for the entries.
    FILE *
        xml_file = fopen("RetroRewind6/xml/RetroRewind6.xml", "r");
    if (!xml_file)
    {
        RRC_FATAL("failed to open RetroRewind6.xml file: %d", errno);
    }

    mxml_node_t *xml_top = mxmlLoadFile(NULL, xml_file, NULL);

    // TODO: load these active patches from the settings
    const char *active_patches[] = {"RRLoadPack", "RRFileReplacements"};
    // TODO: maybe handle <memory> patches here also?

    for (mxml_node_t *cur = mxmlFindElement(xml_top, xml_top, "patch", NULL, NULL, MXML_DESCEND_FIRST); cur != NULL; cur = mxmlGetNextSibling(cur))
    {
        RRC_ASSERT(riivo_disc->count < MAX_PATCHES, "too many file/folder replacements!");

        if (mxmlGetType(cur) != MXML_ELEMENT)
            continue;

        if (strcmp(mxmlGetElement(cur), "patch") != 0)
            continue;

        // We have a <patch> element. Check if the id is an enabled setting, then process any of its contained <file> and <folder> elements.
        const char *elem_id = mxmlElementGetAttr(cur, "id");
        bool enabled = false;
        for (int i = 0; i < sizeof(active_patches) / sizeof(const char *); i++)
        {
            if (strcmp(active_patches[i], elem_id) == 0)
            {
                enabled = true;
                break;
            }
        }
        if (!enabled)
            continue;

        mxml_index_t *file_repl_index = mxmlIndexNew(cur, "file", NULL);
        for (mxml_node_t *file = mxmlIndexEnum(file_repl_index); file != NULL; file = mxmlIndexEnum(file_repl_index))
        {
            const char *disc_path_mxml = mxmlElementGetAttr(file, "disc");
            RRC_ASSERT(disc_path_mxml != NULL, "missing disc attribute on <file>");

            const char *external_path_mxml = mxmlElementGetAttr(file, "external");
            RRC_ASSERT(external_path_mxml != NULL, "missing external attribute on <file>");

            char *disc_path_m1 = bump_alloc_string(mem1, disc_path_mxml);
            char *external_path_m1 = bump_alloc_string(mem1, external_path_mxml);

            struct rrc_riivo_disc_replacement *patch_dist = &riivo_disc->replacements[riivo_disc->count];
            patch_dist->disc = disc_path_m1;
            patch_dist->external = external_path_m1;
            patch_dist->type = RRC_RIIVO_FILE_REPLACEMENT;
            riivo_disc->count++;
        }

        mxml_index_t *folder_repl_index = mxmlIndexNew(cur, "folder", NULL);
        for (mxml_node_t *folder = mxmlIndexEnum(folder_repl_index); folder != NULL; folder = mxmlIndexEnum(folder_repl_index))
        {
            const char *disc_path_mxml = mxmlElementGetAttr(folder, "disc");
            RRC_ASSERT(disc_path_mxml != NULL, "missing disc attribute on <file>");

            // external can be omitted!
            const char *external_path_mxml = mxmlElementGetAttr(folder, "external");
            RRC_ASSERT(external_path_mxml != NULL, "NULL external is not handled well in the 2nd dol");

            char *disc_path_m1 = bump_alloc_string(mem1, disc_path_mxml);
            char *external_path_m1 = external_path_mxml ? bump_alloc_string(mem1, external_path_mxml) : NULL;

            struct rrc_riivo_disc_replacement *patch_dist = &riivo_disc->replacements[riivo_disc->count];
            patch_dist->disc = disc_path_m1;
            patch_dist->external = external_path_m1;
            patch_dist->type = RRC_RIIVO_FOLDER_REPLACEMENT;
            SYS_Report("Found folder: %s\n", patch_dist->disc);
            riivo_disc->count++;
        }
    }

    SYS_Report("Count = %d\n", riivo_disc->count);

    RRC_ASSERTEQ(riivo_disc, (void *)0x816fd11c, "Riivo patches pointer changed!! Adjust the pointer in linker.ld and here.");
}
