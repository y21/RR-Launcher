/*
    loader.h - main app loader and patcher

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

#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include <wiiuse/wpad.h>

#include "prompt.h"
#include "patch.h"
#include "util.h"
#include "di.h"
#include "loader.h"
#include "res.h"
#include "console.h"
#include "../shared/riivo.h"
#include "util.h"
#include <mxml.h>
#include <wiisocket.h>

int rrc_loader_locate_data_part(u32 *data_part_offset)
{
    int res;
    struct rrc_di_part_group part_groups[4] __attribute__((aligned(32)));
    res = rrc_di_unencrypted_read(&part_groups, sizeof(part_groups), RRC_DI_PART_GROUPS_OFFSET >> 2);
    RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_unencrypted_read for partition group");

    struct rrc_di_part_info partitions[4] __attribute__((aligned(32)));

    for (u32 i = 0; i < 4 && *data_part_offset == UINT32_MAX; i++)
    {
        if (part_groups[i].count == 0 && part_groups[i].offset == 0)
        {
            // No partitions in this group.
            continue;
        }

        if (part_groups[i].count > 4)
        {
            RRC_FATAL("too many partitions in group %d (max: 4, got: %d)", i, part_groups[i].count);
        }

        res = rrc_di_unencrypted_read(&partitions, sizeof(partitions), part_groups[i].offset);
        RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_unencrypted_read for partition");
        for (u32 j = 0; j < part_groups[i].count; j++)
        {
            if (partitions[j].type == RRC_DI_PART_TYPE_DATA)
            {
                *data_part_offset = partitions[j].offset;
                break;
            }
        }
    }

    return 0;
}

int rrc_loader_await_mkw(void *xfb)
{
    int res;
    unsigned int status;

check_cover_register:
    res = rrc_di_get_low_cover_register(&status);
    RRC_ASSERTEQ(res, RRC_DI_RET_OK, "rrc_di_getlowcoverregister");

    // if status = 0 that means that a disk is inserted
    if ((status & RRC_DI_DICVR_CVR) != 0)
    {
    missing_mkwii_alert:
        char *lines[] = {
            "Mario Kart Wii is not inserted!",
            "",
            "Please insert Mario Kart Wii into the console,",
            "and select OK when done."};

        enum rrc_prompt_result pres = rrc_prompt_ok_cancel(xfb, lines, 4);
        RRC_ASSERT(pres != RRC_PROMPT_RESULT_ERROR, "failed to generate prompt");

        if (pres == RRC_PROMPT_RESULT_OK)
        {
            goto check_cover_register;
        }
        else
        {
            return RRC_RES_SHUTDOWN_INTERRUPT;
        }
    }

    rrc_dbg_printf("check disc");

    /* we need to check we actually inserted mario kart wii */
    struct rrc_di_disk_id did;
    res = rrc_di_get_disk_id(&did);
    /* likely drive wasnt spun up */
    if (res != RRC_DI_LIBDI_EIO)
    {
        /* spin up the drive */
        rrc_dbg_printf("failed to read disk_id: attempting drive reset\n");
        RRC_ASSERTEQ(rrc_di_reset(), RRC_DI_LIBDI_OK, "rrc_di_reset");
        res = rrc_di_get_disk_id(&did);
        RRC_ASSERTEQ(res, RRC_DI_LIBDI_OK, "rrc_di_get_disk_id (could not initialise drive)");
    }

    /* this excludes region identifier */
#define DISKID_MKW_ID "RMC"
    if (memcmp(did.game_id, DISKID_MKW_ID, strlen(DISKID_MKW_ID)))
        goto missing_mkwii_alert;

    char gameId[16];
    snprintf(
        gameId, sizeof(gameId), "%c%c%c%cD%02x", did.game_id[0],
        did.game_id[1], did.game_id[2], did.game_id[3], did.disc_ver);

    rrc_dbg_printf("Game ID/Rev: %s\n", gameId);

    return RRC_RES_OK;
}

static char *bump_alloc_string(u32 *mem1, const char *src)
{
    *mem1 -= strlen(src) + 1;
    char *dest = (char *)*mem1;
    strcpy(dest, src);
    return dest;
}

#define MAX_FILE_PATCHES 1000
#define MAX_MEMORY_PATCHES 128
#define MAX_ENABLED_SETTINGS 64
static void append_patches_for_option(mxml_node_t *top, mxml_index_t *index, const char *name, int value, const char **patch_list, int *patch_count)
{
    if (value == 0)
    {
        // 0 = disabled, no patches to append
        return;
    }

    mxmlIndexReset(index);
    for (mxml_node_t *option = mxmlIndexEnum(index); option != NULL; option = mxmlIndexEnum(index))
    {
        const char *option_name = mxmlElementGetAttr(option, "name");
        if (strcmp(option_name, name) == 0)
        {
            // Get the nth-1 (0 is the implicit disabled, handled at the top, does not exist in the XML) child (excluding whitespace nodes), which is the selected option.
            mxml_node_t *selected_choice = mxmlFindElement(option, top, "choice", NULL, NULL, MXML_DESCEND_FIRST);
            for (int i = 0; selected_choice != NULL; selected_choice = mxmlGetNextSibling(selected_choice))
            {
                if (mxmlGetType(selected_choice) != MXML_ELEMENT)
                    continue;

                if (i == value - 1)
                {
                    break;
                }
                i++;
            }

            if (!selected_choice)
            {
                RRC_FATAL("malformed RetroRewind6.xml: option '%s' has no children", name);
            }

            // The children of `selected_choice` are the patches. Append them.
            for (mxml_node_t *patch = mxmlFindElement(selected_choice, top, "patch", NULL, NULL, MXML_DESCEND_FIRST); patch != NULL; patch = mxmlGetNextSibling(patch))
            {
                if (mxmlGetType(patch) != MXML_ELEMENT)
                    continue;

                if (strcmp(mxmlGetElement(patch), "patch") != 0)
                    continue;

                const char *patch_name = mxmlElementGetAttr(patch, "id");
                RRC_ASSERT(patch_name != NULL, "malformed RetroRewind6.xml: patch has no name attribute");

                // Append the patch name to the list.
                if (*patch_count >= MAX_ENABLED_SETTINGS)
                {
                    RRC_FATAL("too many enabled settings in xml");
                }
                patch_list[*patch_count] = patch_name;
                (*patch_count)++;
            }

            return;
        }
    }

    RRC_FATAL("option '%s' not found in xml\n", name);
}

/**
 * Parses <file> and <folder> patches in the XML file and gives runtime-ext a pointer to it.
 * <memory> patches are also parsed
 */
static void parse_riivo_patches(struct rrc_settingsfile *settings, u32 *mem1, u32 *mem2, struct rrc_riivo_memory_patch **mem_patches, int *mem_patches_count)
{
    u32 mem1_orig = *mem1;
    // Reserve space for file/folder replacements.
    *mem1 -= sizeof(struct rrc_riivo_disc_replacement) * MAX_FILE_PATCHES;
    *mem1 -= sizeof(struct rrc_riivo_disc);
    struct rrc_riivo_disc *riivo_disc = (void *)*mem1;
    // Reserve space for memory patches. Note: they don't actually need to be reserved in MEM1,
    // because it's only shortly needed in patch.c and never again at runtime.
    *mem1 -= sizeof(struct rrc_riivo_memory_patch) * MAX_MEMORY_PATCHES;
    *mem_patches = (void *)*mem1;
    *mem_patches_count = 0;

    // Read the XML to extract all possible options for the entries.
    FILE *
        xml_file = fopen("RetroRewind6/xml/RetroRewind6.xml", "r");
    if (!xml_file)
    {
        RRC_FATAL("failed to open RetroRewind6.xml file: %d", errno);
    }

    mxml_node_t *xml_top = mxmlLoadFile(NULL, xml_file, NULL);

    const char *active_patches[MAX_ENABLED_SETTINGS];
    int active_patches_count = 0;

    mxml_index_t *options_index = mxmlIndexNew(xml_top, "option", NULL);

    append_patches_for_option(xml_top, options_index, "My Stuff", settings->my_stuff, active_patches, &active_patches_count);
    append_patches_for_option(xml_top, options_index, "Language", settings->language, active_patches, &active_patches_count);
    // Just always enable the pack, there is no setting for this.
    append_patches_for_option(xml_top, options_index, "Pack", RRC_SETTINGSFILE_PACK_ENABLED_VALUE, active_patches, &active_patches_count);

    for (int i = 0; i < active_patches_count; i++)
    {
        SYS_Report("[DEBUG] Patch enabled: %s\n", active_patches[i]);
    }

    // TODO: handle savegame option?

    // Iterate through <patch> elements.
    for (mxml_node_t *cur = mxmlFindElement(xml_top, xml_top, "patch", NULL, NULL, MXML_DESCEND_FIRST); cur != NULL; cur = mxmlGetNextSibling(cur))
    {
        RRC_ASSERT(riivo_disc->count < MAX_FILE_PATCHES, "too many file/folder replacements!");

        if (mxmlGetType(cur) != MXML_ELEMENT)
            continue;

        if (strcmp(mxmlGetElement(cur), "patch") != 0)
            continue;

        // We have a <patch> element. Check if the id is an enabled setting, then process any of its contained <file> and <folder> elements.
        const char *elem_id = mxmlElementGetAttr(cur, "id");
        bool enabled = false;
        for (int i = 0; i < active_patches_count; i++)
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
        mxmlIndexDelete(file_repl_index);

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
            riivo_disc->count++;
        }
        mxmlIndexDelete(folder_repl_index);

        mxml_index_t *memory_index = mxmlIndexNew(cur, "memory", NULL);
        for (mxml_node_t *memory = mxmlIndexEnum(memory_index); memory != NULL; memory = mxmlIndexEnum(memory_index))
        {
            const char *addr_mxml = mxmlElementGetAttr(memory, "offset");
            RRC_ASSERT(addr_mxml != NULL, "missing addr attribute on <memory>");

            const char *valuefile_mxml = mxmlElementGetAttr(memory, "valuefile");
            if (valuefile_mxml != NULL)
            {
                // In general we can't really handle valuefiles easily.
                // It would require loading an SD card file in the patch function.
                if (strcmp(valuefile_mxml, "/RetroRewind6/Binaries/Loader.pul") == 0)
                {
                    // Loader.pul specifically is handled manually elsewhere, so make an exception for this.
                    // Do make sure that the address is what we've hardcoded, though.
                    RRC_ASSERTEQ(strtoul(addr_mxml, NULL, 16), 0x80004000, "Loader.pul patch address is not 0x80000000");
                    continue;
                }

                RRC_FATAL("Unhandled valuefile memory patch: %s\n", valuefile_mxml);
            }

            const char *value_mxml = mxmlElementGetAttr(memory, "value");
            RRC_ASSERT(value_mxml != NULL, "missing value attribute on <memory>");

            const char *original_mxml = mxmlElementGetAttr(memory, "original");

            struct rrc_riivo_memory_patch *patch_dist = &(*mem_patches)[*mem_patches_count];
            (*mem_patches_count)++;
            // TODO: validate these?
            patch_dist->addr = strtoul(addr_mxml, NULL, 16);
            patch_dist->value = strtoul(value_mxml, NULL, 16);
            patch_dist->original_init = false;
            if (original_mxml)
            {
                patch_dist->original = strtoul(original_mxml, NULL, 16);
                patch_dist->original_init = true;
            }
        }
        mxmlIndexDelete(memory_index);
    }

    // This address is a `static` in the runtime-ext dol that holds a pointer to the replacements, defined in the linker script.
    *((struct rrc_riivo_disc **)(RRC_RIIVO_DISC_PTR)) = riivo_disc;
    DCFlushRange((void *)*mem1, align_up(mem1_orig - *mem1, 32));
    ICInvalidateRange((void *)*mem1, align_up(mem1_orig - *mem1, 32));

    mxmlDelete(xml_top);
    fclose(xml_file);
}

/**
 * Patches the DVD functions in the game DOL to immediately jump to custom DVD functions implemented in runtime-ext.
 * Also allocates trampolines containing the first 4 overwritten instructions + backjump to the original function,
 * which is called when the custom function wants to call the original DVD function.
 */
static void patch_dvd_functions(struct rrc_dol *dol)
{
    struct function_patch_entry
    {
        // Address of the function to patch.
        u32 addr;
        // Instructions to write at the end of the trampoline. This will jump back to the original DVD function + 16 (4 instructions).
        u32 backjmp_to_original[4];
        // Instructions to overwrite the start of the original DVD function with. This will jump to the custom function.
        u32 jmp_to_custom[4];
    };

    struct function_patch_entry entries[] = {
        {.addr = RRC_DVD_CONVERT_PATH_TO_ENTRYNUM, .backjmp_to_original = {0x3d208015, 0x6129df5c, 0x7d2903a6, 0x4e800420}, .jmp_to_custom = {0x3d208178, 0x61292e60, 0x7d2903a6, 0x4e800420}},
        {.addr = RRC_DVD_FAST_OPEN, .backjmp_to_original = {0x3d208015, 0x6129e264, 0x7d2903a6, 0x4e800420}, .jmp_to_custom = {0x3d208178, 0x61292ee0, 0x7d2903a6, 0x4e800420}},
        {.addr = RRC_DVD_OPEN, .backjmp_to_original = {0x3d208015, 0x6129e2cc, 0x7d2903a6, 0x4e800420}, .jmp_to_custom = {0x3d208178, 0x61292ea0, 0x7d2903a6, 0x4e800420}},
        {.addr = RRC_DVD_READ_PRIO, .backjmp_to_original = {0x3d208015, 0x6129e844, 0x7d2903a6, 0x4e800420}, .jmp_to_custom = {0x3d208178, 0x61292f20, 0x7d2903a6, 0x4e800420}},
        {.addr = RRC_DVD_CLOSE, .backjmp_to_original = {0x3d208015, 0x6129e578, 0x7d2903a6, 0x4e800420}, .jmp_to_custom = {0x3d208178, 0x61292f60, 0x7d2903a6, 0x4e800420}},
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

                // 32 bytes (4 instructions for the backjmp + 4 overwritten instructions restored) per patched function.
                // This is the start of the trampoline.
                u32 *hooked_addr = (u32 *)(0x93400000 + (j * 32));

                // Prepare the trampoline: copy the first 4 instructions of the original function that we're about to overwrite to the start,
                // and append the `backjmp_to_original` instructions.
                memcpy(hooked_addr, virt_target, 16);
                memcpy(hooked_addr + 4, entry.backjmp_to_original, 16);
                DCFlushRange(hooked_addr, 32);
                ICInvalidateRange(hooked_addr, 32);

                // Overwrite the original function with a jump to the custom DVD function.
                memcpy(virt_target, entry.jmp_to_custom, 16);
                DCFlushRange(virt_target, 32);
                ICInvalidateRange(virt_target, 32);
            }
        }
    }
}

static void load_pulsar_loader(struct rrc_dol *dol)
{
    RRC_ASSERTEQ(dol->section_addr[0], 0x80004000, "addr");
    u8 *pul_dest = (u8 *)((u32)dol + dol->section[0]);
    u8 *original_pul_dest = pul_dest;

    FILE *loader_pul_file = fopen("RetroRewind6/Binaries/Loader.pul", "r");
    RRC_ASSERT(loader_pul_file != NULL, "failed to open");
    int read = 0;
    while ((read = fread(pul_dest, 1, 4096, loader_pul_file)))
    {
        pul_dest += read;
    }
    DCFlushRange(original_pul_dest, align_up(pul_dest - original_pul_dest, 32));
    ICInvalidateRange(original_pul_dest, align_up(pul_dest - original_pul_dest, 32));

    fclose(loader_pul_file);
}

static void load_runtime_ext()
{
    FILE *patch_file = fopen("runtime-ext.dol", "r");
    RRC_ASSERT(patch_file != NULL, "patch file");
    struct rrc_dol patch_dol __attribute__((aligned(32)));

    int read = fread((void *)&patch_dol, sizeof(patch_dol), 1, patch_file);
    RRC_ASSERTEQ(read, 1, "read dol");

    for (int i = 0; i < patch_dol.bss_size; i++)
        *(u8 *)(patch_dol.bss_addr + i) = 0;

    DCFlushRange((void *)align_down(patch_dol.bss_addr, 32), align_up(patch_dol.bss_size, 32));
    ICInvalidateRange((void *)align_down(patch_dol.bss_addr, 32), align_up(patch_dol.bss_size, 32));

    for (int i = 0; i < RRC_DOL_SECTION_COUNT; i++)
    {
        u32 sec = patch_dol.section[i];
        u32 sec_addr = patch_dol.section_addr[i];
        u32 sec_size = patch_dol.section_size[i];
        if (sec_addr == 0)
            continue;

        if (sec_addr + sec_size > 0x817fffff)
        {
            RRC_FATAL("section %d overflows MEM1: %x + %x > 0x817fffff", i, sec_addr, sec_size);
        }

        fseek(patch_file, sec, SEEK_SET);
        read = fread((void *)sec_addr, sec_size, 1, patch_file);
        DCFlushRange((void *)align_down(sec_addr, 32), align_up(sec_size + 32, 32));
        ICInvalidateRange((void *)align_down(sec_addr, 32), align_up(sec_size + 32, 32));
        RRC_ASSERTEQ(read, 1, "fully read section");
    }
}

typedef void (*patch_dol_func_t)(struct rrc_dol *, struct rrc_riivo_memory_patch *, int, void (*)(void *, u32), void (*)(void *, u32));

/**
 * Wrapper function around `patch_dol` that sets up the stack pointer to a safe location (workaround for missing support for __attribute__((naked))).
 */
void patch_dol_helper(
    /* r3 */ struct rrc_dol *dol,
    /* r4 */ struct rrc_riivo_memory_patch *mem_patches,
    /* r5 */ int mem_patch_count,
    /* r6 */ void (*ic_invalidate_range)(void *, u32),
    /* r7 */ void (*dc_flush_range)(void *, u32),
    /* r8 */ patch_dol_func_t);

asm("patch_dol_helper:\n"
    // Adjust the stack pointer to 0x808ffa00 (arbitrary, temporary, random safe address not used by game sections)
    // so we don't overwrite local variables while copying sections.
    "lis 9, -32625\n"
    "ori 9, 9, 64000\n"
    "mr 1,9\n"
    // Jump to the function in r8 (patch_dol). All other arguments are already in the right registers (r3-r7).
    "mtctr 8\n"
    "bctrl\n");

void rrc_loader_load(struct rrc_dol *dol, struct rrc_settingsfile *settings, void *bi2_dest, u32 mem1_hi, u32 mem2_hi)
{
    patch_dvd_functions(dol);
    load_pulsar_loader(dol);
    load_runtime_ext();

    struct rrc_riivo_memory_patch *mem_patches;
    int mem_patches_count;
    parse_riivo_patches(settings, &mem1_hi, &mem2_hi, &mem_patches, &mem_patches_count);

    for (int i = 0; i < mem_patches_count; i++)
    {
        struct rrc_riivo_memory_patch *patch = &mem_patches[i];
        SYS_Report("[DEBUG] Memory patch: %08x -> %08x (original: %08x)\n",
                   patch->addr, patch->value, patch->original);
    }

    rrc_con_update("Prepare For Patching: Patch Memory Map", 65);

    // Addresses are taken from <https://wiibrew.org/wiki/Memory_map> for the most part.

    *(u32 *)0xCD006C00 = 0x00000000;           // Reset `AI_CONTROL` to fix audio
    *(u32 *)0x80000034 = 0;                    // Arena High
    *(u32 *)0x800000EC = 0x81800000;           // Dev Debugger Monitor Address
    *(u32 *)0x800000F0 = 0x01800000;           // Simulated Memory Size
    *(u32 *)0x800000F4 = (u32)bi2_dest;        // Pointer to bi2
    *(u32 *)0x800000F8 = 0x0E7BE2C0;           // Console Bus Speed
    *(u32 *)0x800000FC = 0x2B73A840;           // Console CPU Speed
    *(u32 *)0x80003110 = mem1_hi;              // MEM1 Arena End
    *(u32 *)0x80003124 = 0x90000800;           // Usable MEM2 Start
    *(u32 *)0x80003128 = mem2_hi;              // Usable MEM2 End
    *(u32 *)0x80003180 = *(u32 *)(0x80000000); // Game ID
    *(u32 *)0x80003188 = *(u32 *)(0x80003140); // Minimum IOS Version

    memcpy((u32 *)0x80000000, "RMCP01", 6);
    DCFlushRange((u32 *)0x80000000, 32);
    memcpy((u32 *)0x80003180, "RMCP", 4);
    DCFlushRange((u32 *)0x80003180, 32);

    if (*(u32 *)((u32)bi2_dest + 0x30) == 0x7ED40000)
    {
        *(u8 *)0x8000319C = 0x81; // Disc is dual layer
    }
    else
    {
        *(u8 *)0x8000319C = 0x80; // Disc is single layer
    }
    ICInvalidateRange((void *)0x80000000, 0x3400);
    DCFlushRange((void *)0x80000000, 0x01800000);

    rrc_con_update("Patch and Launch Game", 75);

    wiisocket_deinit();

    // The last step is to copy the sections from the safe space to where they actually need to be.
    // This requires copying the function itself to the safe address space so we don't overwrite ourselves.
    // It also needs to call `DCFlushRange` but cannot reference it in the function, so we copy it and pass it as a function pointer.
    // See patch.c comment for a more detailed explanation.

    patch_dol_func_t patch_copy = (void *)RRC_PATCH_COPY_ADDRESS;

    memcpy(patch_copy, patch_dol, PATCH_DOL_LEN);
    DCFlushRange(patch_copy, align_up(PATCH_DOL_LEN, 32));
    ICInvalidateRange(patch_copy, align_up(PATCH_DOL_LEN, 32));

    void (*ic_invalidate_range)() = (void *)align_up(RRC_PATCH_COPY_ADDRESS + PATCH_DOL_LEN, 32);
    memcpy(ic_invalidate_range, ICInvalidateRange, 64);
    DCFlushRange(ic_invalidate_range, 64);
    ICInvalidateRange(ic_invalidate_range, 64);

    void (*dc_flush_range)() = (void *)align_up(RRC_PATCH_COPY_ADDRESS + PATCH_DOL_LEN + 64, 32);
    memcpy(dc_flush_range, DCFlushRange, 64);
    DCFlushRange(dc_flush_range, 64);
    ICInvalidateRange(dc_flush_range, 64);

    __IOS_ShutdownSubsystems();
    for (u32 i = 0; i < 32; i++)
    {
        IOS_Close(i);
    }

    IRQ_Disable();

    patch_dol_helper(
        dol,
        mem_patches,
        mem_patches_count,
        ic_invalidate_range,
        dc_flush_range,
        patch_copy);
}
