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
    if (globals->saveSlotID != NO_SAVE_SLOT)
        out->saveSlot = globals->saveSlotID;
    // The run's save RAM: the slot's, or with no save globals->noSaveSlot, which keeps the emeralds of that run too
    if (SaveGame && SaveGame->saveRAM)
        out->emeralds = SaveGame_GetSaveRAM()->collectedEmeralds;
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
            out->invincible  = p->invincibleTimer;
            out->speedShoes  = p->speedShoesTimer;
            out->hyperRing   = p->hyperRing;
        }
    }
}

void PortGame_Immortal(int on)
{
    static int wasOn;
    if (!Player || !Player->classID) { // no Player here (menus, Special Stage, Blue Spheres)
        wasOn = 0;
        return;
    }
    EntityPlayer *p = RSDK_GET_ENTITY(SLOT_PLAYER1, Player);
    if (p->classID != Player->classID) {
        wasOn = 0;
        return;
    }
    // The effect shows only in play. A cutscene, an act clear or a paused scene takes the player's input away and may
    // recycle his entity, and the stars made here then pointed at something that is not a player (crash in
    // InvincibleStars). Out of play it is undone as if the option were off (no timer, no stars); it comes back with
    // the input.
    bool32 active = on && SceneInfo->state == ENGINESTATE_REGULAR && p->stateInput == Player_Input_P1;

    EntityInvincibleStars *stars = NULL;
    if (InvincibleStars && InvincibleStars->classID)
        stars = RSDK_GET_ENTITY(Player->playerCount + RSDK.GetEntitySlot(p), InvincibleStars);
    bool32 starsLive = stars && stars->classID == InvincibleStars->classID;

    if (active) {
        if (p->invincibleTimer < 600) { // topped up (the item box gives 1260)
            p->invincibleTimer = 1260;
            if (stars && !starsLive && p->superState == SUPERSTATE_NONE)
                RSDK.ResetEntity(stars, InvincibleStars->classID, p);
        }
        wasOn = 1;
    }
    else if (wasOn) {
        if (p->invincibleTimer > 1)
            p->invincibleTimer = 1;
        if (starsLive)
            RSDK.ResetEntity(stars, TYPE_BLANK, NULL);
        wasOn = 0;
    }
}

// One bit per entity slot: that slot's box was already reported (or found open). Cleared again when the slot holds a
// closed box, so a box restored by a checkpoint counts anew when it is opened again.
static uint8 sBoxKnown[ENTITY_COUNT / 8 + 1];

int PortGame_PollItems(int *types, int max, int reset)
{
    if (reset)
        memset(sBoxKnown, 0, sizeof(sBoxKnown));
    if (!ItemBox || !ItemBox->classID)
        return 0;
    int n = 0;
    foreach_all(ItemBox, box)
    {
        const int slot  = RSDK.GetEntitySlot(box);
        const uint8 bit = 1 << (slot & 7);
        // The contents are given at the end of ItemBox_State_Break (a random box has its type by then)
        const bool32 opened = box->state == ItemBox_State_IconFinish || box->state == ItemBox_State_Broken;
        if (!opened)
            sBoxKnown[slot >> 3] &= ~bit;
        else if (!(sBoxKnown[slot >> 3] & bit)) {
            sBoxKnown[slot >> 3] |= bit;
#if MANIA_USE_PLUS
            if (box->type == ITEMBOX_STOCK) // Encore's character boxes: no contents icon
                continue;
#endif
            if (!reset && n < max)
                types[n++] = box->type;
        }
    }
    return n;
}
