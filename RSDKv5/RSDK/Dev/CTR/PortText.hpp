#ifndef PORT_TEXT_CTR_HPP
#define PORT_TEXT_CTR_HPP

// The port's own text (bottom screen, loading box, updater) in the game's language, as ../zelda-tmc-3ds
// (port_text_i18n.c) does it: call sites keep the English literal (upper case) and Tr looks it up. The language is the
// one the game uses (SKU::curSKU.language, which the game's options menu sets), read on every call, so the port follows
// a change made in the game at once. Korean and the two Chinese fall back to English: the system font of the western
// and Japanese consoles has no glyphs for them.

#if RETRO_PLATFORM == RETRO_3DS

namespace RSDK
{
namespace Port
{

// The game's language as one of the port's columns: LANGUAGE_EN..LANGUAGE_JP, anything else LANGUAGE_EN.
int TextLanguage();
// The language's own name (ENGLISH, ESPAÑOL, 日本語...).
const char *LanguageName();

// `english` in the game's language; text with no entry comes back unchanged (new text degrades to English).
const char *Tr(const char *english);

} // namespace Port
} // namespace RSDK

#endif

#endif
