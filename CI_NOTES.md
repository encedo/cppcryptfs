# CI — GitHub Actions for cppcryptfs

## Current state

Workflow: `.github/workflows/build.yml`
Builds **x64 Release only** (`cppcryptfs.exe` + `cppcryptfsctl.exe`).
Artifact: `cppcryptfs-x64-release`.

---

## Dependencies built in CI

### Dokany 2.3.1.1000

No pre-built SDK archive is available (only MSI/installer).
The workflow instead:
1. Clones Dokany source at tag `v2.3.1.1000` (`--depth 1`).
2. Copies headers (`dokan/*.h` + `sys/public.h`) to `C:\Program Files\Dokan\Dokan Library-2.3.1\include\dokan\`.
3. Generates `dokan2.lib` (import library) via `lib.exe /def:` with a DEF listing 26 exported symbols.

**Why not extract the pre-built .lib from the MSI?**
Would require MSI parsing or running the installer — more complex than generating the .lib from source.

**x64**: `lib.exe /def:dokan2-x64.def /machine:X64` — x64 has no stdcall `@N` decoration; export names are plain (`DokanInit`, `DokanMain`, ...). Works correctly.

**x86 — abandoned**: Several approaches were attempted:
- DEF with `_DokanInit@0=DokanInit` — DEF parser treats `@N` as ordinal, producing symbol `_DokanInit` without `@0`; linker needs `__imp__DokanInit@0` → unresolved.
- `lib.exe /export:_DokanInit@0=DokanInit` without `/def:` — lib.exe does not know the DLL name, runs successfully (exit 0) but produces no output file.
- `lib.exe /def:LIBRARY_only.def /export:...` — .lib created but symbols still lack `@N`.
- `link.exe /DLL /EXPORT:_DokanInit@0=DokanInit` — link.exe treats the right-hand side of `=` as an internal symbol expected in input .obj files; stub .c does not define `DokanInit` → LNK2001 for all 25 exports.
- **Conclusion**: creating an MSVC x86 stdcall import library without the real DLL requires non-standard tooling (MinGW `dlltool` produces GNU ar format incompatible with MSVC linker). x86 support was dropped.

### OpenSSL 3.0.13

Checks whether a pre-installed `libcrypto.lib` exists (runner cache).
If not — downloads source from openssl.org and builds statically (`no-shared`) using NASM.

### RapidJSON

Clones headers from GitHub (`--depth 1`). No compilation (headers-only library).

---

## MSBuild configuration

```
msbuild cppcryptfs.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

- `Platform=x64` — not `Win32` (the solution uses `x64`/`x86`, not `Win32`)
- `PlatformToolset=v143` — VS 2022

---

## Notes

- `msbuild_platform` in the matrix must match the platform name in the `.sln` (`x64` for x64; for x86 it would be `x86`, not `Win32` as used in older project formats).
- `dokan_lib_dir` for x64 is `lib` (full path: `...\Dokan Library-2.3.1\lib\dokan2.lib`).
- `public.h` in the Dokany source tree lives under `sys/`, not `dokan/` — it must be copied separately.

## Spectre-mitigated libraries (MSB8040)

Every project sets `SpectreMitigation`, so the toolset must have its Spectre-mitigated
libraries installed or msbuild stops with:

```
error MSB8040: Spectre-mitigated libraries are required for this project.
```

This appeared without any change on our side. The build forces
`/p:PlatformToolset=v143`, and the job used to run on `windows-latest`; when that image
moved to a Visual Studio that no longer ships the v143 Spectre libraries, a pinned
toolset on a floating image stopped matching. The runner is now pinned to
`windows-2022`, which carries them.

Do **not** fix this by passing `/p:SpectreMitigation=false`. Upstream enabled the
setting deliberately on a filesystem that holds master keys in memory, and turning it
off in CI would quietly ship a weaker binary than upstream intends.

A preflight step checks for `VC\Tools\MSVC\<ver>\lib\spectre\x64` and installs
`Microsoft.VisualStudio.Component.VC.Runtimes.x86.x64.Spectre` if it is absent, so a
future image change degrades into a slower build rather than a failed one.

Each matrix entry names its own component, so ARM64 pulls
`Microsoft.VisualStudio.Component.VC.Runtimes.ARM64.Spectre` and x64 the x86/x64 one.

## ARM64

The matrix builds `x64` and `ARM64`. ARM64 is cross-compiled from the x64 runner —
`ilammy/msvc-dev-cmd` gets `amd64_arm64` (host x64, target ARM64) rather than `arm64`,
which would ask for a native ARM64 host we do not have.

`fail-fast: false`, so a break on one architecture still tells you about the other.

## Dependency paths come from the .vcxproj files

Do not pick install locations in the workflow. The project files hardcode them, and
upstream v1.4.4.10 moved them:

| | expected by the projects |
|---|---|
| OpenSSL x64 | `C:\git\openssl-amd64-static` |
| OpenSSL ARM64 | `C:\git\openssl-arm64-static` |
| Dokany x64 | `…\Dokan Library-2.3.1\lib\dokan2.lib` |
| Dokany ARM64 | `…\Dokan Library-2.3.1\arm64\lib\dokan2.lib` |

Before that move the projects looked in `C:\Program Files\OpenSSL`, which is what the
workflow used to install to. Following upstream's layout rather than patching the
projects keeps our fork's diff to the CI workflow alone, and out of the way of the next
merge.

Two consequences of the move:

- no runner image preinstalls `C:\git\openssl-*-static`, so OpenSSL always builds from
  source. It is cached per architecture (`actions/cache`, keyed on version and arch) —
  without that, every run rebuilds it twice.
- NASM is only fetched for `VC-WIN64A`; `VC-WIN64-ARM` does not use it.

The Dokany import library is generated from a `.def` with `lib.exe /machine:<arch>`.
The export list has no stdcall decoration, so one list serves both 64-bit targets.
