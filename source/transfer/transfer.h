#ifndef RRC_TRANSFER_H
#define RRC_TRANSFER_H

// Extra sanity assert that we never do any such NAND shenanigans on console if other places start calling these functions
// without a dolphin check.
#define RRC_ASSERT_DOLPHIN() RRC_ASSERT(rrc_is_dolphin(), "file transfer should only happen on dolphin")

#endif