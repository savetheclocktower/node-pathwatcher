#include "core.h"
#pragma once

static int g_next_addon_data_id = 1;

class AddonData final {
public:
  explicit AddonData(Napi::Env env) {
    id = g_next_addon_data_id++;
  }

  int id;
  int watchCount = 0;
  efsw::FileWatcher* fileWatcher = nullptr;
  std::unordered_map<WatcherHandle, PathWatcherListener*> listeners;
};
