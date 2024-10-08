const {
  Worker, isMainThread, parentPort, workerData
} = require('node:worker_threads');
const {
  performance
} = require('node:perf_hooks');

const temp = require('temp');
const fs = require('fs');
const path = require('path');
const util = require('util');

const wait = util.promisify(setTimeout);

const EXPECTED_CALL_COUNT = 3;

if (isMainThread) {
  module.exports = function spawnThread(id) {
    return new Promise(async (resolve, reject) => {
      console.log('Spawning worker:', id);
      const worker = new Worker(__filename, {
        workerData: id,
      });
      worker.on('message', async (msg) => {
        console.log('[parent] Worker', id, 'reported call count:', msg);
        await wait(500);
        if (msg >= EXPECTED_CALL_COUNT) {
          resolve();
        } else {
          reject(`Not enough calls! Expected: ${EXPECTED_CALL_COUNT} Actual: ${msg}`);
        }
      });
      worker.on('error', reject);
      worker.on('exit', (code) => {
        if (code !== 0) {
          console.log(`Worker stopped with exit code ${code}`);
          reject();
        }
      });
    });
  };
} else {
  const tempDir = temp.mkdirSync('node-pathwatcher-directory');
  const tempFile = path.join(tempDir, 'file');

  const { watch, closeAllWatchers } = require('../src/main');

  class Scheduler {
    constructor(id, pathToWatch) {
      this.id = id;
      this.path = pathToWatch;
      this.callCount = 0;
    }

    async start () {
      console.log('Scheduler', this.id, 'starting at', performance.now(), 'watching path:', this.path);
      this.watcher = watch(this.path, (event) => {
        this.callCount++;
        console.log('PathWatcher event for worker', this.id, event)
        console.log('callCount is now:', this.callCount);
      });
      console.log('Scheduler', this.id, 'ready at:', performance.now());
    }

    stop () {
      this.watcher?.close();
    }
  }

  (async () => {
    console.log('Worker', workerData, 'creating file:', tempFile);
    fs.writeFileSync(tempFile, '');
    await wait(500);
    const scheduler = new Scheduler(workerData, tempFile);
    scheduler.start();
    await wait(2000);

    console.log('Worker', workerData, 'changing file:', tempFile);
    // Should generate one or two events:
    fs.writeFileSync(tempFile, 'changed');
    await wait(1000);
    console.log('Worker', workerData, 'changing file again:', tempFile);
    // Should generate another event:
    fs.writeFileSync(tempFile, 'changed again');
    await wait(1000);
    // Should generate a final event (total count 3 or 4):
    console.log('Worker', workerData, 'deleting file:', tempFile);
    fs.rmSync(tempFile);
    await wait(1000);

    parentPort.postMessage(scheduler.callCount);

    closeAllWatchers();
    console.log('Worker', workerData, 'closing');
    process.exit(0);
  })();
}
