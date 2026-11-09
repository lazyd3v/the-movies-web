#ifndef MOVIES_TEXT_API_H
#define MOVIES_TEXT_API_H
#include "runtime.h"
/* ACP=1252; CP1252 and UTF8 only. GetStringTypeW CT_CTYPE1 supports the CP1252 Unicode repertoire.
 * LCMapStringW supports simple lower/upper casing of that repertoire.
 * Not a complete Windows NLS implementation. */
ImportHandler *movies_text_handlers(uint32_t *count);
/* Pure helpers; encoding returns -1 when unmappable. Uppercase is the
 * supported western repertoire mapping, not general Unicode case folding. */
uint16_t movies_cp1252_decode(uint8_t byte);
int movies_cp1252_encode(uint32_t scalar, int bestfit);
uint16_t movies_western_upper(uint16_t character);
/* Guest callback dispatch for EnumSystemLocalesA (runtime lookup_function/guest_call). */
void movies_text_set_dispatcher(GuestFn (*lookup)(uint32_t),void (*call)(CPU *,GuestFn,int,const uint32_t *));
#endif
