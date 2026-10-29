// Builds web_audio_main.cpp with Emscripten (em++ on PATH) and runs it in headless Chromium:
//
//   node platforms/web/tests/web-audio/run.mjs
//
// Checks the real browser side of the web audio output: the AudioContext and AudioWorklet glue in
// src/dusk/web/audio.cpp and platforms/web/shell/audio-worklet.js, pausing, hidden pages and
// shutdown. Needs Playwright's Chromium.
import { execFileSync } from 'node:child_process';
import { copyFile, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';
import { createIsolatedServer } from '../../shell/serve.mjs';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../../../..');
const out = await mkdtemp(path.join(os.tmpdir(), 'dusk-web-audio-'));

execFileSync('em++', [
  '-std=c++20', '-O1', '-pthread', '-fwasm-exceptions', `-I${root}/src`,
  `${root}/src/dusk/web/audio.cpp`, path.join(here, 'web_audio_main.cpp'),
  '-sJSPI', '-sPTHREAD_POOL_SIZE=2', '-sENVIRONMENT=web,worker', '-sEXIT_RUNTIME=1',
  '-o', path.join(out, 'web_audio.js'),
], { stdio: 'inherit' });
await copyFile(path.join(root, 'platforms/web/shell/audio-worklet.js'), path.join(out, 'audio-worklet.js'));
await writeFile(path.join(out, 'index.html'), `<!doctype html><meta charset="utf-8">
<script>
  var Module = { print: (t) => console.log(t), printErr: (t) => console.error(t),
                 onExit: (s) => console.log('exit=' + s), onAbort: (w) => console.log('abort=' + w) };
</script>
<script src="web_audio.js"></script>`);

const server = createIsolatedServer(out);
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const { chromium } = require('playwright');
const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
// Runs the program once as is, and once in a browser that refuses to open an AudioContext at a
// requested sample rate (as some do), which must fall back to the device's rate.
async function run(refuseSampleRate) {
  const page = await browser.newPage();
  if (refuseSampleRate) {
    await page.addInitScript(() => {
      const Original = AudioContext;
      globalThis.AudioContext = class extends Original {
        constructor(options = {}) {
          if (options.sampleRate !== undefined) throw new DOMException('unsupported rate', 'NotSupportedError');
          super(options);
        }
      };
    });
  }
  const done = new Promise((resolve) => {
    page.on('console', (m) => {
      const text = m.text();
      console.log(`[page] ${text}`);
      if (text.startsWith('exit=') || text.startsWith('abort=')) resolve(text);
    });
    page.on('pageerror', (e) => resolve(`page error: ${e.message}`));
  });
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  const result = await Promise.race([done, new Promise((r) => setTimeout(() => r('timeout'), 60_000))]);
  await page.close();
  return result;
}

let status = 1;
try {
  const results = [await run(false), await run(true)];
  status = results.every((r) => r === 'exit=0') ? 0 : 1;
  if (status) console.error(`web-audio: FAIL (${results.join(', ')})`);
  else console.log('web-audio: all tests passed');
} finally {
  await browser.close();
  server.close();
  await rm(out, { recursive: true, force: true });
}
process.exit(status);
