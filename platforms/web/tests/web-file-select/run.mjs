// Builds web_file_select_main.cpp against a web build's borealis and SDL libraries (em++ on PATH)
// and runs it in headless Chromium:
//
//   node platforms/web/tests/web-file-select/run.mjs build/web
//
// Checks the browser backend of borealis::file_select, behind the game's import and export
// buttons: a file chosen in the browser's chooser reaches the game, an export becomes a download,
// and a chooser the browser refuses to open (no recent click or key press) fails instead of
// hanging. Needs Playwright's Chromium.
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
const cache = await readFile(path.join(buildDir, 'CMakeCache.txt'), 'utf8');
const sdlSource = /^SDL3_SOURCE_DIR:STATIC=(.*)$/m.exec(cache)?.[1];
if (!sdlSource) throw new Error(`no SDL3_SOURCE_DIR in ${buildDir}/CMakeCache.txt`);
const out = await mkdtemp(path.join(os.tmpdir(), 'dusk-web-file-select-'));

const lib = (name) => path.join(buildDir, 'extern/borealis', `lib${name}.a`);
execFileSync('em++', [
  '-std=c++20', '-O1', '-pthread', '-fwasm-exceptions', '-sSUPPORT_LONGJMP=wasm',
  `-I${root}/extern/borealis/include`, `-I${sdlSource}/include`, path.join(here, 'web_file_select_main.cpp'),
  lib('borealis_file_select'), lib('borealis_io'), lib('borealis_log'),
  path.join(buildDir, '_deps/sdl-build/libSDL3.a'),
  '-sJSPI', '-sWASMFS', '-sINVOKE_RUN=0', "-sEXPORTED_RUNTIME_METHODS=['callMain']", '-sPTHREAD_POOL_SIZE=2',
  '-sENVIRONMENT=web,worker', '-sINITIAL_MEMORY=128MB', '-o', path.join(out, 'web_file_select.js'),
], { stdio: 'inherit' });
await writeFile(path.join(out, 'index.html'), `<!doctype html><meta charset="utf-8">
<button id="start">Start</button>
<script>
  var Module = { print: (t) => console.log(t), printErr: (t) => console.error(t),
                 onRuntimeInitialized: () => console.log('ready') };
  // "open" starts from the button's click, as the game's buttons follow a click or key press;
  // "refused" starts by itself, without one.
  document.getElementById('start').onclick = () => Module.callMain(['open']);
  if (location.search === '?refused') Module.onRuntimeInitialized = () => Module.callMain(['refused']);
</script>
<script src="web_file_select.js"></script>`);

const server = createIsolatedServer(out);
await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
const { chromium } = require('playwright');
const browser = await chromium.launch();
let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? 'ok' : 'FAIL'}: ${what}`);
  if (!ok) failures++;
};

async function open(page, query = '') {
  const lines = [];
  const ready = new Promise((resolve) => page.on('console', (m) => {
    lines.push(m.text());
    if (m.text() === 'ready') resolve();
  }));
  await page.goto(`http://127.0.0.1:${server.address().port}/${query}`);
  if (!query) await ready;
  return lines;
}

const waitFor = (lines, pattern) => new Promise((resolve, reject) => {
  const started = Date.now();
  const poll = () => {
    const line = lines.find((l) => pattern.test(l));
    if (line) resolve(line);
    else if (Date.now() - started > 30_000) reject(new Error(`timed out waiting for ${pattern}`));
    else setTimeout(poll, 50);
  };
  poll();
});

try {
  {
    const page = await browser.newPage();
    const lines = await open(page);
    const [chooser] = await Promise.all([page.waitForEvent('filechooser'), page.click('#start')]);
    check(await chooser.element().evaluate((input) => input.accept) === '.gci,.raw', 'the chooser offers the filter\'s extensions');
    const [download] = await Promise.all([
      page.waitForEvent('download'),
      chooser.setFiles({ name: 'Link.gci', mimeType: 'application/octet-stream', buffer: Buffer.from('save data') }),
    ]);
    check((await waitFor(lines, /^open:/)) === 'open: Selected 1', 'the chosen file is selected');
    check((await waitFor(lines, /^content:/)) === 'content: save data', 'the game reads what was chosen');
    check(download.suggestedFilename() === 'exported.gci', 'an export downloads under the suggested name');
    check((await readFile(await download.path(), 'utf8')) === 'save data', 'the download has the exported contents');
    check((await waitFor(lines, /^export:/)) === 'export: Selected', 'the export completes');
    check(lines.includes('capabilities: open=1 folder=0 export=1'), 'folders are not offered');
    await page.close();
  }
  {
    const page = await browser.newPage();
    // (Not page.evaluate: Playwright runs it as if the user had clicked.)
    const lines = await open(page, '?refused');
    const refused = await waitFor(lines, /^open:/);
    check(refused === 'open: Failed 0', `a refused chooser fails instead of hanging (${refused})`);
    check((await waitFor(lines, /^busy after open:/)) === 'busy after open: 0', 'and does not stay busy');
    await page.close();
  }
} finally {
  await browser.close();
  server.close();
  await rm(out, { recursive: true, force: true });
}
console.log(failures ? `web-file-select: ${failures} check(s) failed` : 'web-file-select: all tests passed');
process.exit(failures ? 1 : 0);
