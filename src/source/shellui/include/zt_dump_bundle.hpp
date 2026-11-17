#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Validate code, not merely the unencrypted RNPS container header. */
inline bool zt_dump_plain_payload(const unsigned char *p, size_t n) {
  if (!p || n < 8) return false;
  const unsigned char hermes[] = {0xc6,0x1f,0xbc,0x03,0xc1,0x03,0x19,0x1f};
  if (!memcmp(p, hermes, 8)) {
    if (n < 128) return false;
    const uint32_t size = (uint32_t)p[32] | (uint32_t)p[33]<<8 |
                          (uint32_t)p[34]<<16 | (uint32_t)p[35]<<24;
    return size >= 128 && size <= n;
  }
  const unsigned char legacy[] = {0xe5,0xd1,0x0b,0xfb};
  return !memcmp(p, legacy, 4) || (n >= 26 && !memcmp(p, "/*! For license information", 26));
}
inline bool zt_dump_plain_bundle(const unsigned char *p, size_t n) {
  if (zt_dump_plain_payload(p,n)) return true;
  if (!p || n < 32 || memcmp(p,"RNPSHEDR",8)) return false;
  const uint32_t off = (uint32_t)p[28] | (uint32_t)p[29]<<8 |
                       (uint32_t)p[30]<<16 | (uint32_t)p[31]<<24;
  return off >= 32 && off < n && zt_dump_plain_payload(p+off,n-off);
}
