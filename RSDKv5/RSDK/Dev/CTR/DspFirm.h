// The 3DS DSP firmware (sdmc:/3ds/dspfirm.cdc) that libctru's ndspInit() loads.
//
// It is Nintendo's code, so it is never shipped: DspFirm_Extract copies it from this console's own
// Home Menu (a system title) to the SD card, the same thing the DSP1 homebrew does. Self-contained
// on purpose (libctru only, no project headers), so other ports can take the two files as they are.
//
// Typical use at startup, before ndspInit():
//   if (DspFirm_Check(DSPFIRM_PATH) != DSPFIRM_OK) { ask the player; DspFirm_Extract(...) }
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DSPFIRM_PATH "sdmc:/3ds/dspfirm.cdc"

typedef enum {
  DSPFIRM_OK = 0,
  DSPFIRM_MISSING,   // no file
  DSPFIRM_INVALID,   // a file that is not a whole, intact DSP firmware
} DspFirmStatus;

// Checks a firmware image in memory: the "DSP1" magic, the size in the header against `len`, and the
// SHA-256 of every segment against the hash the header carries. Pure; host-testable.
int DspFirm_ValidateBuffer(const uint8_t *buf, size_t len);   // 1 = valid

// Reads `path` and validates it.
DspFirmStatus DspFirm_Check(const char *path);

typedef enum {
  DSPFIRM_EX_OK = 0,
  DSPFIRM_EX_OPEN,      // no Home Menu title could be opened (rc in *detail)
  DSPFIRM_EX_READ,      // reading or decompressing its code failed
  DSPFIRM_EX_NOTFOUND,  // no firmware inside it
  DSPFIRM_EX_WRITE,     // the SD card refused the file
  DSPFIRM_EX_VERIFY,    // what was written does not read back as a valid firmware
} DspFirmExtractResult;

// Copies the firmware out of this console's Home Menu (where the system keeps it) to `path` (written aside and renamed, so a
// failure never leaves a half file where a good one was). `detail` (may be NULL) gets the failing
// system result or errno for the log. libctru's ndspInit() may be called right after an OK.
DspFirmExtractResult DspFirm_Extract(const char *path, int32_t *detail);

// What each Home Menu title answered in the last extraction (for the log: which regions exist, what failed).
extern char g_dspfirm_trace[400];

const char *DspFirm_ExtractText(DspFirmExtractResult r);

#ifdef __cplusplus
}
#endif
