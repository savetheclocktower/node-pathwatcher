#include "common.h"
#pragma once

class AddonData final {
public:
  explicit AddonData(Napi::Env env) {}

  Napi::FunctionReference callback;
  PathWatcherWorker* worker;
  int watch_count;

#ifdef __APPLE__
  // macOS.
  int kqueue;
  int init_errno;
#endif

#ifdef __linux__
  // Linux.
  int inotify;
  int init_errno;
#endif
};
