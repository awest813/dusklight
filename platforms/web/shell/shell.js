// The page around the web build: checks the browser, lets the player attach their disc image, and
// starts the game. See docs/web-port-plan.md.
//
// The disc image is served to the game by disc-worker.js. The player chooses whether it is copied
// into the browser's private file system (OPFS), so it is there next visit, or read straight from
// the picked file for this session only. Saves and settings live in OPFS too (the game mounts it
// for its user data), and can be backed up to and restored from a zip file here.

'use strict';

const config = window.duskConfig;
const DISC_DIR = 'dusklight-disc';
const STORED_DISC_KEY = 'dusklight.storedDisc';
const GCN_MAGIC = 0xc2339f3d;
const WII_MAGIC = 0x5d1c9ea3;

const $ = (id) => document.getElementById(id);
const log = [];

// Keeps the last 5000 lines or so; trimming in batches keeps logging cheap.
function remember(line) {
  log.push(line);
  if (log.length > 6000) log.splice(0, log.length - 5000);
}

function formatSize(bytes) {
  if (bytes >= 1e9) return `${(bytes / 1e9).toFixed(2)} GB`;
  if (bytes >= 1e6) return `${(bytes / 1e6).toFixed(0)} MB`;
  return `${Math.max(1, Math.round(bytes / 1e3))} KB`;
}

function download(name, blob) {
  const url = URL.createObjectURL(blob);
  const a = Object.assign(document.createElement('a'), { href: url, download: name });
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 10_000);
}

// --- Leaving ----------------------------------------------------------------------------------

// While the game runs, closing or reloading the tab asks first, since anything not saved in the
// game is lost. Leaving on purpose (the game's own restart or quit, or Reload after an error)
// doesn't.
let leaving = false;
let playing = false;
window.addEventListener('beforeunload', (e) => {
  if (playing && !leaving) e.preventDefault();
});

function leave() {
  leaving = true;
  location.reload();
}
// The game calls this to restart or to go back to the disc picker (src/dusk/web/platform.cpp).
window.duskLeave = leave;

// --- Fatal errors -----------------------------------------------------------------------------

let fatalShown = false;
const fatalReasons = [];

// Several reports usually arrive for one failure (the game's own fatal log line, Emscripten's
// abort, the resulting trap), and the abort reason itself is often empty, so collect them all and
// show the end of the log with them.
function showFatal(message) {
  message = String(message ?? '').trim();
  if (message) {
    remember(`fatal: ${message}`);
    console.error(message);
    fatalReasons.push(message);
  }
  const tail = log.filter((line) => !line.startsWith('fatal: ')).slice(-15).join('\n');
  $('fatal-message').textContent = [...fatalReasons, tail ? `Last log lines:\n${tail}` : '']
    .filter(Boolean).join('\n\n') || 'The game stopped unexpectedly.';
  const hint = fatalHint(fatalReasons.join('\n') + '\n' + tail);
  $('fatal-hint').textContent = hint ?? '';
  $('fatal-hint').hidden = !hint;
  fatalShown = true;
  playing = false;
  $('fatal').hidden = false;
}

// What a player can make of the failures that have a likely cause.
function fatalHint(text) {
  if (/Device lost/.test(text)) {
    return 'The browser lost its connection to the graphics card, for example because the driver restarted. ' +
      'Reloading usually fixes this. Progress since your last save is lost.';
  }
  if (/WebGPU error/.test(text)) {
    return 'The browser rejected something the game asked the graphics card to do. This is a bug in Dusklight: ' +
      'please report it with the log.';
  }
  if (/Cannot enlarge memory|out of memory|could not allocate memory|Array buffer allocation failed/i.test(text)) {
    return 'The browser could not give the game enough memory. Close other tabs or programs and reload.';
  }
  if (/Failed to create (adapter|device)|Device request failed|Ignoring CPU adapter|Error creating window/.test(text)) {
    return 'The game could not use the graphics card. chrome://gpu shows whether WebGPU is available.';
  }
  return null;
}

$('fatal-log').onclick = () => download('dusklight-log.txt', new Blob([log.join('\n')], { type: 'text/plain' }));
$('fatal-reload').onclick = leave;
window.addEventListener('error', (e) => showFatal(e.error?.stack ?? e.message));
window.addEventListener('unhandledrejection', (e) => showFatal(e.reason?.stack ?? String(e.reason)));

// --- Browser support --------------------------------------------------------------------------

async function missingFeatures() {
  const missing = [];
  if (!globalThis.crossOriginIsolated) {
    missing.push('Cross-origin isolation (the site must be served with COOP/COEP headers).');
  }
  if (!navigator.gpu) {
    missing.push('WebGPU.');
  } else {
    const problem = await webgpuProblem();
    if (problem) missing.push(problem);
  }
  if (typeof WebAssembly.Suspending !== 'function') {
    missing.push('WebAssembly JavaScript Promise Integration (JSPI).');
  }
  if (!navigator.storage?.getDirectory) missing.push('The origin private file system (OPFS).');
  return missing;
}

// The game asks for the same kind of adapter (aurora's webgpu::initialize), so finding out here tells
// the player before they choose a disc rather than after.
async function webgpuProblem() {
  let adapter = null;
  try {
    adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance', featureLevel: 'compatibility' });
  } catch {
    // Treated like no adapter.
  }
  if (!adapter) {
    return 'A WebGPU graphics adapter. WebGPU may be turned off for this graphics card or driver; chrome://gpu says why.';
  }
  // Every draw passes 64 bytes of immediate data (aurora's gx::DrawImmediateData).
  if (!(adapter.limits.maxImmediateSize >= 64) || typeof GPURenderPassEncoder.prototype.setImmediates !== 'function') {
    return 'WebGPU immediate data, in newer versions of Chrome and Edge.';
  }
  return null;
}

// --- Disc images ------------------------------------------------------------------------------

async function discDir(create = false) {
  const root = await navigator.storage.getDirectory();
  return root.getDirectoryHandle(DISC_DIR, { create });
}

async function storedDisc() {
  let meta;
  try {
    meta = JSON.parse(localStorage.getItem(STORED_DISC_KEY) ?? 'null');
  } catch {
    return null;
  }
  if (!meta?.name) return null;
  try {
    const file = await (await (await discDir()).getFileHandle(meta.name)).getFile();
    // An interrupted copy leaves a short file behind.
    return file.size === meta.size ? meta : null;
  } catch {
    return null;
  }
}

// Removes the stored disc image. For a couple of seconds after a reload, the previous page's disc
// worker can still hold the image open, so a locked image is retried for a while. Returns whether
// nothing is stored any more; the disc is only forgotten once its files are gone.
async function forgetStoredDisc() {
  const root = await navigator.storage.getDirectory();
  for (const deadline = performance.now() + 5000; ;) {
    try {
      await root.removeEntry(DISC_DIR, { recursive: true });
      break;
    } catch (e) {
      if (e.name === 'NotFoundError') break;
      if (e.name !== 'NoModificationAllowedError' || performance.now() > deadline) return false;
      await new Promise((resolve) => setTimeout(resolve, 200));
    }
  }
  localStorage.removeItem(STORED_DISC_KEY);
  return true;
}

// Returns an error message for images the game can't read, or null.
async function checkImage(file) {
  if (file.size < 0x440) return 'This file is too small to be a disc image.';
  const header = new DataView(await file.slice(0, 0x20).arrayBuffer());
  if (header.getUint32(0x18) === WII_MAGIC) {
    return 'Wii disc images are not supported in the browser yet. Use the GameCube version.';
  }
  if (header.getUint32(0x1c) !== GCN_MAGIC) {
    return 'This is not an uncompressed GameCube disc image. Convert RVZ, WIA, CISO and other ' +
      'formats to ISO with Dolphin or nodtool first.';
  }
  const gameId = new TextDecoder().decode(new Uint8Array(header.buffer, 0, 6));
  if (!gameId.startsWith('GZ2')) {
    return `This disc (${gameId}) is not Twilight Princess for GameCube.`;
  }
  return null;
}

async function copyIntoBrowser(file, onProgress) {
  if (!(await forgetStoredDisc())) {
    throw new Error('the disc image stored before is still in use. Reload the page and try again');
  }
  const handle = await (await discDir(true)).getFileHandle(file.name, { create: true });
  const writable = await handle.createWritable();
  let written = 0;
  const reader = file.stream().getReader();
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      await writable.write(value);
      written += value.length;
      onProgress(written / file.size);
    }
    await writable.close();
  } catch (e) {
    await writable.abort().catch(() => {});
    await forgetStoredDisc();
    throw e;
  }
  localStorage.setItem(STORED_DISC_KEY, JSON.stringify({ name: file.name, size: file.size }));
}

// --- Save backups (a plain, uncompressed zip of the game's OPFS files) ------------------------

const CRC_TABLE = new Uint32Array(256).map((_, n) => {
  let c = n;
  for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});

function crc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; ++i) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

// Everything the game keeps in OPFS except the stored disc and its logs (up to 100 MB, and not
// something to restore).
async function* walk(dir, prefix = '') {
  for await (const [name, handle] of dir.entries()) {
    if (!prefix && name === DISC_DIR) continue;
    if (handle.kind === 'directory' && name === 'logs') continue;
    const path = prefix + name;
    if (handle.kind === 'directory') {
      yield* walk(handle, `${path}/`);
    } else {
      yield [path, handle];
    }
  }
}

async function exportSaves() {
  const parts = [];
  const central = [];
  let offset = 0;
  const encoder = new TextEncoder();
  for await (const [path, handle] of walk(await navigator.storage.getDirectory())) {
    const data = new Uint8Array(await (await handle.getFile()).arrayBuffer());
    const name = encoder.encode(path);
    const crc = crc32(data);
    const local = new DataView(new ArrayBuffer(30));
    local.setUint32(0, 0x04034b50, true);
    local.setUint16(4, 20, true);
    local.setUint16(6, 0x0800, true); // UTF-8 names
    local.setUint32(14, crc, true);
    local.setUint32(18, data.length, true);
    local.setUint32(22, data.length, true);
    local.setUint16(26, name.length, true);
    parts.push(local, name, data);

    const entry = new DataView(new ArrayBuffer(46));
    entry.setUint32(0, 0x02014b50, true);
    entry.setUint16(4, 20, true);
    entry.setUint16(6, 20, true);
    entry.setUint16(8, 0x0800, true);
    entry.setUint32(16, crc, true);
    entry.setUint32(20, data.length, true);
    entry.setUint32(24, data.length, true);
    entry.setUint16(28, name.length, true);
    entry.setUint32(42, offset, true);
    central.push(entry, name);
    offset += 30 + name.length + data.length;
  }
  const centralSize = central.reduce((n, part) => n + part.byteLength, 0);
  const end = new DataView(new ArrayBuffer(22));
  end.setUint32(0, 0x06054b50, true);
  end.setUint16(8, central.length / 2, true);
  end.setUint16(10, central.length / 2, true);
  end.setUint32(12, centralSize, true);
  end.setUint32(16, offset, true);
  const date = new Date().toISOString().slice(0, 10);
  download(`dusklight-saves-${date}.zip`, new Blob([...parts, ...central, end], { type: 'application/zip' }));
}

async function importSaves(file) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  const view = new DataView(bytes.buffer);
  const decoder = new TextDecoder();
  const files = [];
  for (let at = 0; at + 30 <= bytes.length && view.getUint32(at, true) === 0x04034b50;) {
    const method = view.getUint16(at + 8, true);
    const size = view.getUint32(at + 18, true);
    const nameLength = view.getUint16(at + 26, true);
    const extraLength = view.getUint16(at + 28, true);
    const name = decoder.decode(bytes.subarray(at + 30, at + 30 + nameLength));
    const start = at + 30 + nameLength + extraLength;
    if (method !== 0 || (view.getUint16(at + 6, true) & 0x8) !== 0) {
      throw new Error('This is not a Dusklight backup.');
    }
    const segments = name.split('/');
    if (segments.some((s) => s === '' || s === '.' || s === '..') || segments[0] === DISC_DIR) {
      throw new Error(`Unexpected path in backup: ${name}`);
    }
    files.push([segments, bytes.subarray(start, start + size)]);
    at = start + size;
  }
  if (files.length === 0) throw new Error('This is not a Dusklight backup.');

  const root = await navigator.storage.getDirectory();
  for (const [segments, data] of files) {
    let dir = root;
    for (const segment of segments.slice(0, -1)) dir = await dir.getDirectoryHandle(segment, { create: true });
    const writable = await (await dir.getFileHandle(segments.at(-1), { create: true })).createWritable();
    await writable.write(data);
    await writable.close();
  }
  return files.length;
}

// --- Starting the game ------------------------------------------------------------------------

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const script = Object.assign(document.createElement('script'), { src, async: true });
    script.onload = resolve;
    script.onerror = () => reject(new Error(`Failed to load ${src}`));
    document.body.append(script);
  });
}

// Downloads, compiles and initializes the module (including its thread pool) without starting the
// game. It starts as soon as the browser is known to be supported, so most of that work is done
// while the player is still choosing their disc.
let runtime = null;
function loadRuntime() {
  runtime ??= (async () => {
    const ready = new Promise((resolve) => {
      window.Module = {
        canvas: $('canvas'),
        noInitialRun: true,
        print: (text) => { remember(text); console.log(text); },
        printErr: (text) => { remember(text); console.warn(text); },
        onAbort: (what) => showFatal(String(what)),
        // Quitting from the game's menu comes back to this page. (An exit proxied from another
        // thread can arrive without its status.)
        onExit: (status) => (!status ? leave() : showFatal(`Exited with status ${status}`)),
        onRuntimeInitialized: resolve,
      };
    });
    await loadScript(config.script);
    await ready;
  })();
  return runtime;
}

async function start(source) {
  $('setup').hidden = true;
  $('loading').hidden = false;
  const canvas = $('canvas');
  canvas.hidden = false;
  canvas.oncontextmenu = (e) => e.preventDefault(); // the game may use the right mouse button
  canvas.focus();
  playing = true;

  await loadRuntime();
  // The module's memory (platforms/web/shell/pre.js), shared with the disc worker.
  const memory = Module.duskMemory();

  const extraArgs = new URLSearchParams(location.search).getAll('arg');
  if (source === null) {
    // No disc (?nodisc): the game opens its prelaunch screen. Used by the smoke test.
    $('loading').hidden = true;
    await Module.callMain(extraArgs);
    return;
  }

  const worker = new Worker('disc-worker.js');
  // Release the disc image (and its OPFS lock) as soon as the page goes away.
  window.addEventListener('pagehide', () => worker.terminate());
  const attached = new Promise((resolve, reject) => {
    worker.onmessage = ({ data }) => (data.type === 'ready' ? resolve(data) : reject(new Error(data.message)));
    worker.onerror = (e) => reject(new Error(e.message));
  });
  worker.postMessage({ memory, channel: Module._dusk_web_disc_channel(), source });
  await attached;

  const name = source.kind === 'opfs' ? source.name : source.file.name;
  const args = ['--dvd', `webdisc://${name}`, ...extraArgs];
  $('loading').hidden = true;
  await Module.callMain(args);
}

// --- Setup screen -----------------------------------------------------------------------------

let pickedFile = null;

function showError(message) {
  $('disc-error').textContent = message ?? '';
  $('disc-error').hidden = !message;
}

async function pick(file) {
  showError(null);
  $('picked').hidden = true;
  const problem = await checkImage(file);
  if (problem) {
    showError(problem);
    return;
  }
  pickedFile = file;
  $('picked-name').textContent = file.name;
  $('picked-size').textContent = `(${formatSize(file.size)})`;

  const { quota = 0, usage = 0 } = (await navigator.storage.estimate?.()) ?? {};
  // A stored disc is removed before the new one is copied.
  const available = quota - usage + ((await storedDisc())?.size ?? 0);
  const fits = available > file.size * 1.05;
  const copy = document.querySelector('input[value=copy]');
  copy.disabled = !fits;
  (fits ? copy : document.querySelector('input[value=session]')).checked = true;
  $('copy-note').textContent = fits ?
    `Uses ${formatSize(file.size)} of the ${formatSize(available)} this browser allows.` :
    `Not enough browser storage (${formatSize(available)} available).`;
  $('picked').hidden = false;
}

async function playPicked() {
  const storage = document.querySelector('input[name=storage]:checked').value;
  if (storage === 'session') {
    await start({ kind: 'file', file: pickedFile });
    return;
  }
  $('play-picked').disabled = true;
  const progress = $('progress');
  progress.hidden = false;
  try {
    await copyIntoBrowser(pickedFile, (fraction) => {
      progress.querySelector('progress').value = fraction;
      progress.querySelector('span').textContent = `Copying… ${Math.floor(fraction * 100)}%`;
    });
  } catch (e) {
    $('play-picked').disabled = false;
    progress.hidden = true;
    showError(`Couldn't copy the disc image into the browser: ${e.message}`);
    return;
  }
  await start({ kind: 'opfs', dir: [DISC_DIR], name: pickedFile.name });
}

// Registers coi-sw.js and reloads once, on hosts that don't send the isolation headers. Returns
// true when a reload is on its way.
async function isolateThroughServiceWorker() {
  if (globalThis.crossOriginIsolated || !window.isSecureContext || !('serviceWorker' in navigator)) return false;
  if (sessionStorage.getItem('dusklight.isolationReload')) return false; // already tried
  try {
    await navigator.serviceWorker.register('coi-sw.js');
    await navigator.serviceWorker.ready;
  } catch {
    return false;
  }
  sessionStorage.setItem('dusklight.isolationReload', '1');
  location.reload();
  return true;
}

// Holds a lock for as long as this page lives, so only one tab plays at a time: two would fight
// over the stored disc image and the saves. Waits briefly, since after a reload the previous page
// may not have released it yet. Resolves to whether this page got it.
function claimSingleInstance() {
  if (!navigator.locks) return Promise.resolve(true);
  return new Promise((resolve) => {
    navigator.locks.request('dusklight', { signal: AbortSignal.timeout(3000) }, () => {
      resolve(true);
      return new Promise(() => {});
    }).catch(() => resolve(false));
  });
}

// Without persistent storage, the browser may clear this site's data (saves included) when the disk
// runs low. Chrome decides on its own whether to grant it (bookmarked sites, for example), and asks
// nobody, so asking on every visit costs nothing and picks up a new bookmark.
async function showPersistence() {
  let persisted = true;
  try {
    persisted = (await navigator.storage.persist?.()) ?? true;
  } catch {
    // Unknown: say nothing.
  }
  $('not-persisted').hidden = persisted;
}

async function setup() {
  if (await isolateThroughServiceWorker()) return;
  sessionStorage.removeItem('dusklight.isolationReload');

  const missing = await missingFeatures();
  if (missing.length) {
    $('unsupported-list').replaceChildren(...missing.map((m) => Object.assign(document.createElement('li'), { textContent: m })));
    $('unsupported').hidden = false;
    return;
  }
  if (!(await claimSingleInstance())) {
    $('elsewhere').hidden = false;
    return;
  }
  loadRuntime().catch((e) => showFatal(`Couldn't load Dusklight: ${e.message}`));

  const stored = await storedDisc();
  if (!stored) {
    // An interrupted copy (the tab closed while copying) leaves up to a whole disc image behind.
    await forgetStoredDisc();
  }
  if (stored) {
    $('stored-name').textContent = stored.name;
    $('stored-size').textContent = `(${formatSize(stored.size)})`;
    $('stored').hidden = false;
    $('play-stored').onclick = () => start({ kind: 'opfs', dir: [DISC_DIR], name: stored.name });
    $('forget-stored').onclick = async () => {
      $('forget-stored').disabled = true;
      if (await forgetStoredDisc()) {
        $('stored').hidden = true;
      } else {
        showError("Couldn't remove the stored disc image. Reload the page and try again.");
      }
      $('forget-stored').disabled = false;
    };
  }

  $('file').onchange = (e) => e.target.files[0] && pick(e.target.files[0]);
  const drop = $('drop');
  drop.ondragover = (e) => { e.preventDefault(); drop.classList.add('over'); };
  drop.ondragleave = () => drop.classList.remove('over');
  drop.ondrop = (e) => {
    e.preventDefault();
    drop.classList.remove('over');
    if (e.dataTransfer.files[0]) pick(e.dataTransfer.files[0]);
  };
  $('play-picked').onclick = playPicked;

  $('export-saves').onclick = () => exportSaves().catch((e) => alert(`Backup failed: ${e.message}`));
  $('import-saves').onchange = async (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (!file || !confirm('Restoring copies the saves and settings in the backup into this browser, overwriting ' +
      'files with the same names. Continue?')) return;
    try {
      alert(`Restored ${await importSaves(file)} files.`);
    } catch (err) {
      alert(`Restore failed: ${err.message}`);
    }
  };

  $('disc').hidden = false;
  $('saves').hidden = false;
  showPersistence();

  if (new URLSearchParams(location.search).has('nodisc')) {
    await start(null);
  }
}

// Exposed for the browser tests (platforms/web/tests). `window.duskState` is polled by run-page.mjs.
window.duskState = { log, get fatal() { return fatalShown ? $('fatal-message').textContent : null; } };
window.duskShell = { checkImage, copyIntoBrowser, storedDisc, forgetStoredDisc, exportSaves, importSaves, crc32 };

setup();
