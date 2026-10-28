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
    // The Special Stage has no Player: its ring counter (time drain, traps) is kept at 50 or more
    if (on && UFO_Setup && UFO_Setup->classID && UFO_Setup->rings < 50)
        UFO_Setup->rings = 50;
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
        // No drowning (the air never runs out) and no crushing (squeezed between two solids: the flags are read by
        // the player's next update, so they are cleared before it). Pits and other deaths still kill.
        p->drownTimer = 0;
        if (p->deathType == PLAYER_DEATH_DROWN)
            p->deathType = PLAYER_DEATH_NONE;
        if (p->collisionFlagH == (1 | 2))
            p->collisionFlagH = 0;
        if (p->collisionFlagV == (1 | 2))
            p->collisionFlagV = 0;
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

void PortGame_Describe(char *out, unsigned cap)
{
    unsigned n = 0;
#define ADD(...) (n += (unsigned)snprintf(out + n, n < cap ? cap - n : 0, __VA_ARGS__))
    out[0] = 0;
    if (Player && Player->classID)
        for (int i = 0; i < 4; ++i) {
            EntityPlayer *p = RSDK_GET_ENTITY(i, Player);
            if (p->classID == Player->classID)
                ADD("P%d char %d state %p input %p anim %d inv %d | ", i, p->characterID, (void *)p->state, (void *)p->stateInput,
                    p->animator.animationID, p->invincibleTimer);
        }
    if (ActClear && ActClear->classID) {
        EntityActClear *a = RSDK_GET_ENTITY(SLOT_ACTCLEAR, ActClear);
        ADD("ACTCLEAR slot class %d state %p timer %d | ", a->classID, (void *)a->state, a->timer);
    }
    if (CutsceneSeq && CutsceneSeq->classID)
        for (int i = 0; i < ENTITY_COUNT; ++i) {
            EntityCutsceneSeq *c = RSDK_GET_ENTITY(i, CutsceneSeq);
            if (c->classID == CutsceneSeq->classID)
                ADD("SEQ slot %d stateID %d timer %d current %p | ", i, c->stateID, c->timer, (void *)c->currentState);
        }
    if (PhantomRuby && PhantomRuby->classID)
        for (int i = 0; i < ENTITY_COUNT; ++i) {
            EntityPhantomRuby *r = RSDK_GET_ENTITY(i, PhantomRuby);
            if (r->classID == PhantomRuby->classID)
                ADD("RUBY slot %d act %d vis %d onScr %d alpha %d ink %d pos %d,%d state %p grp %d anim %d | ", i, r->active, r->visible,
                    r->onScreen, r->alpha, r->inkEffect, r->position.x >> 16, r->position.y >> 16, (void *)r->state, r->drawGroup,
                    r->rubyAnimator.animationID);
        }
    ADD("CAM %d,%d", ScreenInfo->position.x, ScreenInfo->position.y);
#undef ADD
}

void PortGame_SceneMode(const char *category)
{
    if (!globals)
        return;
    if (!strcmp(category, "Mania Mode"))
        globals->gameMode = MODE_MANIA;
#if MANIA_USE_PLUS
    else if (!strcmp(category, "Encore Mode"))
        globals->gameMode = MODE_ENCORE;
#endif
}

void PortGame_AirJump(int on)
{
    if (!on || !Player || !Player->classID || SceneInfo->state != ENGINESTATE_REGULAR)
        return;
    EntityPlayer *p = RSDK_GET_ENTITY(SLOT_PLAYER1, Player);
    if (p->classID != Player->classID || p->stateInput != Player_Input_P1 || p->onGround)
        return;
    if (p->state == Player_State_Death || p->state == Player_State_Drown || p->state == Player_State_Hurt)
        return;
    // The 3DS Y button (the game's X, one of the three jump buttons) held: keep rising
    if (ControllerInfo[p->controllerID].keyX.down && p->velocity.y > -0x60000)
        p->velocity.y = -0x60000;
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
