// Compiled with the game (GAME_SOURCES in the Makefile): reads the run in progress for the bottom screen.
#include "Game.h"
#include "GameInfo.h"

void PortGame_GetInfo(PortGameInfo *out)
{
    memset(out, 0, sizeof(*out));
    out->gameMode = -1;
    out->saveSlot = -1;
    out->emeralds = -1;
    if (!globals)
        return;
    out->gameMode  = globals->gameMode;
    out->character = globals->playerID & 0xFF;
    out->sidekick  = (globals->playerID >> 8) & 0xFF;
    if (globals->saveSlotID != NO_SAVE_SLOT) {
        out->saveSlot = globals->saveSlotID;
        SaveRAM *save = SaveGame_GetSaveRAM();
        if (save)
            out->emeralds = save->collectedEmeralds;
    }
    if (Player && Player->classID) {
        EntityPlayer *p = RSDK_GET_ENTITY(SLOT_PLAYER1, Player);
        if (p && p->classID == Player->classID) {
            out->playerValid = 1;
            out->character   = p->characterID;
            out->rings       = p->rings;
            out->score       = p->score;
            out->lives       = p->lives;
            out->shield      = p->shield;
            out->super       = p->superState == SUPERSTATE_SUPER;
        }
    }
}
