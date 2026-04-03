#ifndef RTE_PROFILE
#define RTE_PROFILE

#include "sd.h"
#include "util.h"
#include <inttypes.h>
#include <rvl/OSTime.h>

#ifdef PROFILE
#define RTE_PROFILE_START(name) u64 name_tick_start = OSGetTime();

#define RTE_PROFILE_END(name, extra_data)                                      \
  u64 name_tick_diff = OSGetTime() - name_tick_start;                          \
  FILE_STRUCT fs;                                                              \
  int res = SD_open(&fs, "RetroRewindChannel/logs.txt", O_APPEND | O_WRONLY | O_CREAT);   \
  if (res == -1) {                                                             \
    RTE_FATAL("failed to open logs file");                                     \
  }                                                                            \
  char name_buf[64];                                                           \
  snprintf(name_buf, sizeof(name_buf), "[%s] %" PRIu64 " us (%s)\n",           \
           RTE_STRINGIFY(name), ticks_to_microsecs(name_tick_diff),            \
           extra_data);                                                        \
  SD_write(res, name_buf, strlen(name_buf));                                   \
  SD_close(res);                                                               \
  OS_Report(name_buf);

#endif

#ifndef PROFILE
#define RTE_PROFILE_START(name)
#define RTE_PROFILE_END(name, extra)

#endif

#endif