#include "core.h"
#include "include/efsw/efsw.hpp"
#include "napi.h"
#include <uv.h>
#include <string>

#ifdef __APPLE__
#include <sys/stat.h>
#endif

static std::string EventType(efsw::Action action, bool isChild) {
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
      return "unknown";
  }
}

// This is a bit hacky, but it allows us to stop invoking callbacks more
// quickly when the environment is terminating.
static bool EnvIsStopping(Napi::Env env) {
  PathWatcher* pw = env.GetInstanceData<PathWatcher>();
  return pw->isStopping;
}

// Ensure a given path has a trailing separator for comparison purposes.
static std::string NormalizePath(std::string path) {
  if (path.back() == PATH_SEPARATOR) return path;
  return path + PATH_SEPARATOR;
}

static bool PathsAreEqual(std::string pathA, std::string pathB) {
  return NormalizePath(pathA) == NormalizePath(pathB);
}

// This is the main-thread function that receives all `ThreadSafeFunction`
// calls. It converts the `PathWatcherEvent` struct into JS values before
// invoking our callback.
static void ProcessEvent(Napi::Env env, Napi::Function callback, PathWatcherEvent* event) {
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

  if (EnvIsStopping(env)) return;

  std::string newPath;
  std::string oldPath;

  if (!event->new_path.empty()) {
    newPath.assign(event->new_path.begin(), event->new_path.end());
  }

  if (!event->old_path.empty()) {
    oldPath.assign(event->old_path.begin(), event->old_path.end());
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

PathWatcherListener::PathWatcherListener(
  Napi::Env env,
  std::string realPath,
  Napi::ThreadSafeFunction tsfn
): realPath(realPath), tsfn(tsfn) {}

void PathWatcherListener::Stop() {
  if (isShuttingDown) return;
  // Prevent responders from acting while we shut down.
  std::lock_guard<std::mutex> lock(shutdownMutex);
  if (isShuttingDown) return;
  isShuttingDown = true;
}

void PathWatcherListener::handleFileAction(
  efsw::WatchID watchId,
  const std::string& dir,
  const std::string& filename,
  efsw::Action action,
  std::string oldFilename
) {
  // std::cout << "PathWatcherListener::handleFileAction dir: " << dir << " filename: " << filename << std::endl;
  // Don't try to proceed if we've already started the shutdown process.
  if (isShuttingDown) return;
  std::lock_guard<std::mutex> lock(shutdownMutex);
  if (isShuttingDown) return;

  std::string newPathStr = dir + filename;
  std::vector<char> newPath(newPathStr.begin(), newPathStr.end());

  if (PathsAreEqual(newPathStr, realPath)) {
    // This is an event that is happening to the directory itself — like the
    // directory being deleted. Allow it through.
  } else if (dir != NormalizePath(realPath)) {
    // Otherwise, we would expect `dir` to be equal to `realPath`; if it isn't,
    // then we should ignore it. This might be an event that happened to an
    // ancestor folder or a descendent folder somehow.
    return;
  }

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
      return;
    }
  }
#endif

  std::vector<char> oldPath;
  if (!oldFilename.empty()) {
    std::string oldPathStr = dir + oldFilename;
    oldPath.assign(oldPathStr.begin(), oldPathStr.end());
  }

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

static int next_env_id = 1;

PathWatcher::PathWatcher(Napi::Env env, Napi::Object exports) {
  envId = next_env_id++;

  DefineAddon(exports, {
    InstanceMethod("watch", &PathWatcher::Watch),
    InstanceMethod("unwatch", &PathWatcher::Unwatch),
    InstanceMethod("setCallback", &PathWatcher::SetCallback)
  });

  env.SetInstanceData<PathWatcher>(this);
}

PathWatcher::~PathWatcher() {
  std::cout << "Finalizing PathWatcher with ID: " << envId << std::endl;
  isFinalizing = true;
  StopAllListeners();
}

// Watch a given path. Returns a handle.
Napi::Value PathWatcher::Watch(const Napi::CallbackInfo& info) {
  auto env = info.Env();

  // First argument must be a string.
  if (!info[0].IsString()) {
    Napi::TypeError::New(env, "String required").ThrowAsJavaScriptException();
    return env.Null();
  }

  // The wrapper JS will resolve this to the file's real path. We expect to be
  // dealing with real locations on disk, since that's what EFSW will report to
  // us anyway.
  Napi::String path = info[0].ToString();
  std::string cppPath(path);

  // It's invalid to call `watch` before having set a callback via
  // `setCallback`.
  if (callback.IsEmpty()) {
    Napi::TypeError::New(env, "No callback set").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (listeners.size() == 0) {
    tsfn = Napi::ThreadSafeFunction::New(
      env,
      callback.Value(),
      "pathwatcher-efsw-listener",
      0,
      1,
      [this](Napi::Env env) {
        // std::cout << "Finalizing tsfn" << std::endl;
        StopAllListeners();
      }
    );
  }

  PathWatcherListener* listener = new PathWatcherListener(env, cppPath, tsfn);

  // The first call to `Watch` initializes a `FileWatcher`.
  if (listeners.size() == 0) {
    fileWatcher = new efsw::FileWatcher();
    fileWatcher->followSymlinks(true);
    fileWatcher->watch();
  }

  // EFSW represents watchers as unsigned `int`s; we can easily convert these
  // to JavaScript.
  WatcherHandle handle = fileWatcher->addWatch(cppPath, listener, false);

  if (handle >= 0) {
    listeners[handle] = listener;
  } else {
    delete listener;
    Napi::Error::New(env, "Failed to add watch; unknown error").ThrowAsJavaScriptException();
    return env.Null();
  }

  // The `watch` function returns a JavaScript number much like `setTimeout` or
  // `setInterval` would; this is the handle that the consumer can use to
  // unwatch the path later.
  return WatcherHandleToV8Value(handle, env);
}

// Unwatch the given handle.
Napi::Value PathWatcher::Unwatch(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  if (!IsV8ValueWatcherHandle(info[0])) {
    Napi::TypeError::New(env, "Argument must be a number").ThrowAsJavaScriptException();
    return env.Null();
  }

  WatcherHandle handle = V8ValueToWatcherHandle(info[0].As<Napi::Number>());

  // EFSW doesn’t mind if we give it a handle that it doesn’t recognize; it’ll
  // just silently do nothing.
  fileWatcher->removeWatch(handle);

  // Since we’re not listening anymore, we have to stop the associated
  // `PathWatcherListener` so that we know when to invoke cleanup and close the
  // open handle.
  auto it = listeners.find(handle);
  if (it != listeners.end()) {
    it->second->Stop();
    listeners.erase(it);
  }

  if (listeners.size() == 0) {
    Cleanup(env);
  }

  return env.Undefined();
}

void PathWatcher::StopAllListeners() {
  // This function is called internally in situations where we detect that the
  // environment is terminating. At that point, it's not safe to try to release
  // any `ThreadSafeFunction`s; but we can do the rest of the cleanup work
  // here.
  for (auto& it: listeners) {
    fileWatcher->removeWatch(it.first);
    it.second->Stop();
  }
  listeners.clear();
}

// Set the JavaScript callback that will be invoked whenever a file changes.
//
// The user-facing API allows for an arbitrary number of different callbacks;
// this is an internal API for the wrapping JavaScript to use. That internal
// callback can multiplex to however many other callbacks need to be invoked.
void PathWatcher::SetCallback(const Napi::CallbackInfo& info) {
  auto env = info.Env();
  if (!info[0].IsFunction()) {
    Napi::TypeError::New(env, "Function required").ThrowAsJavaScriptException();
  }

  Napi::Function fn = info[0].As<Napi::Function>();
  callback.Reset();
  callback = Napi::Persistent(fn);
}

void PathWatcher::Cleanup(Napi::Env env) {
  if (!isFinalizing) {
    // `ThreadSafeFunction` wraps an internal `napi_threadsafe_function` that,
    // in some occasional scenarios, might already be `null` by the time we get
    // this far. It's not entirely understood why. But if that's true, we can
    // skip this part instead of trying to abort a function that doesn't exist
    // and causing a segfault.
    napi_threadsafe_function _tsfn = tsfn;
    if (_tsfn == nullptr) {
      return;
    }
    // The `ThreadSafeFunction` is the thing that will keep the environment
    // from terminating if we keep it open. When there are no active watchers,
    // we should release `tsfn`; when we add a new watcher thereafter, we can
    // create a new `tsfn`.
    tsfn.Abort();
  }
}

NODE_API_ADDON(PathWatcher)
