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
  // Not macOS.
#ifdef _WIN32
  // Mutex for the HandleWrapper map.
  uv_mutex_t handle_wrap_map_mutex;
  // The events to be waited on.
  std::vector<HANDLE> events;
  // The dummy event to wakeup the thread.
  HANDLE wake_up_event;
  // The dummy event to ensure we are not waiting on a file handle when
  // destroying it.
  HANDLE file_handles_free_event;
#endif
#ifdef __linux__
  // Linux.
  int inotify;
  int init_errno;
#endif
};
