"""Host regressions for ZIP integrity, link-safe cleanup and complete USB backups."""
from pathlib import Path
import json
import os
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def function(text, head):
    start = text.index(head)
    return text[start:text.index("\n}", start) + 2]


with tempfile.TemporaryDirectory(prefix="zt-io-safety-") as tmp:
    temp = Path(tmp)
    fixtures = []
    for name, method, data in [
        ("stored.zip", zipfile.ZIP_STORED, b"VALID"),
        ("deflated.zip", zipfile.ZIP_DEFLATED, b"X" * (1 << 20)),
    ]:
        path = temp / name
        with zipfile.ZipFile(path, "w", compression=method) as archive:
            archive.writestr("asset.png", data)
        fixtures.append(path)
    raw = bytearray(fixtures[0].read_bytes())
    data_at = 30 + struct.unpack_from("<H", raw, 26)[0] + struct.unpack_from("<H", raw, 28)[0]
    raw[data_at] ^= 1
    bad = temp / "crc.zip"
    bad.write_bytes(raw)
    fixtures.append(bad)
    raw = bytearray(fixtures[1].read_bytes())
    central = raw.index(b"PK\x01\x02")
    struct.pack_into("<I", raw, central + 24, 16)
    bad = temp / "size.zip"
    bad.write_bytes(raw)
    fixtures.append(bad)
    raw = bytearray(fixtures[1].read_bytes())
    central = raw.index(b"PK\x01\x02")
    struct.pack_into("<I", raw, central + 24, (2 << 30) + 1)
    bad = temp / "oversize.zip"
    bad.write_bytes(raw)
    fixtures.append(bad)

    install = (ROOT / "src/source/shellui/src/zt_install.cpp").read_text()
    tools = (ROOT / "src/source/shellui/src/zt_tools.cpp").read_text()
    code = r"""
#include <algorithm>
#include <cassert>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#define ZT_THEMES "Themes"
const size_t kMaxText = 8u << 20;
std::string usb_target() { return "usb"; }
bool usb_present() { return true; }
std::string stamp() { return "same-second"; }
"""
    code += install[install.index("uint16_t le16("):install.index("bool read_at(")]
    code += function(install, "bool read_at(") + function(install, "bool write_out(")
    code += install[install.index("struct Entry {"):install.index("void tell(")]
    code += function(tools, "bool copy_file(")
    code += tools[tools.index("struct Count {"):tools.index("/* size and number")]
    code += function(tools, "int clear_tree(") + function(tools, "std::string to_usb(")
    code += r"""
std::string contents(const std::string &p) {
  std::ifstream f(p); return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void put(const std::string &p,const std::string &s) { std::ofstream(p) << s; }
int main(int argc,char **argv) {
  namespace fs = std::filesystem;
  assert(argc == 6);
  for (int i = 1; i < argc; ++i) {
    int fd = open(argv[i],O_RDONLY); assert(fd >= 0);
    struct stat st{}; assert(fstat(fd,&st) == 0);
    std::vector<Entry> entries;
    bool ok = zip_list(fd,st.st_size,entries);
    put("extracted", "old");
    if (ok) { assert(entries.size() == 1); ok = zip_file(fd,entries[0],"extracted"); }
    if (i <= 2) assert(ok);
    else { assert(!ok); assert(contents("extracted") == "old"); }
    close(fd);
  }
  fs::create_directories("Themes/A/icons");
  fs::create_directories("Themes/.zt-stage-pending/icons");
  put("Themes/A.ztheme","settings"); put("Themes/A/icons/a.png","icon");
  put("Themes/.zt-stage-pending/icons/incomplete","not committed");
  for (const auto &kind : {"customtile","custombg","avatars","folders","cache"}) {
    fs::create_directories(std::string("Assets/") + kind);
    put(std::string("Assets/") + kind + "/keep.bin",kind);
  }
  put("Assets/customtile/upload.part","unfinished");
  put("favorites.txt","PPSA00001");
  put("active_theme.txt","A");
  fs::create_directories("usb/ZhiThemes");
  Count c; std::string where;
  const std::vector<std::pair<std::string,std::string>> items = {
    {"Themes","Themes"},{"Assets","Assets"},{"favorites.txt","favorites.txt"},{"active_theme.txt","active_theme.txt"}
  };
  assert(to_usb("Backup",items,&where,c).empty());
  assert(c.files >= 8 && c.failed == 0);
  for (const auto &kind : {"customtile","custombg","avatars","folders","cache"})
    assert(contents(where + "/Assets/" + kind + "/keep.bin") == kind);
  assert(contents(where + "/Themes/A.ztheme") == "settings");
  assert(!fs::exists(where + "/Assets/customtile/upload.part"));
  assert(!fs::exists(where + "/Themes/.zt-stage-pending"));
  const std::string first = where;
  Count second;
  assert(to_usb("Backup",items,&where,second).empty());
  assert(where != first && fs::exists(first + "/Assets/customtile/keep.bin"));

  fs::create_directory("outside"); put("outside/keep","user data");
  fs::create_directory("cache"); fs::create_directory_symlink(fs::absolute("outside"),"cache/link");
  assert(clear_tree("cache") == 1);
  assert(contents("outside/keep") == "user data");
  fs::create_directory_symlink(fs::absolute("outside"),"cache-root-link");
  assert(clear_tree("cache-root-link") == 1 && contents("outside/keep") == "user data");
  fs::create_directory_symlink(fs::absolute("outside"),"Assets/external");
  Count links;
  copy_tree("Assets","safe-copy",links);
  assert(links.failed == 1 && !fs::exists("safe-copy/external"));
  std::cout << "IO safety tests passed: stored/deflate ZIP, CRC/size limits, old-file preservation, symlink cleanup, Assets backup and unique USB snapshots.\n";
}
"""
    cpp = temp / "test.cpp"
    cpp.write_text(code)
    binary = temp / "test"
    subprocess.run(["g++", "-std=c++20", "-O1", str(cpp), "-lz", "-o", str(binary)], check=True)
    subprocess.run([str(binary), *map(str, fixtures)], cwd=temp, check=True)
    # The web editor must enforce the same integrity rules before uploading.
    js = r"""
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const {Duplex}=require('node:stream'),zlib=require('node:zlib');
const html=fs.readFileSync(process.argv[2],'utf8');
const a=html.indexOf('  var CRC_TABLE = null;'),b=html.indexOf('  function textValue(',a);
const c=vm.createContext({Blob,TextEncoder,TextDecoder,DecompressionStream:class {
  constructor() { return Duplex.toWeb(zlib.createInflateRaw()); }
}});
vm.runInContext(html.slice(a,b),c);
(async()=>{
 for(let i=3;i<process.argv.length;i++) {
  const data=fs.readFileSync(process.argv[i]);
  let valid=true;
  try { await c.unzip(data.buffer.slice(data.byteOffset,data.byteOffset+data.byteLength)); }
  catch(e) { valid=false; }
  assert.equal(valid,i<5,process.argv[i]);
 }
 console.log('Editor ZIP tests passed: valid stored/deflate, bad CRC, wrong size and oversize rejected.');
})().catch(e=>{console.error(e);process.exitCode=1;});
"""
    jsfile = temp / "test.js"
    jsfile.write_text(js)
    subprocess.run(["node", str(jsfile), str(ROOT / "theme/editor.html"), *map(str, fixtures)], check=True)
