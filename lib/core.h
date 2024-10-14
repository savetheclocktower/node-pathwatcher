#pragma once

#define DEBUG 1
#include <napi.h>
#include <string>
#include <atomic>
#include <mutex>

#include "../vendor/efsw/include/efsw/efsw.hpp"

typedef efsw::WatchID WatcherHandle;

#define WatcherHandleToV8Value(h, e) Napi::Number::New(e, h)
#define V8ValueToWatcherHandle(v) v.Int32Value()
#define IsV8ValueWatcherHandle(v) v.IsNumber()

#ifdef _WIN32
#define PATH_SEPARATOR "\\"
#else
#define PATH_SEPARATOR "/"
#endif

struct PathWatcherEvent {
  efsw::Action type;
  efsw::WatchID handle;
  std::vector<char> new_path;
  std::vector<char> old_path;

  // Default constructor
  PathWatcherEvent() = default;

  // Constructor
  PathWatcherEvent(efsw::Action t, efsw::WatchID h, const std::vector<char>& np, const std::vector<char>& op = std::vector<char>())
    : type(t), handle(h), new_path(np), old_path(op) {}

  // Copy constructor
  PathWatcherEvent(const PathWatcherEvent& other)
    : type(other.type), handle(other.handle), new_path(other.new_path), old_path(other.old_path) {}

  // Copy assignment operator
  PathWatcherEvent& operator=(const PathWatcherEvent& other) {
    if (this != &other) {
      type = other.type;
      handle = other.handle;
      new_path = other.new_path;
      old_path = other.old_path;
    }
    return *this;
  }

  // Move constructor
  PathWatcherEvent(PathWatcherEvent&& other) noexcept
    : type(other.type), handle(other.handle),
    new_path(std::move(other.new_path)), old_path(std::move(other.old_path)) {}

  // Move assignment operator
  PathWatcherEvent& operator=(PathWatcherEvent&& other) noexcept {
    if (this != &other) {
      type = other.type;
      handle = other.handle;
      new_path = std::move(other.new_path);
      old_path = std::move(other.old_path);
    }
    return *this;
  }
};


class PathWatcherListener: public efsw::FileWatchListener {
public:
  PathWatcherListener(Napi::Env env, Napi::Function fn);
  ~PathWatcherListener();
  void handleFileAction(
    efsw::WatchID watchId,
    const std::string& dir,
    const std::string& filename,
    efsw::Action action,
    std::string oldFilename
  ) override;

  void Stop();

private:
  std::atomic<bool> isShuttingDown{false};
  std::mutex shutdownMutex;
  Napi::Function callback;
  Napi::ThreadSafeFunction tsfn;
};

void ProcessEvent(Napi::Env env, Napi::Function callback, PathWatcherEvent* event);

namespace EFSW {
  class Watcher {
  public:
    Watcher(const char* path, Napi::Function fn, Napi::Env env);
    ~Watcher();

    WatcherHandle Start();
    void Stop();
  private:
    const char* path;
    Napi::Env env;
    Napi::FunctionReference callback;
  };

  void Init(Napi::Env env);
  void Cleanup(Napi::Env env);

  Napi::Value Watch(const Napi::CallbackInfo& info);
  Napi::Value Unwatch(const Napi::CallbackInfo& info);
  Napi::Value SetCallback(const Napi::CallbackInfo& info);
}
