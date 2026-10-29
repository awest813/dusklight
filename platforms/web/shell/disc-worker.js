// Serves disc image reads for the game (src/dusk/web/disc.cpp).
//
// shell.js starts this worker with the game's shared wasm memory, the address of the disc channel
// in it, and the image to serve: a file the player stored in the browser's private file system
// (OPFS), or a picked File for this session only. The worker then blocks on the channel for good,
// so game threads can read the disc synchronously without ever involving the browser main thread.
//
// Channel layout (keep in sync with dusk::web::disc::Channel):
//   0 i32 state (0 idle, 1 request posted, 2 reply ready)   4 i32 result (bytes read or -1)
//   8 u32 length   16 f64 offset   24 f64 destination   32 f64 size   40 char[256] name
//
// Sources: { kind: 'opfs', dir: [names...], name } or { kind: 'file', file }.

'use strict';

const STATE_IDLE = 0;
const STATE_REQUEST = 1;
const STATE_REPLY = 2;
const NAME_CAPACITY = 256;

// Only one sync access handle may be open per file. After a reload, the previous page's worker can
// still hold it for a moment, so retry for a while before giving up.
async function openAccessHandle(handle) {
  for (let attempt = 0; ; ++attempt) {
    try {
      return await handle.createSyncAccessHandle();
    } catch (e) {
      if (e.name !== 'NoModificationAllowedError' || attempt >= 50) throw e;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
}

async function openSource(source) {
  if (source.kind === 'opfs') {
    let dir = await navigator.storage.getDirectory();
    for (const name of source.dir ?? []) dir = await dir.getDirectoryHandle(name);
    const handle = await dir.getFileHandle(source.name);
    const access = await openAccessHandle(handle);
    let shared = true;
    return {
      size: access.getSize(),
      read(offset, view) {
        // read() takes shared memory where the browser allows it; otherwise go through a copy.
        if (shared) {
          try {
            return access.read(view, { at: offset });
          } catch (e) {
            if (!(e instanceof TypeError)) throw e;
            shared = false;
          }
        }
        const copy = new Uint8Array(view.length);
        const n = access.read(copy, { at: offset });
        view.set(copy.subarray(0, n));
        return n;
      },
    };
  }
  if (source.kind === 'file') {
    const file = source.file;
    const reader = new FileReaderSync();
    return {
      size: file.size,
      read(offset, view) {
        const bytes = new Uint8Array(reader.readAsArrayBuffer(file.slice(offset, offset + view.length)));
        view.set(bytes);
        return bytes.length;
      },
    };
  }
  throw new Error(`unknown disc source ${source.kind}`);
}

function readFully(source, offset, view) {
  let total = 0;
  while (total < view.length) {
    const n = source.read(offset + total, view.subarray(total));
    if (n <= 0) break;
    total += n;
  }
  return total;
}

function serve(memory, channel, source) {
  for (;;) {
    // Views are rebuilt every time: growing the memory replaces its buffer.
    const ints = new Int32Array(memory.buffer, channel, 2);
    Atomics.wait(ints, 0, STATE_IDLE);
    const state = Atomics.load(ints, 0);
    if (state === STATE_REPLY) {
      // The previous reply has not been taken yet.
      Atomics.wait(ints, 0, STATE_REPLY);
      continue;
    }
    if (state !== STATE_REQUEST) continue;

    const length = new Uint32Array(memory.buffer, channel + 8, 1)[0];
    const [offset, destination] = new Float64Array(memory.buffer, channel + 16, 2);
    let result = -1;
    try {
      result = readFully(source, offset, new Uint8Array(memory.buffer, destination, length));
    } catch (e) {
      console.error(`disc-worker: read of ${length} bytes at ${offset} failed:`, e);
    }
    ints[1] = result;
    Atomics.store(ints, 0, STATE_REPLY);
    Atomics.notify(ints, 0);
  }
}

self.onmessage = async (event) => {
  const { memory, channel, source } = event.data;
  let opened;
  try {
    opened = await openSource(source);
  } catch (e) {
    self.postMessage({ type: 'error', message: String(e?.message ?? e) });
    return;
  }

  // Publish the name and size, then the (unchanged) state as a release point for them.
  const name = new TextEncoder().encode(source.displayName ?? source.name ?? source.file?.name ?? 'disc.iso');
  const nameBytes = new Uint8Array(memory.buffer, channel + 40, NAME_CAPACITY);
  nameBytes.fill(0);
  nameBytes.set(name.subarray(0, NAME_CAPACITY - 1));
  new Float64Array(memory.buffer, channel + 32, 1)[0] = opened.size;
  Atomics.store(new Int32Array(memory.buffer, channel, 1), 0, STATE_IDLE);

  self.postMessage({ type: 'ready', size: opened.size });
  serve(memory, channel, opened);
};
