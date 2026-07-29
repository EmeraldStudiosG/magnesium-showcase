#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#define STDERR_FILENO 2
#else
#include <unistd.h>
#endif
#include "magnesium.h"

#if defined(__GNUC__) || defined(__clang__)
#define MG_LSP_UNUSED __attribute__((unused))
#else
#define MG_LSP_UNUSED
#endif

#define MAX_OPEN_DOCS 256
#define MAX_CAPTURE 65536

typedef struct {
    char uri[1024];
    char *text;
    int version;
} OpenDoc;

static OpenDoc open_docs[MAX_OPEN_DOCS];
static int open_doc_count = 0;

static OpenDoc *find_doc(const char *uri) {
    for (int i = 0; i < open_doc_count; i++) {
        if (strcmp(open_docs[i].uri, uri) == 0) return &open_docs[i];
    }
    return NULL;
}

static OpenDoc *add_or_find_doc(const char *uri) {
    OpenDoc *d = find_doc(uri);
    if (d) return d;
    if (open_doc_count >= MAX_OPEN_DOCS) return NULL;
    d = &open_docs[open_doc_count++];
    strncpy(d->uri, uri, sizeof(d->uri) - 1);
    d->uri[sizeof(d->uri) - 1] = '\0';
    d->text = NULL;
    d->version = 0;
    return d;
}

static void write_response(const char *json) {
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", strlen(json), json);
    fflush(stdout);
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} JsonBuffer;

static bool json_buffer_init(JsonBuffer *buffer, size_t initial_capacity) {
    if (!buffer) return false;
    if (initial_capacity < 256) initial_capacity = 256;
    buffer->data = malloc(initial_capacity);
    buffer->length = 0;
    buffer->capacity = buffer->data ? initial_capacity : 0;
    buffer->failed = buffer->data == NULL;
    if (buffer->data) buffer->data[0] = '\0';
    return !buffer->failed;
}

static void json_buffer_free(JsonBuffer *buffer) {
    if (!buffer) return;
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    buffer->failed = false;
}

static bool json_buffer_reserve(JsonBuffer *buffer, size_t additional) {
    if (!buffer || buffer->failed) return false;
    if (additional > SIZE_MAX - buffer->length - 1) {
        buffer->failed = true;
        return false;
    }
    size_t required = buffer->length + additional + 1;
    if (required <= buffer->capacity) return true;

    size_t capacity = buffer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char *grown = realloc(buffer->data, capacity);
    if (!grown) {
        buffer->failed = true;
        return false;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

static bool json_buffer_append_bytes(JsonBuffer *buffer, const char *text, size_t length) {
    if (!text || !json_buffer_reserve(buffer, length)) return false;
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

static bool json_buffer_append(JsonBuffer *buffer, const char *text) {
    return text && json_buffer_append_bytes(buffer, text, strlen(text));
}

static bool json_buffer_appendf(JsonBuffer *buffer, const char *format, ...) {
    if (!buffer || buffer->failed || !format) return false;

    while (true) {
        size_t remaining = buffer->capacity - buffer->length;
        va_list args;
        va_start(args, format);
        int written = vsnprintf(buffer->data + buffer->length, remaining, format, args);
        va_end(args);

        if (written >= 0 && (size_t)written < remaining) {
            buffer->length += (size_t)written;
            return true;
        }

        size_t needed = written >= 0 ? (size_t)written : remaining;
        if (needed < 64) needed = 64;
        if (!json_buffer_reserve(buffer, needed)) return false;
    }
}

static bool json_buffer_append_string(JsonBuffer *buffer, const char *text) {
    if (!json_buffer_append_bytes(buffer, "\"", 1)) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        const char *escape = NULL;
        switch (*p) {
            case '"': escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\b': escape = "\\b"; break;
            case '\f': escape = "\\f"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            default: break;
        }
        if (escape) {
            if (!json_buffer_append(buffer, escape)) return false;
        } else if (*p < 0x20) {
            if (!json_buffer_appendf(buffer, "\\u%04x", (unsigned int)*p)) return false;
        } else if (!json_buffer_append_bytes(buffer, (const char *)p, 1)) {
            return false;
        }
    }
    return json_buffer_append_bytes(buffer, "\"", 1);
}

static void write_buffer_response(JsonBuffer *buffer, int id) {
    if (buffer && !buffer->failed && buffer->data) {
        write_response(buffer->data);
        return;
    }
    char error_response[256];
    snprintf(error_response, sizeof(error_response),
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":-32603,"
             "\"message\":\"Out of memory while building response\"}}", id);
    write_response(error_response);
}

static bool append_json(char *buffer, size_t buffer_size, int *offset,
                        const char *format, ...) {
    if (!buffer || !offset || *offset < 0 || (size_t)*offset >= buffer_size) return false;

    size_t remaining = buffer_size - (size_t)*offset;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *offset, remaining, format, args);
    va_end(args);

    if (written < 0) return false;
    if ((size_t)written >= remaining) {
        *offset = (int)buffer_size - 1;
        buffer[*offset] = '\0';
        return false;
    }
    *offset += written;
    return true;
}

static size_t utf8_sequence_length(const unsigned char *text, size_t remaining,
                                   uint32_t *codepoint) {
    unsigned char first = text[0];
    if (first < 0x80) {
        *codepoint = first;
        return 1;
    }

    size_t length = 0;
    uint32_t value = 0;
    if ((first & 0xe0) == 0xc0) {
        length = 2;
        value = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        length = 3;
        value = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        length = 4;
        value = first & 0x07;
    } else {
        *codepoint = 0xfffd;
        return 1;
    }

    if (length > remaining) {
        *codepoint = 0xfffd;
        return 1;
    }
    for (size_t i = 1; i < length; i++) {
        if ((text[i] & 0xc0) != 0x80) {
            *codepoint = 0xfffd;
            return 1;
        }
        value = (value << 6) | (text[i] & 0x3f);
    }
    if ((length == 2 && value < 0x80) ||
        (length == 3 && value < 0x800) ||
        (length == 4 && value < 0x10000) ||
        value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
        *codepoint = 0xfffd;
        return 1;
    }
    *codepoint = value;
    return length;
}

static int utf16_units_for_bytes(const char *text, size_t byte_count) {
    size_t offset = 0;
    int units = 0;
    while (offset < byte_count && text[offset]) {
        uint32_t codepoint = 0;
        size_t length = utf8_sequence_length(
            (const unsigned char *)text + offset, byte_count - offset, &codepoint);
        units += codepoint > 0xffff ? 2 : 1;
        offset += length;
    }
    return units;
}

static int byte_offset_from_utf16(const char *text, int utf16_offset) {
    if (utf16_offset <= 0) return 0;

    size_t byte_length = strlen(text);
    size_t offset = 0;
    int units = 0;
    while (offset < byte_length && units < utf16_offset) {
        uint32_t codepoint = 0;
        size_t length = utf8_sequence_length(
            (const unsigned char *)text + offset, byte_length - offset, &codepoint);
        int next_units = units + (codepoint > 0xffff ? 2 : 1);
        if (next_units > utf16_offset) break;
        units = next_units;
        offset += length;
    }
    return (int)offset;
}

static char *read_message(void) {
    char line[256];
    size_t content_length = 0;
    bool found_length = false;

    while (fgets(line, sizeof(line), stdin)) {
        if (strncmp(line, "Content-Length:", 15) == 0) {
            char *end = NULL;
            unsigned long long parsed = strtoull(line + 15, &end, 10);
            if (end != line + 15 && parsed > 0 && parsed <= SIZE_MAX - 1) {
                content_length = (size_t)parsed;
                found_length = true;
            }
        }
        if (line[0] == '\r' || line[0] == '\n') break;
    }

    if (!found_length) return NULL;

    char *body = malloc(content_length + 1);
    if (!body) return NULL;

    size_t total = 0;
    while (total < content_length) {
        size_t n = fread(body + total, 1, content_length - total, stdin);
        if (n == 0) { free(body); return NULL; }
        total += n;
    }
    body[total] = '\0';
    return body;
}

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_json_hex4(const unsigned char *text,
                            const unsigned char *end, uint32_t *value) {
    if (!text || !end || text > end || (size_t)(end - text) < 4) {
        return false;
    }
    uint32_t result = 0;
    for (int i = 0; i < 4; i++) {
        int digit = hex_digit_value((char)text[i]);
        if (digit < 0) return false;
        result = (result << 4) | (uint32_t)digit;
    }
    *value = result;
    return true;
}

static bool append_utf8_codepoint(JsonBuffer *buffer, uint32_t codepoint) {
    char bytes[4];
    size_t length;
    if (codepoint <= 0x7f) {
        bytes[0] = (char)codepoint;
        length = 1;
    } else if (codepoint <= 0x7ff) {
        bytes[0] = (char)(0xc0 | (codepoint >> 6));
        bytes[1] = (char)(0x80 | (codepoint & 0x3f));
        length = 2;
    } else if (codepoint <= 0xffff) {
        bytes[0] = (char)(0xe0 | (codepoint >> 12));
        bytes[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (char)(0x80 | (codepoint & 0x3f));
        length = 3;
    } else {
        bytes[0] = (char)(0xf0 | (codepoint >> 18));
        bytes[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[3] = (char)(0x80 | (codepoint & 0x3f));
        length = 4;
    }
    return json_buffer_append_bytes(buffer, bytes, length);
}

static char *decode_json_string(const char *text) {
    JsonBuffer decoded;
    if (!json_buffer_init(&decoded, 256)) return NULL;

    const unsigned char *p = (const unsigned char *)text;
    const unsigned char *end = p + strlen(text);
    while (p < end && *p != '"') {
        if (*p < 0x20) {
            json_buffer_free(&decoded);
            return NULL;
        }
        if (*p != '\\') {
            if (!json_buffer_append_bytes(&decoded, (const char *)p, 1)) break;
            p++;
            continue;
        }

        p++;
        if (p >= end) {
            decoded.failed = true;
            break;
        }
        unsigned char escape = *p++;
        char decoded_char = '\0';
        switch (escape) {
            case '"': decoded_char = '"'; break;
            case '\\': decoded_char = '\\'; break;
            case '/': decoded_char = '/'; break;
            case 'b': decoded_char = '\b'; break;
            case 'f': decoded_char = '\f'; break;
            case 'n': decoded_char = '\n'; break;
            case 'r': decoded_char = '\r'; break;
            case 't': decoded_char = '\t'; break;
            case 'u': {
                uint32_t codepoint;
                if (!parse_json_hex4(p, end, &codepoint)) {
                    decoded.failed = true;
                    break;
                }
                p += 4;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    uint32_t low;
                    if ((size_t)(end - p) < 6 ||
                        p[0] != '\\' || p[1] != 'u' ||
                        !parse_json_hex4(p + 2, end, &low) ||
                        low < 0xdc00 || low > 0xdfff) {
                        decoded.failed = true;
                        break;
                    }
                    p += 6;
                    codepoint = 0x10000 +
                        ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    decoded.failed = true;
                    break;
                }
                if (codepoint == 0 || !append_utf8_codepoint(&decoded, codepoint))
                    decoded.failed = true;
                break;
            }
            default:
                decoded.failed = true;
                break;
        }
        if (decoded.failed) break;
        if (escape != 'u' &&
            !json_buffer_append_bytes(&decoded, &decoded_char, 1)) {
            break;
        }
    }

    if (p >= end || *p != '"' || decoded.failed) {
        json_buffer_free(&decoded);
        return NULL;
    }
    return decoded.data;
}

static char *extract_string(const char *json, const char *key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return NULL;
    pos += strlen(search);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    if (*pos != '"') return NULL;
    return decode_json_string(pos + 1);
}

static int extract_int(const char *json, const char *key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return -1;
    pos += strlen(search);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    return atoi(pos);
}

static char *extract_text(const char *json) {
    const char *pos = json;
    while ((pos = strstr(pos, "\"text\""))) {
        const char *peek = pos + 6;
        while (*peek && (*peek == ' ' || *peek == '\t')) peek++;
        if (*peek == ':') break;
        pos += 6;
    }
    
    if (!pos) return NULL;
    pos += 6;
    while (*pos && (*pos != '"')) pos++;
    if (*pos != '"') return NULL;
    return decode_json_string(pos + 1);
}

static void json_escape(const char *src, char *dst, size_t dst_size) {
    while (*src && dst_size > 2) {
        if (*src == '"')       { *dst++ = '\\'; *dst++ = '"';  src++; dst_size -= 2; }
        else if (*src == '\\') { *dst++ = '\\'; *dst++ = '\\'; src++; dst_size -= 2; }
        else if (*src == '\n') { *dst++ = '\\'; *dst++ = 'n';  src++; dst_size -= 2; }
        else if (*src == '\r') { *dst++ = '\\'; *dst++ = 'r';  src++; dst_size -= 2; }
        else if (*src == '\t') { *dst++ = '\\'; *dst++ = 't';  src++; dst_size -= 2; }
        else if ((unsigned char)*src < 0x20) { src++; }
        else { *dst++ = *src++; dst_size--; }
    }
    *dst = '\0';
}

static char compiler_err_buf[MAX_CAPTURE];

static void append_lsp_diagnostic(char *json, size_t json_size, int *offset, bool *first,
                                  int line, int start_char, int end_char, int severity,
                                  const char *message) {
    if (*offset >= (int)json_size - 1) return;
    if (line < 0) line = 0;
    if (start_char < 0) start_char = 0;
    if (end_char <= start_char) end_char = start_char + 1;

    char escaped[1024];
    json_escape(message, escaped, sizeof(escaped));

    int original_offset = *offset;
    if (!*first && !append_json(json, json_size, offset, ",")) return;

    if (!append_json(json, json_size, offset,
        "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
        "\"end\":{\"line\":%d,\"character\":%d}},"
        "\"severity\":%d,\"message\":\"%s\",\"source\":\"magnesium\"}",
        line, start_char, line, end_char, severity, escaped)) {
        *offset = original_offset;
        json[*offset] = '\0';
        return;
    }
    *first = false;
}

static bool is_ident_start_char(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static bool starts_with_ident_call(const char *expr, const char *name) {
    size_t len = strlen(name);
    expr = skip_ws(expr);
    if (strncmp(expr, name, len) != 0) return false;
    if (is_ident_char(expr[len])) return false;
    expr = skip_ws(expr + len);
    return *expr == '(';
}

static bool is_builtin_name(const char *name) {
    static const char *builtins[] = {
        "array", "assert", "coroutine", "dict", "error", "fs", "input", "len",
        "math", "os", "path", "print", "process", "spawn", "string", "task",
        "tonumber", "tostring", "type", "vm", "io", "__ffi_bind"
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (strcmp(name, builtins[i]) == 0) return true;
    }
    return false;
}

typedef enum {
    LSP_TYPE_UNKNOWN,
    LSP_TYPE_STRING,
    LSP_TYPE_NUMBER,
    LSP_TYPE_NUMBER_OR_NULL,
    LSP_TYPE_ARRAY,
    LSP_TYPE_DICT,
    LSP_TYPE_CALLABLE
} LspKnownType;

typedef struct {
    char name[64];
    LspKnownType type;
} LspKnown;

static const char *lsp_type_name(LspKnownType type) {
    switch (type) {
        case LSP_TYPE_STRING: return "string";
        case LSP_TYPE_NUMBER: return "number";
        case LSP_TYPE_NUMBER_OR_NULL: return "number or null";
        case LSP_TYPE_ARRAY: return "array";
        case LSP_TYPE_DICT: return "dict";
        case LSP_TYPE_CALLABLE: return "callable";
        default: return "unknown";
    }
}

static LspKnownType infer_simple_type(const char *expr) {
    expr = skip_ws(expr);
    if (starts_with_ident_call(expr, "input")) return LSP_TYPE_STRING;
    if (starts_with_ident_call(expr, "tonumber")) return LSP_TYPE_NUMBER_OR_NULL;
    if (starts_with_ident_call(expr, "__ffi_bind")) return LSP_TYPE_CALLABLE;
    if (*expr == '"' || (expr[0] == '_' && expr[1] == '"')) return LSP_TYPE_STRING;
    if (*expr == '[') return LSP_TYPE_ARRAY;
    if (expr[0] == '&' && expr[1] == '<') return LSP_TYPE_DICT;
    if (strncmp(expr, "fn", 2) == 0 && !is_ident_char(expr[2])) return LSP_TYPE_CALLABLE;
    if (isdigit((unsigned char)*expr) ||
        ((*expr == '-' || *expr == '+') && isdigit((unsigned char)expr[1]))) {
        return LSP_TYPE_NUMBER;
    }
    return LSP_TYPE_UNKNOWN;
}

static bool parse_declaration(const char *line, char *name, size_t name_size,
                              int *name_start, const char **expr_out) {
    const char *p = skip_ws(line);
    bool is_decl = false;
    if (strncmp(p, "let", 3) == 0 && isspace((unsigned char)p[3])) {
        p += 3;
        is_decl = true;
    } else if (strncmp(p, "const", 5) == 0 && isspace((unsigned char)p[5])) {
        p += 5;
        is_decl = true;
    }
    if (!is_decl) return false;

    p = skip_ws(p);
    if (!is_ident_start_char(*p)) return false;
    const char *name_begin = p;
    p++;
    while (is_ident_char(*p)) p++;

    size_t len = (size_t)(p - name_begin);
    if (len == 0 || len >= name_size) return false;
    memcpy(name, name_begin, len);
    name[len] = '\0';
    *name_start = (int)(name_begin - line);

    p = skip_ws(p);
    if (*p != '=') return false;
    *expr_out = p + 1;
    return true;
}

static void remember_symbol(LspKnown *known, int *known_count, const char *name,
                            LspKnownType type) {
    if (type == LSP_TYPE_UNKNOWN) return;
    for (int i = 0; i < *known_count; i++) {
        if (strcmp(known[i].name, name) == 0) {
            known[i].type = type;
            return;
        }
    }
    if (*known_count >= 256) return;
    strncpy(known[*known_count].name, name, sizeof(known[*known_count].name) - 1);
    known[*known_count].name[sizeof(known[*known_count].name) - 1] = '\0';
    known[*known_count].type = type;
    (*known_count)++;
}

static const char *find_identifier_use(const char *line, const char *name) {
    size_t len = strlen(name);
    const char *p = line;
    while ((p = strstr(p, name)) != NULL) {
        bool left_ok = (p == line) || !is_ident_char(p[-1]);
        bool right_ok = !is_ident_char(p[len]);
        if (left_ok && right_ok) return p;
        p += len;
    }
    return NULL;
}

static bool find_identifier_call(const char *line, const char *name,
                                 int *start_char, int *end_char) {
    size_t len = strlen(name);
    const char *p = line;
    while ((p = find_identifier_use(p, name)) != NULL) {
        const char *after = skip_ws(p + len);
        if (*after == '(') {
            *start_char = (int)(p - line);
            *end_char = *start_char + (int)len;
            return true;
        }
        p += len;
    }
    return false;
}

static bool find_numeric_comparison(const char *line, const char *name,
                                    int *start_char, int *end_char) {
    size_t len = strlen(name);
    const char *p = line;
    while ((p = find_identifier_use(p, name)) != NULL) {
        const char *after = skip_ws(p + len);
        if (*after == '<' || *after == '>') {
            *start_char = (int)(p - line);
            *end_char = *start_char + (int)len;
            return true;
        }

        const char *before = p;
        while (before > line && isspace((unsigned char)before[-1])) before--;
        if (before > line && (before[-1] == '<' || before[-1] == '>')) {
            *start_char = (int)(p - line);
            *end_char = *start_char + (int)len;
            return true;
        }
        p += len;
    }
    return false;
}

static void MG_LSP_UNUSED append_semantic_diagnostics(OpenDoc *doc, char *json,
                                                       size_t json_size,
                                                       int *offset, bool *first) {
    LspKnown known[256];
    int known_count = 0;
    const char *line_start = doc->text;
    int line_no = 0;

    while (*line_start) {
        const char *line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char line[2048];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, line_start, line_len);
        line[line_len] = '\0';
        if (line_len > 0 && line[line_len - 1] == '\r') line[line_len - 1] = '\0';

        char *comment = strstr(line, "//");
        if (comment) *comment = '\0';

        char decl_name[64] = {0};
        int decl_start = -1;
        const char *decl_expr = NULL;
        bool is_decl = parse_declaration(line, decl_name, sizeof(decl_name),
                                         &decl_start, &decl_expr);

        for (int i = 0; i < known_count; i++) {
            int start = 0;
            int end = 0;
            if (!(is_decl && strcmp(known[i].name, decl_name) == 0) &&
                known[i].type != LSP_TYPE_CALLABLE &&
                known[i].type != LSP_TYPE_UNKNOWN &&
                find_identifier_call(line, known[i].name, &start, &end)) {
                char message[256];
                snprintf(message, sizeof(message),
                         "Cannot call '%s' because it is known to hold a %s.",
                         known[i].name, lsp_type_name(known[i].type));
                append_lsp_diagnostic(json, json_size, offset, first, line_no,
                                      start, end, 1, message);
            }

            if ((known[i].type == LSP_TYPE_STRING ||
                 known[i].type == LSP_TYPE_ARRAY ||
                 known[i].type == LSP_TYPE_DICT) &&
                find_numeric_comparison(line, known[i].name, &start, &end)) {
                char message[256];
                snprintf(message, sizeof(message),
                         "Numeric comparison uses '%s', which is known to hold a %s.",
                         known[i].name, lsp_type_name(known[i].type));
                append_lsp_diagnostic(json, json_size, offset, first, line_no,
                                      start, end, 1, message);
            }
        }

        if (is_decl) {
            if (is_builtin_name(decl_name)) {
                char message[256];
                snprintf(message, sizeof(message),
                         "Local '%s' shadows a builtin; later '%s(...)' calls this local value.",
                         decl_name, decl_name);
                append_lsp_diagnostic(json, json_size, offset, first, line_no,
                                      decl_start, decl_start + (int)strlen(decl_name),
                                      2, message);
            }
            remember_symbol(known, &known_count, decl_name, infer_simple_type(decl_expr));
        }

        if (!line_end) break;
        line_start = line_end + 1;
        line_no++;
    }
}

typedef struct {
    const char *label;
    int kind;
    const char *detail;
    const char *doc;
} LspItem;

static const LspItem keyword_items[] = {
    {"let", 14, "keyword", "Creates a mutable local binding. The value can be reassigned later.\n\n```mg\nlet x = 10\nx = 20 // ok\n```"},
    {"const", 14, "keyword", "Creates an immutable local binding. The value cannot be reassigned.\n\n```mg\nconst x = 10\nx = 20 // compile error\n```"},
    {"fn", 14, "keyword", "Defines a function with optional parameters and return values.\n\n```mg\nfn greet(name: string)\n  print(\"Hello, \" + name)\nend\n```"},
    {"return", 14, "keyword", "Returns one or more values from the current function. Multiple values are separated by commas.\n\n```mg\nfn swap(a, b)\n  return b, a\nend\n```"},
    {"if", 14, "keyword", "Starts a conditional block. Must be closed with `end`.\n\n```mg\nif x > 0 then\n  print(\"positive\")\nelseif x < 0 then\n  print(\"negative\")\nelse\n  print(\"zero\")\nend\n```"},
    {"elseif", 14, "keyword", "Adds a conditional branch after an `if` or `elseif` block."},
    {"else", 14, "keyword", "Adds a fallback branch after an `if` or `elseif` block."},
    {"then", 14, "keyword", "Separates the condition from the body in `if`/`elseif` blocks."},
    {"end", 14, "keyword", "Closes the current block (`if`, `fn`, `for`, `loop`, `struct`, `enum`, `try`)."},
    {"loop", 14, "keyword", "Starts an unconditional loop. Use `break` to exit.\n\n```mg\nloop\n  if done then break end\nend\n```"},
    {"for", 14, "keyword", "Iterates over a range or iterable.\n\n```mg\nfor i in 0..10 then\n  print(i)\nend\n```"},
    {"in", 14, "keyword", "Binds a loop variable to an iterable in a `for` loop."},
    {"break", 14, "keyword", "Exits the nearest enclosing loop immediately."},
    {"continue", 14, "keyword", "Skips the rest of the current loop iteration and continues to the next."},
    {"struct", 14, "keyword", "Defines a fixed-shape record type with named fields.\n\n```mg\nstruct Point\n  x: number\n  y: number\nend\n```"},
    {"enum", 14, "keyword", "Defines a set of named states.\n\n```mg\nenum Color\n  Red\n  Green\n  Blue\nend\n```"},
    {"import", 14, "keyword", "Imports a module by file path.\n\n```mg\nimport \"utils\" as u\n```"},
    {"export", 14, "keyword", "Marks a binding as visible to other modules via `import`."},
    {"defer", 14, "keyword", "Schedules an expression to run when the current scope exits, even if an error occurs.\n\n```mg\ndefer print(\"cleanup\")\n```"},
    {"try", 14, "keyword", "Starts an error-handling block. Must be paired with `catch`.\n\n```mg\ntry\n  let val = dict.require(key)\ncatch err\n  print(err)\nend\n```"},
    {"catch", 14, "keyword", "Handles an error from the preceding `try` block."},
    {"type", 14, "keyword", "Defines a static-only type alias. Has no runtime effect.\n\n```mg\ntype Vec2 = &< x: number, y: number >\n```"},
    {"extern", 14, "keyword", "Declares a host-provided symbol for static diagnostics. The host must register the symbol at runtime.\n\n```mg\nextern fn read_score(player: string): number\nextern const MAX_SCORE: number\n```"},
    {"!strict", 14, "directive", "Enables static type checking and diagnostics for this file. Errors are reported but do not change runtime behavior."},
    {"!nocheck", 14, "directive", "Suppresses all static diagnostics for this file, even in strict mode."},
    {"and", 14, "keyword", "Logical and. Returns the first falsey operand, or the last operand.\n\n```mg\nif x > 0 and x < 100 then ... end\n```"},
    {"or", 14, "keyword", "Logical or. Returns the first truthy operand, or the last operand.\n\n```mg\nlet val = x or default\n```"},
    {"not", 14, "keyword", "Logical negation. Inverts the truthiness of a value.\n\n```mg\nif not found then ... end\n```"},
    {"true", 12, "bool", "Boolean true value."},
    {"false", 12, "bool", "Boolean false value."},
    {"null", 21, "null", "Represents the absence of a value. Is falsey in conditions."}
};

static const LspItem builtin_items[] = {
    {"print", 3, "fn print(...)", "Prints all arguments separated by tabs, followed by a newline. Returns null."},
    {"len", 3, "fn len(value)", "Returns the length of a string, array, or dict. Returns -1 for other types."},
    {"push", 3, "fn push(array, value)", "Appends a value to the end of an array. Returns null."},
    {"type", 3, "fn type(value)", "Returns a string describing the runtime type: \"number\", \"string\", \"bool\", \"null\", \"array\", \"dict\", \"function\", \"struct\", \"native_handle\", etc."},
    {"tostring", 3, "fn tostring(value)", "Converts any value to its string representation. Numbers, bools, and null are formatted; other types return a type description."},
    {"tonumber", 3, "fn tonumber(value)", "Converts a string to a number. Returns null if the string is not a valid number."},
    {"assert", 3, "fn assert(condition, message?)", "Raises a runtime error if `condition` is falsey. The optional `message` string is included in the error."},
    {"error", 3, "fn error(kind, message?)", "Creates or raises an error value. If called inside a `try` block, the error is caught by the matching `catch`."},
    {"input", 3, "fn input(prompt?)", "Reads one line from stdin. The optional `prompt` string is printed before reading. Returns the line without the trailing newline."},
    {"Err", 3, "fn Err(kind, message)", "Creates a recoverable error value. Use with `?` to propagate, or pattern-match in `catch`."},
    {"Error", 3, "fn Error(kind, message)", "Creates an error value. Typically used for unrecoverable errors."},
    {"math", 9, "module", "Mathematical functions: abs, floor, ceil, sqrt, sin, cos, tan, pow, min, max, random, randomseed, round, clamp."},
    {"string", 9, "module", "String manipulation functions: len, lower, upper, sub, find, trim, byte, char, split, contains, starts_with, ends_with, repeat, reverse, replace, at."},
    {"array", 9, "module", "Array functions: new, len, push, pop, insert, remove, get, at, clear, contains, index_of, first, last, extend, slice."},
    {"dict", 9, "module", "Dictionary functions: new, has, get, set, delete, keys, values, require, len, clear, clone, merge."},
    {"io", 9, "module", "Input/output helpers: write, read_line, read_file, write_file, append_file."},
    {"fs", 9, "module", "Filesystem helpers: read, write, append, exists, remove, rename, cwd."},
    {"path", 9, "module", "Path manipulation: join, basename, dirname, ext."},
    {"os", 9, "module", "OS interop: clock (CPU time), time (wall clock), getenv."},
    {"process", 9, "module", "Process info: clock, time, getenv, cwd, platform."},
    {"task", 9, "module", "Cooperative same-VM task queue. Use `task.run(fn)` to schedule a function."},
    {"vm", 9, "module", "Isolated VM tasks: spawn, status, join, try_join, cancel. Each spawned task runs in its own VM."},
    {"coroutine", 9, "module", "Coroutine support: create, resume, yield, status. Coroutines enable cooperative multitasking within the same VM."}
};

static const LspItem type_items[] = {
    {"number", 25, "type", "A numeric value. Magnesium uses a single `number` type for both integers and floats."},
    {"string", 25, "type", "A text value. Strings are immutable and UTF-8 encoded."},
    {"bool", 25, "type", "A boolean value: `true` or `false`."},
    {"null", 25, "type", "Represents the absence of a value. Is falsey in conditions."},
    {"any", 25, "type", "Escape hatch: disables type checking for this binding. Use sparingly."},
    {"unknown", 25, "type", "An opaque value that must be narrowed with type checks before use."},
    {"[number]", 25, "array type", "An array whose elements are expected to be numbers."},
    {"&<string, unknown>", 25, "dict type", "A dict with string keys and unknown values."},
    {"fn(number): null", 25, "function type", "A function taking a number and returning null."}
};

static const LspItem math_items[] = {
    {"abs", 3, "math.abs(x)", "Returns the absolute value of `x`."},
    {"floor", 3, "math.floor(x)", "Returns the greatest integer less than or equal to `x`."},
    {"ceil", 3, "math.ceil(x)", "Returns the smallest integer greater than or equal to `x`."},
    {"sqrt", 3, "math.sqrt(x)", "Returns the square root of `x`. Requires `x >= 0`."},
    {"sin", 3, "math.sin(x)", "Returns the sine of `x` (in radians)."},
    {"cos", 3, "math.cos(x)", "Returns the cosine of `x` (in radians)."},
    {"tan", 3, "math.tan(x)", "Returns the tangent of `x` (in radians)."},
    {"asin", 4, "math.asin(x)", "Returns the arcsine of `x` in radians. `x` must be in [-1, 1]."},
    {"acos", 4, "math.acos(x)", "Returns the arccosine of `x` in radians. `x` must be in [-1, 1]."},
    {"atan", 4, "math.atan(x)", "Returns the arctangent of `x` in radians."},
    {"atan2", 5, "math.atan2(y, x)", "Returns the arctangent of `y/x` in radians, using the signs to determine the quadrant."},
    {"pow", 3, "math.pow(base, exponent)", "Returns `base` raised to `exponent`."},
    {"min", 3, "math.min(...)", "Returns the smallest of the given numeric arguments."},
    {"max", 3, "math.max(...)", "Returns the largest of the given numeric arguments."},
    {"random", 3, "math.random()", "Returns a pseudo-random number in [0, 1)."},
    {"randomseed", 3, "math.randomseed(seed)", "Seeds the pseudo-random number generator."},
    {"round", 3, "math.round(x)", "Returns `x` rounded to the nearest integer."},
    {"clamp", 3, "math.clamp(x, min, max)", "Returns `x` clamped to the range `[min, max]`."}
};

static const LspItem string_items[] = {
    {"len", 3, "string.len(s)", "Returns the number of characters in `s`."},
    {"lower", 3, "string.lower(s)", "Returns `s` converted to lowercase."},
    {"upper", 3, "string.upper(s)", "Returns `s` converted to uppercase."},
    {"sub", 3, "string.sub(s, start, end?)", "Returns the substring from `start` to `end` (exclusive). Negative indices count from the end."},
    {"find", 3, "string.find(s, needle)", "Returns the index of the first occurrence of `needle` in `s`, or -1 if not found."},
    {"trim", 3, "string.trim(s)", "Returns `s` with leading and trailing whitespace removed."},
    {"byte", 3, "string.byte(s, index?)", "Returns the byte value at `index` (default 0)."},
    {"char", 3, "string.char(...)", "Builds a string from the given byte values."},
    {"split", 3, "string.split(s, separator)", "Splits `s` by `separator` and returns an array of parts."},
    {"contains", 3, "string.contains(s, needle)", "Returns `true` if `s` contains `needle`, `false` otherwise."},
    {"starts_with", 3, "string.starts_with(s, prefix)", "Returns `true` if `s` starts with `prefix`."},
    {"ends_with", 3, "string.ends_with(s, suffix)", "Returns `true` if `s` ends with `suffix`."},
    {"repeat", 3, "string.repeat(s, count)", "Returns `s` repeated `count` times."},
    {"reverse", 3, "string.reverse(s)", "Returns `s` with characters in reverse order."},
    {"replace", 3, "string.replace(s, old, new)", "Returns `s` with all occurrences of `old` replaced by `new`."},
    {"at", 3, "string.at(s, index)", "Returns the character at `index` as a single-character string."}
};

static const LspItem array_items[] = {
    {"new", 3, "array.new()", "Creates a new empty array."},
    {"len", 3, "array.len(array)", "Returns the number of elements in the array."},
    {"push", 3, "array.push(array, value)", "Appends `value` to the end of the array."},
    {"pop", 3, "array.pop(array)", "Removes and returns the last element of the array."},
    {"insert", 3, "array.insert(array, index, value)", "Inserts `value` at `index`, shifting elements right."},
    {"remove", 3, "array.remove(array, index)", "Removes and returns the element at `index`, shifting elements left."},
    {"get", 3, "array.get(array, index)", "Returns the element at `index`, or null if out of bounds."},
    {"at", 3, "array.at(array, index)", "Returns the element at `index`, or null if out of bounds."},
    {"clear", 3, "array.clear(array)", "Removes all elements from the array."},
    {"contains", 3, "array.contains(array, value)", "Returns `true` if the array contains `value`."},
    {"index_of", 3, "array.index_of(array, value)", "Returns the first index of `value`, or -1 if not found."},
    {"first", 3, "array.first(array)", "Returns the first element, or null if the array is empty."},
    {"last", 3, "array.last(array)", "Returns the last element, or null if the array is empty."},
    {"extend", 3, "array.extend(array, other)", "Appends all elements from `other` to the array."},
    {"slice", 3, "array.slice(array, start, end?)", "Returns a new array with elements from `start` to `end` (exclusive)."}
};

static const LspItem dict_items[] = {
    {"new", 3, "dict.new()", "Creates a new empty dict."},
    {"has", 3, "dict.has(dict, key)", "Returns `true` if the dict contains `key`."},
    {"get", 3, "dict.get(dict, key, default?)", "Returns the value for `key`, or `default` if not found (null if omitted)."},
    {"set", 3, "dict.set(dict, key, value)", "Sets `key` to `value`. Overwrites existing entries."},
    {"delete", 3, "dict.delete(dict, key)", "Removes the entry for `key`. Returns `true` if the key existed."},
    {"keys", 3, "dict.keys(dict)", "Returns an array of all keys in the dict."},
    {"values", 3, "dict.values(dict)", "Returns an array of all values in the dict."},
    {"require", 3, "dict.require(dict, key)", "Returns the value for `key`. If not found, produces a recoverable error that can be caught with `try`/`catch` or propagated with `?`."},
    {"len", 3, "dict.len(dict)", "Returns the number of entries in the dict."},
    {"clear", 3, "dict.clear(dict)", "Removes all entries from the dict."},
    {"clone", 3, "dict.clone(dict)", "Returns a shallow copy of the dict."},
    {"merge", 3, "dict.merge(a, b)", "Returns a new dict with entries from both `a` and `b`. Keys in `b` override keys in `a`."}
};

static const LspItem fs_items[] = {
    {"read", 3, "fs.read(path)", "Reads the entire contents of the file at `path` as a string."},
    {"write", 3, "fs.write(path, text)", "Writes `text` to the file at `path`, replacing any existing content."},
    {"append", 3, "fs.append(path, text)", "Appends `text` to the end of the file at `path`."},
    {"exists", 3, "fs.exists(path)", "Returns `true` if a file or directory exists at `path`."},
    {"remove", 3, "fs.remove(path)", "Deletes the file at `path`."},
    {"rename", 3, "fs.rename(old, new)", "Renames or moves a file from `old` path to `new` path."},
    {"cwd", 3, "fs.cwd()", "Returns the current working directory as a string."}
};

static const LspItem path_items[] = {
    {"join", 3, "path.join(...)", "Joins path segments with the OS-specific separator."},
    {"basename", 3, "path.basename(path)", "Returns the last component of a path (the file name)."},
    {"dirname", 3, "path.dirname(path)", "Returns the directory portion of a path."},
    {"ext", 3, "path.ext(path)", "Returns the file extension including the dot, or empty string if none."}
};

static const LspItem coroutine_items[] = {
    {"create", 3, "coroutine.create(fn)", "Creates a new coroutine from a function. The coroutine starts suspended."},
    {"resume", 3, "coroutine.resume(co, ...)", "Resumes a suspended coroutine, passing additional arguments. Returns values from the next `yield`."},
    {"yield", 3, "coroutine.yield(...)", "Suspends the current coroutine, passing values back to the caller of `resume`."},
    {"status", 3, "coroutine.status(co)", "Returns the coroutine status: \"suspended\", \"running\", \"dead\", or \"normal\"."}
};

static const LspItem task_items[] = {
    {"run", 3, "task.run(fn)", "Queues `fn` to run on the same VM's cooperative task queue. Returns a task handle."}
};

static const LspItem vm_items[] = {
    {"spawn", 3, "vm.spawn(path)", "Spawns an isolated VM task that runs the script at `path` in its own VM."},
    {"status", 3, "vm.status(task)", "Returns the status of an isolated VM task: \"pending\", \"running\", \"done\", or \"error\"."},
    {"join", 3, "vm.join(task, timeout?)", "Blocks until the isolated VM task completes. Optional `timeout` in seconds."},
    {"try_join", 3, "vm.try_join(task)", "Non-blocking poll: returns the task result if done, or null if still running."},
    {"cancel", 3, "vm.cancel(task)", "Requests cancellation of an isolated VM task."}
};

static const LspItem os_items[] = {
    {"clock", 3, "os.clock()", "Returns the CPU time used by the process, in seconds."},
    {"time", 3, "os.time()", "Returns the current wall-clock time as a Unix timestamp in seconds."},
    {"getenv", 3, "os.getenv(name)", "Returns the value of the environment variable `name`, or null if not set."}
};

static const LspItem process_items[] = {
    {"clock", 3, "process.clock()", "Returns the CPU time used by the process, in seconds."},
    {"time", 3, "process.time()", "Returns the current wall-clock time as a Unix timestamp in seconds."},
    {"getenv", 3, "process.getenv(name)", "Returns the value of the environment variable `name`, or null if not set."},
    {"cwd", 3, "process.cwd()", "Returns the current working directory as a string."},
    {"platform", 3, "process.platform()", "Returns the platform name: \"linux\", \"macos\", or \"windows\"."}
};

static const LspItem io_items[] = {
    {"write", 3, "io.write(...)", "Writes all arguments to stdout without appending a newline (unlike `print`)."},
    {"read_line", 3, "io.read_line(prompt?)", "Reads one line from stdin. Prints the optional `prompt` before reading."},
    {"read_file", 3, "io.read_file(path)", "Reads the entire contents of the file at `path` as a string."},
    {"write_file", 3, "io.write_file(path, text)", "Writes `text` to the file at `path`, replacing any existing content."},
    {"append_file", 3, "io.append_file(path, text)", "Appends `text` to the end of the file at `path`."}
};

static bool module_items(const char *module, const LspItem **items, int *count) {
    if (strcmp(module, "math") == 0) {
        *items = math_items; *count = (int)(sizeof(math_items) / sizeof(math_items[0])); return true;
    }
    if (strcmp(module, "string") == 0) {
        *items = string_items; *count = (int)(sizeof(string_items) / sizeof(string_items[0])); return true;
    }
    if (strcmp(module, "array") == 0) {
        *items = array_items; *count = (int)(sizeof(array_items) / sizeof(array_items[0])); return true;
    }
    if (strcmp(module, "dict") == 0) {
        *items = dict_items; *count = (int)(sizeof(dict_items) / sizeof(dict_items[0])); return true;
    }
    if (strcmp(module, "fs") == 0) {
        *items = fs_items; *count = (int)(sizeof(fs_items) / sizeof(fs_items[0])); return true;
    }
    if (strcmp(module, "path") == 0) {
        *items = path_items; *count = (int)(sizeof(path_items) / sizeof(path_items[0])); return true;
    }
    if (strcmp(module, "coroutine") == 0) {
        *items = coroutine_items; *count = (int)(sizeof(coroutine_items) / sizeof(coroutine_items[0])); return true;
    }
    if (strcmp(module, "task") == 0) {
        *items = task_items; *count = (int)(sizeof(task_items) / sizeof(task_items[0])); return true;
    }
    if (strcmp(module, "vm") == 0) {
        *items = vm_items; *count = (int)(sizeof(vm_items) / sizeof(vm_items[0])); return true;
    }
    if (strcmp(module, "os") == 0) {
        *items = os_items; *count = (int)(sizeof(os_items) / sizeof(os_items[0])); return true;
    }
    if (strcmp(module, "process") == 0) {
        *items = process_items; *count = (int)(sizeof(process_items) / sizeof(process_items[0])); return true;
    }
    if (strcmp(module, "io") == 0) {
        *items = io_items; *count = (int)(sizeof(io_items) / sizeof(io_items[0])); return true;
    }
    return false;
}

static OpenDoc *request_doc(const char *json) {
    char *uri = extract_string(json, "uri");
    if (!uri) return NULL;
    OpenDoc *doc = find_doc(uri);
    free(uri);
    return doc;
}

static bool copy_doc_line(OpenDoc *doc, int wanted_line, char *out, size_t out_size) {
    if (!doc || !doc->text || wanted_line < 0 || out_size == 0) return false;
    const char *line_start = doc->text;
    int line = 0;
    while (*line_start && line < wanted_line) {
        const char *nl = strchr(line_start, '\n');
        if (!nl) return false;
        line_start = nl + 1;
        line++;
    }
    if (line != wanted_line) return false;
    const char *line_end = strchr(line_start, '\n');
    size_t len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, line_start, len);
    out[len] = '\0';
    if (len > 0 && out[len - 1] == '\r') out[len - 1] = '\0';
    return true;
}

static int count_doc_lines(OpenDoc *doc) {
    if (!doc || !doc->text || doc->text[0] == '\0') return 1;
    int lines = 1;
    for (const char *p = doc->text; *p; p++) {
        if (*p == '\n') lines++;
    }
    return lines;
}

static bool word_at_position(OpenDoc *doc, int line, int character,
                             char *word, size_t word_size, int *start_char, int *end_char) {
    char text_line[2048];
    if (!copy_doc_line(doc, line, text_line, sizeof(text_line))) return false;
    int len = (int)strlen(text_line);
    if (character < 0) character = 0;

    int pos = byte_offset_from_utf16(text_line, character);
    if (pos == len && pos > 0) pos--;
    if (pos < len && !is_ident_char(text_line[pos]) && pos > 0 && is_ident_char(text_line[pos - 1])) pos--;
    if (pos >= len || !is_ident_char(text_line[pos])) return false;

    int start = pos;
    int end = pos + 1;
    while (start > 0 && is_ident_char(text_line[start - 1])) start--;
    while (end < len && is_ident_char(text_line[end])) end++;

    size_t word_len = (size_t)(end - start);
    if (word_len == 0 || word_len >= word_size) return false;
    memcpy(word, text_line + start, word_len);
    word[word_len] = '\0';
    if (start_char) *start_char = utf16_units_for_bytes(text_line, (size_t)start);
    if (end_char) *end_char = utf16_units_for_bytes(text_line, (size_t)end);
    return true;
}

static bool module_context_at(OpenDoc *doc, int line, int character,
                              char *module, size_t module_size) {
    char text_line[2048];
    if (!copy_doc_line(doc, line, text_line, sizeof(text_line))) return false;
    int len = (int)strlen(text_line);
    if (character < 0) character = 0;
    character = byte_offset_from_utf16(text_line, character);
    if (character > len) character = len;

    int i = character - 1;
    while (i >= 0 && (is_ident_char(text_line[i]) || isspace((unsigned char)text_line[i]))) i--;
    if (i < 0 || text_line[i] != '.') return false;
    int end = i;
    i--;
    while (i >= 0 && isspace((unsigned char)text_line[i])) i--;
    int module_end = i + 1;
    while (i >= 0 && is_ident_char(text_line[i])) i--;
    int module_start = i + 1;
    if (module_start >= module_end) return false;

    size_t len_module = (size_t)(module_end - module_start);
    if (len_module >= module_size) return false;
    (void)end;
    memcpy(module, text_line + module_start, len_module);
    module[len_module] = '\0';
    return true;
}

static char *trim_line(char *line) {
    char *start = line;
    while (*start && isspace((unsigned char)*start)) start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return start;
}

static bool parse_symbol_line(const char *line, char *name, size_t name_size,
                              int *start_char, int *kind) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncmp(p, "export", 6) == 0 && isspace((unsigned char)p[6])) {
        p = skip_ws(p + 6);
    }

    int symbol_kind = 13;
    if (strncmp(p, "type", 4) == 0 && isspace((unsigned char)p[4])) {
        p = skip_ws(p + 4);
        symbol_kind = 5;
    } else if (strncmp(p, "extern", 6) == 0 && isspace((unsigned char)p[6])) {
        p = skip_ws(p + 6);
        if (strncmp(p, "fn", 2) == 0 && isspace((unsigned char)p[2])) {
            p = skip_ws(p + 2);
            symbol_kind = 12;
        } else if (strncmp(p, "const", 5) == 0 && isspace((unsigned char)p[5])) {
            p = skip_ws(p + 5);
            symbol_kind = 14;
        } else {
            return false;
        }
    } else if (strncmp(p, "import", 6) == 0 && isspace((unsigned char)p[6])) {
        const char *alias = strstr(p, " as ");
        if (alias) {
            p = skip_ws(alias + 4);
            symbol_kind = 2;
        } else {
            p = strchr(p, '"');
            if (!p) return false;
            const char *end = strchr(p + 1, '"');
            if (!end) return false;
            const char *base = p + 1;
            for (const char *s = p + 1; s < end; s++) {
                if (*s == '/') base = s + 1;
            }
            const char *dot = NULL;
            for (const char *s = base; s < end; s++) {
                if (*s == '.') dot = s;
            }
            const char *name_end = dot ? dot : end;
            size_t len = (size_t)(name_end - base);
            if (len == 0 || len >= name_size) return false;
            memcpy(name, base, len);
            name[len] = '\0';
            if (start_char) *start_char = (int)(base - line);
            if (kind) *kind = 2;
            return true;
        }
    } else if (strncmp(p, "fn", 2) == 0 && isspace((unsigned char)p[2])) {
        p = skip_ws(p + 2);
        symbol_kind = 12;
    } else if (strncmp(p, "struct", 6) == 0 && isspace((unsigned char)p[6])) {
        p = skip_ws(p + 6);
        symbol_kind = 23;
    } else if (strncmp(p, "enum", 4) == 0 && isspace((unsigned char)p[4])) {
        p = skip_ws(p + 4);
        symbol_kind = 10;
    } else if (strncmp(p, "let", 3) == 0 && isspace((unsigned char)p[3])) {
        p = skip_ws(p + 3);
        symbol_kind = 13;
    } else if (strncmp(p, "const", 5) == 0 && isspace((unsigned char)p[5])) {
        p = skip_ws(p + 5);
        symbol_kind = 14;
    } else {
        return false;
    }

    if (!is_ident_start_char(*p) && *p != '@') return false;
    if (*p == '@') p++;
    if (!is_ident_start_char(*p)) return false;

    const char *start = p;
    p++;
    while (is_ident_char(*p) || *p == '.') p++;
    size_t len = (size_t)(p - start);
    if (len == 0 || len >= name_size) return false;
    memcpy(name, start, len);
    name[len] = '\0';
    if (start_char) *start_char = (int)(start - line);
    if (kind) *kind = symbol_kind;
    return true;
}

static const LspItem *find_known_item(const char *name) {
    for (size_t i = 0; i < sizeof(keyword_items) / sizeof(keyword_items[0]); i++) {
        if (strcmp(keyword_items[i].label, name) == 0) return &keyword_items[i];
    }
    for (size_t i = 0; i < sizeof(builtin_items) / sizeof(builtin_items[0]); i++) {
        if (strcmp(builtin_items[i].label, name) == 0) return &builtin_items[i];
    }
    for (size_t i = 0; i < sizeof(type_items) / sizeof(type_items[0]); i++) {
        if (strcmp(type_items[i].label, name) == 0) return &type_items[i];
    }
    const char *dot = strchr(name, '.');
    if (dot) {
        char module[64];
        size_t module_len = (size_t)(dot - name);
        if (module_len < sizeof(module)) {
            memcpy(module, name, module_len);
            module[module_len] = '\0';
            const LspItem *items = NULL;
            int count = 0;
            if (module_items(module, &items, &count)) {
                const char *member = dot + 1;
                for (int i = 0; i < count; i++) {
                    if (strcmp(items[i].label, member) == 0) return &items[i];
                }
            }
        }
    }
    return NULL;
}

static void append_completion_item(JsonBuffer *json, bool *first, const LspItem *item) {
    if (!*first && !json_buffer_append(json, ",")) return;
    if (!json_buffer_append(json, "{\"label\":") ||
        !json_buffer_append_string(json, item->label) ||
        !json_buffer_appendf(json, ",\"kind\":%d,\"detail\":", item->kind) ||
        !json_buffer_append_string(json, item->detail ? item->detail : "") ||
        !json_buffer_append(json, ",\"documentation\":{\"kind\":\"markdown\",\"value\":") ||
        !json_buffer_append_string(json, item->doc ? item->doc : "") ||
        !json_buffer_append(json, "}}")) {
        return;
    }
    *first = false;
}

static void append_completion_symbol(JsonBuffer *json, bool *first,
                                     const char *name, int kind) {
    LspItem item = {name, kind == 12 ? 3 : (kind == 23 ? 22 : kind == 10 ? 13 : 6),
                    "document symbol", "Declared in this document."};
    append_completion_item(json, first, &item);
}

static void append_doc_symbol_completions(OpenDoc *doc, JsonBuffer *json, bool *first) {
    const char *line_start = doc->text;
    while (line_start && *line_start) {
        const char *line_end = strchr(line_start, '\n');
        size_t len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char *line = malloc(len + 1);
        if (!line) {
            json->failed = true;
            return;
        }
        memcpy(line, line_start, len);
        line[len] = '\0';
        char name[128];
        int start = 0;
        int kind = 13;
        if (parse_symbol_line(line, name, sizeof(name), &start, &kind)) {
            append_completion_symbol(json, first, name, kind);
        }
        free(line);
        if (!line_end) break;
        line_start = line_end + 1;
    }
}

static bool find_doc_symbol_detail(OpenDoc *doc, const char *needle,
                                   char *detail, size_t detail_size,
                                   int *line_out, int *start_out, int *end_out) {
    if (!doc || !doc->text || !needle || needle[0] == '\0') return false;
    const char *line_start = doc->text;
    int line_no = 0;
    while (*line_start) {
        const char *line_end = strchr(line_start, '\n');
        size_t len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char line[2048];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, line_start, len);
        line[len] = '\0';

        char name[128];
        int start = 0;
        int kind = 13;
        if (parse_symbol_line(line, name, sizeof(name), &start, &kind) &&
            strcmp(name, needle) == 0) {
            char tmp[2048];
            memcpy(tmp, line, strlen(line) + 1);
            char *trimmed = trim_line(tmp);
            snprintf(detail, detail_size, "%s", trimmed);
            if (line_out) *line_out = line_no;
            if (start_out) *start_out = start;
            if (end_out) *end_out = start + (int)strlen(name);
            (void)kind;
            return true;
        }
        if (!line_end) break;
        line_start = line_end + 1;
        line_no++;
    }
    return false;
}

static void handle_completion(const char *json, int id) {
    OpenDoc *doc = request_doc(json);
    JsonBuffer response;
    json_buffer_init(&response, MAX_CAPTURE);
    bool first = true;
    int line = extract_int(json, "line");
    int character = extract_int(json, "character");
    char module[64];

    json_buffer_appendf(&response,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"isIncomplete\":false,\"items\":[", id);

    if (doc && module_context_at(doc, line, character, module, sizeof(module))) {
        const LspItem *items = NULL;
        int count = 0;
        if (module_items(module, &items, &count)) {
            for (int i = 0; i < count; i++) {
                append_completion_item(&response, &first, &items[i]);
            }
        }
    } else {
        for (size_t i = 0; i < sizeof(keyword_items) / sizeof(keyword_items[0]); i++) {
            append_completion_item(&response, &first, &keyword_items[i]);
        }
        for (size_t i = 0; i < sizeof(builtin_items) / sizeof(builtin_items[0]); i++) {
            append_completion_item(&response, &first, &builtin_items[i]);
        }
        for (size_t i = 0; i < sizeof(type_items) / sizeof(type_items[0]); i++) {
            append_completion_item(&response, &first, &type_items[i]);
        }
        if (doc) append_doc_symbol_completions(doc, &response, &first);
    }

    json_buffer_append(&response, "]}}");
    write_buffer_response(&response, id);
    json_buffer_free(&response);
}

static void handle_hover(const char *json, int id) {
    OpenDoc *doc = request_doc(json);
    int line = extract_int(json, "line");
    int character = extract_int(json, "character");
    char word[128];
    char qualified[256];
    int start = 0, end = 0;
    JsonBuffer response;
    json_buffer_init(&response, 4096);

    if (!doc || !word_at_position(doc, line, character, word, sizeof(word), &start, &end)) {
        json_buffer_appendf(&response, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        write_buffer_response(&response, id);
        json_buffer_free(&response);
        return;
    }

    char module[64];
    qualified[0] = '\0';
    if (module_context_at(doc, line, character, module, sizeof(module))) {
        snprintf(qualified, sizeof(qualified), "%s.%s", module, word);
    }

    const LspItem *item = qualified[0] ? find_known_item(qualified) : find_known_item(word);
    if (!item) item = find_known_item(word);
    if (!item) {
        char detail_line[2048];
        int def_line = 0, def_start = 0, def_end = 0;
        if (find_doc_symbol_detail(doc, word, detail_line, sizeof(detail_line),
                                   &def_line, &def_start, &def_end)) {
            json_buffer_appendf(&response,
                "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"contents\":{\"kind\":\"markdown\","
                "\"value\":", id);
            JsonBuffer hover_text;
            json_buffer_init(&hover_text, strlen(detail_line) + 64);
            json_buffer_append(&hover_text, "`");
            json_buffer_append(&hover_text, detail_line);
            json_buffer_append(&hover_text, "`\n\nDeclared in this document.");
            if (!hover_text.failed) json_buffer_append_string(&response, hover_text.data);
            else response.failed = true;
            json_buffer_free(&hover_text);
            json_buffer_appendf(&response,
                "},\"range\":{\"start\":{\"line\":%d,"
                "\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}}",
                line, start, line, end);
            write_buffer_response(&response, id);
            json_buffer_free(&response);
            (void)def_line;
            (void)def_start;
            (void)def_end;
            return;
        }
        json_buffer_appendf(&response, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        write_buffer_response(&response, id);
        json_buffer_free(&response);
        return;
    }

    JsonBuffer hover_text;
    json_buffer_init(&hover_text, 1024);
    json_buffer_append(&hover_text, "`");
    json_buffer_append(&hover_text, item->detail ? item->detail : item->label);
    json_buffer_append(&hover_text, "`\n\n");
    json_buffer_append(&hover_text, item->doc ? item->doc : "");
    json_buffer_appendf(&response,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"contents\":{\"kind\":\"markdown\","
        "\"value\":", id);
    if (!hover_text.failed) json_buffer_append_string(&response, hover_text.data);
    else response.failed = true;
    json_buffer_free(&hover_text);
    json_buffer_appendf(&response,
        "},\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
        "\"end\":{\"line\":%d,\"character\":%d}}}}",
        line, start, line, end);
    write_buffer_response(&response, id);
    json_buffer_free(&response);
}

static void handle_document_symbols(const char *json, int id) {
    OpenDoc *doc = request_doc(json);
    JsonBuffer response;
    json_buffer_init(&response, MAX_CAPTURE);
    bool first = true;
    json_buffer_appendf(&response, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":[", id);

    if (doc && doc->text) {
        const char *line_start = doc->text;
        int line_no = 0;
        while (*line_start) {
            const char *line_end = strchr(line_start, '\n');
            size_t len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
            char *line = malloc(len + 1);
            if (!line) {
                response.failed = true;
                break;
            }
            memcpy(line, line_start, len);
            line[len] = '\0';

            char name[128];
            int start = 0;
            int kind = 13;
            if (parse_symbol_line(line, name, sizeof(name), &start, &kind)) {
                int line_utf16 = utf16_units_for_bytes(line, strlen(line));
                int start_utf16 = utf16_units_for_bytes(line, (size_t)start);
                int end_utf16 = utf16_units_for_bytes(line, (size_t)start + strlen(name));
                if (!first) json_buffer_append(&response, ",");
                json_buffer_append(&response, "{\"name\":");
                json_buffer_append_string(&response, name);
                json_buffer_appendf(&response,
                    ",\"kind\":%d,\"range\":{\"start\":{\"line\":%d,\"character\":0},"
                    "\"end\":{\"line\":%d,\"character\":%d}},\"selectionRange\":{\"start\":{\"line\":%d,"
                    "\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
                    kind, line_no, line_no, line_utf16, line_no, start_utf16, line_no,
                    end_utf16);
                first = false;
            }

            free(line);
            if (!line_end) break;
            line_start = line_end + 1;
            line_no++;
        }
    }

    json_buffer_append(&response, "]}");
    write_buffer_response(&response, id);
    json_buffer_free(&response);
}

static bool find_definition(OpenDoc *doc, const char *needle, int *line_out,
                            int *start_out, int *end_out) {
    if (!doc || !doc->text || !needle || needle[0] == '\0') return false;
    const char *line_start = doc->text;
    int line_no = 0;
    while (*line_start) {
        const char *line_end = strchr(line_start, '\n');
        size_t len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char line[2048];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, line_start, len);
        line[len] = '\0';

        char name[128];
        int start = 0;
        int kind = 13;
        if (parse_symbol_line(line, name, sizeof(name), &start, &kind) &&
            strcmp(name, needle) == 0) {
            *line_out = line_no;
            *start_out = utf16_units_for_bytes(line, (size_t)start);
            *end_out = utf16_units_for_bytes(line, (size_t)start + strlen(name));
            return true;
        }
        (void)kind;
        if (!line_end) break;
        line_start = line_end + 1;
        line_no++;
    }
    return false;
}

static void handle_definition(const char *json, int id) {
    OpenDoc *doc = request_doc(json);
    int line = extract_int(json, "line");
    int character = extract_int(json, "character");
    char word[128];
    JsonBuffer response;
    json_buffer_init(&response, 4096);
    int start = 0, end = 0;
    int def_line = 0, def_start = 0, def_end = 0;

    char *uri = extract_string(json, "uri");
    if (!doc || !uri ||
        !word_at_position(doc, line, character, word, sizeof(word), &start, &end) ||
        !find_definition(doc, word, &def_line, &def_start, &def_end)) {
        json_buffer_appendf(&response, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        write_buffer_response(&response, id);
        json_buffer_free(&response);
        free(uri);
        return;
    }

    json_buffer_appendf(&response,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"uri\":", id);
    json_buffer_append_string(&response, uri);
    json_buffer_appendf(&response,
        ",\"range\":{\"start\":{\"line\":%d,"
        "\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}}",
        def_line, def_start, def_line, def_end);
    write_buffer_response(&response, id);
    json_buffer_free(&response);
    free(uri);
}

static const char *signature_for_name(const char *name) {
    if (strcmp(name, "print") == 0) return "print(...)";
    if (strcmp(name, "len") == 0) return "len(value)";
    if (strcmp(name, "push") == 0) return "push(array, value)";
    if (strcmp(name, "type") == 0) return "type(value)";
    if (strcmp(name, "tostring") == 0) return "tostring(value)";
    if (strcmp(name, "tonumber") == 0) return "tonumber(value)";
    if (strcmp(name, "input") == 0) return "input(prompt?)";
    if (strcmp(name, "math.pow") == 0) return "math.pow(base, exponent)";
    if (strcmp(name, "math.clamp") == 0) return "math.clamp(value, min, max)";
    if (strcmp(name, "array.push") == 0) return "array.push(array, value)";
    if (strcmp(name, "array.get") == 0) return "array.get(array, index)";
    if (strcmp(name, "dict.get") == 0) return "dict.get(dict, key, default?)";
    if (strcmp(name, "dict.set") == 0) return "dict.set(dict, key, value)";
    if (strcmp(name, "dict.require") == 0) return "dict.require(dict, key)";
    if (strcmp(name, "string.sub") == 0) return "string.sub(string, start, end?)";
    if (strcmp(name, "string.replace") == 0) return "string.replace(string, old, new)";
    if (strcmp(name, "task.run") == 0) return "task.run(function)";
    if (strcmp(name, "coroutine.resume") == 0) return "coroutine.resume(coroutine, ...)";
    return NULL;
}

static bool call_name_at(OpenDoc *doc, int line, int character, char *name, size_t name_size) {
    char text_line[2048];
    if (!copy_doc_line(doc, line, text_line, sizeof(text_line))) return false;
    int len = (int)strlen(text_line);
    character = byte_offset_from_utf16(text_line, character);
    if (character > len) character = len;
    int paren = -1;
    int depth = 0;
    for (int i = character - 1; i >= 0; i--) {
        if (text_line[i] == ')') depth++;
        else if (text_line[i] == '(') {
            if (depth == 0) { paren = i; break; }
            depth--;
        }
    }
    if (paren <= 0) return false;
    int end = paren;
    int start = end - 1;
    while (start >= 0 && (is_ident_char(text_line[start]) || text_line[start] == '.')) start--;
    start++;
    if (start >= end) return false;
    size_t len_name = (size_t)(end - start);
    if (len_name >= name_size) return false;
    memcpy(name, text_line + start, len_name);
    name[len_name] = '\0';
    return true;
}

static bool find_doc_signature(OpenDoc *doc, const char *needle, char *out, size_t out_size) {
    if (!doc || !doc->text || !needle || strchr(needle, '.')) return false;
    const char *line_start = doc->text;
    while (*line_start) {
        const char *line_end = strchr(line_start, '\n');
        size_t len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char line[2048];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, line_start, len);
        line[len] = '\0';
        char tmp[2048];
        memcpy(tmp, line, strlen(line) + 1);
        char *p = trim_line(tmp);
        if (strncmp(p, "export", 6) == 0 && isspace((unsigned char)p[6])) p = (char *)skip_ws(p + 6);
        if (strncmp(p, "extern", 6) == 0 && isspace((unsigned char)p[6])) p = (char *)skip_ws(p + 6);
        if (strncmp(p, "fn", 2) == 0 && isspace((unsigned char)p[2])) {
            const char *name_start = skip_ws(p + 2);
            const char *name_end = name_start;
            while (is_ident_char(*name_end) || *name_end == '.') name_end++;
            size_t name_len = (size_t)(name_end - name_start);
            if (name_len == strlen(needle) && memcmp(name_start, needle, name_len) == 0) {
                snprintf(out, out_size, "%s", p);
                return true;
            }
        }
        if (!line_end) break;
        line_start = line_end + 1;
    }
    return false;
}

static void handle_signature_help(const char *json, int id) {
    OpenDoc *doc = request_doc(json);
    int line = extract_int(json, "line");
    int character = extract_int(json, "character");
    char name[160];
    JsonBuffer response;
    json_buffer_init(&response, 4096);
    const char *sig = NULL;
    name[0] = '\0';

    if (doc && call_name_at(doc, line, character, name, sizeof(name))) {
        sig = signature_for_name(name);
    }
    char doc_sig[2048];
    if (!sig && name[0] && doc && find_doc_signature(doc, name, doc_sig, sizeof(doc_sig))) {
        sig = doc_sig;
    }
    if (!sig) {
        json_buffer_appendf(&response,
            "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"signatures\":[],\"activeSignature\":0,\"activeParameter\":0}}",
            id);
        write_buffer_response(&response, id);
        json_buffer_free(&response);
        return;
    }

    json_buffer_appendf(&response,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"signatures\":[{\"label\":", id);
    json_buffer_append_string(&response, sig);
    json_buffer_append(&response, "}],"
        "\"activeSignature\":0,\"activeParameter\":0}}");
    write_buffer_response(&response, id);
    json_buffer_free(&response);
}

static void handle_rename(const char *json, int id) {
    OpenDoc *doc = request_doc(json);
    int line = extract_int(json, "line");
    int character = extract_int(json, "character");
    char *new_name = extract_string(json, "newName");
    char *uri = extract_string(json, "uri");
    char old_name[128];
    JsonBuffer response;
    json_buffer_init(&response, MAX_CAPTURE);
    bool first = true;
    int start = 0, end = 0;

    if (!doc || !uri || !new_name || !word_at_position(doc, line, character, old_name, sizeof(old_name), &start, &end)) {
        json_buffer_appendf(&response, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
        write_buffer_response(&response, id);
        json_buffer_free(&response);
        free(new_name);
        free(uri);
        return;
    }

    json_buffer_appendf(&response,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"changes\":{", id);
    json_buffer_append_string(&response, uri);
    json_buffer_append(&response, ":[");

    const char *line_start = doc->text;
    int line_no = 0;
    while (*line_start) {
        const char *line_end = strchr(line_start, '\n');
        size_t len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char *line_buf = malloc(len + 1);
        if (!line_buf) {
            response.failed = true;
            break;
        }
        memcpy(line_buf, line_start, len);
        line_buf[len] = '\0';

        const char *p = line_buf;
        while ((p = find_identifier_use(p, old_name)) != NULL) {
            int byte_start = (int)(p - line_buf);
            int byte_end = byte_start + (int)strlen(old_name);
            int s = utf16_units_for_bytes(line_buf, (size_t)byte_start);
            int e = utf16_units_for_bytes(line_buf, (size_t)byte_end);
            if (!first) json_buffer_append(&response, ",");
            first = false;
            json_buffer_appendf(&response,
                "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,"
                "\"character\":%d}},\"newText\":",
                line_no, s, line_no, e);
            json_buffer_append_string(&response, new_name);
            json_buffer_append(&response, "}");
            p += strlen(old_name);
        }

        free(line_buf);
        if (!line_end) break;
        line_start = line_end + 1;
        line_no++;
    }

    json_buffer_append(&response, "]}}}");
    write_buffer_response(&response, id);
    json_buffer_free(&response);
    free(new_name);
    free(uri);
}

static bool line_opens_block(const char *trimmed) {
    if (strncmp(trimmed, "fn ", 3) == 0) return true;
    if (strncmp(trimmed, "export fn ", 10) == 0) return true;
    if (strncmp(trimmed, "struct ", 7) == 0) return true;
    if (strncmp(trimmed, "enum ", 5) == 0) return true;
    if (strncmp(trimmed, "loop", 4) == 0 && !is_ident_char(trimmed[4])) return true;
    if (strncmp(trimmed, "for ", 4) == 0) return true;
    if (strncmp(trimmed, "if ", 3) == 0) return true;
    if (strncmp(trimmed, "try", 3) == 0 && !is_ident_char(trimmed[3])) return true;
    if (strncmp(trimmed, "else", 4) == 0 && !is_ident_char(trimmed[4])) return true;
    if (strncmp(trimmed, "elseif ", 7) == 0) return true;
    if (strncmp(trimmed, "catch", 5) == 0 && !is_ident_char(trimmed[5])) return true;
    return false;
}

static bool line_closes_before(const char *trimmed) {
    return (strncmp(trimmed, "end", 3) == 0 && !is_ident_char(trimmed[3])) ||
           (strncmp(trimmed, "else", 4) == 0 && !is_ident_char(trimmed[4])) ||
           (strncmp(trimmed, "elseif ", 7) == 0) ||
           (strncmp(trimmed, "catch", 5) == 0 && !is_ident_char(trimmed[5]));
}

static bool line_reopens_after(const char *trimmed) {
    return (strncmp(trimmed, "else", 4) == 0 && !is_ident_char(trimmed[4])) ||
           (strncmp(trimmed, "elseif ", 7) == 0) ||
           (strncmp(trimmed, "catch", 5) == 0 && !is_ident_char(trimmed[5]));
}

static void handle_formatting(const char *json, int id) {
    OpenDoc *doc = request_doc(json);
    JsonBuffer formatted;
    JsonBuffer response;
    json_buffer_init(&formatted, doc && doc->text ? strlen(doc->text) + 256 : 256);
    json_buffer_init(&response, doc && doc->text ? strlen(doc->text) + 512 : 512);
    int indent = 0;

    if (!doc || !doc->text) {
        json_buffer_appendf(&response, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":[]}", id);
        write_buffer_response(&response, id);
        json_buffer_free(&response);
        json_buffer_free(&formatted);
        return;
    }

    const char *line_start = doc->text;
    while (*line_start && !formatted.failed) {
        const char *line_end = strchr(line_start, '\n');
        size_t len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        char *line = malloc(len + 1);
        if (!line) {
            formatted.failed = true;
            break;
        }
        memcpy(line, line_start, len);
        line[len] = '\0';
        char *trimmed = trim_line(line);

        if (line_closes_before(trimmed) && indent > 0) indent--;
        if (trimmed[0] != '\0') {
            for (int i = 0; i < indent; i++) {
                if (!json_buffer_append(&formatted, "    ")) break;
            }
            json_buffer_append(&formatted, trimmed);
        }
        json_buffer_append(&formatted, "\n");
        if (line_reopens_after(trimmed)) {
            indent++;
        } else if (line_opens_block(trimmed) &&
                   !(strncmp(trimmed, "elseif ", 7) == 0) &&
                   !(strncmp(trimmed, "else", 4) == 0 && !is_ident_char(trimmed[4])) &&
                   !(strncmp(trimmed, "catch", 5) == 0 && !is_ident_char(trimmed[5]))) {
            indent++;
        }
        free(line);

        if (!line_end) break;
        line_start = line_end + 1;
    }

    int line_count = count_doc_lines(doc);
    json_buffer_appendf(&response,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
        "\"end\":{\"line\":%d,\"character\":0}},\"newText\":",
        id, line_count);
    if (!formatted.failed) json_buffer_append_string(&response, formatted.data);
    else response.failed = true;
    json_buffer_append(&response, "}]}");
    write_buffer_response(&response, id);
    json_buffer_free(&response);
    json_buffer_free(&formatted);
}

static int semantic_type_for_token(const char *token) {
    static const char *keywords[] = {
        "let", "const", "fn", "return", "if", "elseif", "else", "then", "end",
        "loop", "for", "in", "break", "continue", "struct", "enum", "import",
        "export", "defer", "try", "catch", "type", "extern", "strict", "nocheck",
        "and", "or", "not"
    };
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strcmp(token, keywords[i]) == 0) return 0;
    }
    if (strcmp(token, "true") == 0 || strcmp(token, "false") == 0) return 8;
    if (strcmp(token, "null") == 0) return 9;
    for (size_t i = 0; i < sizeof(builtin_items) / sizeof(builtin_items[0]); i++) {
        if (strcmp(token, builtin_items[i].label) == 0) {
            return builtin_items[i].kind == 9 ? 3 : 2;
        }
    }
    return 1;
}

static bool append_semantic_token(char *json, size_t json_size, int *offset,
                                  int *last_line, int *last_start,
                                  int line, int start, int length, int token_type) {
    if (length <= 0) return true;
    int delta_line = line - *last_line;
    int delta_start = delta_line == 0 ? start - *last_start : start;
    int original_offset = *offset;
    if (*offset > 0 && json[*offset - 1] != '[') {
        if (!append_json(json, json_size, offset, ",")) return false;
    }
    if (!append_json(json, json_size, offset, "%d,%d,%d,%d,0",
                     delta_line, delta_start, length, token_type)) {
        *offset = original_offset;
        json[*offset] = '\0';
        return false;
    }
    *last_line = line;
    *last_start = start;
    return true;
}

static void handle_semantic_tokens(const char *json, int id) {
    OpenDoc *doc = request_doc(json);
    size_t source_length = doc && doc->text ? strlen(doc->text) : 0;
    if (source_length > (SIZE_MAX - 1024) / 24) source_length = (SIZE_MAX - 1024) / 24;
    size_t response_size = source_length * 24 + 1024;
    if (response_size < MAX_CAPTURE) response_size = MAX_CAPTURE;
    char *response = malloc(response_size);
    if (!response) {
        char error_response[256];
        snprintf(error_response, sizeof(error_response),
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":-32603,"
                 "\"message\":\"Out of memory\"}}", id);
        write_response(error_response);
        return;
    }
    int offset = 0;
    int last_line = 0;
    int last_start = 0;
    bool full = !append_json(response, response_size, &offset,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"data\":[", id);

    if (!full && doc && doc->text) {
        const char *line_start = doc->text;
        int line_no = 0;
        while (*line_start && !full) {
            const char *line_end = strchr(line_start, '\n');
            size_t len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
            char *line = malloc(len + 1);
            if (!line) {
                full = true;
                break;
            }
            memcpy(line, line_start, len);
            line[len] = '\0';

            for (int i = 0; line[i];) {
                if (line[i] == '/' && line[i + 1] == '/') {
                    int start_utf16 = utf16_units_for_bytes(line, (size_t)i);
                    int length_utf16 = utf16_units_for_bytes(line + i, strlen(line + i));
                    full = !append_semantic_token(response, response_size, &offset,
                                                  &last_line, &last_start, line_no,
                                                  start_utf16, length_utf16, 10);
                    break;
                }
                if (line[i] == '"') {
                    int start = i++;
                    while (line[i] && line[i] != '"') {
                        if (line[i] == '\\' && line[i + 1]) i += 2;
                        else i++;
                    }
                    if (line[i] == '"') i++;
                    int start_utf16 = utf16_units_for_bytes(line, (size_t)start);
                    int length_utf16 = utf16_units_for_bytes(
                        line + start, (size_t)(i - start));
                    full = !append_semantic_token(response, response_size, &offset,
                                                  &last_line, &last_start, line_no,
                                                  start_utf16, length_utf16, 7);
                    if (full) break;
                    continue;
                }
                if (isdigit((unsigned char)line[i])) {
                    int start = i++;
                    while (isdigit((unsigned char)line[i]) || line[i] == '.') i++;
                    int start_utf16 = utf16_units_for_bytes(line, (size_t)start);
                    full = !append_semantic_token(response, response_size, &offset,
                                                  &last_line, &last_start, line_no,
                                                  start_utf16, i - start, 8);
                    if (full) break;
                    continue;
                }
                if (is_ident_start_char(line[i]) || line[i] == '@') {
                    int start = i;
                    if (line[i] == '@') i++;
                    while (is_ident_char(line[i])) i++;
                    char token[128];
                    int token_len = i - start;
                    int token_start = start;
                    if (line[token_start] == '@') {
                        token_start++;
                        token_len--;
                    }
                    if (token_len > 0 && token_len < (int)sizeof(token)) {
                        memcpy(token, line + token_start, (size_t)token_len);
                        token[token_len] = '\0';
                        int start_utf16 = utf16_units_for_bytes(line, (size_t)start);
                        full = !append_semantic_token(
                            response, response_size, &offset, &last_line, &last_start,
                            line_no, start_utf16, i - start,
                            semantic_type_for_token(token));
                        if (full) break;
                    }
                    continue;
                }
                if (strchr("+-*/%=<>!&|?.", line[i])) {
                    int start_utf16 = utf16_units_for_bytes(line, (size_t)i);
                    full = !append_semantic_token(response, response_size, &offset,
                                                  &last_line, &last_start, line_no,
                                                  start_utf16, 1, 11);
                    if (full) break;
                }
                i++;
            }

            free(line);
            if (!line_end || full) break;
            line_start = line_end + 1;
            line_no++;
        }
    }

    if (!append_json(response, response_size, &offset, "]}}")) {
        response[0] = '\0';
        offset = 0;
        append_json(response, response_size, &offset,
                    "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":-32603,"
                    "\"message\":\"Semantic token response too large\"}}", id);
    }
    write_response(response);
    free(response);
}

static void publish_diagnostics(OpenDoc *doc) {
    if (!doc->text) return;

    FILE *error_stream = tmpfile();
    if (!error_stream) return;
    int fd = fileno(error_stream);
    int old_stderr = dup(STDERR_FILENO);
    if (fd == -1 || old_stderr == -1 || dup2(fd, STDERR_FILENO) == -1) {
        if (old_stderr != -1) close(old_stderr);
        fclose(error_stream);
        return;
    }

    VM *vm = calloc(1, sizeof(VM));
    if (!vm) {
        dup2(old_stderr, STDERR_FILENO);
        close(old_stderr);
        fclose(error_stream);
        return;
    }
    vm_init(vm);
    ObjFunction *func = vm_compile(vm, doc->text);

    fflush(stderr);
    dup2(old_stderr, STDERR_FILENO);
    close(old_stderr);

    rewind(error_stream);
    size_t n = 0;
    n = fread(compiler_err_buf, 1, sizeof(compiler_err_buf) - 1, error_stream);
    fclose(error_stream);
    compiler_err_buf[n] = '\0';

    char *err_output = compiler_err_buf;
    char diag_json[MAX_CAPTURE];
    int offset = 0;
    bool first = true;

    char escaped_uri[2048];
    json_escape(doc->uri, escaped_uri, sizeof(escaped_uri));
    append_json(diag_json, sizeof(diag_json), &offset,
        "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
        "\"params\":{\"uri\":\"%s\",\"diagnostics\":[", escaped_uri);

    if (!func && err_output[0]) {
        const char *line_start = err_output;
        while (*line_start) {
            int line = 0;
            const char *msg_pos = strstr(line_start, "Error");
            if (msg_pos && sscanf(line_start, "[line %d]", &line) == 1) {
                const char *colon = strchr(msg_pos, ':');
                if (colon) {
                    colon++;
                    while (*colon == ' ') colon++;
                    char msg_buf[1024];
                    int i = 0;
                    while (colon[i] && colon[i] != '\n' && i < 1023) {
                        msg_buf[i] = colon[i]; i++;
                    }
                    msg_buf[i] = '\0';
                    append_lsp_diagnostic(diag_json, sizeof(diag_json), &offset, &first,
                                          line - 1, 0, 1000, 1, msg_buf);
                }
            }
            const char *nl = strchr(line_start, '\n');
            if (!nl) break;
            line_start = nl + 1;
        }
    }

    append_json(diag_json, sizeof(diag_json), &offset, "]}}");

    vm_free(vm);
    free(vm);
    write_response(diag_json);
}

static void handle_initialize(int id) {
    char resp[8192];
    snprintf(resp, sizeof(resp),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{"
        "\"capabilities\":{"
        "\"textDocumentSync\":{"
        "\"openClose\":true,"
        "\"change\":1,"
        "\"save\":true"
        "},"
        "\"completionProvider\":{\"resolveProvider\":false,\"triggerCharacters\":[\".\",\"<\",\":\",\"!\",\"\\\"\"]},"
        "\"hoverProvider\":true,"
        "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]},"
        "\"documentSymbolProvider\":true,"
        "\"definitionProvider\":true,"
        "\"renameProvider\":true,"
        "\"documentFormattingProvider\":true,"
        "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":["
        "\"keyword\",\"variable\",\"function\",\"module\",\"property\",\"struct\",\"enum\","
        "\"string\",\"number\",\"null\",\"comment\",\"operator\"],\"tokenModifiers\":[]},"
        "\"full\":true}"
        "},\"serverInfo\":{\"name\":\"Magnesium Language Server\",\"version\":\"1.1.0\"}}}", id);
    write_response(resp);
}

static void handle_did_open(const char *json) {
    char *uri = extract_string(json, "uri");
    if (!uri) return;
    OpenDoc *doc = add_or_find_doc(uri);
    if (!doc) { free(uri); return; }
    char *text = extract_text(json);
    if (text) {
        free(doc->text);
        doc->text = text;
        doc->version = extract_int(json, "version");
        publish_diagnostics(doc);
    }
    free(uri);
}

static void handle_did_change(const char *json) {
    char *uri = extract_string(json, "uri");
    if (!uri) return;
    OpenDoc *doc = find_doc(uri);
    if (!doc) { free(uri); return; }
    char *text = extract_text(json);
    if (text) {
        free(doc->text);
        doc->text = text;
        doc->version = extract_int(json, "version");
        publish_diagnostics(doc);
    }
    free(uri);
}

static void handle_did_close(const char *json) {
    char *uri = extract_string(json, "uri");
    if (!uri) return;
    for (int i = 0; i < open_doc_count; i++) {
        if (strcmp(open_docs[i].uri, uri) == 0) {
            free(open_docs[i].text);
            if (i + 1 < open_doc_count) {
                memmove(&open_docs[i], &open_docs[i + 1],
                        (size_t)(open_doc_count - i - 1) * sizeof(OpenDoc));
            }
            open_doc_count--;
            memset(&open_docs[open_doc_count], 0, sizeof(OpenDoc));
            break;
        }
    }
    free(uri);
}

static void handle_shutdown(int id) {
    char resp[256];
    snprintf(resp, sizeof(resp), "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}", id);
    write_response(resp);
}

bool mg_lsp_run(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    while (true) {
        char *msg = read_message();
        if (!msg) break;
        int id = extract_int(msg, "id");
        char *method = extract_string(msg, "method");
        if (method) {
            if (strcmp(method, "initialize") == 0) handle_initialize(id);
            else if (strcmp(method, "textDocument/didOpen") == 0) handle_did_open(msg);
            else if (strcmp(method, "textDocument/didChange") == 0) handle_did_change(msg);
            else if (strcmp(method, "textDocument/didClose") == 0) handle_did_close(msg);
            else if (strcmp(method, "textDocument/completion") == 0) handle_completion(msg, id);
            else if (strcmp(method, "textDocument/hover") == 0) handle_hover(msg, id);
            else if (strcmp(method, "textDocument/signatureHelp") == 0) handle_signature_help(msg, id);
            else if (strcmp(method, "textDocument/documentSymbol") == 0) handle_document_symbols(msg, id);
            else if (strcmp(method, "textDocument/definition") == 0) handle_definition(msg, id);
            else if (strcmp(method, "textDocument/rename") == 0) handle_rename(msg, id);
            else if (strcmp(method, "textDocument/formatting") == 0) handle_formatting(msg, id);
            else if (strcmp(method, "textDocument/semanticTokens/full") == 0) handle_semantic_tokens(msg, id);
            else if (strcmp(method, "shutdown") == 0) handle_shutdown(id);
            else if (strcmp(method, "exit") == 0) { free(method); free(msg); exit(0); }
            free(method);
        }
        free(msg);
    }
    return true;
}
