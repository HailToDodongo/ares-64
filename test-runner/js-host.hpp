#pragma once

#include <nall/nall.hpp>
#include <vector>

//QuickJS scripting host wrapping EmulatorRunner as the global `ares` object.
//All bindings are synchronous: the emulator advances inline inside wait*() calls.
auto jsHostInit(const std::vector<nall::string>& scriptArgs) -> bool;
auto jsHostEvalFile(const nall::string& path) -> bool;  //false = uncaught exception (already printed)
auto jsHostShutdown() -> void;
