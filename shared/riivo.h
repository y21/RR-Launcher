/*
    riivo.h - definition Riivolution types for XML parsing

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

#ifndef RRC_RIIVO_H
#define RRC_RIIVO_H

#include "types.h"
#include "path_tree.h"

#define RRC_RIIVO_XML_PATH "RetroRewind6/xml/RetroRewind6.xml"

enum rrc_riivo_disc_replacement_type
{
    RRC_RIIVO_FILE_REPLACEMENT,
    RRC_RIIVO_FOLDER_REPLACEMENT,
    /// It's technically a folder replacement, but in order to save space we just use this enum.
    RRC_RIIVO_MY_STUFF_REPLACEMENT,
};

struct rrc_riivo_disc_replacement
{
    enum rrc_riivo_disc_replacement_type type;
    /// The replacement path on the SD card.
    const char *external;
    /// The path on the disc itself.
    const char *disc;
    /// If a folder, this is a list of all found files in that folder. NULL if not a folder replacement.
    /// We need to handle the edge case where we may have too many entries here, so this is not considered
    /// guaranteed to be fully populated. We just hope it can catch 99% of lookups.
    const char **folder_contents;
    /// Same as above: 0 if not a folder replacement, otherwise the number of entries in `folder_contents`.
    int folder_contents_count;
};

struct rrc_riivo_disc
{
    u32 count;
    struct rrc_riivo_disc_replacement replacements[0];
};

struct rrc_riivo_file_patch
{
    struct rrc_path_component *disc_path;
    struct rrc_path_component *external_path;
    void *active_file;
};

struct rrc_riivo_disc_v2
{
    u32 filename_search_file_count;
    struct rrc_riivo_file_patch *filename_search_file_replacements;

    u32 absolute_file_count;
    struct rrc_riivo_file_patch *absolute_file_replacements;
};

struct rrc_riivo_memory_patch
{
    u32 addr;
    u32 value;
    u32 original; // uninitialized if !original_init
    bool original_init;
};

#endif
