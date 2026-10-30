// Browser test of the web page (platforms/web/shell): the disc picker in both storage modes, the
// disc worker serving reads through the shared-memory channel, and save backup/restore.
//
//   node platforms/web/tests/shell-test.mjs
//
// Runs in headless Chromium through Playwright (`npm i playwright`, or a preinstalled copy). No
// wasm build is needed: fake-module.js stands in for dusklight.js with the same contract the page
// relies on (Module.wasmMemory, onRuntimeInitialized, _dusk_web_disc_channel, callMain), and reads
// the disc from a worker exactly as a game thread does (src/dusk/web/disc.cpp).
import { cp, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createIsolatedServer } from '../shell/serve.mjs';

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
    .replace('@DUSK_WEB_VERSION@', 'test'));
  await cp(path.join(here, 'fake-module.js'), path.join(dir, 'fake-module.js'));
  return dir;
}

function serve(root, isolated) {
  const server = createIsolatedServer(root, { isolated });
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

  // A browser without WebGPU gets an explanation instead of the disc picker.
  {
    const noGpu = await context.newPage();
    await noGpu.addInitScript(() => { delete Navigator.prototype.gpu; });
    await noGpu.goto(isolated.base);
    await noGpu.waitForSelector('#unsupported:not([hidden])');
    check((await noGpu.textContent('#unsupported-list')).includes('WebGPU'), 'unsupported browser is told what is missing');
    await noGpu.close();
  }

  // WebGPU without a usable adapter (a blocklisted GPU or driver), or without immediate data (an
  // older Chrome), is also caught before a disc is chosen.
  for (const [what, stub, expected] of [
    ['no adapter', () => { GPU.prototype.requestAdapter = async () => null; }, 'graphics adapter'],
    ['no immediate data', () => { delete GPURenderPassEncoder.prototype.setImmediates; }, 'immediate data'],
  ]) {
    const limited = await context.newPage();
    await limited.addInitScript(stub);
    await limited.goto(isolated.base);
    await limited.waitForSelector('#unsupported:not([hidden])');
    check((await limited.textContent('#unsupported-list')).includes(expected), `${what}: the player is told before choosing a disc`);
    await limited.close();
  }

  // Failures with a likely cause come with an explanation as well as the log.
  for (const [what, error, expected] of [
    ['a lost device', 'FATAL: Device lost: GPU process exited', 'Reloading usually fixes this'],
    ['running out of memory', 'Aborted(Cannot enlarge memory arrays to size 4294967296 bytes (OOM).)', 'enough memory'],
  ]) {
    const failed = await context.newPage();
    await failed.goto(isolated.base);
    await failed.waitForSelector('#disc:not([hidden])');
    await failed.evaluate((message) => setTimeout(() => { throw new Error(message); }), error);
    await failed.waitForSelector('#fatal:not([hidden])');
    check((await failed.textContent('#fatal-hint')).includes(expected), `${what} is explained`);
    await failed.close();
  }

  // A host that doesn't send the isolation headers: coi-sw.js adds them after one reload.
  {
    const plainContext = await browser.newContext();
    const viaWorker = await plainContext.newPage();
    await viaWorker.goto(plain.base);
    await viaWorker.waitForSelector('#disc:not([hidden])', { timeout: 30_000 });
    check(await viaWorker.evaluate(() => crossOriginIsolated), 'the service worker isolates a page served without headers');
    await plainContext.close();
  }

  await page.goto(isolated.base);
  await page.waitForSelector('#disc:not([hidden])');
  check(true, 'isolated page offers the disc picker');

  // A second tab is told the game is already open; once the first is gone, it can start.
  {
    const second = await context.newPage();
    await second.goto(isolated.base);
    await second.waitForSelector('#elsewhere:not([hidden])', { timeout: 10_000 });
    check(await second.isHidden('#disc'), 'a second tab does not offer to start');
    await page.goto('about:blank');
    await second.reload();
    await second.waitForSelector('#disc:not([hidden])', { timeout: 10_000 });
    check(true, 'the second tab can start once the first is closed');
    await second.close();
    await page.goto(isolated.base);
    await page.waitForSelector('#disc:not([hidden])');
  }

  // The game's file dialogs: when the browser refuses to open its chooser (no recent click or key
  // press, as after a gamepad button), the page asks for a click. (Started from a timer, and waited
  // for without polling: Playwright's calls into the page count as clicks for a few seconds.)
  for (const proceed of [true, false]) {
    await page.evaluate(() => setTimeout(() => {
      window.chooseResult = borealisFileSelect.open({ accept: '.gci', multiple: false }).then((files) => files.map((f) => f.name));
    }, 6000));
    await page.waitForTimeout(6500);
    check(await page.isVisible('#choose'), 'a refused file chooser makes the page ask for a click');
    if (proceed) {
      const [chooser] = await Promise.all([page.waitForEvent('filechooser'), page.click('#choose-file')]);
      await chooser.setFiles({ name: 'Link.gci', mimeType: 'application/octet-stream', buffer: Buffer.from('save') });
      check(JSON.stringify(await page.evaluate(() => window.chooseResult)) === '["Link.gci"]',
        'the chooser opens after that click');
    } else {
      await page.click('#choose-cancel');
      check(JSON.stringify(await page.evaluate(() => window.chooseResult)) === '[]', 'and can be canceled');
    }
    check(await page.isHidden('#choose'), 'the prompt closes');
  }

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
  check((await page.textContent('#stored-size')).includes('MB'), 'its size is shown');
  await page.click('#play-stored');
  await expectReads('stored disc');

  // Save backup and restore round trip; the disc image and logs stay out of backups.
  await page.goto(isolated.base);
  await page.waitForSelector('#saves:not([hidden])');
  // Headless Chromium doesn't grant persistent storage, so players are told what that means.
  check(await page.isVisible('#not-persisted'), 'the page says when storage is not persistent');
  await page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    const dir = await (await root.getDirectoryHandle('TwilitRealm', { create: true })).getDirectoryHandle('Dusklight', { create: true });
    const logs = await dir.getDirectoryHandle('logs', { create: true });
    for (const [where, name, text] of [[dir, 'config.json', '{"a":1}'], [dir, 'MemoryCardA.USA.raw', 'card-bytes'],
      [logs, 'dusklight-20261030-000000.log', 'log']]) {
      const w = await (await where.getFileHandle(name, { create: true })).createWritable();
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
  check(!zip.includes('/logs/'), 'backup leaves out the logs');
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
  await page.waitForSelector('#stored', { state: 'hidden', timeout: 10_000 });
  await page.goto(isolated.base);
  await page.waitForSelector('#disc:not([hidden])');
  check(await page.isHidden('#stored'), 'a removed disc is no longer offered');

  // A copy interrupted by closing the tab leaves part of an image that was never recorded; it is
  // removed on the next visit instead of taking up storage unseen.
  await page.evaluate(async () => {
    const dir = await (await navigator.storage.getDirectory()).getDirectoryHandle('dusklight-disc', { create: true });
    const w = await (await dir.getFileHandle('Partial.iso', { create: true })).createWritable();
    await w.write(new Uint8Array(4096));
    await w.close();
  });
  await page.reload();
  await page.waitForSelector('#disc:not([hidden])');
  check(await page.evaluate(async () => {
    try {
      await (await navigator.storage.getDirectory()).getDirectoryHandle('dusklight-disc');
      return false;
    } catch {
      return true;
    }
  }), 'an interrupted copy is cleaned up');
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
