#include <gctypes.h>
#include <fcntl.h>
#include <string.h>
#include "sd.h"
#include "slots.h"
#include "dvd.h"
#include "util.h"
#include "../brainslug-wii/bslug_include/io/fat.h"
#include "../brainslug-wii/bslug_include/io/fat-sd.h"
#include "../brainslug-wii/bslug_include/rvl/cache.h"
#include "../../source/riivo.h"

__attribute__((section(".set_riivo_disc_ptr"))) static struct rrc_riivo_disc *riivo_disc = NULL;

#define DVD_CONVERT_PATH_TO_ENTRYNUM_ADDR 0x93400000
#define DVD_FAST_OPEN 0x93400020
#define DVD_OPEN 0x93400040
#define DVD_READ_PRIO 0x93400060
#define DVD_CLOSE 0x93400080

#define SPECIAL_ENTRYNUM (0b0111111101 << 22)
#define SPECIAL_ENTRYNUM_MASK (0b1111111111 << 22)

#define MAX_PATH_LEN 64
#define ENTRYNUM_SLOTS 1000

struct rrc_rt_slot
{
    s32 sd_fd;
    s32 filesize;
    s32 sd_refcount;
    bool in_use;
    char path[MAX_PATH_LEN];
};

static struct rrc_rt_slot special_entrynum_slots[ENTRYNUM_SLOTS] = {0};

static s32 path_to_entrynum(const char *path)
{
    int next_free_slot = -1;
    for (int i = 0; i < ENTRYNUM_SLOTS; i++)
    {
        if (!special_entrynum_slots[i].in_use)
        {
            next_free_slot = i;
            break;
        }
        if (strcmp(special_entrynum_slots[i].path, path) == 0)
        {
            // Found a match
            return i;
        }
    }
    if (next_free_slot == -1)
    {
        FATAL("Out of slots");
    }

    // Path doesn't have an entrynum yet and we have a free slot.
    special_entrynum_slots[next_free_slot].in_use = true;
    strncpy(special_entrynum_slots[next_free_slot].path, path, MAX_PATH_LEN);
    special_entrynum_slots[next_free_slot].sd_fd = -1;
    special_entrynum_slots[next_free_slot].filesize = 0;
    special_entrynum_slots[next_free_slot].sd_refcount = 0;
    return next_free_slot;
}

static FILE_STRUCT fs; // TODO: shouldn't this actually be stored in entrynum_to_path?

static bool try_path_to_entry_num(const char *filename, s32 *entry_num)
{
    rrc_rt_sd_init();

    for (int i = riivo_disc->count - 1; i >= 0; i--)
    {
        const struct rrc_riivo_disc_replacement *replacement = &riivo_disc->replacements[i];
        switch (replacement->type)
        {
        case RRC_RIIVO_FILE_REPLACEMENT:
        {
            const char *disc_path = replacement->disc;
            if (*disc_path == '/')
            {
                disc_path++;
            }
            const char *ffilename = filename;
            if (*ffilename == '/')
            {
                ffilename++;
            }

            if (strcmp(disc_path, ffilename) == 0)
            {
                if (rrc_rt_sd_file_exists(replacement->external))
                {
                    OS_Report("Found a file replacement! %d (%s)\n", i, disc_path);
                    *entry_num = path_to_entrynum(replacement->external);
                    return true;
                }
            }
            break;
        }
        case RRC_RIIVO_FOLDER_REPLACEMENT:
        {
            const char *external_path = replacement->external;
            const char *disc_path = replacement->disc;

            int disc_len = strlen(disc_path);
            int external_len = strlen(external_path);
            int filename_len = strlen(filename);
            if (disc_len > filename_len)
            {
                break;
            }

            bool matches = true;
            int fi = 0;
            for (int di = 0; di < disc_len; di++)
            {
                // NB: filename_len >= disc_len, so any `di` is also valid for `filename`
                if (di == 0 && disc_path[0] == '/' && filename[0] != '/')
                {
                    // No explicit / in the requested filename. Allow this.
                    continue;
                }
                if (disc_path[di] != filename[fi])
                {
                    matches = false;
                    break;
                }
                fi++;
            }
            OS_Report("Found folder rename: '%s' == '%s' -> %d %d\n", disc_path, filename, matches, fi);

            if (matches)
            {
                // It matches. Let's see if the file actually exists in the directory.
                struct stat st;

                char new_path[64];
                char *path_ptr = new_path;
                strncpy(new_path, external_path, 64);
                path_ptr += external_len;
                if (filename[fi] != '/' && external_path[external_len - 1] != '/')
                {
                    // Add a /
                    *path_ptr = '/';
                    path_ptr++;
                }
                strncpy(path_ptr, filename + fi, 64 - ((u32)path_ptr - (u32)new_path));
                OS_Report("Try open '%s'\n", new_path);
                if (rrc_rt_sd_file_exists(new_path))
                {
                    OS_Report("Found a folder replacement! %d (%s %s %s %s)\n", i, disc_path, external_path, filename, new_path);
                    *entry_num = path_to_entrynum(new_path);
                    return true;
                }
                else
                {
                    OS_Report("NOTE: %s not applied because it doesn't exist.\n", disc_path);
                }
            }

            break;
        }
        }
    }

    return false;
}

__attribute__((noinline))
s32
custom_convert_path_to_entry_num_impl(const char *filename)
{

    OS_Report("ConvertPathToEntrynum(%s)\n", filename);

    s32 entry_num;
    if (try_path_to_entry_num(filename, &entry_num))
    {
        OS_Report("Found entrynum replacement: %d\n", entry_num);
        return SPECIAL_ENTRYNUM | entry_num;
    }

    // Return to original overwritten function
    s32 (*cb)(const char *) = (void *)DVD_CONVERT_PATH_TO_ENTRYNUM_ADDR;
    s32 res = cb(filename);
    if ((res & SPECIAL_ENTRYNUM_MASK) == SPECIAL_ENTRYNUM)
    {
        OS_Report("Res = %d\n", res);
        FATAL("DVD Convert path returned special entry");
    }
    else
    {
        return res;
    }
}

static void open_entry_num(s32 entry_num, FileInfo *file_info)
{
    struct rrc_rt_slot *etp = &special_entrynum_slots[entry_num];

    if (etp->sd_fd != -1)
    {
        OS_Report("FastOpen: reusing fd %d\n", etp->sd_fd);
        file_info->startAddr = SPECIAL_ENTRYNUM | entry_num;
        file_info->length = etp->filesize;
    }
    else
    {
        etp->sd_fd = SD_open(&fs, etp->path, O_RDONLY);
        OS_Report("Open path '%s', fd = %d\n", etp->path, etp->sd_fd);
        if (etp->sd_fd == -1)
        {
            FATAL("FastOpen: SD error!");
        }
        etp->filesize = fs.filesize;
        file_info->startAddr = SPECIAL_ENTRYNUM | entry_num;
        file_info->length = fs.filesize;
    }
    etp->sd_refcount++;
}

__attribute__((noinline))
s32
custom_open_impl(const char *path, FileInfo *file_info)
{
    OS_Report("Open(%s)\n", path);
    s32 entry_num;
    if (try_path_to_entry_num(path, &entry_num))
    {
        open_entry_num(entry_num, file_info);
        OS_Report("Found entrynum replacement: %d (addr %d)\n", entry_num, file_info->startAddr);
        return 1;
    }

    s32 (*cb)(const char *, FileInfo *) = (void *)DVD_OPEN;
    s32 res = cb(path, file_info);
    OS_Report("Default DVD Open (%d) address: %d\n", res, file_info->startAddr);
    return res;
}

__attribute__((noinline))
s32
custom_fast_open_impl(s32 entry_num, FileInfo *file_info)
{
    OS_Report("FastOpen(%d)\n", entry_num);

    if ((entry_num & SPECIAL_ENTRYNUM_MASK) == SPECIAL_ENTRYNUM)
    {
        entry_num &= ~SPECIAL_ENTRYNUM_MASK;

        open_entry_num(entry_num, file_info);
        return 1;
    }

    // Return to original overwritten function
    s32 (*cb)(s32, FileInfo *) = (void *)DVD_FAST_OPEN;
    s32 res = cb(entry_num, file_info);
    if (res != -1 && (file_info->startAddr & SPECIAL_ENTRYNUM_MASK) == SPECIAL_ENTRYNUM)
    {
        FATAL("Normal FastOpen() returned special bitpattern");
    }
    return res;
}

__attribute__((noinline))
s32
custom_read_prio_impl(FileInfo *file_info, void *buffer, s32 length, s32 offset, s32 prio)
{
    OS_Report("ReadPrio(%x, %d, %d) (startAddr=%d,size=%d)\n", buffer, length, offset, file_info->startAddr, file_info->length);

    if ((file_info->startAddr & SPECIAL_ENTRYNUM_MASK) == SPECIAL_ENTRYNUM)
    {
        int slot = file_info->startAddr & ~SPECIAL_ENTRYNUM_MASK;
        OS_Report("ReadPrio slot = %d\n", slot);

        struct rrc_rt_slot *etp = &special_entrynum_slots[slot];
        if (!etp->in_use)
        {
            FATAL("Uninitialized slot used in ReadPrio!");
        }

        if (etp->sd_fd == -1)
        {
            OS_Report("ReadPrio: fd %d is already closed!\n", etp->sd_fd);
            return -1;
        }

        if (SD_seek(etp->sd_fd, offset, 0) == -1)
        {
            OS_Report("Warning: Failed to seek in ReadPrio!\n");
        }
        OS_Report("Sd fd = %d\n", etp->sd_fd);
        int bytes = SD_read(etp->sd_fd, buffer, length);
        if (bytes == -1)
        {
            FATAL("Failed to read bytes in ReadPrio!");
        }

        DCFlushRange(buffer, align_up(length, 32));
        return bytes;
    }

    s32 (*cb)(FileInfo *, void *, s32, s32, s32) = (void *)DVD_READ_PRIO;
    return cb(file_info, buffer, length, offset, prio);
}

__attribute__((noinline))
s32
custom_close_impl(FileInfo *file_info)
{
    OS_Report("Close(%d)\n", file_info->startAddr);

    if ((file_info->startAddr & SPECIAL_ENTRYNUM_MASK) == SPECIAL_ENTRYNUM)
    {
        struct rrc_rt_slot *etp = &special_entrynum_slots[file_info->startAddr & ~SPECIAL_ENTRYNUM_MASK];

        if (!etp->in_use)
        {
            FATAL("Attempted to close slot that is uninitialized!");
        }

        if (etp->sd_fd == -1)
        {
            OS_Report("Close: fd %d is already closed!\n", etp->sd_fd);
            return 1;
        }

        etp->sd_refcount--;
        if (etp->sd_refcount == 0)
        {
            if (SD_close(etp->sd_fd) == -1)
            {
                FATAL("Failed to close SD file due to SD error!");
            }
            etp->sd_fd = -1;
        }
        return 1;
    }

    bool (*cb)(FileInfo *) = (void *)0x80162fec; // call DVDCancel(), special case
    cb(file_info);
    return 1;
}
