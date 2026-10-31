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
3. Click **Play**. The first start downloads about 13 MB, and the game may take a moment to load.

Click into the game once if you don't hear sound; browsers only start audio after you interact
with the page.

To use a different disc later, choose **Select disc image** in the game's menu, or remove the stored
disc on the page.

## Controls

- **Controllers:** connect one, then **press any button on it**. Browsers only report a controller
  once it has been used on the page.
- **Keyboard:** works from the start, as a GameCube controller on Port 1:

  | GameCube        | Keys              | GameCube | Keys       |
  | --------------- | ----------------- | -------- | ---------- |
  | Control stick   | W A S D           | A        | Space      |
  | C-stick         | Arrow keys        | B        | E          |
  | L               | Left Shift        | X        | Q          |
  | R               | C                 | Y        | F          |
  | Z               | Z                 | Start    | Enter      |
  | D-pad           | 1 2 3 4 (up, down, left, right) |  |      |

  Change it in **Settings → Input → Configure Inputs**; **Restore Default Controls** brings it
  back. A controller on Port 1 works alongside it. The layout avoids Ctrl, Alt and Cmd, since
  shortcuts such as Ctrl+W would close the tab.
- **F1** opens the Dusklight menu (settings, quit). The menus also work with the mouse.
- **Fullscreen:** press **F11**, or use **Toggle Fullscreen** in **Settings → Video**. **Esc**
  leaves it. The game starts in a window each visit.

## Saves and settings

Saves and settings are kept in your browser's storage for this site, so they are still there next
time. Clearing the site's data (or your browser's history, with "cookies and site data") deletes
them, so **back them up** with the page's **Back up** button. **Restore a backup** brings a backup
back into this browser, for example after clearing data or on another computer. Backups hold your
saves and settings, not the disc image or logs.

Individual saves can also be moved in and out with the game's **Save Manager** (Settings → Prelaunch
on the main menu). **Import Save** opens the browser's file chooser, and exports are saved where
your browser saves downloads. If you used a controller, the page may first ask you to click
**Choose file…**, since browsers only open their file chooser after a click or key press.

- **From the desktop version or Dolphin:** export the save there (a `.gci` or `.dusksave` file,
  or a Dolphin memory card `.raw`), then use **Import Save** in the browser.
- **To the desktop version:** use **Export** in the browser's Save Manager, then import the file
  there.
- **Save File Type:** keep the default, **GCI Folder**, in the browser. Saving takes a few
  hundredths of a second, against about half a second with **Card Image**, since a card image
  is copied whole on every write.

A backup that was unpacked and zipped again (for example with Finder or Windows Explorer) can still
be restored. Restore only reads Dusklight's own folder from the zip, and checks every file before
writing any.

Unless the browser has agreed to keep the site's data (the page tells you if it hasn't), it may
also delete it by itself when your disk is nearly full. Bookmarking the page can help.

Only one tab can run Dusklight at a time.

## Troubleshooting

- **"This browser can't run Dusklight":** update Chrome or Edge. The message lists what's missing.
  On some computers, WebGPU is turned off for the graphics driver. Check `chrome://gpu`.
- **"Dusklight stopped":** something went wrong in the game. **Download log** saves what happened.
  Please include it when you report the problem. **Reload** goes back to the start page.
- **"The browser could not give the game enough memory":** Dusklight needs about 1 GB. Close other
  tabs or programs and reload.
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

`platforms/web/stage-site.sh build/web <folder>` copies just the files the site needs into a folder
of their own, ready to upload.

### GitHub Pages

`.github/workflows/pages.yml` builds the web target and publishes it to the repository's GitHub
Pages site on every push to `main`. It can also be started by hand from the Actions tab. It first
smoke-tests the published files the way Pages serves them. One-time setup:

1. **Actions tab:** on a fork, GitHub doesn't run workflows until you allow them there.
2. **Settings → Pages → Build and deployment:** set **Source** to **GitHub Actions**.
3. **Run it:** push to `main`, or open **Actions → Deploy web build to GitHub Pages → Run
   workflow**.

The site is then at `https://<owner>.github.io/<repository>/`. The first visit reloads once while
the service worker sets up isolation.
