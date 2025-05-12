#include <stdbool.h>
#include <gctypes.h>
#include <fcntl.h>
#include "util.h"
#include "../brainslug-wii/bslug_include/io/fat.h"
#include "../brainslug-wii/bslug_include/io/fat-sd.h"
#include "../brainslug-wii/bslug_include/stdio.h"
#include "../brainslug-wii/bslug_include/errno.h"

s32 rrc_rt_sd_init()
{
    static bool mounted = false;
    if (!mounted)
    {
        int res = SD_Mount();
        if (res != 0)
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "SD_Mount failed: %d (errno:%d)\n", res, errno);
            FATAL(buf);
        }
        mounted = true;
        res = SD_chdir("sd:/");
        if (res != 0)
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "SD_chdhir failed: %d (errno:%d)\n", res, errno);
            FATAL(buf);
        }
        return res;
    }
    return 0;
}

bool rrc_rt_sd_file_exists(const char *path)
{
    FILE_STRUCT fs;
    s32 tmpfd = SD_open(&fs, path, O_RDONLY);
    if (tmpfd != -1)
    {
        OS_Report("DEBUG: File size of %s: %d\n", path, fs.filesize);
        SD_close(tmpfd);
        return true;
    }

    return false;
}
