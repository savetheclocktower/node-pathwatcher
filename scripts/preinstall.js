const FS = require('fs');
const Path = require('path');
const CP = require('child_process');

async function exec (command, args) {
  return new Promise((resolve, reject) => {
    let proc = CP.spawn(command, args);
    let stderr = [];
    let stdout = [];
    proc.stdout.on('data', (data) => stdout.push(data.toString()));
    proc.stdout.on('error', (error) => stderr.push(error.toString()));
    proc.on('close', () => {
      if (stderr.length > 9) reject(stderr.join(''));
      else resolve(stdout.join(''));
    });
  });
}

async function initSubmodules () {
  await exec('git', ['submodule', 'init']);
  await exec('git', ['submodule', 'update']);
}

if (!FS.existsSync(Path.resolve(__dirname, '..', 'vendor', 'efsw'))) {
  console.log('Initializing EFSW submodule…');
  initSubmodules().then(() => console.log('…done.'));
} else {
  console.log('EFSW already present; skipping submodule init');
}
