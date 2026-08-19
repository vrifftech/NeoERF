# Building NeoERF on macOS

NeoERF has separate native builds for Apple Silicon and Intel.

| Target | Architecture | GitHub runner | Artifact suffix |
|---|---|---|---|
| Apple Silicon | `arm64` | `macos-15` | `macos-arm64` |
| Intel Mac | `x86_64` | `macos-15-intel` | `macos-x86_64` |

The first implementation deliberately emits two native artifacts rather than merging Homebrew and wxWidgets dependencies into one universal bundle.

## Prerequisites

Install the Xcode Command Line Tools and Homebrew dependencies:

```bash
xcode-select --install
brew install cmake ninja wxwidgets
```

Check out the repositories as siblings:

```text
NeoTools/
├── neoshared/
└── NeoERF/
```

## Native build and package

Run on a Mac whose native architecture matches the desired artifact:

```bash
cd NeoERF
bash ./scripts/build-macos.sh --clean
```

The script configures with Ninja, requires the wxWidgets GUI, builds the GUI and CLI, runs existing CTest tests, installs the `.app`, copies and rewrites non-system dynamic dependencies, verifies architecture and linkage, performs ad-hoc signing, and creates:

```text
dist/NeoERF-<version>-macos-<arm64|x86_64>.zip
dist/NeoERF-<version>-macos-<arm64|x86_64>.zip.sha256
```

The installed CLI remains under `stage-macos-<arch>/bin/`; the distributable ZIP contains the Finder-launchable application bundle.

## Explicit options

```bash
bash ./scripts/build-macos.sh \
  --arch "$(uname -m)" \
  --deployment-target 15.0 \
  --neoshared-root ../neoshared \
  --jobs 4 \
  --clean
```

Cross-architecture packaging is rejected. Build `arm64` on Apple Silicon and `x86_64` on Intel.

The default deployment target is macOS 15.0 because the hosted workflow installs current Homebrew bottles on macOS 15. A lower requested target does not guarantee compatibility when a bundled dependency requires a newer system.

## GitHub Actions

`.github/workflows/macos.yml` runs both native jobs and checks out:

```text
$GITHUB_WORKSPACE/tool
$GITHUB_WORKSPACE/neoshared
```

Select the shared revision through the repository variable `NEOSHARED_REF`, the manual workflow input, or a `neoshared-updated` repository-dispatch payload. Blank selection falls back to `main`.

## Signing and notarization

The generated CI bundle is ad-hoc signed so its internal code signature can be verified. It is not Developer-ID signed or notarized. Public distribution without normal Gatekeeper warnings requires a later signing/notarization workflow and Apple Developer credentials.
