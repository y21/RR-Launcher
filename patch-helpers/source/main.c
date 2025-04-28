#include <stdbool.h>
#include <gctypes.h>
#include <fcntl.h>
#include <string.h>
#include <fcntl.h>
#include "../brainslug-wii/modules/libfat/fatfile.h"
#include "../brainslug-wii/bslug_include/io/fat-sd.h"
#include "../brainslug-wii/bslug_include/rvl/cache.h"

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

#define DVD_CONVERT_PATH_TO_ENTRYNUM_ADDR 0x93400000
#define DVD_FAST_OPEN 0x93400020
#define DVD_OPEN 0x93400040
#define DVD_READ_PRIO 0x93400060
#define DVD_CLOSE 0x93400080

s32 custom_convert_path_to_entrynum(const char *filename)
{
	init_sd();

	OS_Report("ConvertPathToEntrynum(%s)\n", filename);
	if (strcmp(filename, "/Binaries/Code.pul") == 0)
	{
		OS_Report("found\n");
		static FILE_STRUCT fs;
		code_pul_fd = SD_open(&fs, "RetroRewind6/Binaries/Code.pul", O_RDONLY);
		OS_Report("Result -> %d, file size:  %d\n", code_pul_fd, fs.filesize);
		return 918273645;
	}

	// Return to original overwritten function
	s32 (*cb)(const char *) = (void *)DVD_CONVERT_PATH_TO_ENTRYNUM_ADDR;
	return cb(filename);
}

s32 custom_fast_open(s32 entry_num, FileInfo *file_info)
{
	OS_Report("FastOpen(%d)\n", entry_num);
	if (entry_num == 918273645)
	{
		file_info->startAddr = (u32)-1;
		return 1;
	}

	// Return to original overwritten function
	s32 (*cb)(s32, FileInfo *) = (void *)DVD_FAST_OPEN;
	return cb(entry_num, file_info);
}

s32 custom_read_prio(FileInfo *file_info, void *buffer, s32 length, s32 offset, s32 prio)
{
	OS_Report("ReadPrio(%x, %d, %d) (startAddr=%d)\n", buffer, length, offset, file_info->startAddr);
	if (file_info->startAddr == (u32)-1)
	{
		SD_seek(code_pul_fd, offset, 0);
		int bytes = SD_read(code_pul_fd, buffer, length);
		DCFlushRange(buffer, length);
		OS_Report("Read %d bytes.\n", bytes);
		return bytes;
	}

	s32 (*cb)(FileInfo *, void *, s32, s32, s32) = (void *)DVD_READ_PRIO;
	return cb(file_info, buffer, length, offset, prio);
}

bool custom_close(FileInfo *file_info)
{
	OS_Report("Close(%d)\n", file_info->startAddr);
	if (file_info->startAddr == (u32)-1)
	{
		return 1;
	}
	bool (*cb)(FileInfo *) = (void *)0x80162fec; // call DVDCancel()
	return cb(file_info);
}

int main()
{
	// Prevent linker gcing the functions
	*(volatile u32 *)custom_convert_path_to_entrynum;
	*(volatile u32 *)custom_fast_open;
	*(volatile u32 *)custom_read_prio;
	*(volatile u32 *)custom_close;

	while (1)
		;
}
