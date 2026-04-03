#include <stdlib.h>
#include <string.h>
#include "interner.h"
#include "util.h"

#define DEFAULT_INTERNER_SIZE 4

static void* get_index(struct rrc_interner* interner, int index) {
    return (void*)(((u8*)interner->ptr) + interner->value_size * index);
}

void rrc_interner_init(struct rrc_interner* interner, rrc_interner_compare_fn comparator, u32 value_size) {
    interner->ptr = NULL;
    interner->size = 0;
    interner->length = 0;
    interner->value_size = value_size;
    interner->comparator = comparator;
}

void* rrc_interner_intern(struct rrc_interner* interner, void* data) {
    // Before actually trying to insert: check if the value already exists.
    for (int i = 0; i < interner->length; i++) {
        void* other = get_index(interner, i);
        enum rrc_interner_cmp_result res = interner->comparator(data, other);
        if (res == RRC_INTERNER_CMP_EQ) {
            return other;
        }
    }

    if (interner->length >= interner->size) {
        // Not enough space for another element. This includes the initial insert (0 >= 0) where we do the first allocation.
        if (interner->size == 0)
            interner->size = DEFAULT_INTERNER_SIZE;
        else
            interner->size *= 2;

        interner->ptr = realloc(interner->ptr, interner->size);

        if (interner->ptr == NULL) {
            RRC_FATAL("failed to allocate space for interner");
        }
    }

    // Note: at this point size > length, so there's guaranteed space for another element.
    void* end = get_index(interner, interner->length);
    memcpy(end, data, interner->value_size);
    interner->length++;
    return end;
}


enum rrc_interner_cmp_result rrc_interner_string_comparator(void* left, void* right) {
    s32 res = strcmp(left, right);
    if (res == 0) {
        return RRC_INTERNER_CMP_EQ;
    } else if (res < 0) {
        return RRC_INTERNER_CMP_LESS;
    } else {
        return RRC_INTERNER_CMP_GREATER;
    }
}