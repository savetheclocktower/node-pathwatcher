#include <errno.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/inotify.h>
#include <linux/limits.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>

#include "common.h"

static int g_inotify;
static int g_init_errno;

void PlatformInit(Napi::Env _env) {
  g_inotify = inotify_init();
  if (g_inotify == -1) {
    g_init_errno = errno;
    return;
  }
}

void PlatformThread(
  const PathWatcherWorker::ExecutionProgress& progress,
  bool& shouldStop
) {
  std::cout << "PlatformThread START" << std::endl;
  // Needs to be large enough for sizeof(inotify_event) + strlen(filename).
  char buf[4096];

  while (!shouldStop) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(g_inotify, &read_fds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms timeout

    int ret = select(g_inotify + 1, &read_fds, NULL, NULL, &tv);

    if (ret == -1 && errno != EINTR) {
      break;
    }

    if (ret == 0) {
      // Timeout.
      continue;
    }

    int size = read(g_inotify, buf, sizeof(buf));
    if (size <= 0) break;

    inotify_event* e;
    for (char* p = buf; p < buf + size; p += sizeof(*e) + e->len) {
      e = reinterpret_cast<inotify_event*>(p);

      int fd = e->wd;
      EVENT_TYPE type;
      std::vector<char> path;

      // Note that inotify won't tell us where the file or directory has been
      // moved to, so we just treat IN_MOVE_SELF as file being deleted.
      if (e->mask & (IN_ATTRIB | IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVE)) {
        type = EVENT_CHANGE;
      } else if (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
        type = EVENT_DELETE;
      } else {
        continue;
      }

      PathWatcherEvent event(type, fd, path);
      progress.Send(&event, 1);
    }
  }

  std::cout << "PlatformThread END" << std::endl;
}

WatcherHandle PlatformWatch(const char* path) {
  if (g_inotify == -1) {
    return -g_init_errno;
  }

  int fd = inotify_add_watch(g_inotify, path, IN_ATTRIB | IN_CREATE |
      IN_DELETE | IN_MODIFY | IN_MOVE | IN_MOVE_SELF | IN_DELETE_SELF);
  if (fd == -1) {
    return -errno;
  }
  return fd;
}

void PlatformUnwatch(WatcherHandle fd) {
  inotify_rm_watch(g_inotify, fd);
}

bool PlatformIsHandleValid(WatcherHandle handle) {
  return handle >= 0;
}

int PlatformInvalidHandleToErrorNumber(WatcherHandle handle) {
  return -handle;
}
