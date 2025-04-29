#ifndef RRC_RIIVO_H
#define RRC_RIIVO_H

enum rrc_riivo_disc_replacement_type
{
    RRC_RIIVO_FILE_REPLACEMENT,
    RRC_RIIVO_FOLDER_REPLACEMENT,
};

struct rrc_riivo_disc_replacement
{
    enum rrc_riivo_disc_replacement_type type;
    const char *external;
    const char *disc;
};

#endif
