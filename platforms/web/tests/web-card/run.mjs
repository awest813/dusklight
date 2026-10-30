// Builds web_card_main.cpp against a web build's aurora and SDL libraries (em++ on PATH) and runs
// it in headless Chromium:
//
//   node platforms/web/tests/web-card/run.mjs build/web
//
// Saves the way the game does through aurora's memory card on the browser's storage, for both
// save file types (GCI folder and raw card image), then loads the save in a new page of the same
// profile, as after a reload, and deletes it. Prints how long mounting and saving took. Needs
// Playwright's Chromium.
import { execFileSync } from 'node:child_process';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';
import { createIsolatedServer } from '../../shell/serve.mjs';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../../../..');
const buildDir = path.resolve(process.argv[2] ?? 'build/web');
await readFile(path.join(buildDir, 'CMakeCache.txt')); // a configured web build
const out = await mkdtemp(path.join(os.tmpdir(), 'dusk-web-card-'));

const aurora = (name) => path.join(buildDir, 'extern/aurora', `lib${name}.a`);
execFileSync('em++', [
  '-std=c++20', '-O1', '-pthread', '-fwasm-exceptions', '-sSUPPORT_LONGJMP=wasm', '-DAURORA', '-DTARGET_PC',
  `-I${root}/extern/aurora/include`, path.join(here, 'web_card_main.cpp'),
  aurora('aurora_card'), aurora('aurora_core'),
  path.join(buildDir, '_deps/sdl-build/libSDL3.a'), path.join(buildDir, '_deps/fmt-build/libfmt.a'),
  '-sWASMFS', '-sPTHREAD_POOL_SIZE=2', '-sENVIRONMENT=web,worker', '-sEXIT_RUNTIME=1', '-sINITIAL_MEMORY=128MB',
  '-sSTACK_SIZE=8MB', // as the game (WebOptions.cmake): the card code keeps whole blocks on the stack
  '-o', path.join(out, 'web_card.js'),
], { stdio: 'inherit' });
await writeFile(path.join(out, 'index.html'), `<!doctype html><meta charset="utf-8">
<script>
  var Module = { arguments: new URLSearchParams(location.search).get('run').split(','),
                 print: (t) => console.log(t), printErr: (t) => console.error(t),
                 onExit: (s) => console.log('exit=' + s), onAbort: (w) => console.log('abort=' + w) };
</script>
<script src="web_card.js"></script>`);

const server = createIsolatedServer(out);
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const { chromium } = require('playwright');
const browser = await chromium.launch();
const context = await browser.newContext();

async function run(type, phase) {
  const page = await context.newPage();
  const done = new Promise((resolve) => {
    page.on('console', (m) => {
      const text = m.text();
      if (!text.startsWith('[')) console.log(`[${type} ${phase}] ${text}`);
      if (text.startsWith('exit=') || text.startsWith('abort=')) resolve(text);
    });
    page.on('pageerror', (e) => resolve(`page error: ${e.message}`));
  });
  await page.goto(`http://127.0.0.1:${server.address().port}/?run=${type},${phase}`);
  const result = await Promise.race([done, new Promise((r) => setTimeout(() => r('timeout'), 120_000))]);
  await page.close();
  return result;
}

let status = 1;
try {
  const results = [];
  for (const type of ['gci', 'raw']) {
    results.push(await run(type, 'write'), await run(type, 'verify'));
  }
  status = results.every((r) => r === 'exit=0') ? 0 : 1;
  if (status) console.error(`web-card: FAIL (${results.join(', ')})`);
  else console.log('web-card: all tests passed');
} finally {
  await browser.close();
  server.close();
  await rm(out, { recursive: true, force: true });
}
process.exit(status);
