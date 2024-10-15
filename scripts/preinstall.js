const FS = require('fs');
const Path = require('path');
const { x } = require('tinyexec');

async function initSubmodules () {
  await x('git', ['submodule', 'init']);
  await x('git', ['submodule', 'update']);
}

if (!FS.existsSync(Path.resolve(__dirname, '..', 'vendor', 'efsw'))) {
  console.log('Initializing EFSW submodule…');
  initSubmodules().then(() => console.log('…done.'));
} else {
  console.log('EFSW already present; skipping submodule init');
}
