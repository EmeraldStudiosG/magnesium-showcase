/* ========================================================================
 * Magnesium Lexer / Scanner
 * Tokenizes source code into a stream of tokens.
 * ======================================================================== */
#include <string.h>
#include "magnesium.h"

void scanner_init(Scanner *scanner, const char *source) {
    scanner->start = source;
    scanner->current = source;
    scanner->line = 1;
}

/* Character helpers */

static bool is_at_end(Scanner *scanner) {
    return *scanner->current == '\0';
}

static char advance(Scanner *scanner) {
    scanner->current++;
    return scanner->current[-1];
}

static char peek(Scanner *scanner) {
    return *scanner->current;
}

static char peek_next(Scanner *scanner) {
    if (is_at_end(scanner)) return '\0';
    return scanner->current[1];
}

static bool match(Scanner *scanner, char expected) {
    if (is_at_end(scanner)) return false;
    if (*scanner->current != expected) return false;
    scanner->current++;
    return true;
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* Token constructors */

static Token make_token(Scanner *scanner, MgTokenType type) {
    Token token;
    token.type = type;
    token.start = scanner->start;
    token.length = (int)(scanner->current - scanner->start);
    token.line = scanner->line;
    return token;
}

static Token error_token(Scanner *scanner, const char *message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = scanner->line;
    return token;
}

/* Whitespace and comments */

static void skip_whitespace(Scanner *scanner) {
    for (;;) {
        char c = peek(scanner);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance(scanner);
                break;
            case '\n':
                scanner->line++;
                advance(scanner);
                break;
            case '/':
                if (peek_next(scanner) == '/') {
                    advance(scanner); advance(scanner); /* consume // */
                    /* Check for block comment: //[[ */
                    if (peek(scanner) == '[' && peek_next(scanner) == '[') {
                        advance(scanner); advance(scanner); /* consume [[ */
                        int depth = 1;
                        while (!is_at_end(scanner) && depth > 0) {
                            if (peek(scanner) == ']' && peek_next(scanner) == ']') {
                                advance(scanner); advance(scanner);
                                depth--;
                            } else {
                                if (peek(scanner) == '\n') scanner->line++;
                                advance(scanner);
                            }
                        }
                    } else {
                        /* Line comment: skip to end of line */
                        while (peek(scanner) != '\n' && !is_at_end(scanner)) {
                            advance(scanner);
                        }
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

/* Keyword recognition */

static MgTokenType check_keyword(Scanner *scanner, int start, int length,
                               const char *rest, MgTokenType type) {
    if (scanner->current - scanner->start == start + length &&
        memcmp(scanner->start + start, rest, length) == 0) {
        return type;
    }
    return TOKEN_IDENTIFIER;
}

static MgTokenType identifier_type(Scanner *scanner) {
    int len = (int)(scanner->current - scanner->start);

    switch (scanner->start[0]) {
        case 'a':
            if (len > 1 && scanner->start[1] == 's' && len == 2)
                return TOKEN_AS;
            return check_keyword(scanner, 1, 2, "nd", TOKEN_AND);
        case 'b':
            return check_keyword(scanner, 1, 4, "reak", TOKEN_BREAK);
        case 'c':
            if (len == 5 && scanner->start[1] == 'a')
                return check_keyword(scanner, 2, 3, "tch", TOKEN_CATCH);
            if (len > 2 && scanner->start[1] == 'o' && scanner->start[2] == 'n') {
                if (len > 3) {
                    if (scanner->start[3] == 's')
                        return check_keyword(scanner, 4, 1, "t", TOKEN_CONST);
                    if (scanner->start[3] == 't')
                        return check_keyword(scanner, 4, 4, "inue", TOKEN_CONTINUE);
                }
            }
            break;
        case 'd':
            return check_keyword(scanner, 1, 4, "efer", TOKEN_DEFER);
        case 'e':
            if (len > 1) {
                switch (scanner->start[1]) {
                    case 'l':
                        if (len > 3 && scanner->start[2] == 's' && scanner->start[3] == 'e') {
                            if (len > 4 && scanner->start[4] == 'i')
                                return check_keyword(scanner, 5, 1, "f", TOKEN_ELSEIF);
                            return check_keyword(scanner, 2, 2, "se", TOKEN_ELSE);
                        }
                        break;
                    case 'n':
                        if (len == 3 && scanner->start[2] == 'd')
                            return TOKEN_END;
                        if (len == 4)
                            return check_keyword(scanner, 2, 2, "um", TOKEN_ENUM);
                        break;
                    case 'x':
                        return check_keyword(scanner, 2, 4, "port", TOKEN_EXPORT);
                }
            }
            break;
        case 'f':
            if (len > 1) {
                switch (scanner->start[1]) {
                    case 'a': return check_keyword(scanner, 2, 3, "lse", TOKEN_FALSE);
                    case 'n':
                        if (len == 2) return TOKEN_FN;
                        break;
                    case 'o': return check_keyword(scanner, 2, 1, "r", TOKEN_FOR);
                }
            }
            break;
        case 'i':
            if (len > 1) {
                switch (scanner->start[1]) {
                    case 'f':
                        if (len == 2) return TOKEN_IF;
                        break;
                    case 'm': return check_keyword(scanner, 2, 4, "port", TOKEN_IMPORT);
                    case 'n':
                        if (len == 2) return TOKEN_IN;
                        break;
                }
            }
            break;
        case 'l':
            if (len > 1) {
                switch (scanner->start[1]) {
                    case 'e': return check_keyword(scanner, 2, 1, "t", TOKEN_LET);
                    case 'o': return check_keyword(scanner, 2, 2, "op", TOKEN_LOOP);
                }
            }
            break;
        case 'n':
            if (len > 1 && scanner->start[1] == 'o') {
                if (len == 3 && scanner->start[2] == 't')
                    return TOKEN_NOT;
            }
            return check_keyword(scanner, 1, 3, "ull", TOKEN_NULL);
        case 'o':
            return check_keyword(scanner, 1, 1, "r", TOKEN_OR);
        case 'r':
            return check_keyword(scanner, 1, 5, "eturn", TOKEN_RETURN);
        case 's':
            return check_keyword(scanner, 1, 5, "truct", TOKEN_STRUCT);
        case 't':
            if (len > 1) {
                switch (scanner->start[1]) {
                    case 'h': return check_keyword(scanner, 2, 2, "en", TOKEN_THEN);
                    case 'r':
                        if (len == 3 && scanner->start[2] == 'y') return TOKEN_TRY;
                        return check_keyword(scanner, 2, 2, "ue", TOKEN_TRUE);
                }
            }
            break;
    }
    return TOKEN_IDENTIFIER;
}

/* Literal scanners */

static Token scan_identifier(Scanner *scanner) {
    while (is_alpha(peek(scanner)) || is_digit(peek(scanner))) {
        advance(scanner);
    }
    return make_token(scanner, identifier_type(scanner));
}

static Token scan_number(Scanner *scanner) {
    while (is_digit(peek(scanner))) advance(scanner);

    /* Look for decimal part, but not range operator (..) */
    if (peek(scanner) == '.' && peek_next(scanner) != '.' && is_digit(peek_next(scanner))) {
        advance(scanner); /* consume the '.' */
        while (is_digit(peek(scanner))) advance(scanner);
    }

    return make_token(scanner, TOKEN_NUMBER);
}

static Token scan_string(Scanner *scanner) {
    while (peek(scanner) != '"' && !is_at_end(scanner)) {
        if (peek(scanner) == '\\') advance(scanner); /* skip escape */
        if (peek(scanner) == '\n') scanner->line++;
        advance(scanner);
    }

    if (is_at_end(scanner)) return error_token(scanner, "Unterminated string.");
    advance(scanner); /* closing quote */
    return make_token(scanner, TOKEN_STRING);
}

static Token scan_interp_string(Scanner *scanner) {
    /* The '_' has been consumed, now expect '"' */
    if (peek(scanner) != '"') return error_token(scanner, "Expected '\"' after '_'.");
    advance(scanner); /* consume opening '"' */

    while (peek(scanner) != '"' && !is_at_end(scanner)) {
        if (peek(scanner) == '\\') advance(scanner); /* skip escape */
        if (peek(scanner) == '\n') scanner->line++;
        advance(scanner);
    }

    if (is_at_end(scanner)) return error_token(scanner, "Unterminated interpolated string.");
    advance(scanner); /* closing quote */
    return make_token(scanner, TOKEN_INTERP_STRING);
}

/* Main scan function */

Token scan_token(Scanner *scanner) {
    skip_whitespace(scanner);
    scanner->start = scanner->current;

    if (is_at_end(scanner)) return make_token(scanner, TOKEN_EOF);

    char c = advance(scanner);

    /* Identifiers and keywords */
    if (is_alpha(c)) {
        /* Check for interpolated string: _"..." */
        if (c == '_' && peek(scanner) == '"') {
            return scan_interp_string(scanner);
        }
        return scan_identifier(scanner);
    }

    /* Numbers */
    if (is_digit(c)) return scan_number(scanner);

    /* Punctuation */
    switch (c) {
        case '(': return make_token(scanner, TOKEN_LEFT_PAREN);
        case ')': return make_token(scanner, TOKEN_RIGHT_PAREN);
        case '{': return make_token(scanner, TOKEN_LEFT_BRACE);
        case '}': return make_token(scanner, TOKEN_RIGHT_BRACE);
        case '[': return make_token(scanner, TOKEN_LEFT_BRACKET);
        case ']': return make_token(scanner, TOKEN_RIGHT_BRACKET);
        case ';': return make_token(scanner, TOKEN_SEMICOLON);
        case ',': return make_token(scanner, TOKEN_COMMA);
        case '+':
            return make_token(scanner, match(scanner, '=') ? TOKEN_PLUS_EQUAL : TOKEN_PLUS);
        case '-':
            return make_token(scanner, match(scanner, '=') ? TOKEN_MINUS_EQUAL : TOKEN_MINUS);
        case '*':
            return make_token(scanner, match(scanner, '=') ? TOKEN_STAR_EQUAL : TOKEN_STAR);
        case '/':
            return make_token(scanner, match(scanner, '=') ? TOKEN_SLASH_EQUAL : TOKEN_SLASH);
        case '%':
            return make_token(scanner, match(scanner, '=') ? TOKEN_PERCENT_EQUAL : TOKEN_PERCENT);
        case ':': return make_token(scanner, TOKEN_COLON);
        case '@': return make_token(scanner, TOKEN_AT);
        case '&': return make_token(scanner, TOKEN_AMPERSAND);
        case '?': return make_token(scanner, TOKEN_QUESTION);
        case '.':
            if (match(scanner, '.')) {
                if (match(scanner, '=')) return make_token(scanner, TOKEN_DOT_DOT_EQUAL);
                return make_token(scanner, TOKEN_DOT_DOT);
            }
            return make_token(scanner, TOKEN_DOT);
        case '!':
            return make_token(scanner, match(scanner, '=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
        case '=':
            return make_token(scanner, match(scanner, '=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
        case '<':
            return make_token(scanner, match(scanner, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
        case '>':
            return make_token(scanner, match(scanner, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
        case '"':
            return scan_string(scanner);
    }

    return error_token(scanner, "Unexpected character.");
}
