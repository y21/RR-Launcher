#ifndef RRC_RUNTIME_EXT_DVD
#define RRC_RUNTIME_EXT_DVD

typedef struct FileInfo FileInfo;
typedef struct CommandBlock CommandBlock;

typedef void (*Callback)(s32 result, FileInfo *fileInfo);
typedef void (*CBCallback)(s32 result, CommandBlock *block);

typedef struct
{
    char gameName[4];
    char company[2];
    u8 diskNumber;
    u8 gameVersion;
    u8 streaming;
    u8 streamingBufSize;
    u8 padding[14];
    u32 rvlMagic;
    u32 gcMagic;
} DiskID;

struct CommandBlock
{
    CommandBlock *next;
    CommandBlock *prev;
    u32 command;
    u32 state;
    u32 offset;
    u32 size;
    void *buffer;
    u32 curTransferSize;
    u32 transferredSize;
    DiskID *id;
    CBCallback callback;
    void *userData;
};

struct FileInfo
{
    CommandBlock commandBlock;
    u32 startAddr;
    u32 length;
    Callback callback;
};

s32 custom_convert_path_to_entry_num_impl(const char *filename);
s32 custom_open_impl(const char *path, FileInfo *file_info);
s32 custom_fast_open_impl(s32 entry_num, FileInfo *file_info);
s32 custom_read_prio_impl(FileInfo *file_info, void *buffer, s32 length, s32 offset, s32 prio);
s32 custom_close_impl(FileInfo *file_info);

#endif
