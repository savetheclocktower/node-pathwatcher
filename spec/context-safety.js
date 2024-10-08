
// This script tests the library for context safety by creating several
// instances on separate threads.
//
// This test is successful when the script exits gracefully. It fails when the
// script segfaults or runs indefinitely.
const spawnThread = require('./worker');

const NUM_WORKERS = 3;

for (let i = 0; i < NUM_WORKERS; i++) {
  spawnThread(i);
}
