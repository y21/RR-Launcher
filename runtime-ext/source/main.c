#include <stdbool.h>
#include <gctypes.h>
#include <fcntl.h>
#include <string.h>
#include "../brainslug-wii/modules/libfat/fatfile.h"
#include "../brainslug-wii/bslug_include/io/fat-sd.h"
#include "../brainslug-wii/bslug_include/rvl/cache.h"
#include "../../source/riivo.h"
#include "../../source/dol.h"

#include "util.h"
#include "dvd.h"

#define EXPORT_FUNCTION(secname, decl_args, call_args, implname) \
	__attribute__((section(secname))) s32 __##implname decl_args \
	{                                                            \
		return implname call_args;                               \
	}

EXPORT_FUNCTION(".dvd_convert_path_to_entrynum", (const char *path), (path), custom_convert_path_to_entry_num_impl);
EXPORT_FUNCTION(".dvd_open", (const char *path, FileInfo *file_info), (path, file_info), custom_open_impl);
EXPORT_FUNCTION(".dvd_fast_open", (s32 entry_num, FileInfo *file_info), (entry_num, file_info), custom_fast_open_impl);
EXPORT_FUNCTION(".dvd_read_prio", (FileInfo * file_info, void *buffer, s32 length, s32 offset, s32 prio), (file_info, buffer, length, offset, prio), custom_read_prio_impl);
EXPORT_FUNCTION(".dvd_close", (FileInfo * file_info), (file_info), custom_close_impl);

int main()
{
	// Prevent linker from DCE'ing the functions
	// TODO: check if this is actually needed or if the linker is smart enough to not DCE them
	*(volatile u32 *)__custom_convert_path_to_entry_num_impl;
	*(volatile u32 *)__custom_open_impl;
	*(volatile u32 *)__custom_fast_open_impl;
	*(volatile u32 *)__custom_read_prio_impl;
	*(volatile u32 *)__custom_close_impl;

	// Get the compiler to remove all unnecessary libogc deinitialization code, this function is never actually called
	while (1)
		;
}
