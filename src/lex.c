#include "lex.h"

#include "../ur/abortmacros.h"
#include "chartraits.h"

static Token make_token(u8 tag, size_t start, size_t limit)
{
    Token token;
    token.tag = tag;
    token.start = start;
    token.limit = limit;
    return token;
}

u8 identifier_tag(const u8* s, size_t n);

Token lex(const u8* src, size_t len, size_t i)
{
    ASSERT(src[len] == 0);

    for (;;) { switch (src[i]) {
    case 0:
        if (i == len) return make_token(TOKEN_EOF, len, len);
        return make_token(TOKEN_INVALID, i, i + 1);     /* embedded NUL */

    /* whitespace: advance and restart (token start lands past it) */
    case ' ': case '\t': case '\n': case '\r':
        i += 1;
        continue;

    case '#':                                            /* a pp linemarker `# N "file"`: skip to EOL */
        i += 1;
        while (src[i] != '\n' && src[i] != 0) i += 1;
        continue;

    case '/':                                            /* comments are stripped in pp (phase 3) */
        if (src[i + 1] == '=') return make_token(TOKEN_SLASH_ASSIGN, i, i + 2);
        return make_token(TOKEN_SLASH, i, i + 1);        /* division */

    case 'L':
        if (src[i + 1] == '"') {
            size_t start = i;
            i += 2;                                          /* past L and the opening quote */
            while (src[i] != '"') {
                if (src[i] == 0 || src[i] == '\n') return make_token(TOKEN_INVALID, start, i);
                if (src[i] == '\\') {
                    i += 1;
                    if (src[i] == 0 || src[i] == '\n') return make_token(TOKEN_INVALID, start, i);
                    if (!is_escape_char(src[i])) return make_token(TOKEN_INVALID, start, i + 1);
                }
                i += 1;
            }
            return make_token(TOKEN_WIDE_STRING, start, i + 1);   /* span is L"..." */
        }
        {
            size_t start = i;
            i += 1;
            while (is_ident_continue(src[i])) i += 1;
            return make_token(identifier_tag(src + start, i - start), start, i);
        }

    /* identifier / keyword: [A-Za-z_][A-Za-z0-9_]* */
    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f': case 'g': case 'h': case 'i':
    case 'j': case 'k': case 'l': case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
    case 's': case 't': case 'u': case 'v': case 'w': case 'x': case 'y': case 'z':
    case 'A': case 'B': case 'C': case 'D': case 'E': case 'F': case 'G': case 'H': case 'I':
    case 'J': case 'K': case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
    case 'S': case 'T': case 'U': case 'V': case 'W': case 'X': case 'Y': case 'Z':
    case '_': {
        size_t start = i;
        i += 1;
        while (is_ident_continue(src[i])) i += 1;
        return make_token(identifier_tag(src + start, i - start), start, i);
    }

    /* integer constant: [0-9]+ ... but a trailing identifier char makes it
       one invalid token (e.g. `1foo`, `0x`), not a constant then an ident */
    case '.':
        if (src[i + 1] == '.' && src[i + 2] == '.') return make_token(TOKEN_ELLIPSIS, i, i + 3);
        if (!is_decimal(src[i + 1])) return make_token(TOKEN_DOT, i, i + 1);  // member access
        HEDLEY_FALL_THROUGH;   // ".5" etc. is a floating constant
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9': {
        size_t start = i;

        // binary integer constant: 0b<binary digits> with an optional u/l suffix (GCC/C23 extension)
        if (src[i] == '0' && (src[i + 1] == 'b' || src[i + 1] == 'B')) {
            i += 2;
            if (src[i] != '0' && src[i] != '1') {             // `0b` with no digits is malformed
                while (is_ident_continue(src[i])) i += 1;
                return make_token(TOKEN_INVALID, start, i);
            }
            while (src[i] == '0' || src[i] == '1') i += 1;
            Bool have_u = 0, have_l = 0;
            for (int pass = 0; pass < 2; pass++) {
                if (!have_u && (src[i] == 'u' || src[i] == 'U')) { have_u = 1; i += 1; }
                else if (!have_l && (src[i] == 'l' || src[i] == 'L')) { have_l = 1; u8 c = src[i]; i += 1; if (src[i] == c) i += 1; }
            }
            if (src[i] == '.' || is_ident_continue(src[i])) {
                while (src[i] == '.' || is_ident_continue(src[i])) i += 1;
                return make_token(TOKEN_INVALID, start, i);
            }
            return make_token(TOKEN_CONSTANT_INT, start, i);
        }

        // hexadecimal integer constant: 0x<hex digits> with an optional u/l suffix
        if (src[i] == '0' && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
            i += 2;
            if (!is_hex_digit(src[i])) {                  // `0x` with no digits is malformed
                while (is_ident_continue(src[i])) i += 1;
                return make_token(TOKEN_INVALID, start, i);
            }
            while (is_hex_digit(src[i])) i += 1;
            Bool have_u = 0, have_l = 0;
            for (int pass = 0; pass < 2; pass++) {
                if (!have_u && (src[i] == 'u' || src[i] == 'U')) { have_u = 1; i += 1; }
                else if (!have_l && (src[i] == 'l' || src[i] == 'L')) { have_l = 1; u8 c = src[i]; i += 1; if (src[i] == c) i += 1; }
            }
            if (src[i] == '.' || is_ident_continue(src[i])) {
                while (src[i] == '.' || is_ident_continue(src[i])) i += 1;
                return make_token(TOKEN_INVALID, start, i);
            }
            return make_token(TOKEN_CONSTANT_INT, start, i);
        }

        while (is_decimal(src[i])) i += 1;          // integer part (empty for ".5")

        // a '.' fraction or an 'e'/'E' exponent makes it a floating constant
        Bool is_float = 0;
        if (src[i] == '.') {
            is_float = 1;
            i += 1;
            while (is_decimal(src[i])) i += 1;
        }
        if (src[i] == 'e' || src[i] == 'E') {
            is_float = 1;
            i += 1;
            if (src[i] == '+' || src[i] == '-') i += 1;
            if (!is_decimal(src[i])) {                      // exponent requires >= 1 digit
                while (is_ident_continue(src[i])) i += 1;
                return make_token(TOKEN_INVALID, start, i);
            }
            while (is_decimal(src[i])) i += 1;
        }

        if (!is_float) {
            // optional integer suffix: unsigned (u/U) and long (l/L, or ll/LL same
            // case), in either order. mixed-case `lL`/`Ll` is invalid -- it falls
            // through to the identifier-char check below and rejects.
            Bool have_u = 0, have_l = 0;
            for (int pass = 0; pass < 2; pass++) {
                if (!have_u && (src[i] == 'u' || src[i] == 'U')) {
                    have_u = 1;
                    i += 1;
                } else if (!have_l && (src[i] == 'l' || src[i] == 'L')) {
                    have_l = 1;
                    u8 c = src[i];
                    i += 1;
                    if (src[i] == c) i += 1;   // ll / LL
                }
            }
        }

        // a trailing '.' or identifier char means this is a malformed preprocessing
        // number (e.g. `1.0e10.0`, `123abc`), not a valid constant
        if (src[i] == '.' || is_ident_continue(src[i])) {
            while (src[i] == '.' || is_ident_continue(src[i])) i += 1;
            return make_token(TOKEN_INVALID, start, i);
        }
        return make_token(is_float ? TOKEN_CONSTANT_FLOAT : TOKEN_CONSTANT_INT, start, i);
    }

    case '(': return make_token(TOKEN_LPAREN,    i, i + 1);
    case ')': return make_token(TOKEN_RPAREN,    i, i + 1);
    case '{': return make_token(TOKEN_LBRACE,    i, i + 1);
    case '}': return make_token(TOKEN_RBRACE,    i, i + 1);
    case '[': return make_token(TOKEN_LBRACKET,  i, i + 1);
    case ']': return make_token(TOKEN_RBRACKET,  i, i + 1);
    case ';': return make_token(TOKEN_SEMICOLON, i, i + 1);
    case ',': return make_token(TOKEN_COMMA,     i, i + 1);
    case '?': return make_token(TOKEN_QUESTION,  i, i + 1);
    case ':': return make_token(TOKEN_COLON,     i, i + 1);
    case '~': return make_token(TOKEN_TILDE,     i, i + 1);
    case '*':                                            /* '*' or '*=' */
        if (src[i + 1] == '=') return make_token(TOKEN_STAR_ASSIGN, i, i + 2);
        return make_token(TOKEN_ASTERISK, i, i + 1);
    case '%':                                            /* '%' or '%=' */
        if (src[i + 1] == '=') return make_token(TOKEN_PERCENT_ASSIGN, i, i + 2);
        return make_token(TOKEN_PERCENT, i, i + 1);
    case '^':                                            /* '^' or '^=' */
        if (src[i + 1] == '=') return make_token(TOKEN_CARET_ASSIGN, i, i + 2);
        return make_token(TOKEN_CARET, i, i + 1);

    case '+':
        switch (src[i + 1]) {
            case '+': return make_token(TOKEN_INCREMENT,   i, i + 2);
            case '=': return make_token(TOKEN_PLUS_ASSIGN, i, i + 2);
            default:  return make_token(TOKEN_PLUS,        i, i + 1);
        }
    case '&':
        switch (src[i + 1]) {
            case '&': return make_token(TOKEN_AND,        i, i + 2);
            case '=': return make_token(TOKEN_AMP_ASSIGN, i, i + 2);
            default:  return make_token(TOKEN_AMP,        i, i + 1);
        }
    case '|':
        switch (src[i + 1]) {
            case '|': return make_token(TOKEN_OR,          i, i + 2);
            case '=': return make_token(TOKEN_PIPE_ASSIGN, i, i + 2);
            default:  return make_token(TOKEN_PIPE,        i, i + 1);
        }
    case '!':                                            /* '!' or '!=' */
        if (src[i + 1] == '=') return make_token(TOKEN_NOT_EQUAL, i, i + 2);
        return make_token(TOKEN_BANG, i, i + 1);
    case '=':                                            /* '=' or '==' */
        if (src[i + 1] == '=') return make_token(TOKEN_EQUAL_EQUAL, i, i + 2);
        return make_token(TOKEN_ASSIGN, i, i + 1);

    case '<':
        switch (src[i + 1]) {
            case '<':
                if (src[i + 2] == '=') return make_token(TOKEN_LSHIFT_ASSIGN, i, i + 3);
                return make_token(TOKEN_LSHIFT, i, i + 2);
            case '=': return make_token(TOKEN_LESS_EQUAL, i, i + 2);
            default:  return make_token(TOKEN_LESS,       i, i + 1);
        }
    case '>':
        switch (src[i + 1]) {
            case '>':
                if (src[i + 2] == '=') return make_token(TOKEN_RSHIFT_ASSIGN, i, i + 3);
                return make_token(TOKEN_RSHIFT, i, i + 2);
            case '=': return make_token(TOKEN_GREATER_EQUAL, i, i + 2);
            default:  return make_token(TOKEN_GREATER,       i, i + 1);
        }

    case '-':
        switch (src[i + 1]) {
            case '-': return make_token(TOKEN_DECREMENT,    i, i + 2);
            case '>': return make_token(TOKEN_ARROW,        i, i + 2);
            case '=': return make_token(TOKEN_MINUS_ASSIGN, i, i + 2);
            default:  return make_token(TOKEN_MINUS,        i, i + 1);
        }

    case '\'':                                           /* char constant '...' */
    case '"': {                                          /* string literal "..." */
        u8 quote = src[i];
        u8 tag = (quote == '"') ? TOKEN_STRING : TOKEN_CONSTANT_INT;
        size_t start = i;
        i += 1;
        while (src[i] != quote) {
            if (src[i] == 0 || src[i] == '\n') return make_token(TOKEN_INVALID, start, i);
            if (src[i] == '\\') {                        // escape sequence
                i += 1;
                if (src[i] == 0 || src[i] == '\n') return make_token(TOKEN_INVALID, start, i);
                if (!is_escape_char(src[i])) return make_token(TOKEN_INVALID, start, i + 1);
            }
            i += 1;
        }
        return make_token(tag, start, i + 1);            // include the closing quote
    }

    /* anything else (@, `, \, ...) is a lexical error */
    default:
        return make_token(TOKEN_INVALID, i, i + 1);
    } }
}
