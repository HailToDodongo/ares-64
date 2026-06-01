#include <n64/n64.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

//Implementations of angrylion's external logging hooks (src/core/msg.h).
//Keeping these here avoids touching the vendored angrylion source.
extern "C" {

[[noreturn]] void msg_error(const char* fmt, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  ares::Nintendo64::rdp.crash(buffer);
  //msg_error is NORETURN in angrylion; we cannot safely resume the worker, so abort.
  //TODO(milestone 2): unwind to the RDP thread boundary instead of aborting.
  abort();
}

void msg_warning(const char* fmt, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  debug(unusual, "[angrylion] ", buffer);
}

void msg_debug(const char* fmt, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  debug(unusual, "[angrylion] ", buffer);
}

}
