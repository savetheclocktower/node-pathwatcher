
// This script tests the library for context safety by creating several
// instances on separate threads.
//
// This test is successful when the script exits gracefully. It fails when the
// script segfaults or runs indefinitely.
const spawnThread = require('./worker');

const NUM_WORKERS = 2;
const MAX_DURATION = 20 * 1000;

// Pick one of the workers to return earlier than the others.
let earlyReturn = null;
if (NUM_WORKERS > 1) {
  earlyReturn = Math.floor(Math.random() * NUM_WORKERS);
}

function bail () {
  console.error(`Script ran for more than ${MAX_DURATION / 1000} seconds; there's an open handle somewhere!`);
  process.exit(2);
}

let failsafe = setTimeout(bail, MAX_DURATION);
failsafe.unref();

for (let i = 0; i < NUM_WORKERS; i++) {
  spawnThread(i, earlyReturn);
  // .catch((err) => {
  //   console.error(`Worker ${i + 1} threw error:`);
  //   console.error(err);
  // }).finally(() => {
  //   console.log(`Worker ${i + 1} finished.`);
  // });
}
