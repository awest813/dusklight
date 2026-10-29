// Stands in for dusklight.js in shell-test.mjs. It honours the same contract as the Emscripten
// module: it takes the page's memory (Module.wasmMemory), reports onRuntimeInitialized, exposes the
// disc channel's address, and on callMain reads the disc from a worker the way game threads do in
// src/dusk/web/disc.cpp (post a request, wait for the reply, hand the slot back).
(() => {
  const Module = window.Module;
  const memory = Module.wasmMemory;
  const channel = 4096;
  const scratch = 65536;
  new Uint8Array(memory.buffer, channel, 296).fill(0);
  new Float64Array(memory.buffer, channel + 32, 1)[0] = -1;

  const reader = `
    onmessage = ({ data: { memory, channel, reads } }) => {
      const out = [];
      for (const { offset, length, destination } of reads) {
        const ints = new Int32Array(memory.buffer, channel, 2);
        new Float64Array(memory.buffer, channel + 16, 2).set([offset, destination]);
        new Uint32Array(memory.buffer, channel + 8, 1)[0] = length;
        ints[1] = -1;
        Atomics.store(ints, 0, 1);
        Atomics.notify(ints, 0);
        while (Atomics.load(ints, 0) === 1) Atomics.wait(ints, 0, 1);
        const result = ints[1];
        Atomics.store(ints, 0, 0);
        Atomics.notify(ints, 0);
        out.push({ offset, length, result,
          bytes: Array.from(new Uint8Array(memory.buffer, destination, Math.max(0, result))) });
      }
      const nameBytes = new Uint8Array(memory.buffer, channel + 40, 256);
      const name = new TextDecoder().decode(nameBytes.slice(0, nameBytes.indexOf(0)));
      postMessage({ reads: out, size: new Float64Array(memory.buffer, channel + 32, 1)[0], name });
    };`;

  Module._dusk_web_disc_channel = () => channel;
  Module.callMain = (args) => {
    const size = new Float64Array(memory.buffer, channel + 32, 1)[0];
    const reads = [
      { offset: 0, length: 64 },
      { offset: 1000, length: 4096 },
      { offset: 2 * 1024 * 1024 + 5, length: 100000 },
      { offset: size - 10, length: 64 },  // short at the end
      { offset: size + 100, length: 16 }, // past the end
    ].map((r, i) => ({ ...r, destination: scratch + i * 131072 }));
    const worker = new Worker(URL.createObjectURL(new Blob([reader], { type: 'text/javascript' })));
    worker.onmessage = ({ data }) => { window.fakeModuleResult = { ...data, args }; };
    worker.onerror = (e) => { window.fakeModuleResult = { error: e.message, args }; };
    worker.postMessage({ memory, channel, reads });
    return Promise.resolve(0);
  };
  setTimeout(() => Module.onRuntimeInitialized());
})();
