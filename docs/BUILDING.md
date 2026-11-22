# Building ZhiThemes.elf

## Layout of the repository

```
theme/                 the theme engine (theme.js) and the web editor (editor.html)
  theme.js             runs inside the PS5 Home; the THEME block = every field with
                       its stock value "(…)" at the end of the comment
  editor.html          the editor served on port 4444
  make_factory.py      theme.js -> theme_factory.js (every field at its stock value)
  gen_theme_header.py  embeds a file in a C++ header
  gen_particles_header.py  embeds theme/particles/*.png (particle style textures)
  particles/           textures of the built-in particle styles (AniketosHEN, GPL-3.0)
  theme_harness.js     runs theme.js in Node with a fake React Native (tests)
  editor_mock_server.py  serves the editor on the PC without a PS5 (tests)
src/                   OnionHEN's source tree with ZhiThemes added
  source/zhithemes/    the ZhiThemes.elf payload (injector + watcher)
  source/shellui/src/  the module injected into ShellUI; ZhiThemes files:
                       homeui_js_inject.cpp, theme_web.cpp, zt_*.cpp, prx.cpp (ZHITHEMES=1 path)
  ZHITHEMES_CHANGES.md what was changed in OnionHEN's files
scripts/build_zhithemes.sh
docs/
```

## Requirements

Linux (or WSL2 on Windows), with:

- the **PS5 payload SDK** in `/opt/ps5-payload-sdk` (or set `PS5_PAYLOAD_SDK`), including its homebrew libraries (libcurl, OpenSSL, zlib),
- **LLVM/clang 18** (`clang-18`, `lld-18`, `llvm-18`), **cmake**, **ninja**, **python3** (with `pyelftools`),
- **Node.js**, only for the tests.

`src/Dockerfile` (from OnionHEN) installs the same toolchain in a container.

## Build

```bash
./scripts/build_zhithemes.sh
```

The output is `release/ZhiThemes.elf`. The first run configures the CMake tree with OnionHEN's `scripts/build.sh --release --configure-only` and then builds only the `zhithemes` target. The script also regenerates the three embedded files from `theme/`:

- `theme_default_js.hpp` (the stock theme),
- `theme_web_page.hpp` (the editor),
- `zt_particle_textures.hpp` (the particle style textures).

Run it again after every change to `theme/`.

## Tests (no PS5 needed)

```bash
cd theme
node --check theme.js
node test_reflection.js                        # one-image renderer, async cache, bounded ripples
node test_editor_safety.js                     # Apply snapshots and server install failures
python3 test_io_safety.py                      # ZIP, cleanup and complete USB backup (g++, zlib)
python3 test_theme_migration.py                 # actual JSON/storage/installer integration (g++, zlib)
THEME_APP=home node theme_harness.js theme.js     # also: hub, settings, trophy
python3 editor_mock_server.py editor.html theme.js 4444   # then open http://localhost:4444
```

The harness prints what the theme applied and `"errors": 0` when nothing failed.

The native PNG reflection generator also has a host test (needs g++ and zlib headers):

```bash
g++ -std=c++20 -O1 -ffunction-sections -fdata-sections -I src/source/shellui/include theme/test_reflection_pixels.cpp -Wl,--gc-sections -lz -o /tmp/zt-reflection-test
/tmp/zt-reflection-test /tmp/zt-reflection-test-data
python3 theme/test_theme_migration.py
```

## Versions

Name, author, version and build number are in `src/source/shellui/include/zt_brand.hpp`. They are used by the payload's notification, the editor and the log header.

Fork CI is manual (`workflow_dispatch`); pushing an integration branch does not build an ELF.

The shared util suite (`make -C src/source/util/tests test`) also requires the
host Keystone library. During this integration it reached linking but could not
find `-lkeystone`; the focused theme, editor, migration and I/O tests passed.
