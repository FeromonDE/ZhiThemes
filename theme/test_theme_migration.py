"""Host integration of production storage/JSON/installer, without a PS5 SDK or ELF build."""
from pathlib import Path
import json
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src/source/shellui'

def function(source, head):
    start = source.index(head)
    return source[start:source.index('\n}', start) + 2]

with tempfile.TemporaryDirectory(prefix='zt-migration-') as tmp:
    temp = Path(tmp)
    png = b'\x89PNG\r\n\x1a\n' + bytes(range(128))
    good, bad, oldzip, oldtext, out = [temp / n for n in ('good.ztheme', 'bad.ztheme', 'old.zip', 'old.ztheme', 'out.ztheme')]
    with zipfile.ZipFile(good, 'w', compression=zipfile.ZIP_STORED) as z:
        z.writestr('theme.json', json.dumps({'format': 2, 'name': 'Pack', 'author': 'Fixture author',
            'description': 'Localized metadata', 'settings': {'tileSize': 222, 'wallpaperOn': True,
            'wallpaper': 'new.png', 'folders': 'Foreign:PPSA99999'}}))
        z.writestr('wallpapers/new.png', png)
    raw = bytearray(good.read_bytes())
    with zipfile.ZipFile(good) as z:
        offset = z.getinfo('wallpapers/new.png').header_offset
    offset += 30 + struct.unpack_from('<H', raw, offset + 26)[0] + struct.unpack_from('<H', raw, offset + 28)[0]
    raw[offset + 30] ^= 1
    bad.write_bytes(raw)
    with zipfile.ZipFile(oldzip, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr('theme.ztheme', "var THEME = {\n  themeName: 'OldZip',\n  tileSize: 333,\n};\n")
        z.writestr('zhitheme.json', '{"name":"OldZip"}')
    oldtext.write_text("var THEME = {\n  themeName: 'OldText',\n  tileSize: 444,\n};\n")

    # Extract the real value parser and field writer; only PS5 UI/logging is stubbed.
    home = (SRC / 'src/homeui_js_inject.cpp').read_text()
    web = (SRC / 'src/theme_web.cpp').read_text()
    support = '#include "zt_storage.hpp"\n#include <ctype.h>\n#include <cstring>\n#include <string>\n#include <utility>\n#include <vector>\n'
    support += home[home.index('struct Block {'):home.index('/* the user\'s TOKENS block')]
    support += function(home, 'std::vector<std::pair<std::string, std::string>> theme_values(')
    support += '\n' + function(home, 'std::vector<std::pair<std::string, std::string>> zt_theme_values_of(')
    support += '\nbool zt_theme_value_ok(const std::string &v) { return !v.empty() && safe_value(v); }\n'
    support += function(web, 'bool theme_set_field_in(')
    (temp / 'support.cpp').write_text(support)
    (temp / 'external_symbols.hpp').write_text(
        'inline int (*sceSystemServiceParamGetInt)(int, int *) = nullptr;\n')
    installer = (SRC / 'src/zt_install.cpp').read_text().replace('#include "external_symbols.hpp"',
        'struct OrbisNotificationRequest { char message[1024]; };\n'
        'static int (*sceKernelSendNotificationRequest)(int, OrbisNotificationRequest *, unsigned long, int) = nullptr;')
    (temp / 'installer.cpp').write_text(installer)
    subprocess.run(['python3', str(ROOT / 'theme/gen_particles_header.py'), str(ROOT / 'theme/particles'),
                    str(temp / 'zt_particle_textures.hpp')], check=True, stdout=subprocess.DEVNULL)
    binary = temp / 'test'
    subprocess.run(['g++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
        '-DZT_TEST_ROOT="ZhiThemes"', '-DZHITHEMES=1', '-I' + str(temp), '-I' + str(SRC / 'include'),
        str(ROOT / 'theme/test_storage_safety.cpp'), str(SRC / 'src/zt_theme_json.cpp'),
        str(temp / 'support.cpp'), str(temp / 'installer.cpp'), str(SRC / 'src/zt_update.cpp'),
        '-Wl,--gc-sections', '-Wl,--wrap=rename', '-lz', '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(good), str(bad), str(oldzip), str(oldtext), str(out)], check=True)
    with zipfile.ZipFile(out) as z:
        assert z.testzip() is None
        assert json.loads(z.read('theme.json'))['settings']['tileSize'] == 222
        assert z.read('wallpapers/new.png') == png
        assert all(not p.startswith('Assets/') for p in z.namelist())
    print('Exported .ztheme independently validated with Python zipfile: JSON, CRC and exact asset bytes.')
