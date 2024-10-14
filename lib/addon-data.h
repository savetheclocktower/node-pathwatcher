#include "core.h"
#pragma once

static int g_next_addon_data_id = 1;

class AddonData final {
public:
  explicit AddonData(Napi::Env env) {
    id = g_next_addon_data_id++;
  }

  // A unique identifier for each environment.
  int id;
  // The number of watchers active in this environment.
  int watchCount = 0;
  efsw::FileWatcher* fileWatcher = nullptr;

  // A map that associates `WatcherHandle` values with their
  // `PathWatcherListener` instances.
  std::unordered_map<WatcherHandle, PathWatcherListener*> listeners;
};
