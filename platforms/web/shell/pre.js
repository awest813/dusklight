// Linked into dusklight.js with --pre-js, so it runs in the module's scope: hands the page the
// module's wasm memory, which the page shares with its disc worker (shell.js, disc-worker.js).
Module['duskMemory'] = () => wasmMemory;
