#ifndef _CHARTRAITS_H
#define _CHARTRAITS_H

#include "../ur/bool.h"
#include "../ur/int.h"

extern const u8 char_traits[256];

#define CHAR_TRAIT_ALPHA   0x01 // 'A'...'Z', '_', 'a'...'z'
#define CHAR_TRAIT_DECIMAL 0x02 // '0'...'9'
#define CHAR_TRAIT_HEX     0x04 // '0'...'9', 'A'...'F', 'a'...'f'
#define CHAR_TRAIT_ESCAPE  0x08
#define CHAR_TRAIT_PP_BREAK 0x10 // '\n' '"' '\'' '\\' '/': bytes (besides identifier starts) where pp_expand
                                 // must stop a plain byte-run -- lets that scan be one masked table lookup

static inline Bool is_decimal(u8 c)        { return char_traits[c] & CHAR_TRAIT_DECIMAL; }
static inline Bool is_hex_digit(u8 c)      { return char_traits[c] & CHAR_TRAIT_HEX; }
static inline Bool is_ident_start(u8 c)    { return char_traits[c] & CHAR_TRAIT_ALPHA; }
static inline Bool is_ident_continue(u8 c) { return char_traits[c] & (CHAR_TRAIT_ALPHA | CHAR_TRAIT_DECIMAL); }
static inline Bool is_escape_char(u8 c)    { return char_traits[c] & CHAR_TRAIT_ESCAPE; }
// pp_expand: does a plain byte-run stop here? (an identifier start, or one of \n " ' \\)
static inline Bool is_pp_break(u8 c)       { return char_traits[c] & (CHAR_TRAIT_ALPHA | CHAR_TRAIT_PP_BREAK); }

#endif //_CHARTRAITS_H
