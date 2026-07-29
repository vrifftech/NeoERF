# NeoERF

[![CI](https://github.com/vrifftech/NeoERF/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoERF/actions/workflows/ci.yml)

NeoERF is a ERF/RIM archive editor

## Build

This repository consumes shared code from the separate `neoshared` repository. Clone the repositories as siblings:

```text
workspace/
  neoshared/
  NeoERF/
```

CMake automatically detects `../neoshared`. For another layout, pass `--neoshared-root /path/to/neoshared` to `build.sh`, `-NeoSharedRoot C:\path\to\neoshared` to `build.ps1`, or set `NEOSHARED_ROOT` directly.


Linux GUI build:

```sh
./scripts/build.sh --wx ON --require-wx ON --jobs "$(nproc)"
```

Linux CLI/core-only build:

```sh
./scripts/build.sh --wx OFF --jobs "$(nproc)"
```

Windows GUI build with static wxWidgets from vcpkg:

```powershell
.\scripts\build.ps1 `
  -Wx ON `
  -RequireWx ON `
  -VcpkgRoot C:\vcpkg `
  -VcpkgTriplet x64-windows-static `
  -Parallel ([Environment]::ProcessorCount)
```

Use `-Wx OFF` on Windows for a CLI/core-only build. The default build directory is `build/`.


## ERF-family game support

NeoERF supports legacy ResRef/type-keyed archives and the filename/hash-keyed Dragon Age containers:

- KotOR/KotOR II and Jade Empire ERF/RIM/MOD-family archives remain the default behavior. Jade Empire has several resource-type names that differ from KotOR for the same numeric type IDs. NeoERF keeps KotOR names as the default and adds an explicit Jade Empire profile for listing, extraction, deletion, and authoring:
**View > Resource Profile**. Use the Jade Empire profile when working with Jade Empire archives so resources such as `.itm`, `.cre`, `.mat`, `.mab`, `.ndb`, `.xwb`, `.xsb`, `.bip`, `.ttc`, and `.sac` are rendered and authored with the names used by the Jade Empire engine.
- Neverwinter Nights and Neverwinter Nights: Enhanced Edition use the NWN resource-name profile with ERF V1.0 containers such as `.erf`, `.mod`, `.hak`, and `.nwm`.
- Neverwinter Nights 2 uses the NWN2 profile with ERF V1.1 32-byte ResRefs. Existing NWN2 `.mod` and `.hak` archives preserve V1.1 on save; newly created archives use V1.1 when `--game nwn2` is selected. The NWN2 profile includes `.trx` terrain resources.
- The Witcher 1 `.mod` archives use the Witcher resource profile while keeping the ERF V1.0 container layout.
- Dragon Age: Origins ERF V2.0 archives use UTF-16LE filenames in the table of contents instead of ResRef/type keys. This includes DAO files with `.erf` and `.rim` extensions when the on-disk header is `ERF V2.0`. Add/extract/delete operations address these resources by full leaf filename, for example `2da_base.gda`.
- Dragon Age: Origins ERF V2.2 archives use the same UTF-16LE filename key shape plus archive flags and packed/unpacked TOC sizes. Unencrypted compressed payloads using scheme 1 (BioWare zlib wrapper byte plus raw DEFLATE) and scheme 7 (headerless raw DEFLATE) are readable/extractable. Saving a compressed V2.2 archive recompresses inserted/replaced resources and preserves scheme 1 vs scheme 7. `--type DAO22`/`ERF22` creates compressed V2.2; `--type DAO22U`/`ERF22U` creates uncompressed V2.2.
- Dragon Age II ERF V3.0 archives use FNV name/type hashes and optional string-table names. This includes DA2 `.erf`, `.rim`, `.rimp`, `.crf`, and GPU `.rim` containers. Compressed scheme-1 and scheme-7 payloads are decompressed on extraction and are rewritten as uncompressed V3.0 payloads; stripped-name entries are exposed through stable synthetic names such as `hash_905e15b30a2028fb.dds`.


## TSLPatcher/HoloPatcher resource packages

For KotOR/KotOR II ERF V1 and RIM V1 archives, NeoERF can compare a clean archive with a modified archive and create a complete stock-compatible `[InstallList]` package:

New resources are emitted as `InstallN`; changed resources are emitted as `ReplaceN`, with each payload extracted beside `changes.ini`. The GUI exposes the same workflow under **Export > Export TSLPatcher/HoloPatcher Resource Package...**.


### DA2 stripped-name resource notes

DA2 synthetic stripped resources now tolerate unknown FNV32 type hashes. Known DA2 hashes for `.lst` and `.ncc` are displayed with those extensions; any still-unknown type hash is kept in the synthetic filename as `hash_<fnv64>.#<fnv32>` and the original FNV32 value is preserved when saving.
