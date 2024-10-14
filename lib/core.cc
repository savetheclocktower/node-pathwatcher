#include "core.h"
#include "addon-data.h"
#include "include/efsw/efsw.hpp"
#include "napi.h"
#include <string>
#ifdef __APPLE__
#include <sys/stat.h>
#endif

void ProcessEvent(Napi::Env env, Napi::Function callback, PathWatcherEvent* event);

PathWatcherListener::PathWatcherListener(Napi::Env env, Napi::Function fn)
  : callback(fn) {
  tsfn = Napi::ThreadSafeFunction::New(
    env,
    callback,
    "pathwatcher-efsw-listener",
    0,
    1
  );
}

PathWatcherListener::~PathWatcherListener() {
  Stop();
}

void PathWatcherListener::Stop() {
  std::lock_guard<std::mutex> lock(shutdownMutex);
  isShuttingDown = true;
  if (tsfn) {
    tsfn.Release();
  }
}

std::string EventType(efsw::Action action, bool isChild) {
  switch (action) {
    case efsw::Actions::Add:
      return isChild ? "child-create" : "create";
    case efsw::Actions::Delete:
      return isChild ? "child-delete" : "delete";
    case efsw::Actions::Modified:
      return isChild ? "child-change" : "change";
    case efsw::Actions::Moved:
      return isChild ? "child-rename" : "rename";
    default:
      // std::cout << "Unknown action: " << action;
      return "unknown";
  }
}

void PathWatcherListener::handleFileAction(
  efsw::WatchID watchId,
  const std::string& dir,
  const std::string& filename,
  efsw::Action action,
  std::string oldFilename
) {
  if (isShuttingDown) return;
  std::lock_guard<std::mutex> lock(shutdownMutex);
  if (isShuttingDown);
  // std::cout << "PathWatcherListener::handleFileAction" << std::endl;
  // std::cout << "Action: " << EventType(action, true) << ", Dir: " << dir << ", Filename: " << filename << ", Old Filename: " << oldFilename << std::endl;

  std::string newPathStr = dir + filename;
  std::vector<char> newPath(newPathStr.begin(), newPathStr.end());

#ifdef __APPLE__
  // macOS seems to think that lots of file creations happen that aren't
  // actually creations; for instance, multiple successive writes to the same
  // file will sometimes nonsensically produce a `child-create` event preceding
  // each `child-change` event.
  //
  // Luckily, we can easily check whether or not a file has actually been
  // created on macOS; we can compare creation time to modification time.
  if (action == efsw::Action::Add) {
    struct stat file;
    if (stat(newPathStr.c_str(), &file) != 0) {
      return;
    }
    if (file.st_birthtimespec.tv_sec != file.st_mtimespec.tv_sec) {
      // std::cout << "Skipping spurious creation event!" << std::endl;
      return;
    }
  }

#endif

  std::vector<char> oldPath;
  if (!oldFilename.empty()) {
    std::string oldPathStr = dir + oldFilename;
    oldPath.assign(oldPathStr.begin(), oldPathStr.end());
  }

  PathWatcherEvent* event = new PathWatcherEvent(action, watchId, newPath, oldPath);
  napi_status status = tsfn.BlockingCall(event, ProcessEvent);
  if (status != napi_ok) {
    // std::cerr << "Error in BlockingCall: " << status << std::endl;
    delete event;  // Clean up if BlockingCall fails
  }
}

void ProcessEvent(Napi::Env env, Napi::Function callback, PathWatcherEvent* event) {
  std::unique_ptr<PathWatcherEvent> eventPtr(event);
  if (event == nullptr) {
    // std::cerr << "ProcessEvent: event is null" << std::endl;
    return;
  }

  std::string eventName = EventType(event->type, true);
  // std::cout << "ProcessEvent! " << eventName << std::endl;

  std::string newPath;
  std::string oldPath;

  if (!event->new_path.empty()) {
    newPath.assign(event->new_path.begin(), event->new_path.end());
    // std::cout << "new path: " << newPath << std::endl;
  }

  if (!event->old_path.empty()) {
    oldPath.assign(event->old_path.begin(), event->old_path.end());
    // std::cout << "old path: " << oldPath << std::endl;
  }

  // Use a try-catch block only for the Node-API call, which might throw
  try {
    callback.Call({
      Napi::String::New(env, eventName),
      Napi::Number::New(env, event->handle),
      Napi::String::New(env, newPath),
      Napi::String::New(env, oldPath)
    });
  } catch (const Napi::Error& e) {
    // TODO: Unsure why this would happen; but if it's plausible that it would
    // happen sometimes, then figure out how to surface it.

    // std::cerr << "Napi error in callback.Call: " << e.what() << std::endl;
  }
}

Napi::Value EFSW::Watch(const Napi::CallbackInfo& info) {
  // std::cout << "Watch" << std::endl;
  auto env = info.Env();
  auto addonData = env.GetInstanceData<AddonData>();
  Napi::HandleScope scope(env);

  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "String required").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::String path = info[0].ToString();
  std::string cppPath(path);

  if (!info[1].IsFunction()) {
    Napi::TypeError::New(env, "Function required").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::Function fn = info[1].As<Napi::Function>();

  PathWatcherListener* listener = new PathWatcherListener(env, fn);

  // std::cout << "About to add handle for path: " << cppPath << std::endl;

  if (!addonData->fileWatcher) {
    // std::cout << "CREATING WATCHER!!!" << std::endl;
    addonData->fileWatcher = new efsw::FileWatcher();
    addonData->fileWatcher->followSymlinks(true);
    addonData->fileWatcher->watch();
  }

  WatcherHandle handle = addonData->fileWatcher->addWatch(path, listener, true);
  if (handle >= 0) {
    addonData->listeners[handle] = listener;
  } else {
    delete listener;
    Napi::Error::New(env, "Failed to add watch").ThrowAsJavaScriptException();
    return env.Null();
  }
  // std::cout << "Watcher handle: " << handle << std::endl;
  addonData->watchCount++;

  return WatcherHandleToV8Value(handle, env);
}

Napi::Value EFSW::Unwatch(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  auto addonData = env.GetInstanceData<AddonData>();
  // std::cout << "Unwatch ID:" << addonData->id << std::endl;
  Napi::HandleScope scope(env);

  if (!IsV8ValueWatcherHandle(info[0])) {
    Napi::TypeError::New(env, "Argument must be a number").ThrowAsJavaScriptException();
    return env.Null();
  }

  WatcherHandle handle = V8ValueToWatcherHandle(info[0].As<Napi::Number>());
  // std::cout << "About to unwatch handle:" << handle << std::endl;

  addonData->fileWatcher->removeWatch(handle);

  auto it = addonData->listeners.find(handle);
  if (it != addonData->listeners.end()) {
    it->second->Stop();  // Release the ThreadSafeFunction
    addonData->listeners.erase(it);    // Remove from the map
  }

  addonData->watchCount--;
  if (addonData->watchCount == 0) {
    EFSW::Cleanup(env);
  }

  return env.Undefined();
}

void EFSW::Cleanup(Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  delete addonData->fileWatcher;
  if (addonData && addonData->fileWatcher) {
    // Clean up all listeners
    for (auto& pair : addonData->listeners) {
      pair.second->Stop();
    }
    addonData->fileWatcher = nullptr;
  }
}

void EFSW::Init(Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  // std::cout << "Addon data created!" << addonData->id << std::endl;
  addonData->watchCount = 0;
}
