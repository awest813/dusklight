# Playing Dusklight in a browser

> [!NOTE]
> The web version is experimental. It has been tested end to end only with a synthetic disc image,
> not with the full game yet. Please report problems with the log the page offers to download.

## What you need

- **A recent Chrome or Edge on a desktop or laptop computer.** The page checks for everything it
  needs (WebGPU, WebAssembly threads and JSPI, and the browser's private file storage) and tells
  you what is missing. Firefox and Safari are not supported yet.
- **Your own copy of Twilight Princess for GameCube, as an uncompressed disc image** (`.iso` or
  `.gcm`).
  - The Wii version is not supported in the browser yet.
  - Compressed images (RVZ, WIA, GCZ, CISO, WBFS) must be converted to ISO first, with
    [Dolphin](https://dolphin-emu.org/) (right-click the game → Convert File…) or
    [nodtool](https://github.com/encounter/nod/releases).
- **Free storage space** in your browser if you want it to keep a copy of the disc (about 1.4 GB).

Dusklight contains no game data. Your disc image is read in your browser and never uploaded.

## Starting the game

1. Open the page, then choose your disc image, or drop it onto the page.
2. Choose where Dusklight reads it from:
   - **Keep a copy in this browser:** the disc is copied into the browser's private storage once,
     and you can play right away on later visits.
   - **Use this file for now:** nothing is copied, and you choose the file again next time.
3. Click **Play**. The first start downloads about 14 MB, and the game may take a moment to load.

Click into the game once if you don't hear sound; browsers only start audio after you interact
with the page.

To use a different disc later, choose **Select disc image** in the game's menu, or remove the stored
disc on the page.

## Saves and settings

Saves and settings are kept in your browser's storage for this site, so they are still there next
time. Clearing the site's data (or your browser's history, with "cookies and site data") deletes
them, so **back them up** with the page's **Back up** button. **Restore a backup** brings a backup
back into this browser, for example after clearing data or on another computer.

Only one tab can run Dusklight at a time.

## Troubleshooting

- **"This browser can't run Dusklight":** update Chrome or Edge. The message lists what's missing.
  On some computers, WebGPU is turned off for the graphics driver. Check `chrome://gpu`.
- **"Dusklight stopped":** something went wrong in the game. **Download log** saves what happened.
  Please include it when you report the problem. **Reload** goes back to the start page.
- **The game is slow:** close other heavy tabs, and make sure the browser uses your graphics card
  (`chrome://gpu`).
- **Your disc is rejected:** check that it is the GameCube version, uncompressed, and Twilight
  Princess. The page says which of these it isn't.

## Hosting the web version

A build (see [building.md](building.md#building)) produces a folder of static files in `build/web`.
Serve them from any web server.

The pages must be **cross-origin isolated** (threads need it), so every response needs these
headers:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

- **Cloudflare Pages and Netlify** read them from the `_headers` file in the build.
- **Hosts that can't set headers,** such as GitHub Pages, still work: the page installs a small
  service worker (`coi-sw.js`) that adds them, and reloads once.
- **Locally,** run `node serve.mjs` in the build folder and open http://localhost:8080/.

Serve over HTTPS (or `localhost`); browsers only allow these features on secure origins. Compression
(gzip or Brotli) roughly halves the download, and most hosts apply it automatically.
