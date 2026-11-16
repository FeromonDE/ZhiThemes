// Host integration test for the actual storage implementation.
#define ZT_TEST_ROOT "ZhiThemes"
#include "../src/source/shellui/src/zt_storage.cpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
void zt_log(const char *, ...) {}
const char *theme_default_text() { return "var THEME = {\n  themeName: '',\n};\n"; }
bool theme_set_field_in(const std::string &, const std::string &, const std::string &) { return false; }
extern "C" int __real_rename(const char *, const char *);
std::string fail_rename_from;
extern "C" int __wrap_rename(const char *from, const char *to) {
  if (fail_rename_from == from) { fail_rename_from.clear(); errno = EIO; return -1; }
  return __real_rename(from, to);
}
void put(const std::string &p, const std::string &s) { std::ofstream(p) << s; }
int main() {
  namespace fs = std::filesystem;
  char tmp[] = "/tmp/zt-storage-safety-XXXXXX";
  assert(mkdtemp(tmp));
  const fs::path cwd = fs::current_path();
  fs::current_path(tmp);
  fs::create_directories(ZT_THEMES);
  assert(zt_theme_new("A", "").empty());
  put(zt_theme_file("A"), "old theme");
  const std::string old_asset = zt_theme_dir_of("A", "icons") + "/old.png";
  put(old_asset, "old pixels");
  std::string stage;
  assert(zt_theme_install_begin("A", &stage).empty());
  assert(zt_theme_install_stage(stage));
  assert(zt_themes_json().find(".zt-stage-") == std::string::npos);
  put(zt_theme_dir_of(stage, "icons") + "/new.png", "new pixels");
  assert(read_all(old_asset.c_str()) == "old pixels");
  assert(zt_theme_install_abort(stage).empty());
  assert(read_all(old_asset.c_str()) == "old pixels");
  assert(!zt_theme_install_stage(stage));

  assert(zt_theme_install_begin("A", &stage).empty());
  put(zt_theme_file(stage), "new theme");
  put(zt_theme_dir_of(stage, "icons") + "/new.png", "new pixels");
  fail_rename_from = zt_theme_file(stage); // fail the final rename after the folder moved
  assert(!zt_theme_install_commit(stage, false).empty());
  assert(read_all(zt_theme_file("A").c_str()) == "old theme");
  assert(read_all(old_asset.c_str()) == "old pixels");
  assert(!fs::exists(zt_theme_dir_of("A", "icons") + "/new.png"));
  assert(zt_theme_install_abort(stage).empty());

  assert(zt_theme_install_begin("A", &stage).empty());
  put(zt_theme_file(stage), "new theme");
  put(zt_theme_dir_of(stage, "icons") + "/new.png", "new pixels");
  assert(zt_theme_install_commit(stage, false).empty());
  assert(read_all(zt_theme_file("A").c_str()) == "new theme");
  assert(read_all((zt_theme_dir_of("A", "icons") + "/new.png").c_str()) == "new pixels");
  assert(!fs::exists(old_asset));
  assert(!zt_theme_install_stage(stage));
  assert(!zt_theme_install_begin("../bad", &stage).empty());
  assert(!zt_theme_install_abort("../bad").empty());

  // Deleting a theme unlinks a directory symlink, never its target.
  fs::create_directory("outside");
  put("outside/keep", "keep");
  assert(zt_theme_new("B", "").empty());
  fs::remove_all(ZT_THEMES "/B/icons");
  fs::create_directory_symlink(fs::absolute("outside"), ZT_THEMES "/B/icons");
  int removed = 0;
  assert(!zt_theme_clean("B", "icons/keep", &removed).empty());
  assert(read_all("outside/keep") == "keep" && removed == 0);
  assert(zt_theme_clear("B").empty());
  assert(read_all("outside/keep") == "keep" && !fs::is_symlink(ZT_THEMES "/B/icons"));
  fs::create_directory_symlink(fs::absolute("outside"), ZT_THEMES "/B/linked");
  assert(zt_theme_delete("B").empty());
  assert(read_all("outside/keep") == "keep");
  assert(!fs::exists(ZT_THEMES "/B"));
  fs::current_path(cwd);
  fs::remove_all(tmp);
  std::cout << "Storage safety tests passed: staging, abort, rollback, commit and symlink-safe deletion.\n";
}
