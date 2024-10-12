#include "core.h"
#include "addon-data.h"
#include "include/efsw/efsw.hpp"
#include "napi.h"
#include <string>

void ProcessEvent(Napi::Env env, Napi::Function callback, PathWatcherEvent* event);

PathWatcherListener::PathWatcherListener(Napi::Env env, Napi::Function fn)
  : callback(fn) {
  std::cout << "new PathWatcherListener" << std::endl;
  tsfn = Napi::ThreadSafeFunction::New(
    env,
    callback,
    "pathwatcher-efsw-listener",
    0,
    2
  );
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
      std::cout << "Unknown action: " << action;
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
  std::cout << "PathWatcherListener::handleFileAction" << std::endl;
  std::cout << "Action: " << action << ", Dir: " << dir << ", Filename: " << filename << ", Old Filename: " << oldFilename << std::endl;

  std::string newPathStr = dir + PATH_SEPARATOR + filename;
  std::vector<char> newPath(newPathStr.begin(), newPathStr.end());

  std::vector<char> oldPath;
  if (!oldFilename.empty()) {
    std::string oldPathStr = dir + PATH_SEPARATOR + oldFilename;
    oldPath.assign(oldPathStr.begin(), oldPathStr.end());
  }

  PathWatcherEvent* event = new PathWatcherEvent(action, watchId, newPath, oldPath);
  napi_status status = tsfn.BlockingCall(event, ProcessEvent);
  if (status != napi_ok) {
    std::cerr << "Error in BlockingCall: " << status << std::endl;
    delete event;  // Clean up if BlockingCall fails
  }
}

void ProcessEvent(Napi::Env env, Napi::Function callback, PathWatcherEvent* event) {
  if (event == nullptr) {
    std::cerr << "ProcessEvent: event is null" << std::endl;
    return;
  }

  std::string eventName = EventType(event->type, true);
  std::cout << "ProcessEvent! " << eventName << std::endl;

  std::string newPath;
  std::string oldPath;

  if (!event->new_path.empty()) {
    newPath.assign(event->new_path.begin(), event->new_path.end());
    std::cout << "new path: " << newPath << std::endl;
  } else {
    std::cout << "new path is empty" << std::endl;
  }

  if (!event->old_path.empty()) {
    oldPath.assign(event->old_path.begin(), event->old_path.end());
    std::cout << "old path: " << oldPath << std::endl;
  } else {
    std::cout << "old path is empty" << std::endl;
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
    std::cerr << "Napi error in callback.Call: " << e.what() << std::endl;
  }
}

Napi::Value EFSW::Watch(const Napi::CallbackInfo& info) {
  std::cout << "Watch" << std::endl;
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

  std::cout << "About to add handle for path: " << cppPath << std::endl;


  WatcherHandle handle = addonData->fileWatcher->addWatch(path, listener, true);
  std::cout << "Watcher handle: " << handle << std::endl;
  addonData->fileWatcher->watch();
  return WatcherHandleToV8Value(handle, env);
}

Napi::Value EFSW::Unwatch(const Napi::CallbackInfo& info) {
  std::cout << "Unwatch" << std::endl;
  auto env = info.Env();
  auto addonData = env.GetInstanceData<AddonData>();
  Napi::HandleScope scope(env);

  if (!IsV8ValueWatcherHandle(info[0])) {
    Napi::TypeError::New(env, "Argument must be a number").ThrowAsJavaScriptException();
    return env.Null();
  }

  WatcherHandle handle = V8ValueToWatcherHandle(info[0].As<Napi::Number>());
  addonData->fileWatcher->removeWatch(handle);

  return env.Undefined();
}

void EFSW::Init(Napi::Env env) {
  auto addonData = env.GetInstanceData<AddonData>();
  std::cout << "Addon data created!" << addonData->id << std::endl;
  if (!addonData) {
    std::cout << "WHAT THE FUCK" << std::endl;
  }
  addonData->fileWatcher = new efsw::FileWatcher();
  addonData->fileWatcher->followSymlinks(true);
  // addonData->fileWatcher->watch();
}
