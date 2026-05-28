#pragma once
#include <chrono>
#include <cstdio>

static auto _logStart = std::chrono::steady_clock::now();

#define LOG(fmt, ...) \
  do { \
    auto _ms = std::chrono::duration_cast<std::chrono::milliseconds>( \
      std::chrono::steady_clock::now() - _logStart).count(); \
    fprintf(stderr, "[%6lld.%02lld] " fmt "\n", \
      (long long)(_ms / 1000), (long long)(_ms % 1000) / 10, ##__VA_ARGS__); \
  } while(0)
