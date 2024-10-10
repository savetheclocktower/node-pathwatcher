#include <windows.h>
#include <iostream>
#include <unordered_map>
#include "common.h"
#include "addon-data.h"

struct WatcherInfo {
    HANDLE handle;
    OVERLAPPED overlapped;
    char buffer[32 * 1024];
};

std::unordered_map<WatcherHandle, WatcherInfo*> watchers;

void CALLBACK WatchCallback(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped) {
    WatcherInfo* info = reinterpret_cast<WatcherInfo*>(lpOverlapped);
    if (dwErrorCode == ERROR_OPERATION_ABORTED) {
        delete info;
        return;
    }

    FILE_NOTIFY_INFORMATION* event = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(info->buffer);
    do {
        EVENT_TYPE type;
        switch (event->Action) {
            case FILE_ACTION_ADDED:
            case FILE_ACTION_REMOVED:
            case FILE_ACTION_MODIFIED:
                type = EVENT_CHANGE;
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
            case FILE_ACTION_RENAMED_NEW_NAME:
                type = EVENT_RENAME;
                break;
            default:
                type = EVENT_NONE;
        }

        if (type != EVENT_NONE) {
            std::vector<char> path(event->FileNameLength / sizeof(WCHAR) + 1);
            WideCharToMultiByte(CP_UTF8, 0, event->FileName, event->FileNameLength / sizeof(WCHAR),
                                path.data(), path.size(), NULL, NULL);
            path[path.size() - 1] = '\0';

            PathWatcherEvent watcherEvent(type, reinterpret_cast<WatcherHandle>(info->handle), path);
            // You would need to send this event to the JavaScript side here
            // This part depends on how you've set up your N-API communication
        }

        if (event->NextEntryOffset == 0) {
            break;
        }
        event = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
            reinterpret_cast<char*>(event) + event->NextEntryOffset
        );
    } while (true);

    // Queue the next read operation
    ReadDirectoryChangesW(
        info->handle, info->buffer, sizeof(info->buffer), TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
        NULL, &info->overlapped, WatchCallback
    );
}

void PlatformInit(Napi::Env env) {
    // No specific initialization needed for Windows
}

void PlatformThread(const PathWatcherWorker::ExecutionProgress& progress, bool& shouldStop, Napi::Env env) {
    while (!shouldStop) {
        SleepEx(100, TRUE);  // Make the thread alertable
    }
}

WatcherHandle PlatformWatch(const char* path, Napi::Env env) {
    HANDLE handle = CreateFileA(
        path, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL
    );

    if (handle == INVALID_HANDLE_VALUE) {
        return reinterpret_cast<WatcherHandle>(-GetLastError());
    }

    WatcherInfo* info = new WatcherInfo();
    info->handle = handle;
    ZeroMemory(&info->overlapped, sizeof(OVERLAPPED));

    if (!ReadDirectoryChangesW(
        handle, info->buffer, sizeof(info->buffer), TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
        NULL, &info->overlapped, WatchCallback
    )) {
        CloseHandle(handle);
        delete info;
        return reinterpret_cast<WatcherHandle>(-GetLastError());
    }

    watchers[reinterpret_cast<WatcherHandle>(handle)] = info;
    return reinterpret_cast<WatcherHandle>(handle);
}

void PlatformUnwatch(WatcherHandle handle, Napi::Env env) {
    auto it = watchers.find(handle);
    if (it != watchers.end()) {
        CancelIo(it->second->handle);
        CloseHandle(it->second->handle);
        watchers.erase(it);
    }
}

bool PlatformIsHandleValid(WatcherHandle handle) {
    return reinterpret_cast<HANDLE>(handle) != INVALID_HANDLE_VALUE;
}

int PlatformInvalidHandleToErrorNumber(WatcherHandle handle) {
    return -reinterpret_cast<int>(handle);
}

void PlatformStop(Napi::Env env) {
    for (const auto& pair : watchers) {
        CancelIo(pair.second->handle);
        CloseHandle(pair.second->handle);
        delete pair.second;
    }
    watchers.clear();
}
