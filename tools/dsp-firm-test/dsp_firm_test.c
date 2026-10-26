// Builds a firmware image the way the header describes it (hashes from `sha256sum`-equivalent
// known answers), checks the validator accepts it and refuses every damage. With a path as the
// argument it also validates that real dump (a dspfirm.cdc from a console).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "RSDK/Dev/CTR/DspFirm.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static void Put32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

// SHA-256("abc") and SHA-256("") are the standard test vectors.
static const uint8_t kAbc[32] = { 0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                                  0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
static const uint8_t kEmpty[32] = { 0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
                                    0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55 };

static size_t Build(uint8_t *b) {
  const size_t len = 0x200;
  memset(b, 0, len);
  memcpy(b + 0x100, "DSP1", 4);
  Put32(b + 0x104, (uint32_t)len);
  b[0x10E] = 2;
  uint8_t *s0 = b + 0x120, *s1 = s0 + 0x30;
  memcpy(b + 0x1F0, "abc", 3);
  Put32(s0, 0x1F0), Put32(s0 + 8, 3), memcpy(s0 + 16, kAbc, 32);
  Put32(s1, 0x1F8), Put32(s1 + 8, 0), memcpy(s1 + 16, kEmpty, 32);
  return len;
}

int main(int argc, char **argv) {
  uint8_t b[0x200], d[0x200];
  size_t len = Build(b);
  CHECK(DspFirm_ValidateBuffer(b, len));

  memcpy(d, b, len); d[0x1F1] ^= 1;          CHECK(!DspFirm_ValidateBuffer(d, len));   // a segment byte
  memcpy(d, b, len); d[0x120 + 16] ^= 1;     CHECK(!DspFirm_ValidateBuffer(d, len));   // a hash byte
  memcpy(d, b, len); d[0x100] = 'X';         CHECK(!DspFirm_ValidateBuffer(d, len));   // the magic
  CHECK(!DspFirm_ValidateBuffer(b, len - 1));                                           // truncated
  memcpy(d, b, len); d[0x10E] = 0;           CHECK(!DspFirm_ValidateBuffer(d, len));   // no segments
  memcpy(d, b, len); d[0x10E] = 11;          CHECK(!DspFirm_ValidateBuffer(d, len));   // too many
  memcpy(d, b, len); Put32(d + 0x120, 0x1FF); CHECK(!DspFirm_ValidateBuffer(d, len));  // segment past the end
  CHECK(!DspFirm_ValidateBuffer(b, 0x40));
  CHECK(DspFirm_Check("/nonexistent/dspfirm.cdc") == DSPFIRM_MISSING);

  if (argc > 1 && argv[1][0]) {
    DspFirmStatus st = DspFirm_Check(argv[1]);
    printf("%s: %s\n", argv[1], st == DSPFIRM_OK ? "valid" : st == DSPFIRM_MISSING ? "missing" : "INVALID");
    CHECK(st == DSPFIRM_OK);
  }
  if (g_fail) return 1;
  printf("dsp_firm_test: OK\n");
  return 0;
}
