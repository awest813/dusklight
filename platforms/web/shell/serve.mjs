// Serves a web build locally with the cross-origin isolation headers wasm threads need:
//
//   node serve.mjs [directory] [port]
//
// then open http://localhost:<port>/ in Chrome or Edge. The tests (platforms/web/tests) import
// createIsolatedServer from here.
import { createServer } from 'node:http';
import { createReadStream } from 'node:fs';
import { stat } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const types = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.css': 'text/css',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
  '.png': 'image/png',
};

// An http.Server for `root`. With `isolated` false, the COOP/COEP headers are left out, which
// the tests use to check the page's unsupported-browser message.
export function createIsolatedServer(root, { isolated = true } = {}) {
  root = path.resolve(root);
  return createServer(async (req, res) => {
    if (isolated) {
      res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
      res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    }
    try {
      let file = path.join(root, path.normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)));
      if (file !== root && !file.startsWith(root + path.sep)) {
        res.writeHead(403).end();
        return;
      }
      if ((await stat(file)).isDirectory()) file = path.join(file, 'index.html');
      const info = await stat(file);
      // Revalidated on every load, so a rebuild is picked up, but an unchanged file comes from the
      // browser's cache, which also lets it reuse its compiled copy of the wasm module.
      const etag = `"${info.size.toString(16)}-${Math.floor(info.mtimeMs).toString(16)}"`;
      res.setHeader('Cache-Control', 'no-cache');
      res.setHeader('ETag', etag);
      if (req.headers['if-none-match'] === etag) {
        res.writeHead(304).end();
        return;
      }
      res.writeHead(200, {
        'Content-Type': types[path.extname(file)] ?? 'application/octet-stream',
        'Content-Length': info.size,
      });
      createReadStream(file).on('error', () => res.destroy()).pipe(res);
    } catch {
      // Missing files and malformed URLs alike.
      if (!res.headersSent) res.writeHead(404);
      res.end();
    }
  });
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const root = path.resolve(process.argv[2] ?? path.dirname(fileURLToPath(import.meta.url)));
  const port = Number(process.argv[3] ?? 8080);
  createIsolatedServer(root).listen(port, () => console.log(`Serving ${root} at http://localhost:${port}/`));
}
