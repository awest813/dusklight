// Serves a web build locally with the cross-origin isolation headers wasm threads need:
//
//   node serve.mjs [directory] [port]
//
// then open http://localhost:<port>/ in Chrome or Edge.
import { createServer } from 'node:http';
import { createReadStream } from 'node:fs';
import { stat } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(process.argv[2] ?? path.dirname(fileURLToPath(import.meta.url)));
const port = Number(process.argv[3] ?? 8080);
const types = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.css': 'text/css',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
};

createServer(async (req, res) => {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  let file = path.join(root, path.normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname)));
  if (!file.startsWith(root)) {
    res.writeHead(403).end();
    return;
  }
  try {
    if ((await stat(file)).isDirectory()) file = path.join(file, 'index.html');
    const info = await stat(file);
    res.writeHead(200, {
      'Content-Type': types[path.extname(file)] ?? 'application/octet-stream',
      'Content-Length': info.size,
    });
    createReadStream(file).pipe(res);
  } catch {
    res.writeHead(404).end();
  }
}).listen(port, () => console.log(`Serving ${root} at http://localhost:${port}/`));
