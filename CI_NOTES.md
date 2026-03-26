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
