#ifndef GAME_INFO_CTR_H
#define GAME_INFO_CTR_H

// What the bottom screen's game tab shows about the run in progress, read from the game's own state by GameInfo.c,
// which is compiled with the game (its headers, its `globals`, its objects) and linked into the same binary.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int playerValid;      // slot 0 holds a Player (not a menu, not the Special Stage's UFO_Player)
    int character;        // ID_SONIC 1, ID_TAILS 2, ID_KNUCKLES 4, ID_MIGHTY 8, ID_RAY 16 (0: unknown)
    int sidekick;         // the assist's ID (0: none)
    int rings, score, lives, shield, super;
    int gameMode;         // 0 Mania, 1 Encore, 2 Time Attack, 3 Competition (Plus numbering), -1 unknown
    int saveSlot;         // -1: no save
    int emeralds;         // bit mask of the save's chaos emeralds (-1: unknown)
    int invincible;       // frames of invincibility left (the item box's, not the hit's)
    int speedShoes;       // frames of speed shoes left
    int hyperRing;        // the hyper ring is on
} PortGameInfo;

void PortGame_GetInfo(PortGameInfo *out);

// The DEBUG tab's IMMORTAL (dev builds): called every frame. On, player 1 keeps the item box's invincibility (its
// stars too, made again when the scene has none): enemies and spikes do not hurt, touching enemies breaks them. It also
// keeps the Special Stage's rings up, and there is no drowning or crushing. Pits and other deaths still kill. Off, the invincibility runs out at once.
void PortGame_Immortal(int on);

// The DEBUG tab's AIR JUMP (dev builds): called every frame. On, player 1 in the air rises while the 3DS Y button is held
// (jumping again and again, to reach high places quickly).
void PortGame_AirJump(int on);

// SITE ENTS: the players' states, the ActClear entity and the cutscene sequences, as raw function pointers (resolve them
// with addr2line on the ELF), to find what a stuck cutscene waits for.
void PortGame_Describe(char *out, unsigned cap);

// Every frame, before the objects update, in every build: game logic quirks that cost the 3DS what a PC does not notice.
// The credits' text lines that fade out (UICreditsText_State_FadeOut) are never destroyed by the game: at alpha 0 they
// go back to SetupIdleDelay and straight to FadeOut again, for good, so the entities pile up as the credits go on and
// each is still updated and drawn (the credits ran slower and slower on the 2DS). They are destroyed here.
void PortGame_Tick(void);

// A scene picked from the STAGES list: the game mode follows its category ("Encore Mode" -> Encore, "Mania Mode" ->
// Mania), as the menus would have set it; the other categories leave it.
void PortGame_SceneMode(const char *category);

// Item boxes opened since the last call: their contents (the game's ITEMBOX_* numbering, which is also the frame of
// the contents icon in Global/ItemBox.bin animation 2), at most `max`; returns how many. `reset` after a scene load:
// the boxes already open in it (reloaded from a checkpoint) are taken as known, not as news.
int PortGame_PollItems(int *types, int max, int reset);

#ifdef __cplusplus
}
#endif

#endif
