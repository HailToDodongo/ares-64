#pragma once
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

NORETURN void
msg_error(const char *err, ...);
void
msg_warning(const char *err, ...);
void
msg_debug(const char *err, ...);

#ifdef __cplusplus
}
#endif
