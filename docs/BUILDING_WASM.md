# Building NeoERF for WebAssembly and GitHub Pages

The browser edition compiles the existing C++/wxWidgets frontend with
Emscripten and the PCBJam wxWidgets DOM/WebAssembly port. It is not a separate
JavaScript rewrite.

## Pinned toolchain

| Dependency | Pin |
|---|---|
| Emscripten SDK | `4.0.2` |
| wxWidgets-WASM | `PCBJam/wxWidgets` commit `bca69b9fddc88adec57b05e6809467ef9f5158c8` |
| WebAssembly target | `wasm32` |
| Threads | Disabled |
| Exception model | Emscripten JavaScript exceptions |

The build remains single-threaded so the site can run on ordinary GitHub Pages
without COOP/COEP response headers.

## Local prerequisites

On Ubuntu or another Debian-family system:

```bash
sudo apt-get update
sudo apt-get install -y autoconf automake libtool make ninja-build cmake python3 git
```

Check out `NeoERF` and `neoshared` as siblings, then run:

```bash
cd NeoERF
bash ./scripts/build-wasm.sh --clean
```

The first build downloads and compiles the pinned SDK and wxWidgets port under
`../.neo-wasm-deps`. Override that location with `--deps-root` or
`NEO_WASM_DEPS_ROOT`.

Output is written to:

```text
dist-wasm/
├── index.html
├── neoerf.js
├── neoerf.wasm
├── neoerf.svg
├── site.webmanifest
└── THIRD_PARTY_NOTICES.txt
```

Serve the directory over HTTP for local use:

```bash
python3 -m http.server 8000 --directory dist-wasm
```

Then open `http://localhost:8000/`. Loading from a `file://` URL is not
supported.

## Browser behavior

- The existing wxWidgets menus, dialogs, grids, trees, notebooks, and custom
  drawing code are used through the DOM port.
- File dialogs import explicit files into the Emscripten virtual filesystem.
- Extract, Save, and Export prepare a conspicuous **Download
  &lt;filename&gt;** action above the editor. Click that normal browser link to
  complete the transfer. The application does not open a native Save File
  picker from inside wxWidgets-WASM event dispatch.
- Non-path preferences use the port's wxConfig/localStorage backend. IDBFS is mounted for browser-owned files, while imported host-file paths are intentionally not persisted.
- Automatic installed-game discovery is unavailable in the browser. Use the
  normal Open command and explicitly select required files.
- The browser build does not launch Finder, Explorer, another NeoTool, or an
  unrestricted native process.
- Package-aware **Write to INI** is disabled in the browser build because the
  selected INI and every companion payload must be updated together. Patcher
  **Fragment** preview, clipboard copy, and download remain available.
- Writable-directory and directory-wide operations are desktop-only. NeoERF's
  multi-resource extraction is therefore disabled in the browser build; select
  one archive resource and use **Extract** for an individual save transaction.
- Large resources remain constrained by browser memory. The build permits
  memory growth up to 2 GiB but does not guarantee every browser can supply it.

## GitHub Pages

`.github/workflows/pages.yml` builds on Ubuntu, checks out `neoshared` beside
the application, validates the static output, and deploys it with the official
GitHub Pages actions. Set the repository variable `NEOSHARED_REF` to the exact
published shared tag or commit. The workflow also accepts a manual input and a
`neoshared-updated` repository-dispatch payload.

In repository **Settings → Pages**, select **GitHub Actions** as the source.
Pull requests compile and validate the browser build but do not deploy it.

## Licensing boundary

The WebAssembly-specific files in the selected wxWidgets port identify LGPL
v2 terms rather than the normal wxWidgets exception. The generated site
contains the exact source pin and notice. Confirm the repository's overall
licensing and redistribution obligations before making a public Pages release.
