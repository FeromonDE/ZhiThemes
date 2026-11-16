// Host test for the actual PNG generator. Link with zlib and --gc-sections.
#include "../src/source/shellui/src/zt_mosaic.cpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <utime.h>
void zt_log(const char *, ...) {}
int main(int argc, char **argv) {
  assert(argc == 2);
  std::filesystem::create_directories(argv[1]);
  const std::string src = std::string(argv[1]) + "/source.png", dest = std::string(argv[1]) + "/reflection.png";
  Image icon;
  icon.w = icon.h = 64;
  icon.px.resize(64 * 64 * 4);
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      auto *p = &icon.px[(y * 64 + x) * 4];
      p[0] = (unsigned char)(y * 3); p[1] = 100; p[2] = 50; p[3] = x == 20 ? 128 : x == 21 ? 0 : 255;
    }
  assert(save_png(src.c_str(), icon));
  assert(zt_tile_reflection(src, dest, 64, 64, 32, 8));
  Image result;
  assert(load_png(dest.c_str(), result));
  assert(result.w == 64 && result.h == 32);
  assert(result.px[(32 * 4)] == 63 * 3); // bottom row is mirrored to the top
  assert(result.px[(32 * 4) + 3] == 255);
  assert(result.px[3] == 0); // mirrored rounded corner
  assert(result.px[(20 * 4) + 3] == 128); // source transparency is preserved
  unsigned previous = 255;
  for (int y = 0; y < 32; ++y) {
    assert(result.px[(y * 64 + 21) * 4 + 3] == 0); // no dither halo in transparent pixels
    const auto alpha = result.px[(y * 64 + 32) * 4 + 3];
    assert(alpha <= previous + 1); // at most one alpha level of dither
    previous = alpha;
  }
  for (int x = 0; x < 64; ++x) assert(result.px[(31 * 64 + x) * 4 + 3] == 0);
  const auto stamp = std::filesystem::last_write_time(dest);
  assert(zt_tile_reflection(src, dest, 64, 64, 32, 8));
  assert(std::filesystem::last_write_time(dest) == stamp); // cache hit does not rewrite
  const std::string square = std::string(argv[1]) + "/square.png";
  assert(zt_tile_reflection(src, square, 64, 64, 32, 0));
  Image sharp;
  assert(load_png(square.c_str(), sharp));
  assert(sharp.px[3] == 255 && sharp.px[32 * 4 + 3] == 255); // zero radius stays opaque
  assert(!zt_tile_reflection(src, dest, 2000, 64, 32, 8));
  assert(!zt_tile_reflection(src, dest, 64, 64, 65, 8));
  assert(!zt_tile_reflection(src, dest, 64, 64, 32, 33));
  for (size_t i = 0; i < icon.px.size(); i += 4) icon.px[i] = 7;
  assert(save_png(src.c_str(), icon));
  const time_t future = time(nullptr) + 2;
  struct utimbuf times{future, future};
  assert(utime(src.c_str(), &times) == 0);
  assert(zt_tile_reflection(src, dest, 64, 64, 32, 8));
  assert(load_png(dest.c_str(), result));
  assert(result.px[32 * 4] == 7); // changed source invalidates cache
  std::cout << "PNG reflection tests passed: flip, smooth alpha, corners, transparency, cache and bounds.\n";
}
