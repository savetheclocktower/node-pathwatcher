# node-pathwatcher

Watch files and directories for changes.


> [!IMPORTANT]
> This library is used in [Pulsar][] in several places for compatibility reasons. The [nsfw](https://www.npmjs.com/package/nsfw) library is more robust and more widely used; it is available in Pulsar via `atom.watchPath` and is usually a better choice.
>
> The purpose of this library’s continued inclusion in Pulsar is to provide the [File][] and [Directory][] classes that have long been available as exports via `require('atom')`.

## Installing

```bash
npm install pathwatcher
```

## Building

  * Clone the repository
  * Run `npm install` to install the dependencies
  * Run `npm test` to run the specs

## Using

```js
const PathWatcher = require('pathwatcher');
```

### PathWatcher.watch(filename, listener)

Watch for changes on `filename`, where `filename` is either a file or a directory. Returns a number that represents a specific watcher instance.

The listener callback gets two arguments: `(event, path)`. `event` can be `rename`, `delete` or `change`, and `path` is the path of the file which triggered the event.

For directories, the `change` event is emitted when a file or directory under the watched directory is created, deleted, or renamed. The watcher is not recursive; changes to the contents of subdirectories will not be detected.

### PathWatcher.close(handle)

Stop watching for changes on the given `PathWatcher`.

The `handle` argument is a number and should be the return value from the initial call to `PathWatcher.watch`.

### File and Directory

These are convenience wrappers around some filesystem operations. They also wrap `PathWatcher.watch` via their `onDidChange` (and similar) methods.

Documentation can be found on the Pulsar documentation site:

* [File][]
* [Directory][]


[File]: https://docs.pulsar-edit.dev/api/pulsar/latest/File/
[Directory]: https://docs.pulsar-edit.dev/api/pulsar/latest/Directory/
[Pulsar]: https://pulsar-edit.dev
