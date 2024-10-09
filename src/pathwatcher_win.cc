#include <algorithm>
#include <map>
#include <memory>
#include <iostream>

#include "common.h"
#include "addon-data.h"
#include "js_native_api_types.h"
#include "napi.h"
#include "uv.h"

// class VectorMap {
// public:
//     using Vector = std::vector<HANDLE>;
//
//     std::shared_ptr<Vector> get_or_create(int addonDataId) {
//         auto it = vectors_.find(addonDataId);
//         if (it == vectors_.end()) {
//             it = vectors_.emplace(addonDataId, std::make_shared<Vector>()).first;
//         }
//         return it->second;
//     }
//
//     void remove(int addonDataId) {
//         vectors_.erase(addonDataId);
//     }
//
// private:
//     std::map<int, std::shared_ptr<Vector>> vectors_;
// };
//
// // Global instance of VectorMap
// VectorMap g_vector_map;

// Size of the buffer to store result of ReadDirectoryChangesW.
static const unsigned int kDirectoryWatcherBufferSize = 4096;

// Mutex for the HandleWrapper map.
static uv_mutex_t g_handle_wrap_map_mutex;

// The events to be waited on.
static std::vector<HANDLE> g_events;

// The dummy event to wakeup the thread.
static HANDLE g_wake_up_event;

// The dummy event to ensure we are not waiting on a file handle when destroying it.
static HANDLE g_file_handles_free_event;

struct ScopedLocker {
  explicit ScopedLocker(uv_mutex_t& mutex) : mutex_(&mutex) { uv_mutex_lock(mutex_); }
  ~ScopedLocker() { Unlock(); }

  void Unlock() { uv_mutex_unlock(mutex_); }

  uv_mutex_t* mutex_;
};

struct HandleWrapper {
  HandleWrapper(WatcherHandle handle, const char* path_str, int addon_data_id)
      : addonDataId(addon_data_id),
        dir_handle(handle),
        path(strlen(path_str)),
        canceled(false) {
    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_events.push_back(overlapped.hEvent);

    std::copy(path_str, path_str + path.size(), path.data());
    map_[overlapped.hEvent] = this;
  }

  ~HandleWrapper() {
    if (!canceled) {
      Cancel();
    }

    CloseHandle(dir_handle);
    CloseHandle(overlapped.hEvent);
  }

  void Cancel() {
    canceled = true;
    CancelIoEx(dir_handle, &overlapped);
    g_events.erase(std::remove(g_events.begin(), g_events.end(), overlapped.hEvent), g_events.end());
    map_.erase(overlapped.hEvent);
  }

  int addonDataId;
  WatcherHandle dir_handle;
  std::vector<char> path;
  bool canceled;
  OVERLAPPED overlapped;
  char buffer[kDirectoryWatcherBufferSize];

  static HandleWrapper* Get(HANDLE key) { return map_[key]; }

  static std::map<WatcherHandle, HandleWrapper*> map_;
};

std::map<WatcherHandle, HandleWrapper*> HandleWrapper::map_;

struct WatcherEvent {
  EVENT_TYPE type;
  WatcherHandle handle;
  std::vector<char> new_path;
  std::vector<char> old_path;
};

static bool QueueReaddirchanges(HandleWrapper* handle) {
  return ReadDirectoryChangesW(
    handle->dir_handle,
    handle->buffer,
    kDirectoryWatcherBufferSize,
    FALSE,
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
     FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_LAST_ACCESS |
     FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SECURITY,
    NULL,
    &handle->overlapped,
    NULL
  ) == TRUE;
}

Napi::Value WatcherHandleToV8Value(WatcherHandle handle, Napi::Env env) {
  uint64_t handleInt = reinterpret_cast<uint64_t>(handle);
  return Napi::BigInt::New(env, handleInt);
}

WatcherHandle V8ValueToWatcherHandle(Napi::Value value) {
  if (!value.IsBigInt()) {
    return NULL;
  }
  bool lossless;
  uint64_t handleInt = value.As<Napi::BigInt>().Uint64Value(&lossless);
  if (!lossless) {
    return NULL;
  }
  return reinterpret_cast<HANDLE>(handleInt);
}

bool IsV8ValueWatcherHandle(Napi::Value value) {
  return value.IsBigInt();
}

void PlatformInit(Napi::Env _env) {
  uv_mutex_init(&g_handle_wrap_map_mutex);

  g_file_handles_free_event = CreateEvent(NULL, TRUE, TRUE, NULL);
  g_wake_up_event = CreateEvent(NULL, FALSE, FALSE, NULL);
  g_events.push_back(g_wake_up_event);
}

void PlatformThread(
  const PathWatcherWorker::ExecutionProgress& progress,
  bool& shouldStop,
  Napi::Env env
) {
  auto addonData = env.GetInstanceData<AddonData>();
  std::cout << "PlatformThread ID: " << addonData->id << std::endl;

  // std::cout << "PlatformThread" << std::endl;
  while (!shouldStop) {
    // Do not use g_events directly, since reallocation could happen when there
    // are new watchers adding to g_events when WaitForMultipleObjects is still
    // polling.
    ScopedLocker locker(g_handle_wrap_map_mutex);
    std::vector<HANDLE> copied_events(g_events);
    locker.Unlock();

    ResetEvent(g_file_handles_free_event);
    std::cout << "Thread with ID: " << addonData->id << " is waiting..." << std::endl;
    DWORD r = WaitForMultipleObjects(
      copied_events.size(),
      copied_events.data(),
      FALSE,
      100
    );
    SetEvent(g_file_handles_free_event);

    if (r == WAIT_TIMEOUT) {
      // Timeout occurred, check shouldStop flag
      continue;
    }
    std::cout << "Thread with ID: " << addonData->id << " is done waiting." << std::endl;

    int i = r - WAIT_OBJECT_0;
    if (i >= 0 && i < copied_events.size()) {
      // It's a wake up event, there is no fs events.
      if (copied_events[i] == g_wake_up_event) {
        std::cout << "Thread with ID: " << addonData->id << " received wake-up event. Continuing." << std::endl;
        continue;
      }

      ScopedLocker locker(g_handle_wrap_map_mutex);

      HandleWrapper* handle = HandleWrapper::Get(copied_events[i]);
      if (!handle || handle->canceled) {
        continue;
      }

      if (handle->addonDataId != addonData->id) {
        std::cout << "Thread with ID: " << addonData->id << " ignoring handle from different context." << std::endl;
        continue;
      }

      DWORD bytes_transferred;
      if (!GetOverlappedResult(handle->dir_handle, &handle->overlapped, &bytes_transferred, FALSE)) {
        std::cout << "Nothing for thread: " << addonData->id << std::endl;
        continue;
      }
      if (bytes_transferred == 0) {
        std::cout << "Nothing for thread: " << addonData->id << std::endl;
        continue;
      }

      std::vector<char> old_path;
      std::vector<WatcherEvent> events;

      DWORD offset = 0;
      while (true) {
        FILE_NOTIFY_INFORMATION* file_info =
            reinterpret_cast<FILE_NOTIFY_INFORMATION*>(handle->buffer + offset);

        // Emit events for children.
        EVENT_TYPE event = EVENT_NONE;
        switch (file_info->Action) {
          case FILE_ACTION_ADDED:
            event = EVENT_CHILD_CREATE;
            break;
          case FILE_ACTION_REMOVED:
            event = EVENT_CHILD_DELETE;
            break;
          case FILE_ACTION_RENAMED_OLD_NAME:
            event = EVENT_CHILD_RENAME;
            break;
          case FILE_ACTION_RENAMED_NEW_NAME:
            event = EVENT_CHILD_RENAME;
            break;
          case FILE_ACTION_MODIFIED:
            event = EVENT_CHILD_CHANGE;
            break;
        }

        if (event != EVENT_NONE) {
          // The FileNameLength is in "bytes", but the WideCharToMultiByte
          // requires the length to be in "characters"!
          int file_name_length_in_characters =
              file_info->FileNameLength / sizeof(wchar_t);

          char filename[MAX_PATH] = { 0 };
          int size = WideCharToMultiByte(
            CP_UTF8,
            0,
            file_info->FileName,
            file_name_length_in_characters,
            filename,
            MAX_PATH,
            NULL,
            NULL
          );

          // Convert file name to file path, same with:
          // path = handle->path + '\\' + filename
          std::vector<char> path(handle->path.size() + 1 + size);
          std::vector<char>::iterator iter = path.begin();
          iter = std::copy(handle->path.begin(), handle->path.end(), iter);
          *(iter++) = '\\';
          std::copy(filename, filename + size, iter);

          if (file_info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
            // Do not send rename event until the NEW_NAME event, but still keep
            // a record of old name.
            old_path.swap(path);
          } else if (file_info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
            WatcherEvent e = { event, handle->overlapped.hEvent };
            e.new_path.swap(path);
            e.old_path.swap(old_path);
            events.push_back(e);
          } else {
            WatcherEvent e = { event, handle->overlapped.hEvent };
            e.new_path.swap(path);
            events.push_back(e);
          }
        }

        if (file_info->NextEntryOffset == 0) break;
        offset += file_info->NextEntryOffset;
      }

      // Restart the monitor, it was reset after each call.
      QueueReaddirchanges(handle);

      locker.Unlock();

      std::cout << "Total events processed on thread " << addonData->id << ": " << events.size() << std::endl;

      for (size_t i = 0; i < events.size(); ++i) {
        std::cout << "Emitting " << events[i].type << " event on thread " << addonData->id << " for path: " << events[i].new_path.data() << std::endl;
        PathWatcherEvent event(
          events[i].type,
          events[i].handle,
          events[i].new_path,
          events[i].old_path
        );
        progress.Send(&event, 1);
      }
    }
  }
}

// // Function to get the vector for a given AddonData
// std::shared_ptr<VectorMap::Vector> GetVectorForAddonData(AddonData* addonData) {
//   return g_vector_map.get_or_create(addonData->id);
// }

WatcherHandle PlatformWatch(const char* path, Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  std::cout << "PlatformWatch ID: " << addonData->id << " Path: " << path << std::endl;
  wchar_t wpath[MAX_PATH] = { 0 };
  MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);

  // Requires a directory, file watching is emulated in js.
  DWORD attr = GetFileAttributesW(wpath);
  if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    return INVALID_HANDLE_VALUE;
  }

  WatcherHandle dir_handle = CreateFileW(
    wpath,
    FILE_LIST_DIRECTORY,
    FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
    NULL,
    OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
    NULL
  );

  if (!PlatformIsHandleValid(dir_handle)) {
    return INVALID_HANDLE_VALUE;
  }

  std::unique_ptr<HandleWrapper> handle;
  {
    ScopedLocker locker(g_handle_wrap_map_mutex);
    handle.reset(new HandleWrapper(dir_handle, path, addonData->id));
  }

  if (!QueueReaddirchanges(handle.get())) {
    return INVALID_HANDLE_VALUE;
  }

  // Wake up the thread to add the new event.
  SetEvent(g_wake_up_event);

  // The pointer is leaked if no error happened.
  return handle.release()->overlapped.hEvent;
}

void PlatformUnwatch(WatcherHandle key, Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  std::cout << "PlatformUnwatch ID: " << addonData->id << std::endl;
  if (PlatformIsHandleValid(key)) {
    HandleWrapper* handle;
    {
      ScopedLocker locker(g_handle_wrap_map_mutex);
      handle = HandleWrapper::Get(key);
      handle->Cancel();
    }

    do {
      SetEvent(g_wake_up_event);
    } while (WaitForSingleObject(g_file_handles_free_event, 50) == WAIT_TIMEOUT);
    delete handle;
  }
}

bool PlatformIsHandleValid(WatcherHandle handle) {
  return handle != INVALID_HANDLE_VALUE;
}

// We have no errno on Windows.
int PlatformInvalidHandleToErrorNumber(WatcherHandle handle) {
  return 0;
}
