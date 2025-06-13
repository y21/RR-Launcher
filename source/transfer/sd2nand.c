#include "sd2nand.h"
#include "../util.h"
#include "transfer.h"
#include <ogc/isfs.h>
#include <ctype.h>
#include <dirent.h>

#include <ogc/system.h>

static struct rrc_result rrc_sd_to_nand_copy_file(const char *sdpath, const char *nandpath, bool allow_enoent)
{
    FILE *sdfd = fopen(sdpath, "r");
    if (sdfd == NULL)
    {
        if (errno == ENOENT && allow_enoent)
        {
            return rrc_result_success;
        }

        RRC_FATAL("Failed to open sd file: %d", errno);
    }
    s32 create_nand_res = ISFS_CreateFile(nandpath, 0, 3, 3, 3);
    if (create_nand_res != 0 && create_nand_res != -105)
    {
        RRC_FATAL("failed to create nand file: %d", create_nand_res);
    }

    s32 nandfd = ISFS_Open(nandpath, ISFS_OPEN_WRITE);
    RRC_ASSERT(nandfd >= 0, "failed to open nand file");

    char buf[4096];
    int read;
    while ((read = fread(buf, 1, sizeof(buf), sdfd)) > 0)
    {
        RRC_ASSERTEQ(ISFS_Write(nandfd, buf, read), read, "failed to fully write nand file");
    }

    fclose(sdfd);
    ISFS_Close(nandfd);

    return rrc_result_success;
}

#define MAX_GHOST_PATH_SIZE 128

typedef struct rrc_result (*walk_sd_dir_callback_t)(void *arg, const char *sdpath, const char *nandpath);

static struct rrc_result rrc_transfer_walk_sd_dir(const char *cur_sddirpath, walk_sd_dir_callback_t callback, void *arg)
{
#define EXPERTS_PATH "RetroRewind6/Ghosts/Experts"
    if (strncmp(cur_sddirpath, EXPERTS_PATH, strlen(EXPERTS_PATH)) == 0)
    {
        // Don't copy ESGs.
        return rrc_result_success;
    }

    char sdpath[MAX_GHOST_PATH_SIZE];
    char nandpath[MAX_GHOST_PATH_SIZE];
    snprintf(nandpath, sizeof(nandpath), "/shared2/Pulsar/%s", cur_sddirpath);
    s32 dirret = ISFS_CreateDir(nandpath, 0, 3, 3, 3);
    if (dirret != 0 && dirret != -105)
    {
        RRC_FATAL("failed to create nand dir: %d", dirret);
    }

    int cur_sddirpath_len = strlen(cur_sddirpath);

    DIR *dir = opendir(cur_sddirpath);
    RRC_ASSERT(dir != NULL, "failed to open directory");

    struct dirent *dirent;
    while ((dirent = readdir(dir)) != NULL)
    {
        RRC_ASSERT(cur_sddirpath_len + strlen(dirent->d_name) + 1 < MAX_GHOST_PATH_SIZE, "sd path too long");

        snprintf(sdpath, sizeof(sdpath), "%s/%s", cur_sddirpath, dirent->d_name);
        if (dirent->d_type == DT_DIR)
        {
            if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0)
            {
                continue;
            }

            // Directory. Recurse.
            TRY(rrc_transfer_walk_sd_dir(sdpath, callback, arg));
        }
        else if (dirent->d_type == DT_REG)
        {
            // Regular file.
            snprintf(nandpath, sizeof(nandpath), "/shared2/Pulsar/%s", sdpath);
            TRY(callback(arg, sdpath, nandpath));
        }
    }

    closedir(dir);
    return rrc_result_success;
}

static struct rrc_result ghost_count_callback(void *arg, const char *sdpath, const char *nandpath)
{
    *(u32 *)arg += 1;
    return rrc_result_success;
}

struct walkdir_callback_state
{
    u32 total;
    u32 processed;
};

struct rrc_result
main_transfer_callback(void *arg, const char *sdpath, const char *nandpath)
{
    struct walkdir_callback_state *state = (struct walkdir_callback_state *)arg;
    char message[128];
    snprintf(message, sizeof(message), "Copying %s to %s", sdpath, nandpath);
    rrc_con_update(message, (int)(((f32)state->processed / (f32)state->total) * 100.0));

    rrc_sd_to_nand_copy_file(sdpath, nandpath, false);
    state->processed++;
    return rrc_result_success;
}

void rrc_sd_to_nand_create_basepath_dirs()
{
    const char *basepath = "/shared2/Pulsar/RetroRewind6/";
    for (int i = 1; i < strlen(basepath); i++)
    {
        if (basepath[i] == '/')
        {
            char tmp[32];
            memcpy(tmp, basepath, i);
            tmp[i] = 0;
            ISFS_CreateDir(tmp, 0, 3, 3, 3);
        }
    }
}

struct rrc_result
rrc_sd_to_nand(char region)
{
    RRC_ASSERT_DOLPHIN();
    ISFS_Initialize();

    char sdpath[64], nandpath[64];

    // Copy rksys file.
    snprintf(sdpath, sizeof(sdpath), "/riivolution/save/RetroWFC/RMC%c/rksys.dat", region);
    snprintf(nandpath, sizeof(nandpath), "/title/00010004/524d43%02x/data/rksys.dat", toupper(region));
    TRY(rrc_sd_to_nand_copy_file(sdpath, nandpath, true));

    // Create any missing NAND directories in the RR path.
    rrc_sd_to_nand_create_basepath_dirs();

    // Count ghost files for the progress bar.
    u32 ghost_count = 0;
    TRY(rrc_transfer_walk_sd_dir("RetroRewind6/Ghosts", ghost_count_callback, &ghost_count));

    // Start copying ghosts.
    struct walkdir_callback_state state = {.processed = 0, .total = ghost_count};
    TRY(rrc_transfer_walk_sd_dir("RetroRewind6/Ghosts", main_transfer_callback, &state));

    return rrc_result_success;
}
