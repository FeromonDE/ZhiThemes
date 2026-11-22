/* ZhiThemes — PS5 Home theme engine.
   Copyright (C) 2026 Zhi (ZhiThemes author)

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version. See LICENSE and NOTICE.  */

/* Updates of ZhiThemes itself (Options menu > ZhiThemes > Updates, the
   editor's update button).

   1. Check: update/latest.json of the GitHub repo (raw.githubusercontent.com;
      GitHub's "latest release" leaves pre-releases out). Its fields are
      signed with the author's Ed25519 key (public key below): a changed
      file is refused. Newer = a higher "build" than ZT_BUILD. At the
      module's start and once a day (unless turned off), or when asked.
   2. Install: the ELF from the release, checked (size, SHA-256) into
      Update/staged.elf + staged.txt.
   3. Apply: with fewer loads into this ShellUI than the firmware allows,
      the new ELF goes to the ELF loader (127.0.0.1:9021) at once; the new
      payload puts it in the payload manager and replaces this module.
      Otherwise the running payload puts it there and the user restarts.
   The payload (zhithemes/source/main.cpp) counts crashes with the new
   version and goes back to the previous one at the second. */

#include "zt_update.hpp"
#include "zt_brand.hpp"
#include "zt_log.hpp"
#include "zt_paths.hpp"
#include "zt_sgdb.hpp"
#include "zt_storage.hpp"
#include "homeui_js_inject.hpp"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <initializer_list>
#include <utility>
#include <mutex>
#include <string>

#if defined(ZHITHEMES) && ZHITHEMES && ZT_OFFICIAL_UPDATES

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <openssl/evp.h>

#include "cJSON.hpp"

namespace {

constexpr const char *kManifestUrl = "https://raw.githubusercontent.com/itsRealZhi/ZhiThemes/main/update/latest.json";
constexpr const char *kDownloadPrefix = "https://github.com/itsRealZhi/ZhiThemes/releases/download/";
constexpr size_t kMaxElf = 48u << 20;
constexpr int kLoaderPort = 9021;
constexpr long kCheckEveryS = 24 * 60 * 60;
constexpr int kFirstCheckS = 45;

/* the author's update key (Ed25519, public half); the private half signs
   update/latest.json and never leaves the author's PC */
const unsigned char kUpdateKey[32] = {0x42, 0x82, 0xe1, 0xcb, 0x90, 0xf6, 0x0f, 0xb6, 0x46, 0x87, 0x68,
                                      0x2d, 0x35, 0x01, 0x0e, 0x25, 0x33, 0xe3, 0xe8, 0x80, 0x54, 0x64,
                                      0xd2, 0xd9, 0xd3, 0xc7, 0x87, 0xc9, 0x61, 0x8c, 0x8b, 0x6c};

#define ZT_UPD ZT_DIR "/Update"

struct Latest {
  std::string version, url, sha256, notes;
  long build = 0;
  long long size = 0;
};

std::mutex g_lock;
std::string g_state = "idle"; /* idle checking downloading applying error */
std::string g_error;
Latest g_latest; /* build 0 = none known */
time_t g_checked = 0;
int g_progress = -1;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_started{false};

std::string upd(const char *name) { return std::string(ZT_UPD) + "/" + name; }

std::string read_text(const std::string &path, size_t max = 65536) {
  std::string out;
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return out;
  char buf[4096];
  ssize_t n;
  while (out.size() < max && (n = read(fd, buf, sizeof(buf))) > 0)
    out.append(buf, (size_t)n);
  close(fd);
  return out;
}

bool write_text(const std::string &path, const std::string &text) {
  const std::string tmp = path + ".tmp";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    return false;
  const bool ok = write(fd, text.data(), text.size()) == (ssize_t)text.size();
  fsync(fd);
  close(fd);
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

/* "key=value" lines (the files the payload writes) */
std::string kv(const std::string &text, const char *key) {
  const std::string k = std::string(key) + "=";
  size_t at = 0;
  while (at < text.size()) {
    const size_t end = text.find('\n', at);
    const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (line.rfind(k, 0) == 0) {
      std::string v = line.substr(k.size());
      while (!v.empty() && (v.back() == '\r' || v.back() == ' '))
        v.pop_back();
      return v;
    }
    if (end == std::string::npos)
      break;
    at = end + 1;
  }
  return "";
}

std::string jstr(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c == '\n')
      out += "\\n";
    else if (c >= 0x20)
      out += (char)c;
  }
  return out + "\"";
}

void set_state(const std::string &state, const std::string &err = "", int progress = -1) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_state = state;
  g_error = err;
  g_progress = progress;
}

/* Update/state.txt: every update setting and state in one file (key=value
   lines, shared with the payload, zhithemes/source/main.cpp) */
std::string state_text() { return read_text(upd("state.txt")); }

/* some keys changed ("" removes the key) */
void state_set(std::initializer_list<std::pair<const char *, std::string>> keys) {
  const std::string text = state_text();
  std::string out;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos)
      end = text.size();
    const std::string line = text.substr(at, end - at);
    at = end + 1;
    const std::string key = line.substr(0, line.find('='));
    bool drop = line.empty();
    for (const auto &k : keys)
      if (key == k.first)
        drop = true;
    if (!drop)
      out += line + "\n";
  }
  for (const auto &k : keys)
    if (!k.second.empty())
      out += std::string(k.first) + "=" + k.second + "\n";
  write_text(upd("state.txt"), out);
}

bool auto_on() { return kv(state_text(), "auto_check") != "off"; }

/* the text that is signed: every field of latest.json but "sig" */
std::string signed_text(const Latest &l) {
  return "ZhiThemes-update\n" + l.version + "\n" + std::to_string(l.build) + "\n" + std::to_string(l.size) + "\n" +
         l.sha256 + "\n" + l.url + "\n" + l.notes;
}

bool signature_ok(const std::string &text, const std::string &sig_b64) {
  if (sig_b64.size() != 88)
    return false;
  unsigned char sig[72] = {};
  const int n = EVP_DecodeBlock(sig, reinterpret_cast<const unsigned char *>(sig_b64.data()), (int)sig_b64.size());
  if (n < 64)
    return false;
  EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, kUpdateKey, sizeof(kUpdateKey));
  if (!key)
    return false;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  bool ok = false;
  if (ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1)
    ok = EVP_DigestVerify(ctx, sig, 64, reinterpret_cast<const unsigned char *>(text.data()), text.size()) == 1;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);
  return ok;
}

/* lowercase hex SHA-256 of a file ("" when it cannot be read) */
std::string file_sha256(const std::string &path, long long &size) {
  size = 0;
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return "";
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  bool ok = ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
  static char buf[1 << 16];
  ssize_t n;
  while (ok && (n = read(fd, buf, sizeof(buf))) > 0) {
    size += n;
    ok = EVP_DigestUpdate(ctx, buf, (size_t)n) == 1;
  }
  close(fd);
  unsigned char md[32];
  unsigned int len = 0;
  ok = ok && EVP_DigestFinal_ex(ctx, md, &len) == 1 && len == 32;
  EVP_MD_CTX_free(ctx);
  if (!ok)
    return "";
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : md) {
    out += hex[c >> 4];
    out += hex[c & 15];
  }
  return out;
}

/* latest.json -> Latest; "" or why it is refused */
std::string parse_latest(const std::string &body, Latest &l) {
  cJSON *j = cJSON_Parse(body.c_str());
  if (!j)
    return "the update information is not valid";
  auto str = [&](const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
    return cJSON_IsString(v) && v->valuestring ? std::string(v->valuestring) : std::string();
  };
  auto num = [&](const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, k);
    return cJSON_IsNumber(v) ? (long long)v->valuedouble : 0LL;
  };
  l.version = str("version");
  l.url = str("url");
  l.sha256 = str("sha256");
  l.notes = str("notes");
  l.build = (long)num("build");
  l.size = num("size");
  const std::string sig = str("sig");
  cJSON_Delete(j);
  if (l.version.empty() || l.version.size() > 40 || l.build <= 0 || l.size <= 0 || (size_t)l.size > kMaxElf ||
      l.sha256.size() != 64 || l.url.rfind(kDownloadPrefix, 0) != 0 || l.notes.size() > 600)
    return "the update information is incomplete";
  if (!signature_ok(signed_text(l), sig))
    return "the update information is not signed by ZhiThemes' author";
  return "";
}

/* how many modules went into this ShellUI (the payload's inject_count:
   "<ShellUI pid> <count> <boot time>") */
int loads_here() {
  long pid = 0, count = 0, boot = 0;
  sscanf(read_text(ZT_DIR "/inject_count", 64).c_str(), "%ld %ld %ld", &pid, &count, &boot);
  return pid == (long)getpid() && count > 0 ? (int)count : 1;
}

/* how many loads into one ShellUI an update may make (the new module is
   one more). 13.xx: 2 (a ShellUI that was already running froze once when a
   module went in, CE-108262-9: kept low there); firmware older than 9.00:
   ShellUI fell at the 4th load on 4.03. */
int load_limit() {
  const int major = atoi(zt_fw_text().c_str());
  if (major >= 13)
    return 2;
  if (major > 0 && major < 9)
    return 3;
  return 5;
}

bool send_to_loader(const std::string &path) {
  const std::string elf = read_text(path, kMaxElf + 1);
  if (elf.size() < 64 || elf.compare(0, 4, "\x7f" "ELF") != 0)
    return false;
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  timeval tv = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kLoaderPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
  const int err = ok ? 0 : errno;
  for (size_t at = 0; ok && at < elf.size();) {
    const ssize_t n = send(fd, elf.data() + at, elf.size() - at, 0);
    if (n <= 0)
      ok = false;
    else
      at += (size_t)n;
  }
  shutdown(fd, SHUT_WR);
  close(fd);
  zt_logl(ok ? 'A' : 'W', "update: new ELF to the ELF loader (:%d): %s", kLoaderPort,
          ok ? "sent" : err ? strerror(err) : "send failed");
  return ok;
}

/* ---- the work, in its own thread ---- */

bool check_now(bool tell) {
  set_state("checking");
  std::string body, err;
  long http = 0;
  const bool answered = zt_github_get(kManifestUrl, body, http, err, 64u << 10);
  if (answered && http == 404) {
    /* nothing published yet */
    zt_logl('I', "update: no update information on GitHub yet");
    {
      std::lock_guard<std::mutex> lock(g_lock);
      g_checked = time(nullptr);
    }
    set_state("idle");
    if (tell)
      zt_menu_notify(std::string("version ") + ZT_VERSION + " is the latest.");
    return false;
  }
  if (!answered || http != 200) {
    const std::string why = err.empty() ? "HTTP " + std::to_string(http) : err;
    zt_logl(tell ? 'W' : 'I', "update: check failed: %s", why.c_str());
    set_state("error", "cannot reach GitHub (" + why + ")");
    if (tell)
      zt_menu_notify("cannot check for updates: " + why);
    return false;
  }
  Latest l;
  const std::string bad = parse_latest(body, l);
  if (!bad.empty()) {
    zt_logl('W', "update: latest.json refused: %s", bad.c_str());
    set_state("error", bad);
    if (tell)
      zt_menu_notify(bad);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(g_lock);
    g_latest = l;
    g_checked = time(nullptr);
  }
  set_state("idle");
  const bool newer = l.build > ZT_BUILD;
  zt_logl('I', "update: latest is %s (build %ld), this is %s (build %d)%s", l.version.c_str(), l.build, ZT_VERSION,
          ZT_BUILD, newer ? ": update available" : "");
  if (!newer) {
    if (tell)
      zt_menu_notify(std::string("version ") + ZT_VERSION + " is the latest.");
    return true;
  }
  /* once per version (and not for the one that crashed before) */
  const std::string tag = l.version + "|" + std::to_string(l.build);
  const std::string st = state_text();
  const bool failed = atol(kv(st, "failed_build").c_str()) == l.build;
  if (tell || (!failed && kv(st, "notified") != tag)) {
    zt_menu_notify("version " + l.version +
                   " is available. Options on a tile > ZhiThemes > Updates, or the update button in the editor.");
    state_set({{"notified", tag}});
  }
  return true;
}

void install_now() {
  Latest l;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    l = g_latest;
  }
  if (l.build <= ZT_BUILD) {
    if (!check_now(false))
      return;
    std::lock_guard<std::mutex> lock(g_lock);
    l = g_latest;
  }
  if (l.build <= ZT_BUILD) {
    set_state("idle");
    zt_menu_notify(std::string("version ") + ZT_VERSION + " is the latest.");
    return;
  }
  zt_logl('A', "update: downloading ZhiThemes %s (build %ld, %lld bytes)", l.version.c_str(), l.build, l.size);
  zt_menu_notify("downloading version " + l.version + "...");
  set_state("downloading", "", 0);
  const std::string part = upd("staged.elf.part");
  std::string err;
  long http = 0;
  const bool got = zt_github_get_file(
      l.url, part, kMaxElf,
      [&](size_t done, long long) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_progress = (int)(done * 100 / (size_t)l.size);
        if (g_progress > 100)
          g_progress = 100;
      },
      http, err);
  if (!got) {
    zt_logl('W', "update: download failed: %s", err.c_str());
    set_state("error", "download failed (" + err + ")");
    zt_menu_notify("update download failed: " + err);
    return;
  }
  long long size = 0;
  const std::string sha = file_sha256(part, size);
  if (size != l.size || sha != l.sha256) {
    unlink(part.c_str());
    zt_logl('W', "update: downloaded file refused: %lld bytes (want %lld), sha256 %s (want %s)", size, l.size,
            sha.c_str(), l.sha256.c_str());
    set_state("error", "the downloaded file is damaged; try again");
    zt_menu_notify("the downloaded update is damaged; try again");
    return;
  }
  if (rename(part.c_str(), upd("staged.elf").c_str()) != 0) {
    unlink(part.c_str());
    set_state("error", "cannot keep the downloaded file");
    return;
  }
  const int loads = loads_here(), limit = load_limit();
  const bool live = loads < limit;
  state_set({{"staged_version", l.version}, {"staged_build", std::to_string(l.build)},
             {"staged_from_version", ZT_VERSION}, {"staged_from_build", ZT_BUILD_TEXT},
             {"staged_mode", live ? "live" : "restart"}, {"failed_build", ""}});
  zt_logl('A', "update: ZhiThemes %s verified; %d loads into this ShellUI, limit %d on FW %s: %s", l.version.c_str(),
          loads, limit, zt_fw_text().c_str(), live ? "starting it now" : "after a restart");
  if (live) {
    set_state("applying");
    zt_menu_notify("version " + l.version + " downloaded. Starting it now...");
    sleep(1);
    if (send_to_loader(upd("staged.elf")))
      return; /* the new payload takes this module out */
    state_set({{"staged_mode", "restart"}});
  }
  set_state("idle");
  zt_menu_notify("version " + l.version + " is installed. Restart the PS5 to use it" +
                 (live ? std::string(" (the ELF loader did not answer).")
                       : limit == 0 ? std::string(" (on this firmware ZhiThemes cannot be replaced while it runs).")
                                    : " (ZhiThemes was already loaded " + std::to_string(loads) +
                                          " times since the PS5 started)."));
}

struct Job {
  std::string what;
};

void *job_thread(void *arg) {
  Job *job = static_cast<Job *>(arg);
  const std::string what = job->what;
  delete job;
  if (what == "check")
    check_now(true);
  else if (what == "install")
    install_now();
  g_busy.store(false);
  return nullptr;
}

std::string run_job(const std::string &what) {
  bool idle = false;
  if (!g_busy.compare_exchange_strong(idle, true))
    return "wait, the update is still busy";
  /* at once, so the editor's answer already shows the work going on */
  set_state(what == "install" ? "downloading" : "checking", "", what == "install" ? 0 : -1);
  pthread_t t;
  if (pthread_create(&t, nullptr, job_thread, new Job{what}) != 0) {
    g_busy.store(false);
    return "cannot start the update";
  }
  pthread_detach(t);
  return "";
}

/* the first check after the start (the Home first), then once a day */
void *auto_thread(void *) {
  long waited = 0, next = kFirstCheckS;
  while (!theme_quitting()) {
    sleep(1);
    if (++waited < next)
      continue;
    waited = 0;
    next = kCheckEveryS;
    bool idle = false;
    if (!auto_on() || !g_busy.compare_exchange_strong(idle, true))
      continue;
    check_now(false);
    g_busy.store(false);
  }
  return nullptr;
}

} // namespace

void zt_update_start() {
  if (g_started.exchange(true))
    return;
  zt_mkdirs(ZT_UPD);
  unlink(upd("staged.elf.part").c_str());
  pthread_t t;
  if (pthread_create(&t, nullptr, auto_thread, nullptr) == 0)
    pthread_detach(t);
}

std::string zt_update_json() {
  std::string state, error;
  Latest l;
  time_t checked;
  int progress;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    state = g_state;
    error = g_error;
    l = g_latest;
    checked = g_checked;
    progress = g_progress;
  }
  const std::string st = state_text();
  const long failed = atol(kv(st, "failed_build").c_str());
  /* a newer version already installed, waiting for a restart */
  const long inst_build = atol(kv(st, "inst_build").c_str());
  const bool restart = inst_build > ZT_BUILD;
  std::string out = "{\"current\":" + jstr(ZT_VERSION) + ",\"build\":" + std::to_string(ZT_BUILD);
  out += ",\"state\":" + jstr(state) + ",\"error\":" + jstr(error) + ",\"progress\":" + std::to_string(progress);
  out += std::string(",\"auto\":") + (auto_on() ? "true" : "false");
  out += ",\"checkedAgo\":" + std::to_string(checked ? (long)(time(nullptr) - checked) : -1L);
  out += std::string(",\"available\":") + (l.build > ZT_BUILD && !restart ? "true" : "false");
  out += ",\"latest\":" + jstr(l.version) + ",\"latestBuild\":" + std::to_string(l.build);
  out += ",\"notes\":" + jstr(l.notes) + ",\"size\":" + std::to_string(l.size);
  out += std::string(",\"crashedBefore\":") + (failed > 0 && failed == l.build ? "true" : "false");
  out += std::string(",\"restart\":") + (restart ? "true" : "false");
  out += ",\"restartVersion\":" + jstr(restart ? kv(st, "inst_version") : "");
  out += std::string(",\"restartPldmgr\":") + (kv(st, "inst_pldmgr") == "1" ? "true" : "false");
  out += ",\"restartWhere\":" + jstr(restart ? kv(st, "inst_where") : "");
  out += ",\"rolledBack\":" + jstr(kv(st, "rolled_version"));
  out += ",\"rolledBackTo\":" + jstr(kv(st, "rolled_previous"));
  out += ",\"loads\":" + std::to_string(loads_here()) + ",\"loadLimit\":" + std::to_string(load_limit()) + "}";
  return out;
}

std::string zt_update_cmd(const std::string &cmd) {
  zt_logl('A', "update: %s", cmd.c_str());
  zt_mkdirs(ZT_UPD);
  if (cmd == "check")
    return run_job("check");
  if (cmd == "install")
    return run_job("install");
  if (cmd == "retry") {
    /* the version that crashed, once more */
    state_set({{"rolled_version", ""}, {"rolled_previous", ""}, {"failed_build", ""}});
    return run_job("install");
  }
  if (cmd == "dismiss") {
    state_set({{"rolled_version", ""}, {"rolled_previous", ""}});
    return "";
  }
  if (cmd == "auto1" || cmd == "auto0") {
    state_set({{"auto_check", cmd == "auto1" ? "" : "off"}});
    zt_menu_notify(cmd == "auto1" ? "ZhiThemes will check for updates by itself (once a day)."
                                  : "automatic update checks off.");
    return "";
  }
  return "unknown update command " + cmd;
}

#else

void zt_update_start() {}
std::string zt_update_json() {
  return "{\"current\":\"" ZT_VERSION "\",\"build\":" ZT_BUILD_TEXT
         ",\"disabled\":true,\"auto\":false,\"available\":false,\"state\":\"idle\","
         "\"reason\":\"Official updates are disabled to preserve this fork's fixes.\"}";
}
std::string zt_update_cmd(const std::string &) { return "official updates are disabled in this fork"; }

#endif
