#ifndef RRC_INTERNER_H

#define RRC_INTERNER_H

#include <gctypes.h>

enum rrc_interner_cmp_result {
    RRC_INTERNER_CMP_LESS,
    RRC_INTERNER_CMP_EQ,
    RRC_INTERNER_CMP_GREATER,
};

typedef enum rrc_interner_cmp_result (*rrc_interner_compare_fn)(void*, void*);

struct rrc_interner {
    void* ptr;
    u32 size;
    u32 length;
    u32 value_size;
    rrc_interner_compare_fn comparator;
};

void rrc_interner_init(struct rrc_interner* interner, rrc_interner_compare_fn comparator, u32 value_size);

void* rrc_interner_intern(struct rrc_interner* interner, void* data);

enum rrc_interner_cmp_result rrc_interner_string_comparator(void*, void*);



#endif