#include "core.h"
#include "addon-data.h"
#include "include/efsw/efsw.hpp"
#include "napi.h"
#include <string>
#include <iostream>
#ifdef __APPLE__
#include <sys/stat.h>
#endif

static int unique_id = 1;

PathWatcherListener::PathWatcherListener(Napi::Env env, AddonData* addonData): addonData(addonData) {
  id = unique_id++;
}

PathWatcherListener::~PathWatcherListener() {
  std::cout << "PathWatcherListener destructor! " << id << std::endl;
  Stop();
}

void PathWatcherListener::Stop() {
  if (!addonData) return;
  addonData = nullptr;
  // std::cout << "PathWatcherListener::Stop! " << id << std::endl;
  // if (isShuttingDown) return;
  // // Prevent responders from acting while we shut down.
  // std::lock_guard<std::mutex> lock(shutdownMutex);
  // if (isShuttingDown) return;
  // isShuttingDown = true;
  // // if (tsfn) {
  // //   tsfn.Release();
  // // }
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
  if (!addonData) return;
  // Don't try to proceed if we've already started the shutdown process.
  if (isShuttingDown) return;
  std::lock_guard<std::mutex> lock(shutdownMutex);
  if (isShuttingDown) return;

  std::string newPathStr = dir + filename;
  std::vector<char> newPath(newPathStr.begin(), newPathStr.end());

#ifdef __APPLE__
  // macOS seems to think that lots of file creations happen that aren't
  // actually creations; for instance, multiple successive writes to the same
  // file will sometimes nonsensically produce a `child-create` event preceding
  // each `child-change` event.
  //
  // Luckily, we can easily check whether or not a file has actually been
  // created on macOS: we can compare creation time to modification time.
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

  Napi::ThreadSafeFunction tsfn = addonData->tsfn;
  if (!tsfn) return;

  napi_status status = tsfn.Acquire();
  if (status != napi_ok) {
    // We couldn't acquire the `tsfn`; it might be in the process of being
    // aborted because our environment is terminating.
    return;
  }

  PathWatcherEvent* event = new PathWatcherEvent(action, watchId, newPath, oldPath);
  status = tsfn.BlockingCall(event, ProcessEvent);
  tsfn.Release();
  if (status != napi_ok) {
    // TODO: Not sure how this could fail, or how we should present it to the
    // user if it does fail. This action runs on a separate thread and it's not
    // immediately clear how we'd surface an exception from here.
    delete event;
  }
}

void ProcessEvent(Napi::Env env, Napi::Function callback, PathWatcherEvent* event) {
  std::unique_ptr<PathWatcherEvent> eventPtr(event);
  if (event == nullptr) {
    Napi::TypeError::New(env, "Unknown error handling filesystem event").ThrowAsJavaScriptException();
    return;
  }

  // Translate the event type to the expected event name in the JS code.
  //
  // NOTE: This library previously envisioned that some platforms would allow
  // watching of files directly and some would require watching of a file's
  // parent folder. EFSW uses the parent-folder approach on all platforms, so
  // in practice we're not using half of the event names we used to use. That's
  // why the second argument below is `true`.
  //
  // There might be some edge cases that we need to handle here; for instance,
  // if we're watching a directory and that directory itself is deleted, then
  // that should be `delete` rather than `child-delete`. Right now we deal with
  // that in JavaScript, but we could handle it here instead.
  std::string eventName = EventType(event->type, true);

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
    // TODO: Unsure why this would happen.
    Napi::TypeError::New(env, "Unknown error handling filesystem event").ThrowAsJavaScriptException();
  }
}

Napi::Value EFSW::Watch(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  return env.Undefined();
}

Napi::Value EFSW::Unwatch(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  auto addonData = env.GetInstanceData<AddonData>();
  Napi::HandleScope scope(env);

  // Our sole argument must be a JavaScript number; we convert it to a watcher
  // handle.
  if (!IsV8ValueWatcherHandle(info[0])) {
    Napi::TypeError::New(env, "Argument must be a number").ThrowAsJavaScriptException();
    return env.Null();
  }

  WatcherHandle handle = V8ValueToWatcherHandle(info[0].As<Napi::Number>());

  // EFSW doesn’t mind if we give it a handle that it doesn’t recognize; it’ll
  // just silently do nothing.
  addonData->fileWatcher->removeWatch(handle);

  // Since we’re not listening anymore, we have to stop the associated
  // `PathWatcherListener` or else Node will think there’s an open handle.
  auto it = addonData->listeners.find(handle);
  if (it != addonData->listeners.end()) {
    it->second->Stop();
    addonData->listeners.erase(it);
  }

  addonData->watchCount--;
  if (addonData->watchCount == 0) {
    // When this environment isn’t watching any files, we can stop the
    // `FileWatcher` instance. We’ll start it up again if `Watch` is called.
    EFSW::Cleanup(env);
  }

  return env.Undefined();
}

void EFSW::Cleanup(Napi::Env env) {
  std::cout << "EFSW::Cleanup" << std::endl;
  auto addonData = env.GetInstanceData<AddonData>();
  if (addonData && addonData->fileWatcher) {
    // Clean up all outstanding listeners.
    for (auto& pair : addonData->listeners) {
      pair.second->Stop();
    }
    addonData->fileWatcher = nullptr;
  }
  delete addonData->fileWatcher;
}

void EFSW::Init(Napi::Env env) {
  // auto addonData = env.GetInstanceData<AddonData>();
  // addonData->watchCount = 0;
}

PathWatcher::PathWatcher(Napi::Env env, Napi::Object exports) {
  addonData = new AddonData(env);

  DefineAddon(exports, {
    InstanceMethod("watch", &PathWatcher::Watch),
    InstanceMethod("unwatch", &PathWatcher::Unwatch),
    InstanceMethod("setCallback", &PathWatcher::SetCallback)
  });
}

Napi::Value PathWatcher::Watch(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  // auto addonData = env.GetInstanceData<AddonData>();
  Napi::HandleScope scope(env);

  // First argument must be a string.
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "String required").ThrowAsJavaScriptException();
    return env.Null();
  }

  Napi::String path = info[0].ToString();
  std::string cppPath(path);

  if (addonData->callback.IsEmpty()) {
    Napi::TypeError::New(env, "No callback set").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (!addonData->fileWatcher) {
    // addonData->tsfn =
    addonData->tsfn = Napi::ThreadSafeFunction::New(
      env,
      addonData->callback.Value(),
      "pathwatcher-efsw-listener",
      0,
      1,
      [](Napi::Env env) {
        std::cout << "Testing finalizer" << std::endl;
      }
    );
  }

  PathWatcherListener* listener = new PathWatcherListener(env, addonData);

  // The first call to `Watch` initializes a `FileWatcher`.
  if (!addonData->fileWatcher) {
    addonData->fileWatcher = new efsw::FileWatcher();
    addonData->fileWatcher->followSymlinks(true);
    addonData->fileWatcher->watch();
  }

  // EFSW represents watchers as unsigned `int`s; we can easily convert these
  // to JavaScript.
  WatcherHandle handle = addonData->fileWatcher->addWatch(path, listener, true);

  if (handle >= 0) {
    addonData->listeners[handle] = listener;
  } else {
    delete listener;
    Napi::Error::New(env, "Failed to add watch; unknown error").ThrowAsJavaScriptException();
    return env.Null();
  }

  addonData->watchCount++;

  // The `watch` function returns a JavaScript number much like `setTimeout` or
  // `setInterval` would; this is the handle that the consumer can use to
  // unwatch the path later.
  return WatcherHandleToV8Value(handle, env);
}

Napi::Value PathWatcher::Unwatch(const Napi::CallbackInfo& info) {
  std::cout << "Unwatch!" << std::endl;
  auto env = info.Env();
  Napi::HandleScope scope(env);

  // Our sole argument must be a JavaScript number; we convert it to a watcher
  // handle.
  if (!IsV8ValueWatcherHandle(info[0])) {
    Napi::TypeError::New(env, "Argument must be a number").ThrowAsJavaScriptException();
    return env.Null();
  }

  WatcherHandle handle = V8ValueToWatcherHandle(info[0].As<Napi::Number>());

  // EFSW doesn’t mind if we give it a handle that it doesn’t recognize; it’ll
  // just silently do nothing.
  addonData->fileWatcher->removeWatch(handle);

  // Since we’re not listening anymore, we have to stop the associated
  // `PathWatcherListener` or else Node will think there’s an open handle.
  auto it = addonData->listeners.find(handle);
  if (it != addonData->listeners.end()) {
    it->second->Stop();
    std::cout << "Erasing listener with handle " << handle << std::endl;
    addonData->listeners.erase(it);
  }

  addonData->watchCount--;
  if (addonData->watchCount == 0) {
    // When this environment isn’t watching any files, we can stop the
    // `FileWatcher` instance. We’ll start it up again if `Watch` is called.
    Cleanup(env);
  }

  return env.Undefined();
}

void PathWatcher::Cleanup(Napi::Env env) {
  std::cout << "PathWatcher::Cleanup" << std::endl;
  // auto addonData = env.GetInstanceData<AddonData>();

  if (addonData && addonData->fileWatcher) {
    // Clean up all outstanding listeners.
    for (auto& pair : addonData->listeners) {
      pair.second->Stop();
    }
    addonData->fileWatcher = nullptr;
  }
  if (addonData->tsfn) {
    addonData->tsfn.Unref(env);
    // delete addonData->tsfn;
    addonData->tsfn = NULL;
  }
  delete addonData->fileWatcher;
}


void PathWatcher::SetCallback(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  if (!info[0].IsFunction()) {
    Napi::TypeError::New(env, "Function required").ThrowAsJavaScriptException();
  }

  Napi::Function fn = info[0].As<Napi::Function>();
  addonData->callback = Napi::Persistent(fn);
}

NODE_API_ADDON(PathWatcher)
