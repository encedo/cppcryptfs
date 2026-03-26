# CI — GitHub Actions dla cppcryptfs

## Stan aktualny

Workflow: `.github/workflows/build.yml`
Buduje tylko **x64 Release** (`cppcryptfs.exe` + `cppcryptfsctl.exe`).
Artefakt: `cppcryptfs-x64-release`.

---

## Zależności budowane w CI

### Dokany 2.3.1.1000

Oficjalny SDK nie jest dostępny jako prosty archiwum — MSI lub instalator.
Zamiast tego workflow:
1. Klonuje źródła `dokany` (tag `v2.3.1.1000`, `--depth 1`).
2. Kopiuje nagłówki (`dokan/*.h` + `sys/public.h`) do `C:\Program Files\Dokan\Dokan Library-2.3.1\include\dokan\`.
3. Generuje `dokan2.lib` (import library) przy pomocy `lib.exe /def:` z DEF zawierającym 26 eksportowanych symboli.

**Dlaczego nie pobieramy gotowego .lib z MSI?**
Wymagałoby to parsowania MSI lub uruchamiania instalatora — bardziej złożone niż generowanie .lib ze źródeł.

**x64**: `lib.exe /def:dokan2-x64.def /machine:X64` — na x64 nie ma dekoracji stdcall (`@N`), nazwy eksportów są plain (`DokanInit`, `DokanMain`, ...). Działa poprawnie.

**x86 — porzucone**: Próbowano kilku podejść:
- DEF z `_DokanInit@0=DokanInit` — DEF parser traktuje `@N` jako ordinal, powstaje symbol `_DokanInit` bez `@0`, linker szuka `__imp__DokanInit@0` → unresolved.
- `lib.exe /export:_DokanInit@0=DokanInit` bez `/def:` — lib.exe nie zna nazwy DLL, uruchamia się (exit 0) ale nie tworzy pliku.
- `lib.exe /def:LIBRARY_only.def /export:...` — .lib tworzony ale symbole wciąż bez `@N`.
- `link.exe /DLL /EXPORT:_DokanInit@0=DokanInit` — link.exe interpretuje prawą stronę `=` jako symbol wewnętrzny z .obj; stub .c nie zawiera `DokanInit` → LNK2001 dla wszystkich 25 eksportów.
- **Wniosek**: tworzenie MSVC import library dla x86 stdcall bez prawdziwej DLL wymaga niestandardowych narzędzi (dlltool MinGW, ale ten tworzy format ar niekompatybilny z MSVC). Zdecydowano o porzuceniu x86.

### OpenSSL 3.0.13

Sprawdza czy pre-installed `libcrypto.lib` istnieje (runner cache).
Jeśli nie — pobiera źródła z openssl.org, buduje statycznie (`no-shared`) z NASM.

### RapidJSON

Klonuje nagłówki z GitHub (`--depth 1`). Bez kompilacji (headers-only).

---

## Konfiguracja MSBuild

```
msbuild cppcryptfs.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

- `Platform=x64` — nie `Win32` (sln używa `x64`/`x86`, nie `Win32`)
- `PlatformToolset=v143` — VS 2022

---

## Uwagi

- `msbuild_platform` w matrycy musi odpowiadać nazwie platformy w `.sln` (dla x64: `x64`, dla x86 byłoby `x86` — nie `Win32` jak w starszych projektach).
- `dokan_lib_dir` dla x64 = `lib` (ścieżka `...\Dokan Library-2.3.1\lib\dokan2.lib`).
- `public.h` w źródłach Dokany jest w katalogu `sys/`, nie `dokan/` — kopiowany osobno.
