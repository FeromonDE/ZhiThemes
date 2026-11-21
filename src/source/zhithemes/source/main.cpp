/* ZhiThemes — PS5 Home theme engine.
   Copyright (C) 2026 Zhi (ZhiThemes author)

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version. See LICENSE and NOTICE.  */

/* ZhiThemes — PS5 home screen theme, standalone payload.
 *
 * Based on OnionHEN (GPL-3.0). Only the theme is kept: this payload injects
 * zt_shellui.elf (the theme module) into SceShellUI and watches for ShellUI
 * restarts (rest mode) to inject it again.
 *
 * Replacing a running copy without rebooting: the module already in ShellUI
 * answers on 127.0.0.1:4444; POST /quit makes it take its hooks out (atomic
 * swap back to the original bytes) and close the port, then the new module is
 * injected. A previous watcher process is stopped through its pid file.
 *
 * Files: /data/ZhiThemes/Themes/<active>.ztheme; logs in /data/ZhiThemes/Logs/
 * (zhithemes.log: the module's lines and this payload's "[payload]" lines).
 */

#include <onion/conflict.h>
#include <onion/proc_query.h>
#include <ps5/kernel.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <ctype.h>
#include <stddef.h>
#include <sys/proc.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#include <initializer_list>
#include <string>
#include <utility>

#include "../../shellui/include/zt_brand.hpp"

extern "C" {
bool Inject_Toolbox(int pid, uint8_t *elf);
extern uint8_t zt_shellui_elf_start[];
int sceKernelSendNotificationRequest(int, void *, size_t, int);
}

namespace {

constexpr const char *kDir = "/user/data/ZhiThemes";
constexpr const char *kPidFile = "/user/data/ZhiThemes/watcher.pid";
constexpr const char *kLog = "/user/data/ZhiThemes/Logs/zhithemes.log";
constexpr int kPort = 4444;

struct NotifyRequest {
  char useless1[45];
  char message[3075];
};

void notify(const char *fmt, ...) {
  NotifyRequest req = {};
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* One "[payload]" line in the module's zhithemes.log (O_APPEND: lines of
   the two processes do not mix), with its UTC time and a level letter
   (W when it failed, else I). The module cuts the file at its start; this
   only stops a runaway file when no module ever starts. */
void logf(const char *fmt, ...) {
  struct stat st;
  if (stat(kLog, &st) == 0 && st.st_size > (64 << 20))
    unlink(kLog);
  mkdir(kDir, 0777);
  mkdir("/user/data/ZhiThemes/Logs", 0777);
  const int fd = open(kLog, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd < 0)
    return;
  char text[900];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  if (n < 0)
    n = 0;
  if (n > (int)sizeof(text) - 1)
    n = sizeof(text) - 1;
  text[n] = 0;
  for (int i = 0; i < n; ++i)
    if (text[i] == '\n' || text[i] == '\r')
      text[i] = ' ';
  const bool bad = strstr(text, "fail") || strstr(text, "FAIL") || strstr(text, "cannot") ||
                   strstr(text, "error") || strstr(text, "WARNING") || strstr(text, "still missing") ||
                   strstr(text, "safe mode") || strstr(text, "two modules");
  struct timespec now = {};
  clock_gettime(CLOCK_REALTIME, &now);
  struct tm at = {};
  gmtime_r(&now.tv_sec, &at);
  char line[1000];
  const int len = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d %c  [payload] %s\n",
                           at.tm_year + 1900, at.tm_mon + 1, at.tm_mday, at.tm_hour, at.tm_min,
                           at.tm_sec, (int)(now.tv_nsec / 1000000), bad ? 'W' : 'I', text);
  if (len > 0)
    (void)write(fd, line, len < (int)sizeof(line) ? len : (int)sizeof(line) - 1);
  /* always on the disk at once: few lines, and the ones around an
     injection are the ones a full-console crash would lose */
  fsync(fd);
  close(fd);
}

/* One HTTP request to the module in ShellUI; returns the body ("" = no
   answer, nothing listening). */
std::string ask_module(const char *method, const char *path) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return "";
  timeval tv = {3, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  std::string out;
  if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
    char req[160];
    const int n = snprintf(req, sizeof(req),
                           "%s %s HTTP/1.0\r\nContent-Length: 0\r\n\r\n",
                           method, path);
    (void)send(fd, req, n, 0);
    char buf[512];
    ssize_t got;
    while ((got = recv(fd, buf, sizeof(buf), 0)) > 0)
      out.append(buf, (size_t)got);
  }
  close(fd);
  const size_t body = out.find("\r\n\r\n");
  return body == std::string::npos ? out : out.substr(body + 4);
}

/* A module already in ShellUI leaves (hooks out, port closed). False when
   something answers that cannot leave (an old OnionHEN theme build). */
bool replace_running_module() {
  const std::string version = ask_module("GET", "/version");
  if (version.empty()) {
    logf("no module answering on :%d", kPort);
    return true;
  }
  logf("running module: '%s'", version.c_str());
  const std::string bye = ask_module("POST", "/quit");
  logf("quit -> '%s'", bye.c_str());
  if (bye.rfind("bye", 0) != 0)
    return false;
  sleep(2); /* the old server closes :4444 after answering */
  return true;
}

void stop_previous_watcher() {
  const int fd = open(kPidFile, O_RDONLY);
  if (fd >= 0) {
    char buf[16] = {};
    (void)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    const pid_t old = (pid_t)atoi(buf);
    if (old > 1 && old != getpid() && kill(old, 0) == 0) {
      kill(old, SIGKILL);
      logf("stopped previous watcher pid %d", old);
    }
  }
  const int out = open(kPidFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (out >= 0) {
    char buf[16];
    const int n = snprintf(buf, sizeof(buf), "%d", getpid());
    (void)write(out, buf, n);
    close(out);
  }
}

/* the console's address on the network (first IPv4 that is not loopback) */
std::string console_ip() {
  std::string ip;
  struct ifaddrs *list = nullptr;
  if (getifaddrs(&list) == 0) {
    for (struct ifaddrs *i = list; i && ip.empty(); i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK))
        continue;
      char buf[INET_ADDRSTRLEN] = {};
      const auto *in = reinterpret_cast<const struct sockaddr_in *>(i->ifa_addr);
      if (inet_ntop(AF_INET, &in->sin_addr, buf, sizeof(buf)) && strcmp(buf, "127.0.0.1") != 0)
        ip = buf;
    }
    freeifaddrs(list);
  }
  return ip;
}

/* how many themes wait in Install/ (the module installs them when it
   starts; same filter as zt_install.cpp). 0 = none or already installed. */
int themes_to_install() {
  int n = 0;
  if (DIR *d = opendir((std::string(kDir) + "/Install").c_str())) {
    while (dirent *e = readdir(d)) {
      const std::string f = e->d_name;
      auto ends = [&](const char *x) {
        const size_t l = strlen(x);
        return f.size() >= l && f.compare(f.size() - l, l, x) == 0;
      };
      if (f.empty() || f[0] == '.' || ends(".failed") || ends(".part") || ends(".tmp"))
        continue;
      struct stat st = {};
      if (stat((std::string(kDir) + "/Install/" + f).c_str(), &st) == 0 && S_ISREG(st.st_mode))
        ++n;
    }
    closedir(d);
  }
  return n;
}

/* "PS5 default" (Options menu > ZhiThemes) writes no_theme; a USB
   stick with ZhiThemes/no_theme does the same while it is in (safe mode) */
bool stock_mode() {
  struct stat st = {};
  if (stat((std::string(kDir) + "/no_theme").c_str(), &st) == 0)
    return true;
  /* no theme file (first run, or deleted): the module keeps ZhiThemes off
     too. The name in active_theme.txt, else Default (zt_storage.cpp);
     theme.ztheme = the old layout, moved by the module */
  {
    std::string active;
    const int fd = open((std::string(kDir) + "/active_theme.txt").c_str(), O_RDONLY);
    if (fd >= 0) {
      char nb[64] = {};
      const ssize_t got = read(fd, nb, sizeof(nb) - 1);
      close(fd);
      for (ssize_t i = 0; i < got && nb[i] != '\n' && nb[i] != '\r'; ++i)
        active += nb[i];
    }
    while (!active.empty() && isspace((unsigned char)active.back()))
      active.pop_back();
    const bool named = !active.empty() && active.find('/') == std::string::npos && active[0] != '.';
    const bool has = (named && stat((std::string(kDir) + "/Themes/" + active + ".ztheme").c_str(), &st) == 0) ||
                     stat((std::string(kDir) + "/Themes/Default.ztheme").c_str(), &st) == 0 ||
                     stat((std::string(kDir) + "/theme.ztheme").c_str(), &st) == 0;
    if (!has)
      return true;
  }
  for (int i = 0; i < 8; ++i) {
    const std::string b = "/mnt/usb" + std::to_string(i) + "/ZhiThemes/no_theme";
    if (stat(b.c_str(), &st) == 0 || stat((b + ".txt").c_str(), &st) == 0)
      return true;
  }
  return false;
}

/* the theme that will load: the name in `themeName: '...'` of theme.ztheme
   (or of theme.js, renamed by the module); "default" without one */
std::string current_theme() {
  /* Themes/<active_theme.txt>.ztheme (theme.ztheme before the
     module moves it) */
  std::string active;
  int fd = open((std::string(kDir) + "/active_theme.txt").c_str(), O_RDONLY);
  if (fd >= 0) {
    char nb[64] = {};
    const ssize_t got = read(fd, nb, sizeof(nb) - 1);
    close(fd);
    for (ssize_t i = 0; i < got && nb[i] != '\n' && nb[i] != '\r'; ++i)
      active += nb[i];
  }
  /* the theme's own name (Rename in the editor), not themeName */
  if (!active.empty() && active.find('/') == std::string::npos)
    return active;
  fd = -1;
  if (!active.empty() && active.find('/') == std::string::npos)
    fd = open((std::string(kDir) + "/Themes/" + active + ".ztheme").c_str(), O_RDONLY);
  if (fd < 0)
    fd = open((std::string(kDir) + "/theme.ztheme").c_str(), O_RDONLY);
  if (fd < 0)
    return active.empty() ? "default" : active;
  std::string text;
  char buf[4096];
  ssize_t n;
  while (text.size() < 65536 && (n = read(fd, buf, sizeof(buf))) > 0)
    text.append(buf, (size_t)n);
  close(fd);
  const size_t key = text.find("themeName:");
  if (key != std::string::npos) {
    const size_t q1 = text.find('\'', key);
    const size_t q2 = q1 == std::string::npos ? q1 : text.find('\'', q1 + 1);
    if (q2 != std::string::npos && q2 > q1 + 1 && q2 - q1 < 60)
      return text.substr(q1 + 1, q2 - q1 - 1);
  }
  return "default";
}

/* this watcher is still the one in watcher.pid. On 30/10 two watchers
   ran (the SIGKILL of stop_previous_watcher did not stop the old one): each
   ShellUI restart was logged twice and got two modules (port 4444 busy for 3
   hours, then a crash; later one of two injections failed and ShellUI fell
   18 s after). A watcher that is no longer in the file stops by itself. */
bool still_the_watcher() {
  const int fd = open(kPidFile, O_RDONLY);
  if (fd < 0)
    return true;
  char buf[16] = {};
  (void)read(fd, buf, sizeof(buf) - 1);
  close(fd);
  const pid_t in_file = (pid_t)atoi(buf);
  return in_file <= 0 || in_file == getpid();
}

/* seconds to wait after a ShellUI restart (inject_delay, set in the
   editor's Logs tab; 15 when missing) */
int inject_delay() {
  const std::string path = std::string(kDir) + "/inject_delay";
  const int fd = open(path.c_str(), O_RDONLY);
  int s = 15;
  if (fd >= 0) {
    char buf[16] = {};
    (void)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    s = atoi(buf);
  }
  return s < 5 || s > 120 ? 15 : s;
}

/* how many modules went into this ShellUI. On 28/10 and 04/11 the console crashed after the 6th injection in a row into the same
   ShellUI, with no error in the log (memory not given back on unload is the
   suspect). inject_count holds "<pid> <count> <boot time>"; a new
   ShellUI (or a reboot that reuses the pid) starts again at 1. */
constexpr const char *kCountFile = "/user/data/ZhiThemes/inject_count";
constexpr int kInjectWarn = 4;

long boot_time() {
  struct timeval tv = {};
  size_t len = sizeof(tv);
  if (sysctlbyname("kern.boottime", &tv, &len, nullptr, 0) != 0)
    return 0;
  return (long)tv.tv_sec;
}

int count_injection(pid_t shellui) {
  long pid = 0, count = 0, boot = 0;
  const int fd = open(kCountFile, O_RDONLY);
  if (fd >= 0) {
    char buf[64] = {};
    (void)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    sscanf(buf, "%ld %ld %ld", &pid, &count, &boot);
  }
  const long now_boot = boot_time();
  /* boot times within a minute: the same boot (the value can move a little) */
  const bool same = pid == (long)shellui && count > 0 &&
                    (boot == 0 || now_boot == 0 || labs(boot - now_boot) < 60);
  count = same ? count + 1 : 1;
  const int out = open(kCountFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (out >= 0) {
    char buf[64];
    const int n = snprintf(buf, sizeof(buf), "%d %ld %ld\n", (int)shellui, count, now_boot);
    (void)write(out, buf, n);
    close(out);
  }
  return (int)count;
}

long long now_ms() {
  struct timeval tv = {};
  gettimeofday(&tv, nullptr);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* kinfo_proc entries of a process (one per thread with threads=true). The
   PS5's entries can be bigger than the SDK's struct: each is stepped by its
   own ki_structsize, only the SDK's fields are read. */
std::string proc_entries(pid_t pid, bool threads) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID | (threads ? KERN_PROC_INC_THREAD : 0), (int)pid};
  size_t len = 0;
  if (sysctl(mib, 4, nullptr, &len, nullptr, 0) != 0 || len == 0)
    return std::string();
  std::string buf(len + 4096, '\0');
  len = buf.size();
  if (sysctl(mib, 4, &buf[0], &len, nullptr, 0) != 0)
    return std::string();
  buf.resize(len);
  return buf;
}

template <typename F> void each_entry(const std::string &buf, F f) {
  size_t at = 0;
  while (at + sizeof(int) <= buf.size()) {
    int size = 0;
    memcpy(&size, buf.data() + at, sizeof(int));
    if (size < (int)offsetof(struct kinfo_proc, ki_tid) + 4 || at + (size_t)size > buf.size())
      break;
    struct kinfo_proc kp;
    memset(&kp, 0, sizeof(kp));
    memcpy(&kp, buf.data() + at, (size_t)size < sizeof(kp) ? (size_t)size : sizeof(kp));
    f(kp);
    at += (size_t)size;
  }
}

/* seconds since the ShellUI process started (-1 when unknown) */
long shellui_age(pid_t pid) {
  long start = 0;
  each_entry(proc_entries(pid, false), [&](const struct kinfo_proc &kp) {
    if (!start)
      start = (long)kp.ki_start.tv_sec;
  });
  return start > 0 ? (long)time(nullptr) - start : -1;
}

/* "48 threads: run 2 sleep 45 stop 1 | flags traced stopped | umtxn 20,
   ipcwait 6, ..." : how ShellUI's threads are right now */
std::string shellui_threads(pid_t pid) {
  int total = 0, run = 0, sleep_ = 0, stop = 0, other = 0;
  long flags = 0;
  std::string waits[64];
  int counts[64] = {};
  int kinds = 0;
  each_entry(proc_entries(pid, true), [&](const struct kinfo_proc &kp) {
    ++total;
    flags |= kp.ki_flag;
    if (kp.ki_stat == SRUN)
      ++run;
    else if (kp.ki_stat == SSLEEP)
      ++sleep_;
    else if (kp.ki_stat == SSTOP)
      ++stop;
    else
      ++other;
    char w[WMESGLEN + 1] = {};
    memcpy(w, kp.ki_wmesg, WMESGLEN);
    const std::string wm = w[0] ? w : "-";
    int i = 0;
    while (i < kinds && waits[i] != wm)
      ++i;
    if (i == kinds && kinds < 64)
      waits[kinds++] = wm;
    if (i < 64)
      ++counts[i];
  });
  if (!total)
    return "gone";
  char head[160];
  snprintf(head, sizeof(head), "%d threads: run %d sleep %d stop %d other %d |%s%s%s |", total, run,
           sleep_, stop, other, (flags & P_TRACED) ? " traced" : "",
           (flags & P_STOPPED) ? " stopped" : "", (flags & (P_TRACED | P_STOPPED)) ? "" : " -");
  std::string out = head;
  for (int i = 0; i < kinds; ++i)
    out += " " + waits[i] + " " + std::to_string(counts[i]) + (i + 1 < kinds ? "," : "");
  return out;
}

/* the first seconds after an injection: ShellUI's threads whenever they
   change (an old ShellUI froze right after the module started on 13.60) */
void watch_shellui(pid_t pid, int seconds) {
  std::string last;
  const long long end = now_ms() + seconds * 1000LL;
  int step = 0;
  while (now_ms() < end) {
    const std::string now = shellui_threads(pid);
    if (now != last) {
      logf("ShellUI pid %d +%lld ms: %s", pid, now_ms() - (end - seconds * 1000LL), now.c_str());
      last = now;
    }
    if (now == "gone" || onion_find_pid("SceShellUI") != pid)
      return;
    usleep(step++ < 20 ? 100 * 1000 : 1000 * 1000);
  }
}

/* Another HEN or payload that puts its own module into ShellUI (etaHEN,
   OnionHEN, AniketosHEN, ...): two modules in one ShellUI crash it, so
   ZhiThemes does not go in (OnionHEN and AniketosHEN refuse etaHEN the
   same way). Returns its name, or nullptr. */
const char *foreign_payload() {
  if (const char *c = onion_conflict_detect())
    return c;
  /* OnionHEN / AniketosHEN: their module also lives in ShellUI */
  static const char *const kOnion[] = {"onion_daemon.elf", "onion_util.elf",
                                       "onion_daemon", "onion_util"};
  for (const char *name : kOnion)
    if (onion_find_pid(name) > 0)
      return "OnionHEN / AniketosHEN";
  return nullptr;
}

bool sprx_loaded(pid_t pid, const char *name) {
  uint32_t handle = (uint32_t)-1;
  return kernel_dynlib_handle(pid, name, &handle) == 0 && handle != (uint32_t)-1;
}

/* A ShellUI that is still starting: wait until it has its trophy modules
   (kstuff patches ShellUI once they are in), then 10 s more. A ShellUI that
   is up is not kept waiting (same rule as AniketosHEN). False when that
   ShellUI is gone meanwhile. */
bool wait_shellui_started(pid_t pid) {
  static const char *const kReady[] = {"libSceNpTrophy.sprx", "libSceNpTrophy2.sprx"};
  bool waited = false;
  for (const char *name : kReady) {
    int i = 0;
    for (; i < 60 && !sprx_loaded(pid, name); ++i) {
      if (onion_find_pid("SceShellUI") != pid)
        return false;
      waited = true;
      usleep(500 * 1000);
    }
    if (i == 60)
      logf("ShellUI pid %d: %s still missing after 30 s", pid, name);
  }
  if (waited) {
    logf("ShellUI pid %d was still starting; waiting 10 s more", pid);
    sleep(10);
    if (onion_find_pid("SceShellUI") != pid)
      return false;
  }
  return true;
}

/* The PS5 web browser (a payload manager page) open while ZhiThemes goes
   into ShellUI: ShellUI froze a few ms after the module started and the
   system killed it (seen on 9.60 and 13.60, only when the ELF was sent
   from the browser; from a PC or an autoloader it always worked).
   Only the page process counts: SceNKUIProcess and SceNKNetworkProcess
   stay alive after the browser is closed.
   Returns the name of the browser process still running, or nullptr. */
const char *browser_process() {
  static const char kPage[] = "SceNKWebProcess";
  return onion_find_pid(kPage) > 0 ? kPage : nullptr;
}

/* wait until the browser is closed before injecting. False when a newer
   watcher took over meanwhile. */
bool wait_browser_closed(const char *when) {
  const char *open_now = browser_process();
  if (!open_now)
    return true;
  logf("%s: the web browser is open (%s); waiting until it is closed before injecting", when,
       open_now);
  notify("ZhiThemes will load when you leave the payload manager.");
  const long long t0 = now_ms();
  for (int i = 0; browser_process(); ++i) {
    usleep(250 * 1000);
    if (i % 4 == 3 && !still_the_watcher()) {
      logf("watcher %d: a newer watcher took over, stopping", getpid());
      return false;
    }
  }
  logf("web browser closed after %lld ms", now_ms() - t0);
  usleep(500 * 1000);
  return still_the_watcher();
}

/* The browser's UI and network processes stay alive after the browser is
   closed, and a ShellUI that still has them froze right after the module
   started (a ShellUI where the browser was never opened never did). Close
   them before injecting; the browser starts new ones when it is opened
   again. */
void close_browser_helpers() {
  static const char *const kHelpers[] = {"SceNKUIProcess", "SceNKNetworkProcess"};
  bool any = false;
  for (const char *name : kHelpers) {
    const pid_t pid = onion_find_pid(name);
    if (pid <= 0)
      continue;
    any = true;
    const int rc = kill(pid, SIGKILL);
    logf("closing %s pid %d: %s", name, pid, rc == 0 ? "ok" : strerror(errno));
  }
  if (!any)
    return;
  for (int i = 0; i < 100; ++i) {
    bool left = false;
    for (const char *name : kHelpers)
      if (onion_find_pid(name) > 0)
        left = true;
    if (!left) {
      /* 0.5 s was too short once: the ShellUI still froze at the module's
         start, the same sequence working other times */
      logf("browser helpers closed after %d ms; injecting in 2 s", i * 50);
      sleep(2);
      return;
    }
    usleep(50 * 1000);
  }
  logf("browser helpers still running after 5 s");
}

/* stop when another HEN's module would share ShellUI with ours */
bool refuse_foreign(const char *when) {
  const char *other = foreign_payload();
  if (!other)
    return false;
  logf("%s: %s is running, not injecting (two modules in ShellUI crash it)", when, other);
  notify("ZhiThemes can't run together with %s.\n"
         "Restart the PS5 and load ZhiThemes without it.", other);
  return true;
}

/* diagnostic build: every process running (pid, name, threads), to see
   which HEN, payloads and system services are there */
void log_processes() {
  int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC};
  size_t len = 0;
  if (sysctl(mib, 3, nullptr, &len, nullptr, 0) != 0 || !len)
    return;
  std::string buf(len + 8192, '\0');
  len = buf.size();
  if (sysctl(mib, 3, &buf[0], &len, nullptr, 0) != 0)
    return;
  buf.resize(len);
  std::string line;
  int count = 0;
  each_entry(buf, [&](const struct kinfo_proc &kp) {
    char name[COMMLEN + 1] = {};
    memcpy(name, kp.ki_comm, COMMLEN);
    char item[64];
    snprintf(item, sizeof(item), "%d %s (%d)", (int)kp.ki_pid, name, kp.ki_numthreads);
    line += (line.empty() ? "" : ", ") + std::string(item);
    ++count;
    if (line.size() > 400) {
      logf("diag: processes: %s", line.c_str());
      line.clear();
    }
  });
  if (!line.empty())
    logf("diag: processes: %s", line.c_str());
  logf("diag: %d processes", count);
}

/* returns the injection number in this ShellUI (0 = failed) */
int inject(pid_t shellui) {
  logf("ShellUI pid %d up for %ld s; injecting", shellui, shellui_age(shellui));
  if (ZT_DIAG)
    log_processes();
  const long long t0 = now_ms();
  const bool ok = Inject_Toolbox((int)shellui, zt_shellui_elf_start);
  logf("inject into SceShellUI pid %d: %s (%lld ms)", shellui, ok ? "ok" : "FAILED",
       now_ms() - t0);
  /* diagnostic build: is that ShellUI running, stuck or stopped afterwards */
  if (ZT_DIAG)
    watch_shellui(shellui, 20);
  if (!ok)
    return 0;
  const int n = count_injection(shellui);
  logf("injection %d into this ShellUI%s", n,
       n >= kInjectWarn ? " (warning shown: restart the PS5 soon)" : "");
  return n;
}


/* ---- Updates (Options menu > ZhiThemes > Updates, the editor). The
   module in ShellUI checks GitHub, downloads and verifies the new ELF into
   Update/staged.elf. This payload then:
   - puts it in the payload manager's folder (/data/pldmgr/payloads/local/
     <dir>/: the ELF + meta.json); ShellUI cannot write there, a payload can;
   - counts ShellUI crashes with the new version: the second one puts the
     previous ELF back and starts it;
   - after kUpdateConfirmS without a crash the new version is kept.
   One file holds the state, Update/state.txt (key=value lines, shared with
   the module). ELFs: staged.elf while it is handed over, previous.elf only
   while a new version is on trial, ZhiThemes.elf only without a payload
   manager (the copy the user puts in the autoload). */
constexpr const char *kUpd = "/user/data/ZhiThemes/Update";
constexpr const char *kPldmgr = "/user/data/pldmgr/payloads/local";
constexpr int kLoaderPort = 9021;
constexpr long kUpdateConfirmS = 30 * 60;

std::string upd(const char *name) { return std::string(kUpd) + "/" + name; }

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

/* "key=value" lines */
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

bool copy_path(const std::string &from, const std::string &to) {
  const int in = open(from.c_str(), O_RDONLY);
  if (in < 0)
    return false;
  const std::string tmp = to + ".tmp";
  const int out = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if (out < 0) {
    close(in);
    return false;
  }
  static char buf[1 << 16];
  bool ok = true;
  for (;;) {
    const ssize_t n = read(in, buf, sizeof(buf));
    if (n < 0)
      ok = false;
    if (n <= 0)
      break;
    if (write(out, buf, (size_t)n) != n) {
      ok = false;
      break;
    }
  }
  close(in);
  fsync(out);
  close(out);
  if (!ok || rename(tmp.c_str(), to.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

/* the value of "key": "..." in a small JSON text ("" when missing) */
std::string json_str(const std::string &text, const char *key) {
  const size_t k = text.find(std::string("\"") + key + "\"");
  if (k == std::string::npos)
    return "";
  const size_t colon = text.find(':', k);
  const size_t q1 = colon == std::string::npos ? colon : text.find('"', colon);
  const size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
  if (q2 == std::string::npos)
    return "";
  return text.substr(q1 + 1, q2 - q1 - 1);
}

/* the same text with the string value of "key" replaced */
bool json_set_str(std::string &text, const char *key, const std::string &value) {
  const size_t k = text.find(std::string("\"") + key + "\"");
  if (k == std::string::npos)
    return false;
  const size_t colon = text.find(':', k);
  const size_t q1 = colon == std::string::npos ? colon : text.find('"', colon);
  const size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
  if (q2 == std::string::npos)
    return false;
  text.replace(q1 + 1, q2 - q1 - 1, value);
  return true;
}

/* the payload manager's entry with a ZhiThemes ELF (meta.json schema 2
   only); the newest when there are more. False when there is none. */
bool pldmgr_entry(std::string &dir, std::string &file) {
  DIR *d = opendir(kPldmgr);
  if (!d)
    return false;
  std::string best_at;
  while (dirent *e = readdir(d)) {
    if (e->d_name[0] == '.')
      continue;
    const std::string here = std::string(kPldmgr) + "/" + e->d_name;
    const std::string meta = read_text(here + "/meta.json", 16384);
    if (meta.empty())
      continue;
    const size_t sk = meta.find("\"schema\"");
    const size_t sc = sk == std::string::npos ? sk : meta.find(':', sk);
    if (sc == std::string::npos || atoi(meta.c_str() + sc + 1) != 2)
      continue;
    const std::string f = json_str(meta, "filename");
    std::string low = f;
    for (char &c : low)
      c = (char)tolower((unsigned char)c);
    if (low.rfind("zhithemes", 0) != 0 || low.size() < 5 || low.compare(low.size() - 4, 4, ".elf") != 0 ||
        f.find('/') != std::string::npos)
      continue;
    const std::string at = json_str(meta, "installed_at");
    if (dir.empty() || at > best_at) {
      dir = here;
      file = f;
      best_at = at;
    }
  }
  closedir(d);
  return !dir.empty();
}

/* the whole file to the ELF loader on this console (elfldr) */
bool send_to_loader(const std::string &path) {
  const std::string elf = read_text(path, 64u << 20);
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
  for (size_t at = 0; ok && at < elf.size();) {
    const ssize_t n = send(fd, elf.data() + at, elf.size() - at, 0);
    if (n <= 0)
      ok = false;
    else
      at += (size_t)n;
  }
  shutdown(fd, SHUT_WR);
  close(fd);
  logf("update: %s sent to the ELF loader (:%d): %s", path.c_str(), kLoaderPort, ok ? "ok" : strerror(errno));
  return ok;
}

std::string iso_now() {
  const time_t now = time(nullptr);
  struct tm tm = {};
  gmtime_r(&now, &tm);
  char b[40];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%S+0000", &tm);
  return b;
}

std::string state() { return read_text(upd("state.txt")); }

/* some keys of Update/state.txt changed ("" removes the key) */
void state_set(std::initializer_list<std::pair<const char *, std::string>> keys) {
  const std::string text = state();
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

/* seconds without a crash before a new version is kept; for tests,
   confirm_minutes=N (1-60) in state.txt makes it shorter */
long confirm_seconds() {
  const int m = atoi(kv(state(), "confirm_minutes").c_str());
  return m >= 1 && m <= 60 ? m * 60L : kUpdateConfirmS;
}

/* the files of the first version of the updater: all in state.txt now */
void drop_old_update_files() {
  static const char *const kOld[] = {"staged.txt", "pending.txt", "installed.txt", "previous.txt",
                                     "previous_meta.json", ".current.elf", ".current.txt", "rolledback.txt",
                                     "failed.txt", "notified.txt", "auto_off", "confirm_minutes"};
  for (const char *f : kOld)
    unlink(upd(f).c_str());
}

/* Update/staged.elf into the payload manager (or Update/ZhiThemes.elf when
   there is none). Returns the new version ("" = nothing done). */
std::string install_staged() {
  const std::string st = state();
  const std::string version = kv(st, "staged_version"), build = kv(st, "staged_build");
  const std::string from_v = kv(st, "staged_from_version"), from_b = kv(st, "staged_from_build");
  struct stat sb = {};
  if (version.empty() || atoi(build.c_str()) <= 0 || stat(upd("staged.elf").c_str(), &sb) != 0) {
    logf("update: staged update incomplete, removed");
    state_set({{"staged_version", ""}, {"staged_build", ""}, {"staged_from_version", ""},
               {"staged_from_build", ""}, {"staged_mode", ""}});
    unlink(upd("staged.elf").c_str());
    return "";
  }
  std::string safe;
  for (char c : version)
    safe += (isalnum((unsigned char)c) || c == '.' || c == '-') ? c : '_';
  const std::string name = "ZhiThemes-" + safe + ".elf";
  std::string dir, old_file, old_at, where = "none", cur_v, cur_b;
  bool rollback = false, pldmgr = false;
  if (pldmgr_entry(dir, old_file)) {
    const std::string meta_path = dir + "/meta.json";
    std::string meta = read_text(meta_path, 16384);
    old_at = json_str(meta, "installed_at");
    const bool saved = copy_path(dir + "/" + old_file, upd("previous.elf"));
    if (!saved)
      logf("update: cannot keep the old ELF %s/%s", dir.c_str(), old_file.c_str());
    if (!copy_path(upd("staged.elf"), dir + "/" + name)) {
      logf("update: cannot write %s/%s (%s)", dir.c_str(), name.c_str(), strerror(errno));
    } else {
      json_set_str(meta, "filename", name);
      json_set_str(meta, "installed_at", iso_now());
      if (!write_text(meta_path, meta)) {
        logf("update: cannot write %s; old ELF kept", meta_path.c_str());
        if (old_file != name)
          unlink((dir + "/" + name).c_str());
      } else {
        if (old_file != name)
          unlink((dir + "/" + old_file).c_str());
        where = dir + "/" + name;
        rollback = saved;
        pldmgr = true;
        logf("update: payload manager entry %s: %s -> %s", dir.c_str(), old_file.c_str(), name.c_str());
      }
    }
  }
  if (!pldmgr) {
    dir.clear();
    old_file.clear();
    /* the way back: Update/ZhiThemes.elf, kept by the last update (only
       the loader is told; the user's autoload keeps its own file) */
    const bool have_current = !from_b.empty() && kv(st, "cur_build") == from_b &&
                              stat(upd("ZhiThemes.elf").c_str(), &sb) == 0;
    rollback = have_current && copy_path(upd("ZhiThemes.elf"), upd("previous.elf"));
    if (copy_path(upd("staged.elf"), upd("ZhiThemes.elf"))) {
      where = upd("ZhiThemes.elf");
      cur_v = version;
      cur_b = build;
    }
    logf("update: no payload manager entry; new ELF kept as %s; previous version %s", where.c_str(),
         rollback ? "kept (from the last update)" : "unknown (sent by hand: no going back)");
  }
  if (!rollback)
    unlink(upd("previous.elf").c_str());
  state_set({{"staged_version", ""}, {"staged_build", ""}, {"staged_from_version", ""}, {"staged_from_build", ""},
             {"staged_mode", ""}, {"inst_version", version}, {"inst_build", build}, {"inst_where", where},
             {"inst_pldmgr", pldmgr ? "1" : "0"}, {"pend_version", version}, {"pend_build", build},
             {"pend_from_version", from_v}, {"pend_crashes", "0"}, {"pend_rollback", rollback ? "1" : "0"},
             {"prev_dir", dir}, {"prev_file", old_file}, {"prev_new", pldmgr ? name : ""}, {"prev_at", old_at},
             {"prev_version", from_v}, {"prev_build", from_b}, {"cur_version", cur_v}, {"cur_build", cur_b},
             {"rolled_version", ""}, {"rolled_previous", ""}});
  unlink(upd("staged.elf").c_str());
  logf("update: ZhiThemes %s (build %s) installed (%s)", version.c_str(), build.c_str(), where.c_str());
  return version;
}

/* the new version ran long enough: the way back is not needed any more */
void keep_new_version() {
  state_set({{"pend_version", ""}, {"pend_build", ""}, {"pend_from_version", ""}, {"pend_crashes", ""},
             {"pend_rollback", ""}, {"prev_dir", ""}, {"prev_file", ""}, {"prev_new", ""}, {"prev_at", ""},
             {"prev_version", ""}, {"prev_build", ""}});
  unlink(upd("previous.elf").c_str());
}

/* the second crash with a new version: the previous ELF back in the
   payload manager and started. True when it was started (this payload
   then stops). */
bool roll_back() {
  const std::string st = state();
  const std::string version = kv(st, "pend_version"), from = kv(st, "pend_from_version");
  const std::string dir = kv(st, "prev_dir"), file = kv(st, "prev_file"), now_file = kv(st, "prev_new");
  struct stat sb = {};
  const bool can = kv(st, "pend_rollback") == "1" && stat(upd("previous.elf").c_str(), &sb) == 0;
  bool back = false;
  std::string start = upd("previous.elf"), cur_v = kv(st, "cur_version"), cur_b = kv(st, "cur_build");
  if (can && dir.empty()) {
    /* no payload manager: started now, and the current copy again */
    back = copy_path(upd("previous.elf"), upd("ZhiThemes.elf"));
    if (back) {
      start = upd("ZhiThemes.elf");
      cur_v = kv(st, "prev_version");
      cur_b = kv(st, "prev_build");
    }
  } else if (can && !file.empty() && copy_path(upd("previous.elf"), dir + "/" + file)) {
    std::string meta = read_text(dir + "/meta.json", 16384);
    json_set_str(meta, "filename", file);
    if (!kv(st, "prev_at").empty())
      json_set_str(meta, "installed_at", kv(st, "prev_at"));
    if (!meta.empty() && write_text(dir + "/meta.json", meta)) {
      if (!now_file.empty() && now_file != file)
        unlink((dir + "/" + now_file).c_str());
      back = true;
      start = dir + "/" + file;
    }
  }
  unlink(upd("previous.elf").c_str());
  state_set({{"failed_build", kv(st, "pend_build")}, {"rolled_version", version},
             {"rolled_previous", back ? from : ""}, {"pend_version", ""}, {"pend_build", ""},
             {"pend_from_version", ""}, {"pend_crashes", ""}, {"pend_rollback", ""}, {"prev_dir", ""},
             {"prev_file", ""}, {"prev_new", ""}, {"prev_at", ""}, {"prev_version", ""}, {"prev_build", ""},
             {"inst_version", ""}, {"inst_build", ""}, {"inst_where", ""}, {"inst_pldmgr", ""},
             {"cur_version", cur_v}, {"cur_build", cur_b}});
  logf("update: ZhiThemes %s crashed ShellUI twice; previous version %s", version.c_str(),
       back ? ("put back: " + from).c_str() : "not available (safe mode)");
  if (!back) {
    notify("ZhiThemes %s crashed twice. Theme off (safe mode).\n"
           "Send your previous ZhiThemes.elf, then Options > ZhiThemes > Updates to report it.",
           version.c_str());
    return false;
  }
  notify("ZhiThemes %s crashed twice, so ZhiThemes %s is back.\n"
         "Options > ZhiThemes > Updates: try again or report the problem.",
         version.c_str(), from.empty() ? "(previous)" : from.c_str());
  if (send_to_loader(start))
    return true;
  notify("ZhiThemes: restart the PS5 to load the previous version.");
  return false;
}

} // namespace

int main() {
  mkdir("/user/data", 0777);
  mkdir(kDir, 0777);
  /* logs/ is now Logs/ (the module moves what is left) */
  rename("/user/data/ZhiThemes/logs", "/user/data/ZhiThemes/Logs");
  mkdir("/user/data/ZhiThemes/Logs", 0777);
  logf("%s%s %s (build %d) by %s: payload start (pid %d)", ZT_NAME, ZT_EDITION, ZT_VERSION, ZT_BUILD,
       ZT_AUTHOR, getpid());
  if (ZT_DIAG)
    log_processes();
  stop_previous_watcher();
  mkdir(kUpd, 0777);
  /* this ELF is the update the module just downloaded and sent to the
     loader: put it in the payload manager now */
  drop_old_update_files();
  std::string updated;
  if (atoi(kv(state(), "staged_build").c_str()) == ZT_BUILD)
    updated = install_staged();
  {
    const std::string st = state();
    /* a new version on trial that is not this one: another ELF sent by hand */
    if (!kv(st, "pend_build").empty() && atoi(kv(st, "pend_build").c_str()) != ZT_BUILD) {
      logf("update: update %s on trial is not this build (%d), dropped", kv(st, "pend_version").c_str(), ZT_BUILD);
      keep_new_version();
    }
    /* "restart to use it" is done once that build (or a newer one) runs */
    if (!kv(st, "inst_build").empty() && atoi(kv(st, "inst_build").c_str()) <= ZT_BUILD)
      state_set({{"inst_version", ""}, {"inst_build", ""}, {"inst_where", ""}, {"inst_pldmgr", ""}});
  }

  pid_t shellui = onion_find_pid("SceShellUI");
  if (shellui <= 0) {
    notify("ZhiThemes: SceShellUI not found");
    return 1;
  }
  if (refuse_foreign("start"))
    return 1;
  if (!replace_running_module()) {
    notify("ZhiThemes: an older theme (OnionHEN) is active.\n"
           "Reboot without it, then send ZhiThemes again.");
    return 1;
  }
  if (!wait_browser_closed("start"))
    return 0;
  close_browser_helpers();
  shellui = onion_find_pid("SceShellUI"); /* it may have restarted meanwhile */
  if (shellui <= 0) {
    notify("ZhiThemes: SceShellUI not found");
    return 1;
  }
  if (!wait_shellui_started(shellui)) {
    shellui = onion_find_pid("SceShellUI");
    logf("ShellUI restarted while starting; now pid %d", shellui);
    if (shellui <= 0 || !wait_shellui_started(shellui)) {
      notify("ZhiThemes: SceShellUI not ready");
      return 1;
    }
  }
  const int loads = inject(shellui);
  if (!loads) {
    notify("ZhiThemes: injection failed");
    return 1;
  }
  const std::string ip = console_ip();
  char extra[200] = "";
  if (loads >= kInjectWarn)
    snprintf(extra, sizeof(extra),
             "\n\xE2\x9A\xA0 Loaded %d times without a restart: restart the PS5 before testing more.",
             loads);
  /* a theme still in Install/ is being installed by the module: the
     old theme's name would be wrong (the module tells the new one) */
  const int pending = themes_to_install();
  std::string loading;
  if (pending == 1)
    loading = "Installing the new theme from the Install folder";
  else if (pending > 1)
    loading = "Installing " + std::to_string(pending) + " themes from the Install folder";
  else if (stock_mode())
    loading = "ZhiThemes is off (PS5 default): Options on a tile > ZhiThemes to pick a theme";
  else
    loading = "Now loading " + current_theme();
  notify("%sWelcome to ZhiThemes %s by %s! \xF0\x9F\x8E\xA8\nOpen %s:%d to manage your themes.\n%s%s",
         updated.empty() ? "" : "Updated! ", ZT_VERSION, ZT_AUTHOR, ip.empty() ? "the console's IP" : ip.c_str(), kPort,
         loading.c_str(), extra);
  if (ZT_DIAG)
    notify("ZhiThemes DIAGNOSTIC build: everything goes to /data/ZhiThemes/Logs and Dumps.\n"
           "Use the normal ZhiThemes.elf for everyday use.");

  /* ShellUI restarts (rest mode, crashes): inject again into the new one.
     Safe mode: two restarts within 3 minutes look like the theme crashing
     ShellUI, so the watcher stops injecting (no crash loop) until a new
     ZhiThemes.elf is sent. */
  time_t last_restart = 0;
  time_t calm_since = time(nullptr); /* last start or ShellUI restart */
  for (;;) {
    sleep(3);
    if (!still_the_watcher()) {
      logf("watcher %d: a newer watcher took over, stopping", getpid());
      return 0;
    }
    /* an update the module could not start at once (too many loads into
       this ShellUI, or the loader did not answer): into the payload
       manager now, it runs after the next restart */
    {
      struct stat sb = {};
      const std::string st = state();
      if (!kv(st, "staged_build").empty() && stat(upd("staged.elf").c_str(), &sb) == 0 &&
          (kv(st, "staged_mode") == "restart" || time(nullptr) - sb.st_mtime > 90)) {
        const std::string v = install_staged();
        if (!v.empty())
          notify("ZhiThemes %s is installed.\nRestart the PS5 to use it.", v.c_str());
      }
    }
    /* the new version ran long enough without a crash: kept */
    {
      const std::string st = state();
      if (!kv(st, "pend_build").empty() && atoi(kv(st, "pend_build").c_str()) == ZT_BUILD &&
          time(nullptr) - calm_since >= confirm_seconds()) {
        keep_new_version();
        logf("update: ZhiThemes %s kept (%ld min without a ShellUI crash)", ZT_VERSION, confirm_seconds() / 60);
      }
    }
    const pid_t now = onion_find_pid("SceShellUI");
    if (now <= 0 || now == shellui)
      continue;
    logf("SceShellUI changed %d -> %d", shellui, now);
    shellui = now;
    calm_since = time(nullptr);
    /* a ShellUI crash with a new version: the second one goes back to the
       previous version (or safe mode when there is none) */
    {
      const std::string st = state();
      if (!kv(st, "pend_build").empty() && atoi(kv(st, "pend_build").c_str()) == ZT_BUILD) {
        const int crashes = atoi(kv(st, "pend_crashes").c_str()) + 1;
        logf("update: ShellUI crash %d with the new version %s", crashes, ZT_VERSION);
        if (crashes >= 2) {
          roll_back();
          return 0;
        }
        state_set({{"pend_crashes", std::to_string(crashes)}});
      }
    }
    const time_t t = time(nullptr);
    if (last_restart && t - last_restart < 180) {
      logf("safe mode: ShellUI restarted twice in %ld s, not injecting",
           (long)(t - last_restart));
      notify("ZhiThemes: ShellUI crashed twice in a row.\n"
             "Theme off (safe mode). Send ZhiThemes.elf again.");
      return 0;
    }
    last_restart = t;
    const int wait = inject_delay();
    logf("waiting %d s before injecting", wait);
    sleep(wait); /* let the new ShellUI load its libraries first */
    if (!still_the_watcher()) {
      logf("watcher %d: a newer watcher took over, stopping", getpid());
      return 0;
    }
    if (onion_find_pid("SceShellUI") != shellui)
      continue;
    /* a module already answering in this ShellUI: never a second one */
    const std::string running = ask_module("GET", "/version");
    if (!running.empty()) {
      logf("module already running ('%s'), not injecting", running.substr(0, 80).c_str());
      continue;
    }
    if (refuse_foreign("restart"))
      return 0;
    if (!wait_browser_closed("restart"))
      return 0;
    close_browser_helpers();
    /* a ShellUI restart while the browser was open is not ours: no safe
       mode count, just let the new one start first */
    const pid_t after = onion_find_pid("SceShellUI");
    if (after != shellui) {
      logf("SceShellUI changed %d -> %d while waiting for the browser", shellui, after);
      shellui = after;
      last_restart = 0;
      if (shellui <= 0)
        continue;
      sleep(inject_delay());
      if (onion_find_pid("SceShellUI") != shellui)
        continue;
    }
    if (!wait_shellui_started(shellui))
      continue;
    (void)inject(shellui);
  }
  return 0;
}
