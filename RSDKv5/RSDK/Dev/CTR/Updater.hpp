#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "RSDK/Dev/CTR/UpdateManifest.hpp"
typedef enum UpdateState {
  UPDATE_IDLE, UPDATE_CHECKING, UPDATE_CURRENT, UPDATE_AVAILABLE,
  UPDATE_EMPTY, UPDATE_DOWNLOADING, UPDATE_VERIFYING, UPDATE_INSTALLING,
  UPDATE_DONE, UPDATE_ERROR
} UpdateState;
typedef struct UpdateStatus {
  UpdateState state;
  bool prerelease;
  unsigned progress, revision;
  char version[48], message[80];
} UpdateStatus;
// The in-game updater (from ../zelda-tmc-3ds platform/3ds/source/updater.c): looks for a newer release of this port
// on GitHub, downloads its CIA (or 3DSX, for the Homebrew Launcher), checks its SHA-256 and installs it over the
// running title, which then starts again into it (Port::RequestRelaunch). The channel is Port::betas.
// autoCheck: look for an update now (the player's AUTO UPDATE); a newer build then raises the prompt below.
void Updater_Init(const char *launch_path, bool autoCheck);
void Updater_Check(void);
void Updater_SetChannel(bool prerelease);
void Updater_Download(void);
void Updater_Cancel(void);
void Updater_GetStatus(UpdateStatus *out);
bool Updater_Busy(void);
bool Updater_ShouldClose(void);
void Updater_Shutdown(void);

// The prompt drawn over every bottom-screen tab after the check made at boot found a newer build (BottomUi).
// Nothing installs by itself: ASK is a question (YES/NO), WORKING the download and install progress, RESTART the
// 10 s wait before the game starts again into the new build, ERROR an OK.
typedef enum UpdatePrompt { UPDATE_PROMPT_NONE, UPDATE_PROMPT_ASK, UPDATE_PROMPT_WORKING, UPDATE_PROMPT_RESTART, UPDATE_PROMPT_ERROR } UpdatePrompt;
UpdatePrompt Updater_AutoPrompt(void);
// YES/NO of ASK; NO of WORKING cancels the download; OK of ERROR (either answer).
void Updater_AutoAnswer(bool yes);

unsigned Updater_GetNotes(char *out, unsigned capacity);
