#include <stdbool.h>
#include <gctypes.h>
#include <fcntl.h>
#include <string.h>
#include <fcntl.h>
#include "../brainslug-wii/modules/libfat/fatfile.h"
#include "../brainslug-wii/bslug_include/io/fat-sd.h"
#include "../brainslug-wii/bslug_include/rvl/cache.h"
#include "../../source/riivo.h"

void OS_Report(const char *, ...);

typedef struct FileInfo FileInfo;
typedef struct CommandBlock CommandBlock;

typedef void (*Callback)(s32 result, FileInfo *fileInfo);
typedef void (*CBCallback)(s32 result, CommandBlock *block);

typedef struct
{
	char gameName[4];
	char company[2];
	u8 diskNumber; // 0x6
	u8 gameVersion;
	u8 streaming;		 // 0x8
	u8 streamingBufSize; // 0x9 0 = default
	u8 padding[14];		 // 0xa 0's are stored
	u32 rvlMagic;		 // 0x18 Revolution disk magic number
	u32 gcMagic;		 // 0x1c GC magic number is here
} DiskID;				 // 0x20

struct CommandBlock
{
	CommandBlock *next;
	CommandBlock *prev;	 // 4
	u32 command;		 // 8
	u32 state;			 // c
	u32 offset;			 // 10
	u32 size;			 // 14
	void *buffer;		 // 18
	u32 curTransferSize; // 1c
	u32 transferredSize; // 20
	DiskID *id;			 // 0x24
	CBCallback callback; // 0x28
	void *userData;		 // 0x2c
};

struct FileInfo
{
	CommandBlock commandBlock;
	u32 startAddr; // 0x30 disk address of file
	u32 length;	   // 0x34 file size in bytes
	Callback callback;
};

///////////////
///////////////
///////////////

bool mounted = false;

static void init_sd()
{
	static bool mounted = false;
	if (!mounted)
	{
		int res = SD_Mount();
		OS_Report("Mount = %d\n", res);
		mounted = true;
	}
}

static int code_pul_fd = -1;
static int title_szs_fd = -1;

#define DVD_CONVERT_PATH_TO_ENTRYNUM_ADDR 0x93400000
#define DVD_FAST_OPEN 0x93400020
#define DVD_OPEN 0x93400040
#define DVD_READ_PRIO 0x93400060
#define DVD_CLOSE 0x93400080

#define CODE_PUL_ENTRYNUM 918273645
#define TITLE_SZS_ENTRYNUM (CODE_PUL_ENTRYNUM + 1)

// Arbitrary bit pattern chosen that is unlikely to be a real SD fd or entrynum.
#define SPECIAL_ENTRYNUM (0b0111111101010101 << 16)
#define SPECIAL_ENTRYNUM_MASK (0b1111111111111111 << 16)

const struct rrc_riivo_disc *riivo_disc = (void *)0x816fd11c;

#define MAX_PATH_LEN 64
#define ENTRYNUM_SLOTS 32
struct entrynum_to_path
{
	// zero initialized
	bool in_use;
	char path[MAX_PATH_LEN];
	s32 sd_fd;
};

static struct entrynum_to_path special_entrynum_slots[ENTRYNUM_SLOTS] = {0};

#define FATAL(msg)              \
	do                          \
	{                           \
		OS_Report("%s\n", msg); \
		while (1)               \
			;                   \
	} while (0);

static u32 alloc_slot(const char *path)
{
	for (int i = 0; i < ENTRYNUM_SLOTS; i++)
	{
		if (!special_entrynum_slots[i].in_use)
		{
			if (strlen(path) >= MAX_PATH_LEN)
			{
				FATAL("Path too long!");
			}

			strcpy(special_entrynum_slots[i].path, path);
			special_entrynum_slots[i].in_use = true;
			return i;
		}
	}
	FATAL("Ran out of slots!?");
}

static FILE_STRUCT fs; // TODO: shouldn't this actually be stored in entrynum_to_path?

bool SD_file_exists(const char *path)
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

__attribute__((noinline))
s32
custom_convert_path_to_entry_num_impl(const char *filename)
{

	init_sd();
	// memcpy((u32 *)0x80000000, "RMCP01", 6);
	// memcpy((u32 *)0x80003180, "RMCP", 4);

	OS_Report("ConvertPathToEntrynum(%s)\n", filename);

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
				if (SD_file_exists(replacement->external))
				{
					OS_Report("Found a file replacement! %d (%s)\n", i, disc_path);
					u32 slot = alloc_slot(replacement->external);
					return SPECIAL_ENTRYNUM | slot;
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
				if (SD_file_exists(new_path))
				{
					OS_Report("Found a folder replacement! %d (%s %s %s %s)\n", i, disc_path, external_path, filename, new_path);
					u32 slot = alloc_slot(new_path);
					return SPECIAL_ENTRYNUM | slot;
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

	// Return to original overwritten function
	s32 (*cb)(const char *) = (void *)DVD_CONVERT_PATH_TO_ENTRYNUM_ADDR;
	s32 res = cb(filename);
	if ((res & SPECIAL_ENTRYNUM_MASK) == SPECIAL_ENTRYNUM)
	{
		OS_Report("Res = %d\n", res);
		FATAL("WARNING!!! DVD Convert path returned special entry");
	}
	else
	{
		return res;
	}
}

__attribute__((section(".dvd_convert_path_to_entrynum")))
s32
custom_convert_path_to_entrynum(const char *filename)
{
	return custom_convert_path_to_entry_num_impl(filename);
}

__attribute__((section(".dvd_open")))
s32
custom_open()
{
	// todo
	return -1;
}

__attribute__((section(".dvd_fast_open")))
s32
custom_fast_open(s32 entry_num, FileInfo *file_info)
{
	OS_Report("FastOpen(%d)\n", entry_num);
	if ((entry_num & SPECIAL_ENTRYNUM_MASK) == SPECIAL_ENTRYNUM)
	{
		entry_num &= ~SPECIAL_ENTRYNUM_MASK;

		if (entry_num >= ENTRYNUM_SLOTS)
		{
			FATAL("Entrynum out of bounds?? \n");
		}
		else
		{
			struct entrynum_to_path *etp = &special_entrynum_slots[entry_num];
			if (!etp->in_use)
			{
				FATAL("Use after free detected in FastOpen!")
			}
			if (etp->sd_fd != 0)
			{
				FATAL("SD file is possibly already opened!");
			}
			etp->sd_fd = SD_open(&fs, etp->path, O_RDONLY);
			OS_Report("Open path '%s', fd = %d\n", etp->path, etp->sd_fd);
			if (etp->sd_fd == -1)
			{
				FATAL("SD error!");
			}
			file_info->startAddr = SPECIAL_ENTRYNUM | entry_num;
			file_info->length = fs.filesize;
			return 1;
		}
	}
	// if (entry_num == CODE_PUL_ENTRYNUM)
	// {
	// 	code_pul_fd = SD_open(&fs, "RetroRewind6/Binaries/Code.pul", O_RDONLY);
	// 	if (code_pul_fd == -1)
	// 		return -1;
	// 	file_info->startAddr = -1;
	// 	file_info->length = fs.filesize;
	// 	return 1;
	// }
	// else if (entry_num == TITLE_SZS_ENTRYNUM)
	// {
	// 	title_szs_fd = SD_open(&fs, "RetroRewind6/UI/Title.szs", O_RDONLY);
	// 	if (title_szs_fd == -1)
	// 		return -1;
	// 	file_info->startAddr = -2;
	// 	file_info->length = fs.filesize;
	// 	return 1;
	// }
	else
	{
		// Return to original overwritten function
		s32 (*cb)(s32, FileInfo *) = (void *)DVD_FAST_OPEN;
		s32 res = cb(entry_num, file_info);
		if (res != -1 && (file_info->startAddr & SPECIAL_ENTRYNUM_MASK) == SPECIAL_ENTRYNUM)
		{
			FATAL("WARNING! Normal FastOpen() returned special bitpattern");
		}
		return res;
	}
}

__attribute__((section(".dvd_read_prio")))
s32
custom_read_prio(FileInfo *file_info, void *buffer, s32 length, s32 offset, s32 prio)
{
	OS_Report("ReadPrio(%x, %d, %d) (startAddr=%d,size=%d)\n", buffer, length, offset, file_info->startAddr, file_info->length);
	if ((file_info->startAddr & SPECIAL_ENTRYNUM_MASK) == SPECIAL_ENTRYNUM)
	{
		int slot = file_info->startAddr & ~SPECIAL_ENTRYNUM_MASK;
		OS_Report("ReadPrio slot = %d\n", slot);
		struct entrynum_to_path etp = special_entrynum_slots[slot];
		if (!etp.in_use)
		{
			FATAL("Use after free detected in ReadPrio!");
		}
		if (SD_seek(etp.sd_fd, offset, 0) == -1)
		{
			OS_Report("Warning: Failed to seek in ReadPrio!\n");
		}
		OS_Report("Sd fd = %d\n", etp.sd_fd);
		int bytes = SD_read(etp.sd_fd, buffer, length);
		if (bytes == -1)
		{
			FATAL("Failed to read bytes in ReadPrio!");
		}
		// TODO: align up length to 32
		DCFlushRange(buffer, length);
		return bytes;
	}
	// if (file_info->startAddr == (u32)-1)
	// {
	// 	SD_seek(code_pul_fd, offset, 0);
	// 	int bytes = SD_read(code_pul_fd, buffer, length);
	// 	DCFlushRange(buffer, length);
	// 	OS_Report("Read %d bytes.\n", bytes);
	// 	return bytes;
	// }
	// if (file_info->startAddr == (u32)-2)
	// {
	// 	SD_seek(title_szs_fd, offset, 0);
	// 	int bytes = SD_read(title_szs_fd, buffer, length);
	// 	DCFlushRange(buffer, length);
	// 	OS_Report("Read %d bytes.\n", bytes);
	// 	return bytes;
	// }

	s32 (*cb)(FileInfo *, void *, s32, s32, s32) = (void *)DVD_READ_PRIO;
	return cb(file_info, buffer, length, offset, prio);
}

__attribute__((section(".dvd_close"))) bool custom_close(FileInfo *file_info)
{
	OS_Report("Close(%d)\n", file_info->startAddr);
	if ((file_info->startAddr & SPECIAL_ENTRYNUM_MASK) == SPECIAL_ENTRYNUM)
	{
		struct entrynum_to_path *etp = &special_entrynum_slots[file_info->startAddr & ~SPECIAL_ENTRYNUM_MASK];

		if (!etp->in_use)
		{
			FATAL("Double free detected!");
		}

		if (SD_close(etp->sd_fd) == -1)
		{
			FATAL("Failed to close SD file!");
		}
		etp->in_use = false;
		etp->sd_fd = 0;
		return 1;
	}
	// if (file_info->startAddr == (u32)-1)
	// {
	// 	return SD_close(code_pul_fd);
	// }
	// if (file_info->startAddr == (u32)-2)
	// {
	// 	return SD_close(title_szs_fd);
	// }
	bool (*cb)(FileInfo *) = (void *)0x80162fec; // call DVDCancel()
	return cb(file_info);
}

int main()
{
	// Prevent linker gcing the functions
	*(volatile u32 *)custom_convert_path_to_entrynum;
	*(volatile u32 *)custom_open;
	*(volatile u32 *)custom_fast_open;
	*(volatile u32 *)custom_read_prio;
	*(volatile u32 *)custom_close;

	while (1)
		;
}
