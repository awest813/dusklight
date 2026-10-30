// Builds web_storage_main.cpp with Emscripten (em++ on PATH) and runs it in headless Chromium:
//
//   node platforms/web/tests/web-storage/run.mjs
//
// Checks that the file operations behind saves and settings (exclusive create, rename over a file,
// directory renames, remove_all, in-place updates) work on WasmFS's OPFS backend, and that what
// they wrote is still there in a new page of the same profile, as after a reload. Needs
// Playwright's Chromium.
import { execFileSync } from 'node:child_process';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';
import { createIsolatedServer } from '../../shell/serve.mjs';

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const out = await mkdtemp(path.join(os.tmpdir(), 'dusk-web-storage-'));

execFileSync('em++', [
  '-std=c++20', '-O1', '-pthread', '-fwasm-exceptions', path.join(here, 'web_storage_main.cpp'),
  '-sWASMFS', '-sPTHREAD_POOL_SIZE=2', '-sENVIRONMENT=web,worker', '-sEXIT_RUNTIME=1',
  '-sINITIAL_MEMORY=128MB', '-o', path.join(out, 'web_storage.js'),
], { stdio: 'inherit' });
await writeFile(path.join(out, 'index.html'), `<!doctype html><meta charset="utf-8">
<script>
  var Module = { arguments: [new URLSearchParams(location.search).get('phase')],
                 print: (t) => console.log(t), printErr: (t) => console.error(t),
                 onExit: (s) => console.log('exit=' + s), onAbort: (w) => console.log('abort=' + w) };
</script>
<script src="web_storage.js"></script>`);

const server = createIsolatedServer(out);
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const { chromium } = require('playwright');
const browser = await chromium.launch();
const context = await browser.newContext();

async function run(phase) {
  const page = await context.newPage();
  const done = new Promise((resolve) => {
    page.on('console', (m) => {
      const text = m.text();
      console.log(`[${phase}] ${text}`);
      if (text.startsWith('exit=') || text.startsWith('abort=')) resolve(text);
    });
    page.on('pageerror', (e) => resolve(`page error: ${e.message}`));
  });
  await page.goto(`http://127.0.0.1:${server.address().port}/?phase=${phase}`);
  const result = await Promise.race([done, new Promise((r) => setTimeout(() => r('timeout'), 60_000))]);
  await page.close();
  return result;
}

let status = 1;
try {
  const results = [await run('write'), await run('verify')];
  status = results.every((r) => r === 'exit=0') ? 0 : 1;
  if (status) console.error(`web-storage: FAIL (${results.join(', ')})`);
  else console.log('web-storage: all tests passed');
} finally {
  await browser.close();
  server.close();
  await rm(out, { recursive: true, force: true });
}
process.exit(status);
