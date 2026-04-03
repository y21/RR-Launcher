#ifndef RTE_PROFILE
#define RTE_PROFILE

#include <rvl/OSTime.h>
#include <inttypes.h>
#include "util.h"

#ifdef PROFILE
#define RTE_PROFILE_START(name) \
    u64 name_tick_start = OSGetTime();

#define RTE_PROFILE_END(name, extra_data) \
    u64 name_tick_diff = OSGetTime() - name_tick_start; \
    OS_Report("[%s] %" PRIu64 " us (%s)\n", RTE_STRINGIFY(name), ticks_to_microsecs(name_tick_diff), extra_data);

#endif

#ifndef PROFILE
#define RTE_PROFILE_START(name)
#define RTE_PROFILE_END(name)

#endif

#endif