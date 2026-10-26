#include "RSDK/Dev/CTR/DspFirm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __3DS__
#include <3ds.h>
#endif

// ---- SHA-256 (the segments' hashes; no library dependency) ----

typedef struct { uint32_t h[8]; uint8_t block[64]; uint64_t total; size_t fill; } Sha256;

static const uint32_t kK[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void Sha256Block(Sha256 *s, const uint8_t *p) {
  uint32_t w[64], a, b, c, d, e, f, g, h;
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

static void Sha256Init(Sha256 *s) {
  static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
  memcpy(s->h, iv, sizeof(iv));
  s->total = 0, s->fill = 0;
}

static void Sha256Update(Sha256 *s, const uint8_t *p, size_t n) {
  s->total += n;
  while (n) {
    size_t take = 64 - s->fill < n ? 64 - s->fill : n;
    memcpy(s->block + s->fill, p, take);
    s->fill += take, p += take, n -= take;
    if (s->fill == 64) Sha256Block(s, s->block), s->fill = 0;
  }
}

static void Sha256Final(Sha256 *s, uint8_t out[32]) {
  uint64_t bits = s->total * 8;
  uint8_t pad = 0x80;
  Sha256Update(s, &pad, 1);
  pad = 0;
  while (s->fill != 56) Sha256Update(s, &pad, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
  Sha256Update(s, len, 8);
  for (int i = 0; i < 8; i++)
    out[i * 4] = s->h[i] >> 24, out[i * 4 + 1] = s->h[i] >> 16, out[i * 4 + 2] = s->h[i] >> 8, out[i * 4 + 3] = s->h[i];
}

// ---- The firmware's layout ----
//
//   0x000  RSA signature (0x100 bytes)
//   0x100  "DSP1", u32 size of the whole file, ..., u8 number of segments at 0x10E
//   0x120  up to 10 segments of 0x30 bytes: u32 file offset, u32 DSP address, u32 size, 3 pad,
//          u8 memory type, u8 sha256[32] of the segment's bytes

enum { kMagicAt = 0x100, kNumSegAt = 0x10E, kSegAt = 0x120, kSegSize = 0x30, kMaxSegs = 10 };

static uint32_t Le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int DspFirm_ValidateBuffer(const uint8_t *buf, size_t len) {
  if (len < kSegAt || memcmp(buf + kMagicAt, "DSP1", 4) != 0) return 0;
  if (Le32(buf + kMagicAt + 4) != len) return 0;
  unsigned n = buf[kNumSegAt];
  if (n == 0 || n > kMaxSegs) return 0;
  for (unsigned i = 0; i < n; i++) {
    const uint8_t *seg = buf + kSegAt + i * kSegSize;
    uint32_t off = Le32(seg), size = Le32(seg + 8);
    if (off > len || size > len - off) return 0;
    Sha256 s;
    uint8_t digest[32];
    Sha256Init(&s);
    Sha256Update(&s, buf + off, size);
    Sha256Final(&s, digest);
    if (memcmp(digest, seg + 16, 32) != 0) return 0;
  }
  return 1;
}

static uint8_t *ReadWhole(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  uint8_t *buf = NULL;
  if (fseek(f, 0, SEEK_END) == 0) {
    long n = ftell(f);
    if (n > 0 && n <= (8 << 20) && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n))) {
      if (fread(buf, 1, (size_t)n, f) == (size_t)n) *len = (size_t)n;
      else free(buf), buf = NULL;
    }
  }
  fclose(f);
  return buf;
}

DspFirmStatus DspFirm_Check(const char *path) {
  if (access(path, F_OK) != 0) return DSPFIRM_MISSING;
  size_t len = 0;
  uint8_t *buf = ReadWhole(path, &len);
  if (!buf) return DSPFIRM_INVALID;
  int ok = DspFirm_ValidateBuffer(buf, len);
  free(buf);
  return ok ? DSPFIRM_OK : DSPFIRM_INVALID;
}

char g_dspfirm_trace[400];

const char *DspFirm_ExtractText(DspFirmExtractResult r) {
  switch (r) {
    case DSPFIRM_EX_OK: return "Done.";
    case DSPFIRM_EX_OPEN: return "Could not open the console's Home Menu.";
    case DSPFIRM_EX_READ: return "Could not read the Home Menu.";
    case DSPFIRM_EX_NOTFOUND: return "No DSP firmware found in the Home Menu.";
    case DSPFIRM_EX_WRITE: return "Could not write the file to the SD card.";
    case DSPFIRM_EX_VERIFY: return "The written file did not check out.";
  }
  return "";
}

#ifdef __3DS__

// The firmware travels inside the Home Menu: its ExeFS ".code" (compressed, a backward LZ) holds the
// image DSP services are fed, one Home Menu title per region (the same place the DSP1 dumper reads).
static const u64 kHomeMenuTitles[] = {
  0x0004003000008F02ULL,   // USA
  0x0004003000009802ULL,   // EUR
  0x0004003000008202ULL,   // JPN
  0x000400300000A102ULL,   // CHN
  0x000400300000A902ULL,   // KOR
  0x000400300000B102ULL,   // TWN
};

static bool g_code_linear;   // where the last code buffer came from
static void FreeCode(uint8_t *p) { if (g_code_linear) linearFree(p); else free(p); }

// Undoes the ExeFS "backward LZ" in place: `buf` holds the compressed `len` bytes and has room for
// `len + extra` (the footer's last 4 bytes). Returns the decompressed size, 0 if it is not valid.
static size_t BackwardLzInPlace(uint8_t *buf, size_t len, size_t extra) {
  const uint32_t enc = buf[len - 8] | buf[len - 7] << 8 | buf[len - 6] << 16;
  const unsigned hdr = buf[len - 5];
  if (enc > len || hdr < 8 || hdr > enc) return 0;
  memset(buf + len, 0, extra);
  size_t src = len - hdr, dst = len + extra;
  const size_t stop = len - enc;
  unsigned mask = 0, flags = 0;
  while (src > stop) {
    if ((mask >>= 1) == 0) {
      flags = buf[--src];
      mask = 0x80;
      if (src <= stop) break;
    }
    if (flags & mask) {
      if (src < stop + 2) return 0;
      src -= 2;
      const unsigned pair = buf[src] | buf[src + 1] << 8;
      const size_t n = (pair >> 12) + 3, disp = (pair & 0xFFF) + 3;
      if (dst < n || dst + disp > len + extra) return 0;
      for (size_t i = 0; i < n; i++, dst--) buf[dst - 1] = buf[dst - 1 + disp];
    } else {
      if (dst == 0) return 0;
      buf[--dst] = buf[--src];
    }
  }
  return len + extra;
}

// Opens the Home Menu's ".code", reads it (with room to decompress it in place) and decompresses it.
// `detail`: the system result of the last title tried, or the size that did not fit in memory.
static DspFirmExtractResult ReadHomeMenuCode(uint8_t **out, size_t *out_len, int32_t *detail) {
  Result last = 0;
  size_t tn = 0;
  g_dspfirm_trace[0] = 0;
  for (size_t t = 0; t < sizeof(kHomeMenuTitles) / sizeof(kHomeMenuTitles[0]); t++) {
    const u64 title = kHomeMenuTitles[t];
    u32 archive_path[4] = { (u32)title, (u32)(title >> 32), MEDIATYPE_NAND, 0 };
    u32 file_path[5] = { 0, 0, 2 /* ExeFS */, 0x646F632E /* ".cod" */, 0x65 /* "e" */ };
    // fsMakePath leaves a binary path's size at 0: set both by hand.
    const FS_Path archive = { PATH_BINARY, sizeof(archive_path), archive_path };
    const FS_Path file = { PATH_BINARY, sizeof(file_path), file_path };
    Handle fh;
    Result rc = FSUSER_OpenFileDirectly(&fh, ARCHIVE_SAVEDATA_AND_CONTENT, archive, file, FS_OPEN_READ, 0);
    if (R_FAILED(rc)) {
      last = rc;
      tn += snprintf(g_dspfirm_trace + tn, sizeof(g_dspfirm_trace) - tn, "%04X open %08lX; ", (unsigned)(title & 0xFFFF), (unsigned long)rc);
      continue;
    }

    u64 size = 0;
    u32 got = 0;
    rc = FSFILE_GetSize(fh, &size);
    if (R_SUCCEEDED(rc) && (size < 16 || size > (16 << 20))) rc = -1;
    // One buffer: the file is read at offset 0 in one go (the only read this archive answers: reads at any other
    // offset failed with E0C046F8) with room for what it grows to. The footer says how much that is, and it is only
    // known after the read, so the room is a guess (twice the file; this one grows to 1.7 times) checked afterwards.
    int step = 2;   // what failed, for the trace: 2 file read, 3 not enough room
    const size_t cap = (size_t)size * 2;
    uint8_t *buf = R_SUCCEEDED(rc) ? linearAlloc(cap) : NULL;
    g_code_linear = buf != NULL;
    if (R_SUCCEEDED(rc) && !buf) buf = malloc(cap);   // a game with little linear memory left reads into the heap
    if (R_SUCCEEDED(rc) && !buf) { FSFILE_Close(fh); *detail = (int32_t)cap; return DSPFIRM_EX_READ; }
    if (R_SUCCEEDED(rc)) rc = FSFILE_Read(fh, &got, 0, buf, (u32)size);
    FSFILE_Close(fh);
    uint32_t extra = 0;
    if (R_SUCCEEDED(rc) && got == size) {
      const uint8_t *footer = buf + size - 8;
      extra = footer[4] | footer[5] << 8 | footer[6] << 16 | (uint32_t)footer[7] << 24;
      if ((size_t)size + extra > cap) rc = -3, step = 3, last = (Result)((size_t)size + extra);
    }
    if (R_FAILED(rc) || got != size) {   // opening does not fail for a title this console does not have: reading does
      if (buf) FreeCode(buf);
      if (rc != -3) last = R_FAILED(rc) ? rc : -1;
      tn += snprintf(g_dspfirm_trace + tn, sizeof(g_dspfirm_trace) - tn, "%04X read%d %08lX size %llu %s; ", (unsigned)(title & 0xFFFF), step, (unsigned long)last, (unsigned long long)size, g_code_linear ? "linear" : "heap");
      continue;
    }
    size_t total = BackwardLzInPlace(buf, (size_t)size, extra);
    if (!total) { FreeCode(buf); *detail = 0; return DSPFIRM_EX_READ; }
    *out = buf, *out_len = total;
    return DSPFIRM_EX_OK;
  }
  *detail = (int32_t)last;
  return DSPFIRM_EX_OPEN;
}

DspFirmExtractResult DspFirm_Extract(const char *path, int32_t *detail) {
  int32_t dummy;
  if (!detail) detail = &dummy;
  *detail = 0;
  uint8_t *code = NULL;
  size_t code_len = 0;
  DspFirmExtractResult r = ReadHomeMenuCode(&code, &code_len, detail);
  if (r != DSPFIRM_EX_OK) return r;

  // Find the header: "DSP1" with the whole image's size after it, 0x100 bytes into the image.
  const uint8_t *fw = NULL;
  size_t fw_len = 0;
  for (size_t p = kMagicAt; p + 8 <= code_len; p += 4) {
    if (memcmp(code + p, "DSP1", 4) != 0) continue;
    size_t len = Le32(code + p + 4);
    if (len > code_len - (p - kMagicAt)) continue;
    if (DspFirm_ValidateBuffer(code + p - kMagicAt, len)) { fw = code + p - kMagicAt, fw_len = len; break; }
  }
  if (!fw) {
    *detail = (int32_t)code_len;
    FreeCode(code);
    return DSPFIRM_EX_NOTFOUND;
  }

  char tmp[300];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  mkdir("sdmc:/3ds", 0777);
  FILE *f = fopen(tmp, "wb");
  bool ok = f && fwrite(fw, 1, fw_len, f) == fw_len && fflush(f) == 0 && fsync(fileno(f)) == 0;
  if (!ok) *detail = errno;
  if (f && fclose(f) != 0 && ok) ok = false, *detail = errno;
  FreeCode(code);
  if (!ok) { remove(tmp); return DSPFIRM_EX_WRITE; }
  remove(path);
  if (rename(tmp, path) != 0) { *detail = errno; remove(tmp); return DSPFIRM_EX_WRITE; }
  if (DspFirm_Check(path) != DSPFIRM_OK) return DSPFIRM_EX_VERIFY;
  return DSPFIRM_EX_OK;
}

#else   // host build: the extraction needs a console

DspFirmExtractResult DspFirm_Extract(const char *path, int32_t *detail) {
  (void)path;
  if (detail) *detail = 0;
  return DSPFIRM_EX_OPEN;
}

#endif
