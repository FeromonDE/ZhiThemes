// Host integration: actual storage, theme.json conversion and native ZIP installer.
#define ZT_TEST_ROOT "ZhiThemes"
#include "../src/source/shellui/src/zt_storage.cpp"
#include "../src/source/shellui/include/zt_update.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
void zt_log(const char *, ...) {}
void zt_logl(char, const char *, ...) {}
const char *theme_default_text() { return "var THEME = {\n  tileSize: 100,\n};\n"; }
extern "C" int __real_rename(const char *, const char *);
std::vector<std::string> fail_renames;
extern "C" int __wrap_rename(const char *from, const char *to) {
  if (!fail_renames.empty() && fail_renames.front() == from) {
    fail_renames.erase(fail_renames.begin()); errno = EIO; return -1;
  }
  return __real_rename(from, to);
}
void put(const std::string &p, const std::string &s) { std::ofstream(p, std::ios::binary) << s; }
void save(const std::string &name, int size) {
  assert(zt_theme_write(name, "var THEME = {\n  tileSize: " + std::to_string(size) + ",\n};\n"));
}
int main(int argc, char **argv) {
  namespace fs = std::filesystem;
  assert(argc == 6);
  char tmp[] = "/tmp/zt-storage-safety-XXXXXX";
  assert(mkdtemp(tmp));
  const fs::path cwd = fs::current_path(); fs::current_path(tmp);
  fs::create_directories(ZT_THEMES "/Legacy/fonts");
  fs::create_directories(ZT_ASSETS "/customtile");
  put(ZT_THEMES "/Legacy.ztheme", "var THEME = {\n  themeName: 'Legacy',\n  tileSize: 143,\n  wallpaper: 'city.mp4',\n  folders: 'Personal:PPSA00001',\n  customTileChoice: 'PPSA00001/1x1:1',\n};\n");
  put(ZT_THEMES "/Legacy/fonts/custom.otf", "font bytes");
  put(ZT_ASSETS "/customtile/PPSA00001_1x1_1.png", "personal cover");
  put(ZT_ACTIVE_FILE, "Legacy");
  zt_storage_migrate();
  assert(zt_theme_name() == "Legacy");
  assert(zt_theme_read().find("tileSize: 143") != std::string::npos);
  assert(read_all((std::string(ZT_THEMES) + "/Legacy/fonts/custom.otf").c_str()) == "font bytes");
  assert(read_all(ZT_ASSETS "/customtile/PPSA00001_1x1_1.png") == "personal cover");
  assert(fs::exists(ZT_THEMES "/Legacy.ztheme"));
  fs::remove(zt_theme_file("Legacy")); zt_storage_migrate();
  assert(!fs::exists(zt_theme_file("Legacy")) && "a deliberately deleted theme must not resurrect");

  assert(zt_theme_new("A", "").empty()); save("A", 100); assert(zt_theme_use("A").empty());
  assert(theme_set_field_in(zt_theme_file("A"), "folders", "Personal:PPSA00001"));
  assert(theme_set_field_in(zt_theme_file("A"), "customTileChoice", "PPSA00001/1x1:1"));
  const std::string old = read_all(zt_theme_file("A").c_str());
  const std::string old_asset = zt_theme_dir_of("A", "icons") + "/old.png";
  put(old_asset, "old pixels");
  std::string stage;
  assert(zt_theme_install_begin("A", &stage).empty());
  assert(zt_themes_json().find(".zt-stage-") == std::string::npos);
  put(zt_theme_file(stage), "invalid JSON");
  assert(!zt_theme_install_commit(stage).empty());
  assert(read_all(zt_theme_file("A").c_str()) == old);
  assert(zt_theme_install_abort(stage).empty());

  assert(zt_theme_install_begin("A", &stage).empty()); save(stage, 200);
  put(zt_theme_dir_of(stage, "icons") + "/new.png", "new pixels");
  fail_renames = {std::string(ZT_THEMES) + "/" + stage};
  assert(!zt_theme_install_commit(stage, false).empty());
  assert(read_all(zt_theme_file("A").c_str()) == old && read_all(old_asset.c_str()) == "old pixels");
  assert(!fs::exists(zt_theme_dir_of("A", "icons") + "/new.png"));
  assert(zt_theme_install_abort(stage).empty());

  assert(zt_theme_install_begin("A", &stage).empty()); save(stage, 200);
  const std::string stage_dir = std::string(ZT_THEMES) + "/" + stage;
  fail_renames = {stage_dir, stage_dir + ".old"};
  assert(zt_theme_install_commit(stage, false).find("retained for recovery") != std::string::npos);
  assert(!zt_theme_install_abort(stage).empty());
  assert(read_all((stage_dir + ".old/theme.json").c_str()) == old);
  assert(fs::exists(stage_dir + ".old/icons/old.png"));
  fs::rename(stage_dir + ".old", ZT_THEMES "/A"); assert(zt_theme_install_abort(stage).empty());

  assert(zt_theme_install_begin("A", &stage).empty()); save(stage, 200);
  put(zt_theme_dir_of(stage, "icons") + "/new.png", "new pixels");
  assert(zt_theme_install_commit(stage).empty());
  assert(zt_theme_read("A").find("tileSize: 200") != std::string::npos);
  assert(zt_theme_info("A").name == "A");
  assert(zt_theme_read("A").find("Personal:PPSA00001") != std::string::npos);
  assert(!fs::exists(old_asset));
  assert(!zt_theme_install_stage(stage));
  assert(!zt_theme_install_begin("../bad", &stage).empty());
  assert(!zt_theme_install_abort("../bad").empty());

  const std::string before = read_all(zt_theme_file("A").c_str());
  std::string installed;
  assert(!zt_install_file(argv[2], "broken.ztheme", &installed, "A").empty());
  assert(read_all(zt_theme_file("A").c_str()) == before);
  assert(read_all((zt_theme_dir_of("A", "icons") + "/new.png").c_str()) == "new pixels");
  assert(zt_install_file(argv[1], "new.ztheme", &installed, "A").empty()); assert(installed == "A");
  assert(zt_theme_read("A").find("tileSize: 222") != std::string::npos);
  assert(zt_theme_read("A").find("Personal:PPSA00001") != std::string::npos);
  assert(zt_theme_info("A").author == "Fixture author");
  assert(fs::exists(zt_theme_dir_of("A", "wallpapers") + "/new.png"));
  assert(!fs::exists(zt_theme_dir_of("A", "icons") + "/new.png"));
  assert(zt_install_file(argv[3], "old.zip", &installed).empty());
  assert(zt_theme_read(installed).find("tileSize: 333") != std::string::npos);
  assert(zt_install_file(argv[4], "old.ztheme", &installed).empty());
  assert(zt_theme_read(installed).find("tileSize: 444") != std::string::npos);

  ZtPack pack; assert(zt_theme_pack_plan("A", &pack).empty());
  assert(pack.json.find("Personal:PPSA00001") == std::string::npos);
  std::string zipped;
  assert(zt_theme_pack_write(pack, [&](const char *p, size_t n) { zipped.append(p, n); return true; }));
  put(argv[5], zipped); assert(zipped.size() == pack.size);
  assert(zt_install_file(argv[5], "roundtrip.ztheme", &installed).empty());
  assert(zt_theme_read(installed).find("tileSize: 222") != std::string::npos);
  assert(fs::exists(zt_theme_dir_of(installed, "wallpapers") + "/new.png"));
  assert(read_all(ZT_ASSETS "/customtile/PPSA00001_1x1_1.png") == "personal cover");

  fs::create_directory("outside"); put("outside/keep", "keep");
  assert(zt_theme_new("B", "").empty());
  fs::remove_all(ZT_THEMES "/B/icons");
  fs::create_directory_symlink(fs::absolute("outside"), ZT_THEMES "/B/icons");
  int removed = 0;
  assert(!zt_theme_clean("B", "icons/keep", &removed).empty());
  assert(read_all("outside/keep") == "keep" && removed == 0);
  assert(zt_theme_clear("B").empty());
  assert(read_all("outside/keep") == "keep" && !fs::is_symlink(ZT_THEMES "/B/icons"));
  fs::create_directory_symlink(fs::absolute("outside"), ZT_THEMES "/B/linked");
  assert(zt_theme_delete("B").empty() && read_all("outside/keep") == "keep");

  zt_update_start();
  assert(zt_update_json().find("\"disabled\":true") != std::string::npos);
  for (const auto &cmd : {"check", "install", "retry", "auto1"}) assert(!zt_update_cmd(cmd).empty());
  assert(!fs::exists(ZT_DIR "/Update"));
  fs::current_path(cwd); fs::remove_all(tmp);
  std::cout << "Migration tests passed: old/new themes, metadata and assets, directory rollback/recovery, personal values, ZIP roundtrip and disabled official updater.\n";
}
