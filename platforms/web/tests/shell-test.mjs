// Browser test of the web page (platforms/web/shell): the disc picker in both storage modes, the
// disc worker serving reads through the shared-memory channel, and save backup/restore.
//
//   node platforms/web/tests/shell-test.mjs
//
// Runs in headless Chromium through Playwright (`npm i playwright`, or a preinstalled copy). No
// wasm build is needed: fake-module.js stands in for dusklight.js with the same contract the page
// relies on (Module.wasmMemory, onRuntimeInitialized, _dusk_web_disc_channel, callMain), and reads
// the disc from a worker exactly as a game thread does (src/dusk/web/disc.cpp).
import { createServer } from 'node:http';
import { cp, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const shellDir = path.join(here, '..', 'shell');

let failures = 0;
function check(cond, what) {
  if (!cond) {
    console.error(`FAIL: ${what}`);
    ++failures;
  } else {
    console.log(`ok: ${what}`);
  }
}

// A synthetic GameCube image (no game data): header with game ID and magic, then a byte pattern.
function syntheticImage(size, { gameId = 'GZ2E01', gcn = true, wii = false } = {}) {
  const bytes = new Uint8Array(size);
  for (let i = 0; i < size; ++i) bytes[i] = (i * 31 + (i >> 8)) & 0xff;
  bytes.set(new TextEncoder().encode(gameId), 0);
  const view = new DataView(bytes.buffer);
  view.setUint32(0x18, wii ? 0x5d1c9ea3 : 0);
  view.setUint32(0x1c, gcn ? 0xc2339f3d : 0);
  return bytes;
}

async function makeSite() {
  const dir = await mkdtemp(path.join(os.tmpdir(), 'dusk-shell-'));
  await cp(shellDir, dir, { recursive: true });
  const template = await readFile(path.join(shellDir, 'index.html.in'), 'utf8');
  await writeFile(path.join(dir, 'index.html'), template
    .replace('@DUSK_WEB_JS@', 'fake-module.js')
    .replace('@DUSK_WEB_INITIAL_MEMORY_PAGES@', '64')
    .replace('@DUSK_WEB_MAXIMUM_MEMORY_PAGES@', '256'));
  await cp(path.join(here, 'fake-module.js'), path.join(dir, 'fake-module.js'));
  return dir;
}

function serve(root, isolated) {
  const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css' };
  const server = createServer(async (req, res) => {
    if (isolated) {
      res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
      res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    }
    let file = path.join(root, decodeURIComponent(new URL(req.url, 'http://x').pathname));
    if (file.endsWith('/')) file += 'index.html';
    try {
      const data = await readFile(file);
      res.setHeader('Content-Type', types[path.extname(file)] ?? 'application/octet-stream');
      res.end(data);
    } catch {
      res.statusCode = 404;
      res.end();
    }
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () =>
    resolve({ server, base: `http://127.0.0.1:${server.address().port}/` })));
}

const { chromium } = require('playwright');
const site = await makeSite();
const isolated = await serve(site, true);
const plain = await serve(site, false);
const browser = await chromium.launch({
  args: ['--enable-unsafe-webgpu', '--enable-unsafe-swiftshader', '--use-webgpu-adapter=swiftshader'],
});

try {
  const context = await browser.newContext({ acceptDownloads: true });
  const page = await context.newPage();
  page.on('console', (m) => process.env.VERBOSE && console.log(`[page] ${m.text()}`));
  page.on('pageerror', (e) => check(false, `page error: ${e.message}`));
  page.on('dialog', (d) => d.accept());

  // Without cross-origin isolation the page explains instead of offering to start.
  await page.goto(plain.base);
  await page.waitForSelector('#unsupported:not([hidden])');
  check((await page.textContent('#unsupported-list')).includes('Cross-origin isolation'),
    'unisolated page lists the missing requirement');

  await page.goto(isolated.base);
  await page.waitForSelector('#disc:not([hidden])');
  check(true, 'isolated page offers the disc picker');

  // Image checks.
  const verdict = (bytes) => page.evaluate(async (b) =>
    window.duskShell.checkImage(new File([new Uint8Array(b)], 'x.iso')), [...bytes.subarray(0, 0x1000)]);
  check(await verdict(syntheticImage(0x1000)) === null, 'accepts a GameCube Twilight Princess image');
  check((await verdict(syntheticImage(0x1000, { gcn: false, wii: true })))?.includes('Wii'), 'rejects Wii images');
  check((await verdict(syntheticImage(0x1000, { gcn: false })))?.includes('Convert'), 'rejects compressed/other images');
  check((await verdict(syntheticImage(0x1000, { gameId: 'GALE01' })))?.includes('not Twilight Princess'), 'rejects other games');

  const size = 3 * 1024 * 1024 + 77;
  const image = syntheticImage(size);
  const expectReads = async (label) => {
    const result = await page.waitForFunction(() => window.fakeModuleResult, null, { timeout: 60_000 })
      .then((h) => h.jsonValue());
    check(result.error === undefined, `${label}: no error (${result.error ?? ''})`);
    check(result.size === size, `${label}: worker published the image size`);
    check(result.name === 'Synthetic Disc.iso', `${label}: worker published the image name`);
    check(JSON.stringify(result.args) === JSON.stringify(['--dvd', 'webdisc://Synthetic Disc.iso']), `${label}: main() gets the disc location`);
    for (const r of result.reads) {
      const expected = Math.max(0, Math.min(r.length, size - r.offset));
      const bytesOk = r.result === expected &&
        r.bytes.every((b, i) => b === image[r.offset + i]);
      check(bytesOk, `${label}: read ${r.length} bytes at ${r.offset}`);
    }
  };

  // Session mode: read straight from the picked file.
  await page.setInputFiles('#file', { name: 'Synthetic Disc.iso', mimeType: 'application/octet-stream', buffer: Buffer.from(image) });
  await page.waitForSelector('#picked:not([hidden])');
  await page.check('input[value=session]');
  await page.click('#play-picked');
  await expectReads('session mode');

  // Copy mode: copied into OPFS, then offered again on the next visit.
  await page.goto(isolated.base);
  await page.waitForSelector('#disc:not([hidden])');
  check(await page.isHidden('#stored'), 'session mode stores nothing');
  await page.setInputFiles('#file', { name: 'Synthetic Disc.iso', mimeType: 'application/octet-stream', buffer: Buffer.from(image) });
  await page.waitForSelector('#picked:not([hidden])');
  check(!(await page.isDisabled('input[value=copy]')), 'copy mode is offered when the quota allows');
  await page.check('input[value=copy]');
  await page.click('#play-picked');
  await expectReads('copy mode');

  await page.goto(isolated.base);
  await page.waitForSelector('#stored:not([hidden])');
  check((await page.textContent('#stored-name')) === 'Synthetic Disc.iso', 'stored disc is offered on the next visit');
  await page.click('#play-stored');
  await expectReads('stored disc');

  // Save backup and restore round trip; the disc image stays out of backups.
  await page.goto(isolated.base);
  await page.waitForSelector('#saves:not([hidden])');
  await page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    const dir = await (await root.getDirectoryHandle('TwilitRealm', { create: true })).getDirectoryHandle('Dusklight', { create: true });
    for (const [name, text] of [['config.json', '{"a":1}'], ['MemoryCardA.USA.raw', 'card-bytes']]) {
      const w = await (await dir.getFileHandle(name, { create: true })).createWritable();
      await w.write(text);
      await w.close();
    }
  });
  const [backup] = await Promise.all([page.waitForEvent('download'), page.click('#export-saves')]);
  const zipPath = path.join(site, 'backup.zip');
  await backup.saveAs(zipPath);
  const zip = await readFile(zipPath);
  check(zip.includes('TwilitRealm/Dusklight/config.json') && !zip.includes('dusklight-disc'),
    'backup has the saves and not the disc image');
  await page.evaluate(async () => (await navigator.storage.getDirectory()).removeEntry('TwilitRealm', { recursive: true }));
  await page.setInputFiles('#import-saves', zipPath);
  const readCard = () => page.evaluate(async () => {
    try {
      const root = await navigator.storage.getDirectory();
      const dir = await (await root.getDirectoryHandle('TwilitRealm')).getDirectoryHandle('Dusklight');
      return await (await (await dir.getFileHandle('MemoryCardA.USA.raw')).getFile()).text();
    } catch {
      return null;
    }
  });
  let restored = null;
  for (let i = 0; i < 100 && restored === null; ++i) {
    restored = await readCard();
    if (restored === null) await new Promise((r) => setTimeout(r, 100));
  }
  check(restored === 'card-bytes', 'restore brings the saves back');

  // The audio worklet plays the ring, counts underruns, and hands the space back.
  const audio = await page.evaluate(async () => {
    const memory = new WebAssembly.Memory({ initial: 4, maximum: 8, shared: true });
    const header = 1024;
    const data = 4096;
    const frames = 512; // ring capacity
    const channels = 2;
    const ring = new Uint32Array(memory.buffer, header, 4);
    const samples = new Float32Array(memory.buffer, data, frames * channels);
    // Start near 2^32 so the counters wrap; queue 300 frames of a ramp (L = i, R = -i) / 1000.
    ring[0] = 0xffffff00;
    ring[1] = 0xffffff00 + 300;
    for (let i = 0; i < 300; ++i) {
      const base = ((0xffffff00 + i) % frames) * channels;
      samples[base] = i / 1000;
      samples[base + 1] = -i / 1000;
    }
    const ctx = new OfflineAudioContext({ numberOfChannels: 2, length: 512, sampleRate: 48000 });
    await ctx.audioWorklet.addModule('audio-worklet.js');
    const node = new AudioWorkletNode(ctx, 'dusk-audio', {
      numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
      processorOptions: { memory, header, data, frames, channels },
    });
    node.connect(ctx.destination);
    const rendered = await ctx.startRendering();
    const left = rendered.getChannelData(0);
    const right = rendered.getChannelData(1);
    let ok = true;
    for (let i = 0; i < 512; ++i) {
      const want = i < 300 ? i / 1000 : 0;
      ok &&= Math.abs(left[i] - want) < 1e-6 && Math.abs(right[i] + want) < 1e-6;
    }
    return { ok, read: ring[0], underruns: ring[3] };
  });
  check(audio.ok, 'audio worklet plays the queued samples, then silence');
  check(audio.read === ((0xffffff00 + 300) >>> 0), 'audio worklet consumes exactly what was queued, across the wrap');
  check(audio.underruns === 212, 'audio worklet counts the frames it had to fill');

  await page.click('#forget-stored');
  await page.goto(isolated.base);
  await page.waitForSelector('#disc:not([hidden])');
  check(await page.isHidden('#stored'), 'a removed disc is no longer offered');
} finally {
  await browser.close();
  isolated.server.close();
  plain.server.close();
  await rm(site, { recursive: true, force: true });
}

if (failures) {
  console.error(`${failures} check(s) failed`);
  process.exit(1);
}
console.log('shell: all tests passed');
