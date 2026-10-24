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

// Item boxes opened since the last call: their contents (the game's ITEMBOX_* numbering, which is also the frame of
// the contents icon in Global/ItemBox.bin animation 2), at most `max`; returns how many. `reset` after a scene load:
// the boxes already open in it (reloaded from a checkpoint) are taken as known, not as news.
int PortGame_PollItems(int *types, int max, int reset);

#ifdef __cplusplus
}
#endif

#endif
