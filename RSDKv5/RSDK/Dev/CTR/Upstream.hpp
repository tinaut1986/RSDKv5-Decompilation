#ifndef UPSTREAM_CTR_HPP
#define UPSTREAM_CTR_HPP

// The original 3DS port (SaturnSH2x2's, title 0x3082) keeps its data in /3ds/SonicMania/; ours is /3ds/Sonic Mania 3DS/.
// As ../zelda-tmc-3ds does with its upstream port: the game's data is read from there in place when our folder has none
// (no second copy of Data.rsdk on the SD), and its saves are offered once for import.

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace Port
{

#define UPSTREAM_DIR "/3ds/SonicMania/"

// `path` (in our folder) when that file exists, else the same name in UPSTREAM_DIR when that one does; `out` gets the
// path to open. False when neither exists (`out` keeps ours).
bool UpstreamFallback(const char *path, const char *name, char *out, unsigned cap);

// At boot, before the game reads its saves: when our folder has no saves and the original port's has some, asks once
// (on the bottom screen) whether to copy them here; the answer is kept in save-import.txt.
void OfferSaveImport();

} // namespace Port
} // namespace RSDK

#endif

#endif
