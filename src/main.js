let binding;
try {
  binding = require('../build/Debug/pathwatcher.node');
} catch (err) {
  binding = require('../build/Release/pathwatcher.node');
}
const { Emitter } = require('event-kit');
const fs = require('fs');
const { stat } = require('fs/promises');
const path = require('path');

let initialized = false;

const HANDLE_WATCHERS = new Map();

function wait (ms) {
  return new Promise(r => setTimeout(r, ms));
}

function normalizePath(rawPath) {
  if (!rawPath.endsWith(path.sep)) return rawPath;
  return rawPath.substring(0, rawPath.length - 1);
}

function pathsAreEqual(pathA, pathB) {
  return normalizePath(pathA) == normalizePath(pathB);
}

class HandleWatcher {
  constructor(path) {
    this.path = path;
    this.emitter = new Emitter();
    this.start();
  }

  async onEvent (event, filePath, oldFilePath) {
    filePath &&= path.normalize(filePath);
    oldFilePath &&= path.normalize(oldFilePath);

    switch (event) {
      case 'rename':
        this.close();
        await wait(100);
        try {
          await stat(this.path);
          // File still exists at the same path.
          this.start();
          this.emitter.emit(
            'did-change',
            { event: 'change', newFilePath: null }
          );
        } catch (err) {
          // File does not exist at the old path.
          this.path = filePath;
          if (process.platform === 'darwin' && /\/\.Trash\//.test(filePath)) {
            // We'll treat this like a deletion; no point in continuing to
            // track this file when it's in the trash.
            this.emitter.emit(
              'did-change',
              { event: 'delete', newFilePath: null }
            );
            this.close();
          } else {
            // The file has a new location, so let's resume watching it from
            // there.
            this.start();
            this.emitter.emit(
              'did-change',
              { event: 'rename', newFilePath: filePath }
            );
          }
        }
        return;
      case 'delete':
        // Wait for a very short interval to protect against brief deletions or
        // spurious deletion events. Git will sometimes briefly delete a file
        // before restoring it with different contents.
        await wait(20);
        if (fs.existsSync(filePath)) return;
        this.emitter.emit(
          'did-change',
          { event: 'delete', newFilePath: null }
        );
        this.close();
        return;
      case 'child-delete':
        // Wait for a very short interval to protect against brief deletions or
        // spurious deletion events. Git will sometimes briefly delete a file
        // before restoring it with different contents.
        await wait(20);
        if (fs.existsSync(filePath)) return;
        this.emitter.emit(
          'did-change',
          { event, newFilePath: filePath, oldFilePath, rawFilePath: filePath }
        );
        return;
      case 'unknown':
        throw new Error("Received unknown event for path: " + this.path);
      default:
        this.emitter.emit(
          'did-change',
          { event, newFilePath: filePath, oldFilePath, rawFilePath: filePath }
        );
    }
  }

  onDidChange (callback) {
    return this.emitter.on('did-change', callback);
  }

  start () {
    let troubleWatcher;
    if (!this.path.endsWith(path.sep)) {
      this.path += path.sep;
    }
    this.handle = binding.watch(this.path);
    if (HANDLE_WATCHERS.has(this.handle)) {
      troubleWatcher = HANDLE_WATCHERS.get(this.handle);
      troubleWatcher.close();
      console.error(`The handle (${this.handle}) returned by watching path ${this.path} is the same as an already-watched path: ${troubleWatcher.path}`);
    }
    return HANDLE_WATCHERS.set(this.handle, this);
  }

  closeIfNoListener () {
    if (this.emitter.getTotalListenerCount() === 0) {
      this.close();
    }
  }

  close () {
    if (!HANDLE_WATCHERS.has(this.handle)) return;
    binding.unwatch(this.handle);
    HANDLE_WATCHERS.delete(this.handle);
  }
}

class PathWatcher {
  isWatchingParent = false;
  path = null;
  handleWatcher = null;

  constructor(filePath, callback) {
    this.path = filePath;

    if (!fs.existsSync(filePath)) {
      let err = new Error(`Unable to watch path`);
      err.code = 'ENOENT';
      throw err;
    }

    this.assignRealPath();

    // Resolve the real path before we pass it to the native watcher. It's
    // better at dealing with real paths instead of symlinks and it doesn't
    // otherwise matter for our purposes.
    if (this.realPath) {
      filePath = this.realPath;
    }

    this.emitter = new Emitter();

    let stats = fs.statSync(filePath);
    this.isWatchingParent = !stats.isDirectory();

    if (this.isWatchingParent) {
      filePath = path.dirname(filePath);
    }

    for (let watcher of HANDLE_WATCHERS.values()) {
      if (pathsAreEqual(watcher.path, filePath)) {
        this.handleWatcher = watcher;
        break;
      }
    }

    this.handleWatcher ??= new HandleWatcher(filePath);

    this.onChange = ({ event, newFilePath, oldFilePath, rawFilePath }) => {
      // Filter out strange events.
      let comparisonPath = this.path ?? this.realPath;
      if (rawFilePath && (comparisonPath.length > rawFilePath.length)) {
        // This is weird. Not sure why this happens yet. It's most likely an
        // event for a parent directory of what we're watching. Ideally we can
        // filter this out earlier in the process, like in the native code, but
        // that would involve doing earlier symlink resolution.
        return;
      }
      switch (event) {
        case 'rename':
        case 'change':
        case 'delete':
          if (event === 'rename') {
            this.path = newFilePath;
            this.assignRealPath();
          }
          if (typeof callback === 'function') {
            callback.call(this, event, newFilePath);
          }
          this.emitter.emit(
            'did-change',
            { event, newFilePath }
          );
          return;
        case 'child-rename':
          if (this.isWatchingParent) {
            if (this.matches(oldFilePath)) {
              return this.onChange({ event: 'rename', newFilePath });
            }
          } else {
            return this.onChange({ event: 'change', newFilePath: '' });
          }
          break;
        case 'child-delete':
          if (this.isWatchingParent) {
            if (this.matches(newFilePath)) {
              return this.onChange({ event: 'delete', newFilePath: null });
            }
          } else {
            return this.onChange({ event: 'change', newFilePath: '' });
          }
          break;
        case 'child-change':
          if (this.isWatchingParent && this.matches(newFilePath)) {
            return this.onChange({ event: 'change', newFilePath: '' });
          }
          break;
        case 'child-create':
          if (!this.isWatchingParent) {
            if (this.matches(newFilePath)) {
              // If we are watching a file already, it must exist. There is no
              // `create` event. This should not be handled because it's
              // invalid.
              return;
            }
            return this.onChange({ event: 'change', newFilePath: '', rawFilePath });
          }
      }
    };

    this.disposable = this.handleWatcher.onDidChange(this.onChange);
  }

  matches (otherPath) {
    if (this.realPath) {
      return this.realPath === otherPath;
    } else {
      return this.path === otherPath;
    }
  }

  assignRealPath () {
    try {
      this.realPath = fs.realpathSync(this.path);
    } catch (_error) {
      this.realPath = null;
    }
  }

  onDidChange (callback) {
    return this.emitter.on('did-change', callback);
  }

  close () {
    this.emitter?.dispose();
    this.disposable?.dispose();
    this.handleWatcher?.closeIfNoListener();
  }
}

function DEFAULT_CALLBACK(event, handle, filePath, oldFilePath) {
  if (!HANDLE_WATCHERS.has(handle)) return;

  let watcher = HANDLE_WATCHERS.get(handle);
  watcher.onEvent(event, filePath, oldFilePath);
}

function watch (pathToWatch, callback) {
  if (!initialized) {
    binding.setCallback(DEFAULT_CALLBACK);
    initialized = true;
  }
  return new PathWatcher(path.resolve(pathToWatch), callback);
}

function closeAllWatchers () {
  for (let watcher of HANDLE_WATCHERS.values()) {
    watcher?.close();
  }
  HANDLE_WATCHERS.clear();
}

function getWatchedPaths () {
  let watchers = Array.from(HANDLE_WATCHERS.values());
  return watchers.map(w => w.path);
}

const File = require('./file');
const Directory = require('./directory');

module.exports = {
  watch,
  closeAllWatchers,
  getWatchedPaths,
  File,
  Directory
};
