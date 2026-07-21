/* ========================================================================
 * Register-based VM with computed goto dispatch.
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

#include "magnesium.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h>
  #ifdef IS_ERROR
    #undef IS_ERROR
  #endif
  #define IS_ERROR(v) (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_ERROR)
  #define mg_getcwd _getcwd
#else
  #include <dlfcn.h>
  #include <unistd.h>
  #define mg_getcwd getcwd
#endif

/* Forward declaration for gc_free_all */
extern void gc_free_all(VM *vm);

/* Forward declaration for vm_execute so task.run can call it */
static InterpretResult vm_execute(VM *vm);
static Value native___builtin_spawn(VM *vm, int arg_count, Value *args);
static Value native_coroutine_create(VM *vm, int arg_count, Value *args);
static Value native_coroutine_resume(VM *vm, int arg_count, Value *args);
static Value native_coroutine_yield(VM *vm, int arg_count, Value *args);
static Value native_coroutine_status(VM *vm, int arg_count, Value *args);
static inline void ensure_stack(VM *vm, int needed);
static inline bool dict_get_cached_at(VM *vm, ObjDict *dict, ObjString *key,
                                      uint32_t slot, Value *out);
static inline bool dict_get_cached(VM *vm, ObjDict *dict, ObjString *key, Value *out);
static inline bool dict_get_mono_cached(ObjDict *dict, ObjString *key, Value *out);

/* ========================================================================
 * Native Functions
 * ======================================================================== */
static Value native_print(VM *vm, int arg_count, Value *args) {
    (void)vm;
    for (int i = 0; i < arg_count; i++) {
        if (i > 0) printf("\t");
        print_value(args[i]);
    }
    printf("\n");
    return NULL_VAL;
}

static const char *value_type_name(Value value) {
    if (IS_NULL(value)) return "null";
    if (IS_BOOL(value)) return "bool";
    if (IS_NUMERIC(value)) return "number";
    if (!IS_OBJ(value)) return "unknown";

    switch (AS_OBJ(value)->type) {
        case OBJ_STRING:    return "string";
        case OBJ_ARRAY:     return "array";
        case OBJ_DICT:      return "dict";
        case OBJ_FUNCTION:
        case OBJ_CLOSURE:   return "function";
        case OBJ_UPVALUE:   return "upvalue";
        case OBJ_NATIVE:    return "native";
        case OBJ_FFI:       return "ffi";
        case OBJ_NATIVE_HANDLE: return "native_handle";
        case OBJ_STRUCT:    return "struct";
        case OBJ_INSTANCE:  return "instance";
        case OBJ_ERROR:     return "error";
        case OBJ_VM_TASK:   return "vm_task";
        case OBJ_COROUTINE: return "coroutine";
    }
    return "object";
}

static bool ffi_read_number_arg(VM *vm, ObjFFI *ffi, int index, Value value, double *out) {
    if (!IS_NUMERIC(value)) {
        vm_runtime_error(vm, "FFI %s argument %d expected number, got %s.",
                         ffi->name, index + 1, value_type_name(value));
        return false;
    }
    *out = AS_NUMBER(value);
    return true;
}

static ObjString *current_file_name(VM *vm) {
    if (vm->frame_count == 0) {
        return copy_string(vm, "<host>", 6);
    }
    ObjFunction *fn = vm->frames[vm->frame_count - 1].closure->function;
    ObjString *name = fn->source_name ? fn->source_name : fn->name;
    if (!name) return copy_string(vm, "<script>", 8);
    const char *chars = string_chars(vm, name);
    bool needs_normalize = false;
    for (int i = 0; i < name->length; i++) {
        if (chars[i] == '\\') {
            needs_normalize = true;
            break;
        }
    }
    if (!needs_normalize) return name;
    char *normalized = (char *)malloc((size_t)name->length + 1);
    if (!normalized) {
        fprintf(stderr, "Out of memory normalizing path.\n");
        exit(1);
    }
    for (int i = 0; i < name->length; i++) {
        normalized[i] = chars[i] == '\\' ? '/' : chars[i];
    }
    normalized[name->length] = '\0';
    return take_string(vm, normalized, name->length);
}

static ObjString *current_function_name(VM *vm) {
    if (vm->frame_count == 0) {
        return copy_string(vm, "<host>", 6);
    }
    ObjFunction *fn = vm->frames[vm->frame_count - 1].closure->function;
    if (fn->name && (!fn->source_name || fn->name != fn->source_name)) {
        return fn->name;
    }
    return copy_string(vm, "script", 6);
}

static int current_line_number(VM *vm) {
    if (vm->frame_count == 0) return 0;
    CallFrame *frame = &vm->frames[vm->frame_count - 1];
    ObjFunction *fn = frame->closure->function;
    int offset = (int)(frame->ip - fn->chunk.code - 1);
    if (offset < 0 || offset >= fn->chunk.count) return 0;
    return fn->chunk.lines[offset];
}

static Value make_error_value(VM *vm, const char *kind, const char *message, const char *hint) {
    ObjString *kind_str = copy_string(vm, kind, (int)strlen(kind));
    ObjString *message_str = copy_string(vm, message, (int)strlen(message));
    ObjString *hint_str = hint ? copy_string(vm, hint, (int)strlen(hint)) : NULL;
    ObjString *file = current_file_name(vm);
    ObjString *function = current_function_name(vm);
    return OBJ_VAL(new_error(vm, kind_str, message_str, file,
                             current_line_number(vm), function, hint_str));
}

static Value make_errorf_value(VM *vm, const char *kind, const char *hint,
                               const char *format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    return make_error_value(vm, kind, message, hint);
}

static bool require_args(VM *vm, const char *name, int got, int min, int max);
static bool require_string(VM *vm, const char *name, Value value, ObjString **out);

static void normalize_path_copy(char *dest, size_t dest_size, const char *src) {
    if (dest_size == 0) return;
    if (!src) src = "";
    size_t i = 0;
    for (; i + 1 < dest_size && src[i] != '\0'; i++) {
        dest[i] = src[i] == '\\' ? '/' : src[i];
    }
    dest[i] = '\0';
}

static Value native_len(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count != 1) return NULL_VAL;
    if (IS_STRING(args[0])) return NUMBER_VAL(AS_STRING(args[0])->length);
    if (IS_ARRAY(args[0])) return NUMBER_VAL(AS_ARRAY(args[0])->count);
    if (IS_DICT(args[0])) return NUMBER_VAL(AS_DICT(args[0])->count);
    return NULL_VAL;
}

static Value native_push(VM *vm, int arg_count, Value *args) {
    if (arg_count != 2 || !IS_ARRAY(args[0])) return NULL_VAL;
    array_push(vm, AS_ARRAY(args[0]), args[1]);
    return NULL_VAL;
}

static Value native_type(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1) return NULL_VAL;
    const char *name = value_type_name(args[0]);
    return OBJ_VAL(copy_string(vm, name, (int)strlen(name)));
}

static Value error_to_string(VM *vm, ObjError *err) {
    const char *kind = err->kind ? err->kind->chars : "Error";
    const char *message = err->message ? err->message->chars : "";
    const char *file = err->file ? err->file->chars : "<unknown>";
    const char *function = err->function ? err->function->chars : "<unknown>";
    const char *hint = err->hint ? err->hint->chars : "";
    int needed = snprintf(NULL, 0, "error[%s]: %s\n  at %s:%d in %s%s%s",
                          kind, message, file, err->line, function,
                          hint[0] ? "\n  hint: " : "",
                          hint[0] ? hint : "");
    if (needed < 0) return OBJ_VAL(copy_string(vm, "error", 5));
    char *buffer = (char *)malloc((size_t)needed + 1);
    if (!buffer) {
        fprintf(stderr, "Out of memory formatting error.\n");
        exit(1);
    }
    snprintf(buffer, (size_t)needed + 1, "error[%s]: %s\n  at %s:%d in %s%s%s",
             kind, message, file, err->line, function,
             hint[0] ? "\n  hint: " : "",
             hint[0] ? hint : "");
    return OBJ_VAL(take_string(vm, buffer, needed));
}

static Value value_to_string(VM *vm, Value value) {
    char buf[64];

    if (IS_NULL(value)) return OBJ_VAL(copy_string(vm, "null", 4));
    if (IS_BOOL(value)) return OBJ_VAL(copy_string(vm, AS_BOOL(value) ? "true" : "false",
                                                 AS_BOOL(value) ? 4 : 5));
    if (IS_INT(value)) {
        int32_t num = AS_INT(value);
        if ((uint32_t)num < INT_STR_CACHE_SIZE) {
            ObjString *cached = vm->int_str_cache[num];
            if (cached != NULL) return OBJ_VAL(cached);
            uint32_t whole = (uint32_t)num;
            char tmp[16];
            int len = 0;
            do {
                tmp[len++] = (char)('0' + (whole % 10));
                whole /= 10;
            } while (whole != 0);
            int pos = 0;
            for (int i = 0; i < len; i++) {
                buf[pos++] = tmp[len - i - 1];
            }
            ObjString *s = copy_string(vm, buf, pos);
            vm->int_str_cache[num] = s;
            return OBJ_VAL(s);
        }
        uint32_t whole = num < 0 ? (uint32_t)(-(int64_t)num) : (uint32_t)num;
        char tmp[16];
        int len = 0;
        do {
            tmp[len++] = (char)('0' + (whole % 10));
            whole /= 10;
        } while (whole != 0);
        int pos = 0;
        if (num < 0) buf[pos++] = '-';
        for (int i = 0; i < len; i++) {
            buf[pos++] = tmp[len - i - 1];
        }
        return OBJ_VAL(copy_string(vm, buf, pos));
    }
    if (IS_NUMBER(value)) {
        double num = AS_NUMBER(value);
        if (num != 0.0 || !signbit(num)) {
            double abs_num = num < 0.0 ? -num : num;
            if (abs_num <= 9007199254740991.0) {
                uint64_t whole = (uint64_t)abs_num;
                if ((double)whole == abs_num) {
                char tmp[32];
                int len = 0;
                do {
                    tmp[len++] = (char)('0' + (whole % 10));
                    whole /= 10;
                } while (whole != 0);
                    int pos = 0;
                    if (num < 0.0) {
                        buf[pos++] = '-';
                    }
                    for (int i = 0; i < len; i++) {
                        buf[pos++] = tmp[len - i - 1];
                    }
                    return OBJ_VAL(copy_string(vm, buf, pos));
                }
            }
        }
        int len = snprintf(buf, sizeof(buf), "%g", num);
        return OBJ_VAL(copy_string(vm, buf, len));
    }
    if (IS_OBJ(value)) {
        if (IS_STRING(value)) return value;
        if (IS_ERROR(value)) return error_to_string(vm, AS_ERROR(value));
        return OBJ_VAL(copy_string(vm, "<object>", 8));
    }
    return OBJ_VAL(copy_string(vm, "<unknown>", 9));
}

static Value native_tostring(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1) return OBJ_VAL(copy_string(vm, "", 0));
    return value_to_string(vm, args[0]);
}

static Value native_Err(VM *vm, int arg_count, Value *args) {
    if (!require_args(vm, "Err", arg_count, 2, 3)) return NULL_VAL;
    ObjString *kind;
    ObjString *message;
    if (!require_string(vm, "Err", args[0], &kind) ||
        !require_string(vm, "Err", args[1], &message)) return NULL_VAL;
    ObjString *hint = NULL;
    if (arg_count == 3) {
        if (!require_string(vm, "Err", args[2], &hint)) return NULL_VAL;
    }
    return OBJ_VAL(new_error(vm, kind, message, current_file_name(vm),
                             current_line_number(vm), current_function_name(vm), hint));
}

static Value native_tonumber(VM *vm, int arg_count, Value *args) {
    (void)vm;
    if (arg_count != 1) return NULL_VAL;
    if (IS_NUMERIC(args[0])) return args[0];
    if (IS_STRING(args[0])) {
        ObjString *string = AS_STRING(args[0]);
        const char *chars = string_chars(vm, string);
        char *end;
        double val = strtod(chars, &end);
        if (end != chars) return NUMBER_VAL(val);
    }
    return NULL_VAL;
}

static bool require_args(VM *vm, const char *name, int got, int min, int max) {
    if (got < min || (max >= 0 && got > max)) {
        if (min == max) {
            vm_runtime_error(vm, "%s expected %d arguments but got %d.", name, min, got);
        } else if (max < 0) {
            vm_runtime_error(vm, "%s expected at least %d arguments but got %d.", name, min, got);
        } else {
            vm_runtime_error(vm, "%s expected %d to %d arguments but got %d.", name, min, max, got);
        }
        return false;
    }
    return true;
}

static bool require_number(VM *vm, const char *name, Value value, double *out) {
    if (!IS_NUMERIC(value)) {
        vm_runtime_error(vm, "%s expected a number.", name);
        return false;
    }
    *out = AS_NUMBER(value);
    return true;
}

static bool require_string(VM *vm, const char *name, Value value, ObjString **out) {
    if (!IS_STRING(value)) {
        vm_runtime_error(vm, "%s expected a string.", name);
        return false;
    }
    *out = AS_STRING(value);
    return true;
}

static bool require_array(VM *vm, const char *name, Value value, ObjArray **out) {
    if (!IS_ARRAY(value)) {
        vm_runtime_error(vm, "%s expected an array.", name);
        return false;
    }
    *out = AS_ARRAY(value);
    return true;
}

static bool require_dict(VM *vm, const char *name, Value value, ObjDict **out) {
    if (!IS_DICT(value)) {
        vm_runtime_error(vm, "%s expected a dict.", name);
        return false;
    }
    *out = AS_DICT(value);
    return true;
}

static const char *find_bytes(const char *haystack, int haystack_len,
                              const char *needle, int needle_len) {
    if (needle_len == 0) return haystack;
    if (needle_len > haystack_len) return NULL;
    int last = haystack_len - needle_len;
    for (int i = 0; i <= last; i++) {
        if (haystack[i] == needle[0] &&
            memcmp(haystack + i, needle, (size_t)needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static Value native_assert(VM *vm, int arg_count, Value *args) {
    if (!require_args(vm, "assert", arg_count, 1, -1)) return NULL_VAL;
    if (IS_FALSEY(args[0])) {
        const char *message = "assertion failed";
        if (arg_count > 1 && IS_STRING(args[1])) {
            message = string_chars(vm, AS_STRING(args[1]));
        }
        vm_runtime_error(vm, "%s", message);
        return NULL_VAL;
    }
    return args[0];
}

static Value native_error(VM *vm, int arg_count, Value *args) {
    const char *message = "error";
    if (arg_count > 0) {
        Value text = value_to_string(vm, args[0]);
        message = string_chars(vm, AS_STRING(text));
    }
    vm_runtime_error(vm, "%s", message);
    return NULL_VAL;
}

static Value native_input(VM *vm, int arg_count, Value *args) {
    if (!require_args(vm, "input", arg_count, 0, 1)) return NULL_VAL;
    if (arg_count == 1) {
        print_value(args[0]);
        fflush(stdout);
    }

    char buffer[4096];
    if (!fgets(buffer, sizeof(buffer), stdin)) return NULL_VAL;
    int len = (int)strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
        len--;
    }
    return OBJ_VAL(copy_string(vm, buffer, len));
}

static Value native_io_write(VM *vm, int arg_count, Value *args) {
    (void)vm;
    for (int i = 0; i < arg_count; i++) {
        print_value(args[i]);
    }
    fflush(stdout);
    return NULL_VAL;
}

static Value native_math_abs(VM *vm, int arg_count, Value *args) {
    double x;
    if (!require_args(vm, "math.abs", arg_count, 1, 1) ||
        !require_number(vm, "math.abs", args[0], &x)) return NULL_VAL;
    return NUMBER_AUTO_VAL(fabs(x));
}

static Value native_math_floor(VM *vm, int arg_count, Value *args) {
    double x;
    if (!require_args(vm, "math.floor", arg_count, 1, 1) ||
        !require_number(vm, "math.floor", args[0], &x)) return NULL_VAL;
    return NUMBER_AUTO_VAL(floor(x));
}

static Value native_math_ceil(VM *vm, int arg_count, Value *args) {
    double x;
    if (!require_args(vm, "math.ceil", arg_count, 1, 1) ||
        !require_number(vm, "math.ceil", args[0], &x)) return NULL_VAL;
    return NUMBER_AUTO_VAL(ceil(x));
}

static Value native_math_sqrt(VM *vm, int arg_count, Value *args) {
    double x;
    if (!require_args(vm, "math.sqrt", arg_count, 1, 1) ||
        !require_number(vm, "math.sqrt", args[0], &x)) return NULL_VAL;
    return NUMBER_VAL(sqrt(x));
}

static Value native_math_sin(VM *vm, int arg_count, Value *args) {
    double x;
    if (!require_args(vm, "math.sin", arg_count, 1, 1) ||
        !require_number(vm, "math.sin", args[0], &x)) return NULL_VAL;
    return NUMBER_VAL(sin(x));
}

static Value native_math_cos(VM *vm, int arg_count, Value *args) {
    double x;
    if (!require_args(vm, "math.cos", arg_count, 1, 1) ||
        !require_number(vm, "math.cos", args[0], &x)) return NULL_VAL;
    return NUMBER_VAL(cos(x));
}

static Value native_math_tan(VM *vm, int arg_count, Value *args) {
    double x;
    if (!require_args(vm, "math.tan", arg_count, 1, 1) ||
        !require_number(vm, "math.tan", args[0], &x)) return NULL_VAL;
    return NUMBER_VAL(tan(x));
}

static Value native_math_asin(VM *vm, int arg_count, Value *args) {
    double x;
    if (!require_args(vm, "math.asin", arg_count, 1, 1) ||
        !require_number(vm, "math.asin", args[0], &x)) return NULL_VAL;
    return NUMBER_VAL(asin(x));
}

static Value native_math_acos(VM *vm, int arg_count, Value *args) {
    double x;
    if (!require_args(vm, "math.acos", arg_count, 1, 1) ||
        !require_number(vm, "math.acos", args[0], &x)) return NULL_VAL;
    return NUMBER_VAL(acos(x));
}

static Value native_math_atan(VM *vm, int arg_count, Value *args) {
    double x;
    if (!require_args(vm, "math.atan", arg_count, 1, 1) ||
        !require_number(vm, "math.atan", args[0], &x)) return NULL_VAL;
    return NUMBER_VAL(atan(x));
}

static Value native_math_atan2(VM *vm, int arg_count, Value *args) {
    double y, x;
    if (!require_args(vm, "math.atan2", arg_count, 2, 2) ||
        !require_number(vm, "math.atan2", args[0], &y) ||
        !require_number(vm, "math.atan2", args[1], &x)) return NULL_VAL;
    return NUMBER_VAL(atan2(y, x));
}

static Value native_math_pow(VM *vm, int arg_count, Value *args) {
    double a, b;
    if (!require_args(vm, "math.pow", arg_count, 2, 2) ||
        !require_number(vm, "math.pow", args[0], &a) ||
        !require_number(vm, "math.pow", args[1], &b)) return NULL_VAL;
    return NUMBER_AUTO_VAL(pow(a, b));
}

static Value native_math_min(VM *vm, int arg_count, Value *args) {
    if (!require_args(vm, "math.min", arg_count, 1, -1)) return NULL_VAL;
    double best;
    if (!require_number(vm, "math.min", args[0], &best)) return NULL_VAL;
    for (int i = 1; i < arg_count; i++) {
        double x;
        if (!require_number(vm, "math.min", args[i], &x)) return NULL_VAL;
        if (x < best) best = x;
    }
    return NUMBER_AUTO_VAL(best);
}

static Value native_math_max(VM *vm, int arg_count, Value *args) {
    if (!require_args(vm, "math.max", arg_count, 1, -1)) return NULL_VAL;
    double best;
    if (!require_number(vm, "math.max", args[0], &best)) return NULL_VAL;
    for (int i = 1; i < arg_count; i++) {
        double x;
        if (!require_number(vm, "math.max", args[i], &x)) return NULL_VAL;
        if (x > best) best = x;
    }
    return NUMBER_AUTO_VAL(best);
}

static Value native_math_random(VM *vm, int arg_count, Value *args) {
    if (!require_args(vm, "math.random", arg_count, 0, 2)) return NULL_VAL;
    double r = (double)rand() / ((double)RAND_MAX + 1.0);
    if (arg_count == 0) return NUMBER_VAL(r);
    double hi;
    if (!require_number(vm, "math.random", args[arg_count - 1], &hi)) return NULL_VAL;
    double lo = 1.0;
    if (arg_count == 2 && !require_number(vm, "math.random", args[0], &lo)) return NULL_VAL;
    if (hi < lo) {
        vm_runtime_error(vm, "math.random expected upper bound >= lower bound.");
        return NULL_VAL;
    }
    return NUMBER_AUTO_VAL(floor(r * (hi - lo + 1.0)) + lo);
}

static Value native_math_randomseed(VM *vm, int arg_count, Value *args) {
    double seed;
    if (!require_args(vm, "math.randomseed", arg_count, 1, 1) ||
        !require_number(vm, "math.randomseed", args[0], &seed)) return NULL_VAL;
    srand((unsigned int)seed);
    return NULL_VAL;
}

static Value native_math_round(VM *vm, int arg_count, Value *args) {
    double x;
    if (!require_args(vm, "math.round", arg_count, 1, 1) ||
        !require_number(vm, "math.round", args[0], &x)) return NULL_VAL;
    return NUMBER_AUTO_VAL(round(x));
}

static Value native_math_clamp(VM *vm, int arg_count, Value *args) {
    double x, lo, hi;
    if (!require_args(vm, "math.clamp", arg_count, 3, 3) ||
        !require_number(vm, "math.clamp", args[0], &x) ||
        !require_number(vm, "math.clamp", args[1], &lo) ||
        !require_number(vm, "math.clamp", args[2], &hi)) return NULL_VAL;
    if (hi < lo) {
        vm_runtime_error(vm, "math.clamp expected max >= min.");
        return NULL_VAL;
    }
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    return NUMBER_AUTO_VAL(x);
}

static Value native_string_len(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    if (!require_args(vm, "string.len", arg_count, 1, 1) ||
        !require_string(vm, "string.len", args[0], &s)) return NULL_VAL;
    return INT_VAL(s->length);
}

static Value native_string_lower(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    if (!require_args(vm, "string.lower", arg_count, 1, 1) ||
        !require_string(vm, "string.lower", args[0], &s)) return NULL_VAL;
    const char *chars = string_chars(vm, s);
    char *out = (char *)malloc((size_t)s->length + 1);
    if (!out) {
        vm_runtime_error(vm, "Out of memory in string.lower.");
        return NULL_VAL;
    }
    for (int i = 0; i < s->length; i++) out[i] = (char)tolower((unsigned char)chars[i]);
    out[s->length] = '\0';
    return OBJ_VAL(take_string(vm, out, s->length));
}

static Value native_string_upper(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    if (!require_args(vm, "string.upper", arg_count, 1, 1) ||
        !require_string(vm, "string.upper", args[0], &s)) return NULL_VAL;
    const char *chars = string_chars(vm, s);
    char *out = (char *)malloc((size_t)s->length + 1);
    if (!out) {
        vm_runtime_error(vm, "Out of memory in string.upper.");
        return NULL_VAL;
    }
    for (int i = 0; i < s->length; i++) out[i] = (char)toupper((unsigned char)chars[i]);
    out[s->length] = '\0';
    return OBJ_VAL(take_string(vm, out, s->length));
}

static Value native_string_sub(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    double start_num;
    if (!require_args(vm, "string.sub", arg_count, 2, 3) ||
        !require_string(vm, "string.sub", args[0], &s) ||
        !require_number(vm, "string.sub", args[1], &start_num)) return NULL_VAL;
    double end_num = (double)s->length;
    if (arg_count == 3 && !require_number(vm, "string.sub", args[2], &end_num)) return NULL_VAL;

    int start = (int)start_num;
    int end = (int)end_num;
    if (start < 0) start = s->length + start + 1;
    if (end < 0) end = s->length + end + 1;
    if (start < 1) start = 1;
    if (end > s->length) end = s->length;
    if (start > end) return OBJ_VAL(copy_string(vm, "", 0));
    const char *chars = string_chars(vm, s);
    return OBJ_VAL(copy_string(vm, chars + start - 1, end - start + 1));
}

static Value native_string_find(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    ObjString *needle;
    if (!require_args(vm, "string.find", arg_count, 2, 3) ||
        !require_string(vm, "string.find", args[0], &s) ||
        !require_string(vm, "string.find", args[1], &needle)) return NULL_VAL;
    int start = 1;
    if (arg_count == 3) {
        double n;
        if (!require_number(vm, "string.find", args[2], &n)) return NULL_VAL;
        start = (int)n;
        if (start < 0) start = s->length + start + 1;
    }
    if (start < 1) start = 1;
    if (start > s->length + 1) return NULL_VAL;
    const char *chars = string_chars(vm, s);
    const char *needle_chars = string_chars(vm, needle);
    const char *found = find_bytes(chars + start - 1, s->length - start + 1,
                                   needle_chars, needle->length);
    if (!found) return NULL_VAL;
    return INT_VAL((int)(found - chars) + 1);
}

static Value native_string_trim(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    if (!require_args(vm, "string.trim", arg_count, 1, 1) ||
        !require_string(vm, "string.trim", args[0], &s)) return NULL_VAL;
    const char *chars = string_chars(vm, s);
    int start = 0;
    int end = s->length;
    while (start < end && isspace((unsigned char)chars[start])) start++;
    while (end > start && isspace((unsigned char)chars[end - 1])) end--;
    return OBJ_VAL(copy_string(vm, chars + start, end - start));
}

static Value native_string_byte(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    if (!require_args(vm, "string.byte", arg_count, 1, 2) ||
        !require_string(vm, "string.byte", args[0], &s)) return NULL_VAL;
    int index = 1;
    if (arg_count == 2) {
        double n;
        if (!require_number(vm, "string.byte", args[1], &n)) return NULL_VAL;
        index = (int)n;
        if (index < 0) index = s->length + index + 1;
    }
    if (index < 1 || index > s->length) return NULL_VAL;
    return INT_VAL((unsigned char)string_char_at(s, index - 1));
}

static Value native_string_char(VM *vm, int arg_count, Value *args) {
    if (!require_args(vm, "string.char", arg_count, 1, -1)) return NULL_VAL;
    char *out = (char *)malloc((size_t)arg_count + 1);
    if (!out) {
        vm_runtime_error(vm, "Out of memory in string.char.");
        return NULL_VAL;
    }
    for (int i = 0; i < arg_count; i++) {
        double n;
        if (!require_number(vm, "string.char", args[i], &n)) {
            free(out);
            return NULL_VAL;
        }
        int byte = (int)n;
        if (byte < 0 || byte > 255) {
            free(out);
            vm_runtime_error(vm, "string.char expected byte values from 0 to 255.");
            return NULL_VAL;
        }
        out[i] = (char)byte;
    }
    out[arg_count] = '\0';
    return OBJ_VAL(take_string(vm, out, arg_count));
}

static Value native_string_split(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    ObjString *delim;
    if (!require_args(vm, "string.split", arg_count, 2, 2) ||
        !require_string(vm, "string.split", args[0], &s) ||
        !require_string(vm, "string.split", args[1], &delim)) return NULL_VAL;
    const char *chars = string_chars(vm, s);
    const char *delim_chars = string_chars(vm, delim);
    ObjArray *array = new_array(vm);
    vm_push(vm, OBJ_VAL(array));

    if (delim->length == 0) {
        for (int i = 0; i < s->length; i++) {
            array_push(vm, array, OBJ_VAL(copy_string(vm, chars + i, 1)));
        }
    } else {
        const char *cursor = chars;
        int remaining = s->length;
        for (;;) {
            const char *match = find_bytes(cursor, remaining, delim_chars, delim->length);
            if (!match) break;
            array_push(vm, array, OBJ_VAL(copy_string(vm, cursor, (int)(match - cursor))));
            remaining -= (int)((match - cursor) + delim->length);
            cursor = match + delim->length;
        }
        array_push(vm, array, OBJ_VAL(copy_string(vm, cursor, remaining)));
    }

    Value result = vm_pop(vm);
    return result;
}

static Value native_string_contains(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    ObjString *needle;
    if (!require_args(vm, "string.contains", arg_count, 2, 2) ||
        !require_string(vm, "string.contains", args[0], &s) ||
        !require_string(vm, "string.contains", args[1], &needle)) return NULL_VAL;
    return BOOL_VAL(find_bytes(string_chars(vm, s), s->length,
                               string_chars(vm, needle), needle->length) != NULL);
}

static Value native_string_starts_with(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    ObjString *prefix;
    if (!require_args(vm, "string.starts_with", arg_count, 2, 2) ||
        !require_string(vm, "string.starts_with", args[0], &s) ||
        !require_string(vm, "string.starts_with", args[1], &prefix)) return NULL_VAL;
    if (prefix->length > s->length) return FALSE_VAL;
    return BOOL_VAL(memcmp(string_chars(vm, s), string_chars(vm, prefix), (size_t)prefix->length) == 0);
}

static Value native_string_ends_with(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    ObjString *suffix;
    if (!require_args(vm, "string.ends_with", arg_count, 2, 2) ||
        !require_string(vm, "string.ends_with", args[0], &s) ||
        !require_string(vm, "string.ends_with", args[1], &suffix)) return NULL_VAL;
    if (suffix->length > s->length) return FALSE_VAL;
    const char *chars = string_chars(vm, s);
    return BOOL_VAL(memcmp(chars + s->length - suffix->length,
                           string_chars(vm, suffix), (size_t)suffix->length) == 0);
}

static Value native_string_repeat(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    double n;
    if (!require_args(vm, "string.repeat", arg_count, 2, 2) ||
        !require_string(vm, "string.repeat", args[0], &s) ||
        !require_number(vm, "string.repeat", args[1], &n)) return NULL_VAL;
    int count = (int)n;
    if (count <= 0 || s->length == 0) return OBJ_VAL(copy_string(vm, "", 0));
    if (s->length > INT32_MAX / count) {
        vm_runtime_error(vm, "string.repeat result is too large.");
        return NULL_VAL;
    }
    int len = s->length * count;
    char *out = (char *)malloc((size_t)len + 1);
    if (!out) {
        vm_runtime_error(vm, "Out of memory in string.repeat.");
        return NULL_VAL;
    }
    const char *chars = string_chars(vm, s);
    for (int i = 0; i < count; i++) {
        memcpy(out + i * s->length, chars, (size_t)s->length);
    }
    out[len] = '\0';
    return OBJ_VAL(take_string(vm, out, len));
}

static Value native_string_reverse(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    if (!require_args(vm, "string.reverse", arg_count, 1, 1) ||
        !require_string(vm, "string.reverse", args[0], &s)) return NULL_VAL;
    char *out = (char *)malloc((size_t)s->length + 1);
    if (!out) {
        vm_runtime_error(vm, "Out of memory in string.reverse.");
        return NULL_VAL;
    }
    for (int i = 0; i < s->length; i++) {
        out[i] = string_char_at(s, s->length - i - 1);
    }
    out[s->length] = '\0';
    return OBJ_VAL(take_string(vm, out, s->length));
}

static Value native_string_replace(VM *vm, int arg_count, Value *args) {
    ObjString *s;
    ObjString *needle;
    ObjString *replacement;
    if (!require_args(vm, "string.replace", arg_count, 3, 3) ||
        !require_string(vm, "string.replace", args[0], &s) ||
        !require_string(vm, "string.replace", args[1], &needle) ||
        !require_string(vm, "string.replace", args[2], &replacement)) return NULL_VAL;
    if (needle->length == 0) return args[0];

    const char *chars = string_chars(vm, s);
    const char *needle_chars = string_chars(vm, needle);
    const char *repl_chars = string_chars(vm, replacement);
    int count = 0;
    const char *cursor = chars;
    int remaining = s->length;
    const char *match;
    while ((match = find_bytes(cursor, remaining, needle_chars, needle->length)) != NULL) {
        count++;
        int consumed = (int)(match - cursor) + needle->length;
        cursor += consumed;
        remaining -= consumed;
    }
    if (count == 0) return args[0];

    int len = s->length + count * (replacement->length - needle->length);
    char *out = (char *)malloc((size_t)len + 1);
    if (!out) {
        vm_runtime_error(vm, "Out of memory in string.replace.");
        return NULL_VAL;
    }

    cursor = chars;
    remaining = s->length;
    int written = 0;
    while ((match = find_bytes(cursor, remaining, needle_chars, needle->length)) != NULL) {
        int before = (int)(match - cursor);
        memcpy(out + written, cursor, (size_t)before);
        written += before;
        memcpy(out + written, repl_chars, (size_t)replacement->length);
        written += replacement->length;
        int consumed = before + needle->length;
        cursor += consumed;
        remaining -= consumed;
    }
    memcpy(out + written, cursor, (size_t)remaining);
    written += remaining;
    out[written] = '\0';
    return OBJ_VAL(take_string(vm, out, written));
}

static Value native_string_at(VM *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        return make_error_value(vm, "ArgumentError", "string.at expected 2 arguments",
                                "Call string.at(text, zero_based_index).");
    }
    if (!IS_STRING(args[0])) {
        return make_error_value(vm, "TypeError", "string.at expected a string",
                                "Pass the string as the first argument.");
    }
    if (!IS_NUMERIC(args[1])) {
        return make_error_value(vm, "TypeError", "string.at expected a numeric index",
                                "String indexes are zero-based.");
    }
    ObjString *s = AS_STRING(args[0]);
    int index = IS_INT(args[1]) ? AS_INT(args[1]) : (int)AS_DOUBLE(args[1]);
    if (index < 0) index = s->length + index;
    if (index < 0 || index >= s->length) {
        return make_error_value(vm, "IndexError", "String index out of range",
                                "Check string.len(text) before reading this index.");
    }
    char ch = string_char_at(s, index);
    return OBJ_VAL(copy_string(vm, &ch, 1));
}

static Value native_array_new(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (!require_args(vm, "array.new", arg_count, 0, 0)) return NULL_VAL;
    return OBJ_VAL(new_array(vm));
}

static Value native_array_len(VM *vm, int arg_count, Value *args) {
    ObjArray *array;
    if (!require_args(vm, "array.len", arg_count, 1, 1) ||
        !require_array(vm, "array.len", args[0], &array)) return NULL_VAL;
    return INT_VAL(array->count);
}

static Value native_array_pop(VM *vm, int arg_count, Value *args) {
    ObjArray *array;
    if (!require_args(vm, "array.pop", arg_count, 1, 1) ||
        !require_array(vm, "array.pop", args[0], &array)) return NULL_VAL;
    if (array->count == 0) return NULL_VAL;
    Value value = array->items[--array->count];
    array->items[array->count] = NULL_VAL;
    return value;
}

static Value native_array_insert(VM *vm, int arg_count, Value *args) {
    ObjArray *array;
    if (!require_args(vm, "array.insert", arg_count, 2, 3) ||
        !require_array(vm, "array.insert", args[0], &array)) return NULL_VAL;

    int index = array->count + 1;
    Value value = args[1];
    if (arg_count == 3) {
        double n;
        if (!require_number(vm, "array.insert", args[1], &n)) return NULL_VAL;
        index = (int)n;
        value = args[2];
    }
    if (index < 1) index = 1;
    if (index > array->count + 1) index = array->count + 1;
    array_push(vm, array, NULL_VAL);
    for (int i = array->count - 1; i > index - 1; i--) {
        array->items[i] = array->items[i - 1];
    }
    array->items[index - 1] = value;
    return NULL_VAL;
}

static Value native_array_remove(VM *vm, int arg_count, Value *args) {
    ObjArray *array;
    if (!require_args(vm, "array.remove", arg_count, 1, 2) ||
        !require_array(vm, "array.remove", args[0], &array)) return NULL_VAL;
    if (array->count == 0) return NULL_VAL;
    int index = array->count;
    if (arg_count == 2) {
        double n;
        if (!require_number(vm, "array.remove", args[1], &n)) return NULL_VAL;
        index = (int)n;
    }
    if (index < 1 || index > array->count) return NULL_VAL;
    Value value = array->items[index - 1];
    for (int i = index - 1; i < array->count - 1; i++) {
        array->items[i] = array->items[i + 1];
    }
    array->items[--array->count] = NULL_VAL;
    return value;
}

static Value native_array_get(VM *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        return make_error_value(vm, "ArgumentError", "array.get expected 2 arguments",
                                "Call array.get(items, zero_based_index).");
    }
    if (!IS_ARRAY(args[0])) {
        return make_error_value(vm, "TypeError", "array.get expected an array",
                                "Pass the array as the first argument.");
    }
    if (!IS_NUMERIC(args[1])) {
        return make_error_value(vm, "TypeError", "array.get expected a numeric index",
                                "Array indexes are zero-based.");
    }
    ObjArray *array = AS_ARRAY(args[0]);
    int index = IS_INT(args[1]) ? AS_INT(args[1]) : (int)AS_DOUBLE(args[1]);
    if (index < 0) index = array->count + index;
    if (index < 0 || index >= array->count) {
        return make_error_value(vm, "IndexError", "Array index out of range",
                                "Check array.len(items) before reading this index.");
    }
    return array->items[index];
}

static Value native_array_clear(VM *vm, int arg_count, Value *args) {
    ObjArray *array;
    if (!require_args(vm, "array.clear", arg_count, 1, 1) ||
        !require_array(vm, "array.clear", args[0], &array)) return NULL_VAL;
    for (int i = 0; i < array->count; i++) {
        array->items[i] = NULL_VAL;
    }
    array->count = 0;
    return NULL_VAL;
}

static Value native_array_contains(VM *vm, int arg_count, Value *args) {
    ObjArray *array;
    if (!require_args(vm, "array.contains", arg_count, 2, 2) ||
        !require_array(vm, "array.contains", args[0], &array)) return NULL_VAL;
    for (int i = 0; i < array->count; i++) {
        if (values_equal(array->items[i], args[1])) return TRUE_VAL;
    }
    return FALSE_VAL;
}

static Value native_array_index_of(VM *vm, int arg_count, Value *args) {
    ObjArray *array;
    if (!require_args(vm, "array.index_of", arg_count, 2, 2) ||
        !require_array(vm, "array.index_of", args[0], &array)) return NULL_VAL;
    for (int i = 0; i < array->count; i++) {
        if (values_equal(array->items[i], args[1])) return INT_VAL(i);
    }
    return NULL_VAL;
}

static Value native_array_first(VM *vm, int arg_count, Value *args) {
    ObjArray *array;
    if (!require_args(vm, "array.first", arg_count, 1, 1) ||
        !require_array(vm, "array.first", args[0], &array)) return NULL_VAL;
    return array->count > 0 ? array->items[0] : NULL_VAL;
}

static Value native_array_last(VM *vm, int arg_count, Value *args) {
    ObjArray *array;
    if (!require_args(vm, "array.last", arg_count, 1, 1) ||
        !require_array(vm, "array.last", args[0], &array)) return NULL_VAL;
    return array->count > 0 ? array->items[array->count - 1] : NULL_VAL;
}

static Value native_array_extend(VM *vm, int arg_count, Value *args) {
    ObjArray *array;
    ObjArray *other;
    if (!require_args(vm, "array.extend", arg_count, 2, 2) ||
        !require_array(vm, "array.extend", args[0], &array) ||
        !require_array(vm, "array.extend", args[1], &other)) return NULL_VAL;
    for (int i = 0; i < other->count; i++) {
        array_push(vm, array, other->items[i]);
    }
    return args[0];
}

static Value native_array_slice(VM *vm, int arg_count, Value *args) {
    ObjArray *array;
    double start_num;
    if (!require_args(vm, "array.slice", arg_count, 2, 3) ||
        !require_array(vm, "array.slice", args[0], &array) ||
        !require_number(vm, "array.slice", args[1], &start_num)) return NULL_VAL;
    double end_num = (double)array->count;
    if (arg_count == 3 && !require_number(vm, "array.slice", args[2], &end_num)) return NULL_VAL;

    int start = (int)start_num;
    int end = (int)end_num;
    if (start < 0) start = array->count + start;
    if (end < 0) end = array->count + end;
    if (start < 0) start = 0;
    if (end > array->count) end = array->count;
    if (end < start) end = start;

    ObjArray *out = new_array(vm);
    vm_push(vm, OBJ_VAL(out));
    for (int i = start; i < end; i++) {
        array_push(vm, out, array->items[i]);
    }
    return vm_pop(vm);
}

static Value native_dict_new(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (!require_args(vm, "dict.new", arg_count, 0, 0)) return NULL_VAL;
    return OBJ_VAL(new_dict(vm));
}

static Value native_dict_has(VM *vm, int arg_count, Value *args) {
    ObjDict *dict;
    ObjString *key;
    if (!require_args(vm, "dict.has", arg_count, 2, 2) ||
        !require_dict(vm, "dict.has", args[0], &dict) ||
        !require_string(vm, "dict.has", args[1], &key)) return NULL_VAL;
    Value unused;
    return BOOL_VAL(dict_get(dict, key, &unused));
}

static Value native_dict_get(VM *vm, int arg_count, Value *args) {
    ObjDict *dict;
    ObjString *key;
    if (!require_args(vm, "dict.get", arg_count, 2, 3) ||
        !require_dict(vm, "dict.get", args[0], &dict) ||
        !require_string(vm, "dict.get", args[1], &key)) return NULL_VAL;
    Value value;
    if (dict_get(dict, key, &value)) return value;
    return arg_count == 3 ? args[2] : NULL_VAL;
}

static Value native_dict_set(VM *vm, int arg_count, Value *args) {
    ObjDict *dict;
    ObjString *key;
    if (!require_args(vm, "dict.set", arg_count, 3, 3) ||
        !require_dict(vm, "dict.set", args[0], &dict) ||
        !require_string(vm, "dict.set", args[1], &key)) return NULL_VAL;
    dict_set(vm, dict, key, args[2]);
    return args[2];
}

static Value native_dict_delete(VM *vm, int arg_count, Value *args) {
    ObjDict *dict;
    ObjString *key;
    if (!require_args(vm, "dict.delete", arg_count, 2, 2) ||
        !require_dict(vm, "dict.delete", args[0], &dict) ||
        !require_string(vm, "dict.delete", args[1], &key)) return NULL_VAL;
    return BOOL_VAL(dict_delete(dict, key));
}

static Value native_dict_keys(VM *vm, int arg_count, Value *args) {
    ObjDict *dict;
    if (!require_args(vm, "dict.keys", arg_count, 1, 1) ||
        !require_dict(vm, "dict.keys", args[0], &dict)) return NULL_VAL;
    ObjArray *array = new_array(vm);
    vm_push(vm, OBJ_VAL(array));
    for (int i = 0; i < dict->entry_count; i++) {
        DictEntry *entry = &dict->entries[i];
        if (entry->key != NULL && entry->key != TOMBSTONE_KEY) {
            array_push(vm, array, OBJ_VAL(entry->key));
        }
    }
    return vm_pop(vm);
}

static Value native_dict_values(VM *vm, int arg_count, Value *args) {
    ObjDict *dict;
    if (!require_args(vm, "dict.values", arg_count, 1, 1) ||
        !require_dict(vm, "dict.values", args[0], &dict)) return NULL_VAL;
    ObjArray *array = new_array(vm);
    vm_push(vm, OBJ_VAL(array));
    for (int i = 0; i < dict->entry_count; i++) {
        DictEntry *entry = &dict->entries[i];
        if (entry->key != NULL && entry->key != TOMBSTONE_KEY) {
            array_push(vm, array, entry->value);
        }
    }
    return vm_pop(vm);
}

static Value native_dict_require(VM *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        return make_error_value(vm, "ArgumentError", "dict.require expected 2 arguments",
                                "Call dict.require(dict, key).");
    }
    if (!IS_DICT(args[0])) {
        return make_error_value(vm, "TypeError", "dict.require expected a dict",
                                "Pass the dictionary as the first argument.");
    }
    if (!IS_STRING(args[1])) {
        return make_error_value(vm, "TypeError", "dict.require expected a string key",
                                "Dictionary keys are strings.");
    }
    Value value;
    ObjString *key = AS_STRING(args[1]);
    if (dict_get_cached(vm, AS_DICT(args[0]), key, &value)) return value;
    char message[256];
    if (key->chars) {
        snprintf(message, sizeof(message), "Key not found: %.*s", key->length, key->chars);
    } else {
        snprintf(message, sizeof(message), "Key not found");
    }
    return make_error_value(vm, "KeyError", message,
                            "Check dict.has(dict, key) or provide a default value.");
}

static Value native_dict_len(VM *vm, int arg_count, Value *args) {
    ObjDict *dict;
    if (!require_args(vm, "dict.len", arg_count, 1, 1) ||
        !require_dict(vm, "dict.len", args[0], &dict)) return NULL_VAL;
    return INT_VAL(dict->count);
}

static Value native_dict_clear(VM *vm, int arg_count, Value *args) {
    ObjDict *dict;
    if (!require_args(vm, "dict.clear", arg_count, 1, 1) ||
        !require_dict(vm, "dict.clear", args[0], &dict)) return NULL_VAL;
    if (dict->capacity > 0 && dict->indices != NULL) {
        memset(dict->indices, 0, sizeof(int32_t) * dict->capacity);
    }
    for (int i = 0; i < dict->entry_count; i++) {
        dict->entries[i].key = NULL;
        dict->entries[i].hash = 0;
        dict->entries[i].value = NULL_VAL;
    }
    dict->count = 0;
    dict->entry_count = 0;
    dict->version++;
    dict->mono_cache_key = NULL;
    dict->mono_cache_entry = NULL;
    dict->mono_cache_version = 0;
    return NULL_VAL;
}

static Value native_dict_clone(VM *vm, int arg_count, Value *args) {
    ObjDict *dict;
    if (!require_args(vm, "dict.clone", arg_count, 1, 1) ||
        !require_dict(vm, "dict.clone", args[0], &dict)) return NULL_VAL;
    ObjDict *out = new_dict(vm);
    vm_push(vm, OBJ_VAL(out));
    for (int i = 0; i < dict->entry_count; i++) {
        DictEntry *entry = &dict->entries[i];
        if (entry->key != NULL && entry->key != TOMBSTONE_KEY) {
            dict_set(vm, out, entry->key, entry->value);
        }
    }
    return vm_pop(vm);
}

static Value native_dict_merge(VM *vm, int arg_count, Value *args) {
    ObjDict *target;
    ObjDict *source;
    if (!require_args(vm, "dict.merge", arg_count, 2, 2) ||
        !require_dict(vm, "dict.merge", args[0], &target) ||
        !require_dict(vm, "dict.merge", args[1], &source)) return NULL_VAL;
    for (int i = 0; i < source->entry_count; i++) {
        DictEntry *entry = &source->entries[i];
        if (entry->key != NULL && entry->key != TOMBSTONE_KEY) {
            dict_set(vm, target, entry->key, entry->value);
        }
    }
    return args[0];
}

static Value native_os_clock(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (!require_args(vm, "os.clock", arg_count, 0, 0)) return NULL_VAL;
    return NUMBER_VAL((double)clock() / (double)CLOCKS_PER_SEC);
}

static Value native_os_time(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (!require_args(vm, "os.time", arg_count, 0, 0)) return NULL_VAL;
    return NUMBER_AUTO_VAL((double)time(NULL));
}

static Value native_os_getenv(VM *vm, int arg_count, Value *args) {
    ObjString *name;
    if (!require_args(vm, "os.getenv", arg_count, 1, 1) ||
        !require_string(vm, "os.getenv", args[0], &name)) return NULL_VAL;
    const char *value = getenv(string_chars(vm, name));
    if (!value) return NULL_VAL;
    return OBJ_VAL(copy_string(vm, value, (int)strlen(value)));
}

static Value native_process_platform(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count != 0) {
        return make_error_value(vm, "ArgumentError", "process.platform expected no arguments",
                                "Call process.platform().");
    }
#ifdef _WIN32
    return OBJ_VAL(copy_string(vm, "windows", 7));
#elif defined(__APPLE__)
    return OBJ_VAL(copy_string(vm, "macos", 5));
#elif defined(__linux__)
    return OBJ_VAL(copy_string(vm, "linux", 5));
#else
    return OBJ_VAL(copy_string(vm, "unknown", 7));
#endif
}

static bool path_ends_with(const char *path, const char *suffix) {
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    return suffix_len <= path_len &&
           memcmp(path + path_len - suffix_len, suffix, suffix_len) == 0;
}

static bool read_file_buffer(const char *path, char **out, char *err, size_t err_size) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(err, err_size, "Could not open file: %s", path);
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        snprintf(err, err_size, "Could not seek file: %s", path);
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size < 0) {
        snprintf(err, err_size, "Could not read file size: %s", path);
        fclose(file);
        return false;
    }
    rewind(file);

    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        snprintf(err, err_size, "Out of memory reading file: %s", path);
        fclose(file);
        return false;
    }
    size_t read = fread(buffer, 1, (size_t)size, file);
    if (read != (size_t)size) {
        snprintf(err, err_size, "Could not read file: %s", path);
        free(buffer);
        fclose(file);
        return false;
    }
    buffer[read] = '\0';
    fclose(file);
    *out = buffer;
    return true;
}

static bool write_file_buffer(const char *path, const char *data, size_t len,
                              bool append, char *err, size_t err_size) {
    FILE *file = fopen(path, append ? "ab" : "wb");
    if (!file) {
        snprintf(err, err_size, "Could not open file for writing: %s", path);
        return false;
    }
    if (len > 0 && fwrite(data, 1, len, file) != len) {
        snprintf(err, err_size, "Could not write file: %s", path);
        fclose(file);
        return false;
    }
    if (fclose(file) != 0) {
        snprintf(err, err_size, "Could not close file after writing: %s", path);
        return false;
    }
    return true;
}

static Value native_fs_read(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_STRING(args[0])) {
        return make_error_value(vm, "TypeError", "fs.read expected a string path",
                                "Call fs.read(path).");
    }
    ObjString *path = AS_STRING(args[0]);
    char err[512];
    char *buffer = NULL;
    if (!read_file_buffer(string_chars(vm, path), &buffer, err, sizeof(err))) {
        return make_error_value(vm, "IOError", err, "Pass a readable file path.");
    }
    return OBJ_VAL(take_string(vm, buffer, (int)strlen(buffer)));
}

static Value native_fs_write_common(VM *vm, int arg_count, Value *args, bool append,
                                    const char *name) {
    if (arg_count != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
        return make_errorf_value(vm, "TypeError",
                                 append ? "Call fs.append(path, text)." : "Call fs.write(path, text).",
                                 "%s expected a string path and string contents", name);
    }
    ObjString *path = AS_STRING(args[0]);
    ObjString *text = AS_STRING(args[1]);
    char err[512];
    if (!write_file_buffer(string_chars(vm, path), string_chars(vm, text),
                           (size_t)text->length, append, err, sizeof(err))) {
        return make_error_value(vm, "IOError", err, "Check path permissions and parent directories.");
    }
    return TRUE_VAL;
}

static Value native_fs_write(VM *vm, int arg_count, Value *args) {
    return native_fs_write_common(vm, arg_count, args, false, "fs.write");
}

static Value native_fs_append(VM *vm, int arg_count, Value *args) {
    return native_fs_write_common(vm, arg_count, args, true, "fs.append");
}

static Value native_fs_exists(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_STRING(args[0])) {
        return make_error_value(vm, "TypeError", "fs.exists expected a string path",
                                "Call fs.exists(path).");
    }
    FILE *file = fopen(string_chars(vm, AS_STRING(args[0])), "rb");
    if (!file) return FALSE_VAL;
    fclose(file);
    return TRUE_VAL;
}

static Value native_fs_remove(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_STRING(args[0])) {
        return make_error_value(vm, "TypeError", "fs.remove expected a string path",
                                "Call fs.remove(path).");
    }
    const char *path = string_chars(vm, AS_STRING(args[0]));
    if (remove(path) != 0) {
        return make_errorf_value(vm, "IOError", "Only remove files the process can write.",
                                 "Could not remove file: %s", path);
    }
    return TRUE_VAL;
}

static Value native_fs_rename(VM *vm, int arg_count, Value *args) {
    if (arg_count != 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
        return make_error_value(vm, "TypeError", "fs.rename expected two string paths",
                                "Call fs.rename(from, to).");
    }
    const char *from = string_chars(vm, AS_STRING(args[0]));
    const char *to = string_chars(vm, AS_STRING(args[1]));
    if (rename(from, to) != 0) {
        return make_errorf_value(vm, "IOError", "Check that the source exists and the destination is writable.",
                                 "Could not rename file: %s", from);
    }
    return TRUE_VAL;
}

static Value native_fs_cwd(VM *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count != 0) {
        return make_error_value(vm, "ArgumentError", "fs.cwd expected no arguments",
                                "Call fs.cwd().");
    }
    char buffer[4096];
    if (!mg_getcwd(buffer, sizeof(buffer))) {
        return make_error_value(vm, "IOError", "Could not read current working directory",
                                "Check host process state.");
    }
    return OBJ_VAL(copy_string(vm, buffer, (int)strlen(buffer)));
}

static bool is_path_separator(char ch) {
    return ch == '/' || ch == '\\';
}

static uint64_t monotonic_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void sleep_ms(unsigned int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static Value native_path_join(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1) {
        return make_error_value(vm, "ArgumentError", "path.join expected at least one argument",
                                "Call path.join(base, child, ...).");
    }
    size_t total = 0;
    for (int i = 0; i < arg_count; i++) {
        if (!IS_STRING(args[i])) {
            return make_error_value(vm, "TypeError", "path.join expected only string parts",
                                    "Pass path fragments as strings.");
        }
        total += (size_t)AS_STRING(args[i])->length + 1;
    }
    char *buffer = (char *)malloc(total + 1);
    if (!buffer) {
        fprintf(stderr, "Out of memory joining path.\n");
        exit(1);
    }
    int len = 0;
    for (int i = 0; i < arg_count; i++) {
        ObjString *part = AS_STRING(args[i]);
        const char *chars = string_chars(vm, part);
        int start = 0;
        while (start < part->length && is_path_separator(chars[start])) start++;
        if (i == 0) start = 0;
        int part_len = part->length - start;
        if (part_len <= 0) continue;
        if (len > 0 && !is_path_separator(buffer[len - 1])) buffer[len++] = '/';
        memcpy(buffer + len, chars + start, (size_t)part_len);
        len += part_len;
    }
    if (len == 0) buffer[len++] = '.';
    buffer[len] = '\0';
    return OBJ_VAL(take_string(vm, buffer, len));
}

static Value native_path_basename(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_STRING(args[0])) {
        return make_error_value(vm, "TypeError", "path.basename expected a string path",
                                "Call path.basename(path).");
    }
    ObjString *path = AS_STRING(args[0]);
    const char *chars = string_chars(vm, path);
    int start = path->length;
    while (start > 0 && !is_path_separator(chars[start - 1])) start--;
    return OBJ_VAL(copy_string(vm, chars + start, path->length - start));
}

static Value native_path_dirname(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_STRING(args[0])) {
        return make_error_value(vm, "TypeError", "path.dirname expected a string path",
                                "Call path.dirname(path).");
    }
    ObjString *path = AS_STRING(args[0]);
    const char *chars = string_chars(vm, path);
    int end = path->length;
    while (end > 0 && !is_path_separator(chars[end - 1])) end--;
    while (end > 1 && is_path_separator(chars[end - 1])) end--;
    if (end <= 0) return OBJ_VAL(copy_string(vm, ".", 1));
    return OBJ_VAL(copy_string(vm, chars, end));
}

static Value native_path_ext(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_STRING(args[0])) {
        return make_error_value(vm, "TypeError", "path.ext expected a string path",
                                "Call path.ext(path).");
    }
    ObjString *path = AS_STRING(args[0]);
    const char *chars = string_chars(vm, path);
    int base = path->length;
    while (base > 0 && !is_path_separator(chars[base - 1])) base--;
    for (int i = path->length - 1; i >= base; i--) {
        if (chars[i] == '.') {
            return OBJ_VAL(copy_string(vm, chars + i, path->length - i));
        }
    }
    return OBJ_VAL(copy_string(vm, "", 0));
}

static bool vm_task_detach_child(ObjVMTask *task, VM *child) {
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)task->lock);
    bool cancelled = MG_ATOMIC_LOAD_BOOL(task->cancel_requested) ||
                     MG_ATOMIC_LOAD_BOOL(child->cancel_requested);
    task->child_vm = NULL;
    LeaveCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_lock(&task->lock);
    bool cancelled = MG_ATOMIC_LOAD_BOOL(task->cancel_requested) ||
                     MG_ATOMIC_LOAD_BOOL(child->cancel_requested);
    task->child_vm = NULL;
    pthread_mutex_unlock(&task->lock);
#endif
    return cancelled;
}

static void run_isolated_path(ObjVMTask *task, int *result,
                              char *kind, size_t kind_size,
                              char *message, size_t message_size,
                              char *hint, size_t hint_size) {
    const char *path = task->path;
    kind[0] = '\0';
    message[0] = '\0';
    hint[0] = '\0';

    VM child;
    vm_init(&child);
    MG_ATOMIC_STORE_BOOL(child.cancel_requested,
                         MG_ATOMIC_LOAD_BOOL(task->cancel_requested));

#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)task->lock);
    task->child_vm = &child;
    if (MG_ATOMIC_LOAD_BOOL(task->cancel_requested)) {
        MG_ATOMIC_STORE_BOOL(child.cancel_requested, true);
    }
    LeaveCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_lock(&task->lock);
    task->child_vm = &child;
    if (MG_ATOMIC_LOAD_BOOL(task->cancel_requested)) {
        MG_ATOMIC_STORE_BOOL(child.cancel_requested, true);
    }
    pthread_mutex_unlock(&task->lock);
#endif

    InterpretResult run_result;

    if (path_ends_with(path, ".mgc")) {
        ObjFunction *function = vm_load_bytecode(&child, path);
        if (!function) {
            vm_task_detach_child(task, &child);
            vm_free(&child);
            *result = INTERPRET_RUNTIME_ERROR;
            snprintf(kind, kind_size, "BytecodeError");
            snprintf(message, message_size, "Could not load bytecode: %s", path);
            snprintf(hint, hint_size, "Check that the .mgc file exists and matches this Magnesium version.");
            return;
        }
        run_result = vm_run_function(&child, function);
    } else {
        char err_buf[512];
        char *source = NULL;
        if (!read_file_buffer(path, &source, err_buf, sizeof(err_buf))) {
            vm_task_detach_child(task, &child);
            vm_free(&child);
            *result = INTERPRET_RUNTIME_ERROR;
            snprintf(kind, kind_size, "IOError");
            snprintf(message, message_size, "%s", err_buf);
            snprintf(hint, hint_size, "Pass a readable Magnesium script path.");
            return;
        }
        ObjFunction *function = vm_compile_named(&child, source, path);
        if (!function) {
            free(source);
            vm_task_detach_child(task, &child);
            vm_free(&child);
            *result = INTERPRET_COMPILE_ERROR;
            snprintf(kind, kind_size, "CompileError");
            snprintf(message, message_size, "Isolated VM script failed to compile: %s", path);
            snprintf(hint, hint_size, "Run the child script directly for the compiler diagnostic.");
            return;
        }
        run_result = vm_run_function(&child, function);
        free(source);
    }

    bool cancelled = vm_task_detach_child(task, &child);

    vm_free(&child);
    *result = run_result;
    if (cancelled) {
        *result = INTERPRET_RUNTIME_ERROR;
        snprintf(kind, kind_size, "CancelledError");
        snprintf(message, message_size, "Isolated VM script was cancelled: %s", path);
        snprintf(hint, hint_size, "Use vm.cancel(handle) for cooperative cancellation.");
    } else if (run_result == INTERPRET_COMPILE_ERROR) {
        snprintf(kind, kind_size, "CompileError");
        snprintf(message, message_size, "Isolated VM script failed to compile: %s", path);
        snprintf(hint, hint_size, "Run the child script directly for the compiler diagnostic.");
    } else if (run_result == INTERPRET_RUNTIME_ERROR) {
        snprintf(kind, kind_size, "RuntimeError");
        snprintf(message, message_size, "Isolated VM script failed at runtime: %s", path);
        snprintf(hint, hint_size, "Run the child script directly for the runtime diagnostic.");
    }
}

static void vm_task_finish(ObjVMTask *task, int result,
                           const char *kind, const char *message, const char *hint) {
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_lock(&task->lock);
#endif
    task->result = result;
    snprintf(task->error_kind, sizeof(task->error_kind), "%s", kind ? kind : "");
    snprintf(task->error_message, sizeof(task->error_message), "%s", message ? message : "");
    snprintf(task->error_hint, sizeof(task->error_hint), "%s", hint ? hint : "");
    task->state = VM_TASK_DONE;
#ifdef _WIN32
    LeaveCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_unlock(&task->lock);
#endif
}

static bool vm_task_is_done(ObjVMTask *task) {
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_lock(&task->lock);
#endif
    bool done = task->state == VM_TASK_DONE;
#ifdef _WIN32
    LeaveCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_unlock(&task->lock);
#endif
    return done;
}

static bool vm_task_track(VM *vm, ObjVMTask *task) {
    if (vm->vm_tasks.count >= vm->vm_tasks.capacity) {
        int old_capacity = vm->vm_tasks.capacity;
        int new_capacity = old_capacity < 8 ? 8 : old_capacity * 2;
        ObjVMTask **items = (ObjVMTask **)realloc(
            vm->vm_tasks.items, sizeof(ObjVMTask *) * (size_t)new_capacity);
        if (!items) return false;
        vm->vm_tasks.items = items;
        vm->vm_tasks.capacity = new_capacity;
    }
    vm->vm_tasks.items[vm->vm_tasks.count++] = task;
    return true;
}

static void vm_task_untrack(VM *vm, ObjVMTask *task) {
    for (int i = 0; i < vm->vm_tasks.count; i++) {
        if (vm->vm_tasks.items[i] == task) {
            vm->vm_tasks.count--;
            if (i < vm->vm_tasks.count) {
                memmove(vm->vm_tasks.items + i, vm->vm_tasks.items + i + 1,
                        sizeof(ObjVMTask *) * (size_t)(vm->vm_tasks.count - i));
            }
            return;
        }
    }
}

#ifndef _WIN32
static void *isolated_vm_task_main(void *arg) {
    ObjVMTask *task = (ObjVMTask *)arg;
    int result = INTERPRET_RUNTIME_ERROR;
    char kind[64];
    char message[512];
    char hint[512];
    run_isolated_path(task, &result,
                      kind, sizeof(kind),
                      message, sizeof(message),
                      hint, sizeof(hint));
    vm_task_finish(task, result, kind, message, hint);
    return NULL;
}
#else
static DWORD WINAPI isolated_vm_task_main(LPVOID arg) {
    ObjVMTask *task = (ObjVMTask *)arg;
    int result = INTERPRET_RUNTIME_ERROR;
    char kind[64];
    char message[512];
    char hint[512];
    run_isolated_path(task, &result,
                      kind, sizeof(kind),
                      message, sizeof(message),
                      hint, sizeof(hint));
    vm_task_finish(task, result, kind, message, hint);
    return 0;
}
#endif

static void vm_task_join_if_needed(ObjVMTask *task) {
    bool should_join = false;
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)task->lock);
    if (task->started && !task->joined) {
        task->joined = true;
        should_join = true;
    }
    LeaveCriticalSection((CRITICAL_SECTION *)task->lock);
    if (should_join) {
        WaitForSingleObject((HANDLE)task->thread, INFINITE);
        CloseHandle((HANDLE)task->thread);
        task->thread = NULL;
    }
#else
    pthread_mutex_lock(&task->lock);
    if (task->started && !task->joined) {
        task->joined = true;
        should_join = true;
    }
    pthread_mutex_unlock(&task->lock);
    if (should_join) {
        pthread_join(task->thread, NULL);
    }
#endif
}

static bool vm_task_try_join_if_done(ObjVMTask *task) {
    if (!vm_task_is_done(task)) return false;
    vm_task_join_if_needed(task);
    return true;
}

static bool vm_task_join_with_timeout(ObjVMTask *task, int timeout_ms) {
    if (timeout_ms < 0) {
        vm_task_join_if_needed(task);
        return true;
    }
    uint64_t deadline = monotonic_ms() + (uint64_t)timeout_ms;
    while (!vm_task_is_done(task)) {
        if (monotonic_ms() >= deadline) return false;
        sleep_ms(1);
    }
    vm_task_join_if_needed(task);
    return true;
}

static void vm_task_request_cancel(ObjVMTask *task) {
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_lock(&task->lock);
#endif
    MG_ATOMIC_STORE_BOOL(task->cancel_requested, true);
    if (task->child_vm) {
        MG_ATOMIC_STORE_BOOL(task->child_vm->cancel_requested, true);
    }
#ifdef _WIN32
    LeaveCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_unlock(&task->lock);
#endif
}

void vm_task_destroy(ObjVMTask *task) {
    if (!task) return;
    vm_task_join_if_needed(task);
#ifdef _WIN32
    if (task->lock) {
        DeleteCriticalSection((CRITICAL_SECTION *)task->lock);
        free(task->lock);
    }
#else
    pthread_mutex_destroy(&task->lock);
#endif
    free(task->path);
}

static Value native_vm_spawn(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        return make_error_value(vm, "ArgumentError", "vm.spawn expected 1 argument",
                                "Call vm.spawn(path).");
    }
    if (!IS_STRING(args[0])) {
        return make_error_value(vm, "TypeError", "vm.spawn expected a string path",
                                "Pass a .mg or .mgc path.");
    }

    ObjString *path_string = AS_STRING(args[0]);
    const char *path = string_chars(vm, path_string);
    ObjVMTask *task = new_vm_task(vm, path);
    vm_push(vm, OBJ_VAL(task));

    if (!vm_task_track(vm, task)) {
        vm_pop(vm);
        return make_error_value(vm, "MemoryError", "Could not track isolated VM task",
                                "The host process ran out of memory while starting the task.");
    }
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, isolated_vm_task_main, task, 0, NULL);
    if (!thread) {
        vm_task_untrack(vm, task);
        vm_pop(vm);
        return make_error_value(vm, "ThreadError", "Could not start isolated VM thread",
                                "Check process thread limits and host sandbox policy.");
    }
    EnterCriticalSection((CRITICAL_SECTION *)task->lock);
    task->thread = thread;
    task->started = true;
    LeaveCriticalSection((CRITICAL_SECTION *)task->lock);
    vm_pop(vm);
    return OBJ_VAL(task);
#else
    if (pthread_create(&task->thread, NULL, isolated_vm_task_main, task) != 0) {
        vm_task_untrack(vm, task);
        vm_pop(vm);
        return make_error_value(vm, "ThreadError", "Could not start isolated VM thread",
                                "Check process thread limits and host sandbox policy.");
    }
    pthread_mutex_lock(&task->lock);
    task->started = true;
    pthread_mutex_unlock(&task->lock);
    vm_pop(vm);
    return OBJ_VAL(task);
#endif
}

static Value native_vm_status(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_VM_TASK(args[0])) {
        return make_error_value(vm, "TypeError", "vm.status expected a VM task handle",
                                "Pass the value returned by vm.spawn(path).");
    }
    ObjVMTask *task = AS_VM_TASK(args[0]);
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_lock(&task->lock);
#endif
    VMTaskState state = task->state;
    int result = task->result;
#ifdef _WIN32
    LeaveCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_unlock(&task->lock);
#endif
    const char *status = "running";
    if (state == VM_TASK_DONE) {
        status = result == INTERPRET_OK ? "ok" : "error";
    }
    return OBJ_VAL(copy_string(vm, status, (int)strlen(status)));
}

static Value native_vm_join(VM *vm, int arg_count, Value *args) {
    if ((arg_count != 1 && arg_count != 2) || !IS_VM_TASK(args[0])) {
        return make_error_value(vm, "TypeError", "vm.join expected a VM task handle",
                                "Call vm.join(handle) or vm.join(handle, timeout_ms).");
    }
    int timeout_ms = -1;
    if (arg_count == 2) {
        if (!IS_NUMERIC(args[1])) {
            return make_error_value(vm, "TypeError", "vm.join timeout must be a number",
                                    "Pass a timeout in milliseconds.");
        }
        timeout_ms = (int)AS_NUMBER(args[1]);
        if (timeout_ms < 0) timeout_ms = 0;
    }

    ObjVMTask *task = AS_VM_TASK(args[0]);
    if (!vm_task_join_with_timeout(task, timeout_ms)) {
        return make_error_value(vm, "TimeoutError", "Timed out waiting for isolated VM",
                                "Use vm.try_join(handle), vm.cancel(handle), or a larger timeout.");
    }
    vm_task_untrack(vm, task);

    int result;
    char kind[64];
    char message[512];
    char hint[512];
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_lock(&task->lock);
#endif
    result = task->result;
    snprintf(kind, sizeof(kind), "%s", task->error_kind);
    snprintf(message, sizeof(message), "%s", task->error_message);
    snprintf(hint, sizeof(hint), "%s", task->error_hint);
    task->state = VM_TASK_DONE;
#ifdef _WIN32
    LeaveCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_unlock(&task->lock);
#endif

    if (result == INTERPRET_OK) return TRUE_VAL;
    return make_error_value(vm,
                            kind[0] ? kind : "RuntimeError",
                            message[0] ? message : "Isolated VM script failed",
                            hint[0] ? hint : "Run the child script directly for details.");
}

static Value native_vm_cancel(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_VM_TASK(args[0])) {
        return make_error_value(vm, "TypeError", "vm.cancel expected a VM task handle",
                                "Pass the value returned by vm.spawn(path).");
    }
    vm_task_request_cancel(AS_VM_TASK(args[0]));
    return TRUE_VAL;
}

static Value native_vm_try_join(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_VM_TASK(args[0])) {
        return make_error_value(vm, "TypeError", "vm.try_join expected a VM task handle",
                                "Pass the value returned by vm.spawn(path).");
    }

    ObjVMTask *task = AS_VM_TASK(args[0]);
    if (!vm_task_try_join_if_done(task)) return FALSE_VAL;
    vm_task_untrack(vm, task);

    int result;
    char kind[64];
    char message[512];
    char hint[512];
#ifdef _WIN32
    EnterCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_lock(&task->lock);
#endif
    result = task->result;
    snprintf(kind, sizeof(kind), "%s", task->error_kind);
    snprintf(message, sizeof(message), "%s", task->error_message);
    snprintf(hint, sizeof(hint), "%s", task->error_hint);
#ifdef _WIN32
    LeaveCriticalSection((CRITICAL_SECTION *)task->lock);
#else
    pthread_mutex_unlock(&task->lock);
#endif

    if (result == INTERPRET_OK) return TRUE_VAL;
    return make_error_value(vm,
                            kind[0] ? kind : "RuntimeError",
                            message[0] ? message : "Isolated VM script failed",
                            hint[0] ? hint : "Run the child script directly for details.");
}

static void define_native_with_userdata(VM *vm, const char *name, NativeFn function, int arity,
                                        void *userdata, void (*userdata_finalizer)(void *)) {
    ObjString *str = copy_string(vm, name, (int)strlen(name));
    ObjNative *native = new_native(vm, function, str->chars, arity);
    native->userdata = userdata;
    native->userdata_finalizer = userdata_finalizer;
    vm_push(vm, OBJ_VAL(native));
    table_set(&vm->globals, str, OBJ_VAL(native));
    vm_pop(vm);
}

static void define_native(VM *vm, const char *name, NativeFn function, int arity) {
    define_native_with_userdata(vm, name, function, arity, NULL, NULL);
}

static void define_global_value(VM *vm, const char *name, Value value) {
    vm_push(vm, value);
    ObjString *str = copy_string(vm, name, (int)strlen(name));
    table_set(&vm->globals, str, value);
    vm_pop(vm);
}

static void define_native_field(VM *vm, ObjDict *dict, const char *name,
                                NativeFn function, int arity) {
    ObjString *key = copy_string(vm, name, (int)strlen(name));
    ObjNative *native = new_native(vm, function, key->chars, arity);
    vm_push(vm, OBJ_VAL(native));
    dict_set(vm, dict, key, OBJ_VAL(native));
    vm_pop(vm);
}

static ObjDict *define_std_table(VM *vm, const char *name) {
    ObjDict *dict = new_dict(vm);
    define_global_value(vm, name, OBJ_VAL(dict));
    return dict;
}

static void install_stdlib(VM *vm) {
    ObjDict *module_cache = new_dict(vm);
    define_global_value(vm, "@modules", OBJ_VAL(module_cache));

    define_native(vm, "assert", native_assert, -1);
    define_native(vm, "error", native_error, -1);
    define_native(vm, "input", native_input, -1);

    ObjDict *math_table = define_std_table(vm, "math");
    define_native_field(vm, math_table, "abs", native_math_abs, 1);
    define_native_field(vm, math_table, "floor", native_math_floor, 1);
    define_native_field(vm, math_table, "ceil", native_math_ceil, 1);
    define_native_field(vm, math_table, "sqrt", native_math_sqrt, 1);
    define_native_field(vm, math_table, "sin", native_math_sin, 1);
    define_native_field(vm, math_table, "cos", native_math_cos, 1);
    define_native_field(vm, math_table, "tan", native_math_tan, 1);
    define_native_field(vm, math_table, "asin", native_math_asin, 1);
    define_native_field(vm, math_table, "acos", native_math_acos, 1);
    define_native_field(vm, math_table, "atan", native_math_atan, 1);
    define_native_field(vm, math_table, "atan2", native_math_atan2, 2);
    define_native_field(vm, math_table, "pow", native_math_pow, 2);
    define_native_field(vm, math_table, "min", native_math_min, -1);
    define_native_field(vm, math_table, "max", native_math_max, -1);
    define_native_field(vm, math_table, "random", native_math_random, -1);
    define_native_field(vm, math_table, "randomseed", native_math_randomseed, 1);
    define_native_field(vm, math_table, "round", native_math_round, 1);
    define_native_field(vm, math_table, "clamp", native_math_clamp, 3);
    dict_set(vm, math_table, copy_string(vm, "pi", 2), NUMBER_VAL(3.14159265358979323846));
    dict_set(vm, math_table, copy_string(vm, "huge", 4), NUMBER_VAL(INFINITY));

    ObjDict *string_table = define_std_table(vm, "string");
    define_native_field(vm, string_table, "len", native_string_len, 1);
    define_native_field(vm, string_table, "lower", native_string_lower, 1);
    define_native_field(vm, string_table, "upper", native_string_upper, 1);
    define_native_field(vm, string_table, "sub", native_string_sub, -1);
    define_native_field(vm, string_table, "find", native_string_find, -1);
    define_native_field(vm, string_table, "trim", native_string_trim, 1);
    define_native_field(vm, string_table, "byte", native_string_byte, -1);
    define_native_field(vm, string_table, "char", native_string_char, -1);
    define_native_field(vm, string_table, "split", native_string_split, 2);
    define_native_field(vm, string_table, "contains", native_string_contains, 2);
    define_native_field(vm, string_table, "starts_with", native_string_starts_with, 2);
    define_native_field(vm, string_table, "ends_with", native_string_ends_with, 2);
    define_native_field(vm, string_table, "repeat", native_string_repeat, 2);
    define_native_field(vm, string_table, "reverse", native_string_reverse, 1);
    define_native_field(vm, string_table, "replace", native_string_replace, 3);
    define_native_field(vm, string_table, "at", native_string_at, 2);

    ObjDict *array_table = define_std_table(vm, "array");
    define_native_field(vm, array_table, "new", native_array_new, 0);
    define_native_field(vm, array_table, "len", native_array_len, 1);
    define_native_field(vm, array_table, "push", native_push, 2);
    define_native_field(vm, array_table, "pop", native_array_pop, 1);
    define_native_field(vm, array_table, "insert", native_array_insert, -1);
    define_native_field(vm, array_table, "remove", native_array_remove, -1);
    define_native_field(vm, array_table, "get", native_array_get, 2);
    define_native_field(vm, array_table, "at", native_array_get, 2);
    define_native_field(vm, array_table, "clear", native_array_clear, 1);
    define_native_field(vm, array_table, "contains", native_array_contains, 2);
    define_native_field(vm, array_table, "index_of", native_array_index_of, 2);
    define_native_field(vm, array_table, "first", native_array_first, 1);
    define_native_field(vm, array_table, "last", native_array_last, 1);
    define_native_field(vm, array_table, "extend", native_array_extend, 2);
    define_native_field(vm, array_table, "slice", native_array_slice, -1);

    ObjDict *dict_table = define_std_table(vm, "dict");
    define_native_field(vm, dict_table, "new", native_dict_new, 0);
    define_native_field(vm, dict_table, "has", native_dict_has, 2);
    define_native_field(vm, dict_table, "get", native_dict_get, -1);
    define_native_field(vm, dict_table, "set", native_dict_set, 3);
    define_native_field(vm, dict_table, "delete", native_dict_delete, 2);
    define_native_field(vm, dict_table, "keys", native_dict_keys, 1);
    define_native_field(vm, dict_table, "values", native_dict_values, 1);
    define_native_field(vm, dict_table, "require", native_dict_require, 2);
    define_native_field(vm, dict_table, "len", native_dict_len, 1);
    define_native_field(vm, dict_table, "clear", native_dict_clear, 1);
    define_native_field(vm, dict_table, "clone", native_dict_clone, 1);
    define_native_field(vm, dict_table, "merge", native_dict_merge, 2);

    ObjDict *io_table = define_std_table(vm, "io");
    define_native_field(vm, io_table, "write", native_io_write, -1);
    define_native_field(vm, io_table, "read_line", native_input, -1);
    define_native_field(vm, io_table, "read_file", native_fs_read, 1);
    define_native_field(vm, io_table, "write_file", native_fs_write, 2);
    define_native_field(vm, io_table, "append_file", native_fs_append, 2);

    ObjDict *fs_table = define_std_table(vm, "fs");
    define_native_field(vm, fs_table, "read", native_fs_read, 1);
    define_native_field(vm, fs_table, "write", native_fs_write, 2);
    define_native_field(vm, fs_table, "append", native_fs_append, 2);
    define_native_field(vm, fs_table, "exists", native_fs_exists, 1);
    define_native_field(vm, fs_table, "remove", native_fs_remove, 1);
    define_native_field(vm, fs_table, "rename", native_fs_rename, 2);
    define_native_field(vm, fs_table, "cwd", native_fs_cwd, 0);

    ObjDict *path_table = define_std_table(vm, "path");
    define_native_field(vm, path_table, "join", native_path_join, -1);
    define_native_field(vm, path_table, "basename", native_path_basename, 1);
    define_native_field(vm, path_table, "dirname", native_path_dirname, 1);
    define_native_field(vm, path_table, "ext", native_path_ext, 1);

    ObjDict *os_table = define_std_table(vm, "os");
    define_native_field(vm, os_table, "clock", native_os_clock, 0);
    define_native_field(vm, os_table, "time", native_os_time, 0);
    define_native_field(vm, os_table, "getenv", native_os_getenv, 1);

    ObjDict *process_table = define_std_table(vm, "process");
    define_native_field(vm, process_table, "clock", native_os_clock, 0);
    define_native_field(vm, process_table, "time", native_os_time, 0);
    define_native_field(vm, process_table, "getenv", native_os_getenv, 1);
    define_native_field(vm, process_table, "cwd", native_fs_cwd, 0);
    define_native_field(vm, process_table, "platform", native_process_platform, 0);

    ObjDict *task_table = define_std_table(vm, "task");
    define_native_field(vm, task_table, "run", native___builtin_spawn, 1);

    ObjDict *coroutine_table = define_std_table(vm, "coroutine");
    define_native_field(vm, coroutine_table, "create", native_coroutine_create, 1);
    define_native_field(vm, coroutine_table, "resume", native_coroutine_resume, 1);
    define_native_field(vm, coroutine_table, "yield", native_coroutine_yield, -1);
    define_native_field(vm, coroutine_table, "status", native_coroutine_status, 1);

    ObjDict *vm_table = define_std_table(vm, "vm");
    define_native_field(vm, vm_table, "spawn", native_vm_spawn, 1);
    define_native_field(vm, vm_table, "status", native_vm_status, 1);
    define_native_field(vm, vm_table, "join", native_vm_join, -1);
    define_native_field(vm, vm_table, "cancel", native_vm_cancel, 1);
    define_native_field(vm, vm_table, "try_join", native_vm_try_join, 1);
}

static Value native___ffi_bind(VM *vm, int arg_count, Value *args) {
    if (arg_count != 2 || !IS_STRING(args[0]) || !IS_NUMERIC(args[1])) return NULL_VAL;
    const char *name = string_chars(vm, AS_STRING(args[0]));
    int arity = (int)AS_NUMBER(args[1]);
    
#ifdef _WIN32
    HMODULE modules[4];
    modules[0] = GetModuleHandle(NULL);
    modules[1] = GetModuleHandleA("ucrtbase.dll");
    if (!modules[1]) modules[1] = LoadLibraryA("ucrtbase.dll");
    modules[2] = GetModuleHandleA("msvcrt.dll");
    if (!modules[2]) modules[2] = LoadLibraryA("msvcrt.dll");
    modules[3] = GetModuleHandleA("vcruntime140.dll");
    if (!modules[3]) modules[3] = LoadLibraryA("vcruntime140.dll");
    void *ptr = NULL;
    for (int i = 0; i < 4 && !ptr; i++) {
        if (modules[i]) {
            ptr = (void *)GetProcAddress(modules[i], name);
        }
    }
    if (!ptr) {
        vm_runtime_error(vm, "FFI bind failed for '%s': symbol not found", name);
        return NULL_VAL;
    }
#else
    void *ptr = dlsym(RTLD_DEFAULT, name);
    if (!ptr) {
        vm_runtime_error(vm, "FFI bind failed for '%s': %s", name, dlerror());
        return NULL_VAL;
    }
#endif
    return OBJ_VAL(new_ffi(vm, ptr, name, arity));
}

static Value native___builtin_spawn(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_CLOSURE(args[0])) return NULL_VAL;

    if (vm->task_queue.count >= vm->task_queue.capacity) {
        int old_capacity = vm->task_queue.capacity;
        vm->task_queue.capacity = old_capacity < 8 ? 8 : old_capacity * 2;
        ObjClosure **items = realloc(vm->task_queue.items,
            sizeof(ObjClosure *) * vm->task_queue.capacity);
        if (!items) {
            vm_runtime_error(vm, "Out of memory scheduling task.");
            return NULL_VAL;
        }
        vm->task_queue.items = items;
    }

    vm->task_queue.items[vm->task_queue.count++] = AS_CLOSURE(args[0]);
    return NULL_VAL;
}

/* Module loading is handled at compile time (see compiler.c). */

/* ========================================================================
 * VM Init / Free
 * ======================================================================== */
void vm_init(VM *vm) {
    memset(vm, 0, sizeof(*vm));
    vm->frame_count = 0;
    vm->stack = (Value *)malloc(sizeof(Value) * STACK_INIT_SIZE);
    if (!vm->stack) {
        fprintf(stderr, "Out of memory allocating VM stack.\n");
        exit(1);
    }
    for (int i = 0; i < STACK_INIT_SIZE; i++) {
        vm->stack[i] = NULL_VAL;
    }
    vm->stack_size = 0;
    vm->stack_capacity = STACK_INIT_SIZE;
    vm->stack_top = 0;
    table_init(&vm->globals);
    memset(vm->global_ic, 0, sizeof(vm->global_ic));
    memset(vm->field_ic, 0, sizeof(vm->field_ic));
    memset(vm->method_ic, 0, sizeof(vm->method_ic));
    memset(vm->dict_ic, 0, sizeof(vm->dict_ic));
    table_init(&vm->modules);
    table_init(&vm->strings);
    vm->open_upvalues = NULL;
    vm->current_coroutine = NULL;
    vm->yield_requested = false;
    vm->yield_count = 0;
    vm->native_return_count = 0;
    for (int i = 0; i < 256; i++) {
        vm->yield_values[i] = NULL_VAL;
        vm->native_return_values[i] = NULL_VAL;
    }
    vm->saved_contexts = NULL;
    atomic_init(&vm->cancel_requested, false);
    vm->deadline_ms = 0;
    vm->loop_cancel_counter = 1024;
    vm->strings_interned_since_minor = 0;
    vm->upvalue_cache = NULL;
    vm->upvalue_cache_capacity = 0;
    vm->defer_stack.items = NULL;
    vm->defer_stack.count = 0;
    vm->defer_stack.capacity = 0;
    vm->task_queue.items = NULL;
    vm->task_queue.count = 0;
    vm->task_queue.capacity = 0;
    vm->vm_tasks.items = NULL;
    vm->vm_tasks.count = 0;
    vm->vm_tasks.capacity = 0;
    vm->closure_free_list = NULL;
    vm->upvalue_free_list = NULL;
    vm->closure_free_list_count = 0;
    vm->upvalue_free_list_count = 0;
    vm->int_str_cache = (ObjString **)calloc(INT_STR_CACHE_SIZE, sizeof(ObjString *));
    if (!vm->int_str_cache) {
        fprintf(stderr, "Out of memory initializing integer string cache.\n");
        exit(1);
    }
    vm->young_objects = NULL;
    vm->old_objects = NULL;
    vm->bytes_allocated = 0;
    vm->young_bytes = 0;
    vm->next_gc = 1024 * 1024;
    vm->remembered_set = NULL;
    vm->remembered_count = 0;
    vm->remembered_capacity = 0;
    vm->gray_stack = NULL;
    vm->gray_count = 0;
    vm->gray_capacity = 0;
    vm_clear_error(vm);

    /* Register native functions as built-in globals */
    define_native(vm, "__builtin_print", native_print, -1);
    define_native(vm, "__builtin_len", native_len, 1);
    define_native(vm, "__builtin_push", native_push, 2);
    define_native(vm, "__builtin_type", native_type, 1);
    define_native(vm, "__builtin_tostring", native_tostring, 1);
    define_native(vm, "__builtin_tonumber", native_tonumber, 1);
    define_native(vm, "__ffi_bind", native___ffi_bind, 2);

    /* Also register user-facing names as globals (no import needed) */
    define_native(vm, "print", native_print, -1);
    define_native(vm, "len", native_len, 1);
    define_native(vm, "push", native_push, 2);
    define_native(vm, "type", native_type, 1);
    define_native(vm, "tostring", native_tostring, 1);
    define_native(vm, "tonumber", native_tonumber, 1);
    define_native(vm, "Err", native_Err, -1);
    define_native(vm, "Error", native_Err, -1);

    install_stdlib(vm);
}

void vm_free(VM *vm) {
    table_free(&vm->globals);
    table_free(&vm->modules);
    table_free(&vm->strings);
    free(vm->stack);
    free(vm->upvalue_cache);
    free(vm->defer_stack.items);
    free(vm->task_queue.items);
    free(vm->vm_tasks.items);
    free(vm->gray_stack);
    free(vm->int_str_cache);
    gc_free_all(vm);
}

void vm_push(VM *vm, Value value) {
    if (vm->stack_size >= vm->stack_capacity) {
        int old_capacity = vm->stack_capacity;
        vm->stack_capacity *= 2;
        Value *new_stack = realloc(vm->stack, sizeof(Value) * vm->stack_capacity);
        if (!new_stack) {
            fprintf(stderr, "Out of memory growing VM stack.\n");
            exit(1);
        }
        vm->stack = new_stack;
        for (int i = old_capacity; i < vm->stack_capacity; i++) {
            vm->stack[i] = NULL_VAL;
        }
    }
    vm->stack[vm->stack_size++] = value;
    if (vm->stack_size > vm->stack_top) {
        vm->stack_top = vm->stack_size;
    }
}

Value vm_pop(VM *vm) {
    if (vm->stack_size == 0) return NULL_VAL;
    Value value = vm->stack[--vm->stack_size];
    vm->stack[vm->stack_size] = NULL_VAL;
    if (vm->stack_top > vm->stack_size) {
        vm->stack_top = vm->stack_size;
    }
    return value;
}

void vm_runtime_error(VM *vm, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(vm->last_error_message, sizeof(vm->last_error_message), format, args);
    va_end(args);

    fprintf(stderr, "%s", vm->last_error_message);
    fputs("\n", stderr);

    vm->last_error_trace[0] = '\0';
    vm->last_error_file[0] = '\0';
    vm->last_error_function[0] = '\0';
    vm->last_error_line = 0;

    /* Print stack trace */
    for (int i = vm->frame_count - 1; i >= 0; i--) {
        CallFrame *frame = &vm->frames[i];
        ObjFunction *fn = frame->closure->function;
        int offset = (int)(frame->ip - fn->chunk.code - 1);
        int line = fn->chunk.lines[offset >= 0 ? offset : 0];
        const char *fn_name = fn->name ? fn->name->chars : "script";
        if (i == vm->frame_count - 1) {
            vm->last_error_line = line;
            snprintf(vm->last_error_function, sizeof(vm->last_error_function), "%s", fn_name);
            if (fn->source_name) {
                normalize_path_copy(vm->last_error_file, sizeof(vm->last_error_file), fn->source_name->chars);
            }
        }
        size_t used = strlen(vm->last_error_trace);
        if (used < sizeof(vm->last_error_trace) - 1) {
            char display_fn[512];
            normalize_path_copy(display_fn, sizeof(display_fn), fn_name);
            snprintf(vm->last_error_trace + used, sizeof(vm->last_error_trace) - used,
                     "[line %d] in %s()\n", line, display_fn);
        }
        fprintf(stderr, "[line %d] in ", line);
        if (fn->name) {
            char display_name[512];
            normalize_path_copy(display_name, sizeof(display_name), fn->name->chars);
            fprintf(stderr, "%s()\n", display_name);
        } else {
            fprintf(stderr, "script\n");
        }
    }
}

const char *vm_last_error(VM *vm) {
    return vm->last_error_message;
}

const char *vm_last_error_trace(VM *vm) {
    return vm->last_error_trace;
}

int vm_last_error_line(VM *vm) {
    return vm->last_error_line;
}

const char *vm_last_error_file(VM *vm) {
    return vm->last_error_file;
}

const char *vm_last_error_function(VM *vm) {
    return vm->last_error_function;
}

void vm_clear_error(VM *vm) {
    vm->last_error_message[0] = '\0';
    vm->last_error_trace[0] = '\0';
    vm->last_error_file[0] = '\0';
    vm->last_error_function[0] = '\0';
    vm->last_error_line = 0;
}

/* Ensure stack has room for n more values starting at index */
static inline void ensure_stack(VM *vm, int needed) {
    if (vm->stack_capacity >= needed) return;
    while (vm->stack_capacity < needed) {
        int old_cap = vm->stack_capacity;
        vm->stack_capacity *= 2;
        Value *new_stack = realloc(vm->stack, sizeof(Value) * vm->stack_capacity);
        if (!new_stack) {
            fprintf(stderr, "Out of memory growing VM stack.\n");
            exit(1);
        }
        /* Update all slot pointers in call frames */
        if (new_stack != vm->stack) {
            for (int i = 0; i < vm->frame_count; i++) {
                vm->frames[i].slots = new_stack + (vm->frames[i].slots - vm->stack);
            }
        }
        vm->stack = new_stack;
        for (int i = old_cap; i < vm->stack_capacity; i++) {
            vm->stack[i] = NULL_VAL;
        }
    }
}

static inline void clear_stack_range(VM *vm, int from, int to) {
    if (from < 0) from = 0;
    if (to > vm->stack_capacity) to = vm->stack_capacity;
    for (int i = from; i < to; i++) {
        vm->stack[i] = NULL_VAL;
    }
}

static void clear_upvalue_cache(VM *vm) {
    if (vm->upvalue_cache && vm->upvalue_cache_capacity > 0) {
        memset(vm->upvalue_cache, 0, sizeof(ObjUpvalue *) * (size_t)vm->upvalue_cache_capacity);
    }
}

static inline bool dict_key_equal_fast(ObjString *a, ObjString *b) {
    if (a == b) return true;
    if (a->length != b->length || a->hash != b->hash) return false;
    if (!a->is_rope && !b->is_rope) {
        return memcmp(a->chars, b->chars, a->length) == 0;
    }
    for (int i = 0; i < a->length; i++) {
        if (string_char_at(a, i) != string_char_at(b, i)) return false;
    }
    return true;
}

static inline bool dict_get_mono_cached(ObjDict *dict, ObjString *key, Value *out) {
    if (__builtin_expect(dict->mono_cache_key == key &&
                         dict->mono_cache_version == dict->version &&
                         dict->mono_cache_entry != NULL, 1)) {
        DictEntry *entry = dict->mono_cache_entry;
        if (__builtin_expect(entry->key == key, 1)) {
            *out = entry->value;
            return true;
        }
    }

    if (__builtin_expect(dict->count == 0 || dict->capacity == 0, 0)) return false;

    uint32_t key_hash = key->hash;
    uint32_t index = key_hash & (dict->capacity - 1);
    for (;;) {
        int32_t slot = dict->indices[index];
        if (slot == 0) return false;
        if (slot > 0) {
            DictEntry *entry = &dict->entries[slot - 1];
            if (entry->hash == key_hash && entry->key != TOMBSTONE_KEY && dict_key_equal_fast(entry->key, key)) {
                dict->mono_cache_key = key;
                dict->mono_cache_entry = entry;
                dict->mono_cache_version = dict->version;
                *out = entry->value;
                return true;
            }
        }
        index = (index + 1) & (dict->capacity - 1);
    }
}

static inline bool dict_get_cached_at(VM *vm, ObjDict *dict, ObjString *key,
                                      uint32_t slot, Value *out) {
    if (__builtin_expect(dict->count == 0 || dict->capacity == 0, 0)) return false;
    DictICEntry *ic = &vm->dict_ic[slot];
    if (__builtin_expect(ic->dict == dict && ic->key == key &&
                         ic->version == dict->version &&
                         ic->entry != NULL, 1)) {
        DictEntry *entry = ic->entry;
        if (__builtin_expect(entry->key == key, 1)) {
            *out = entry->value;
            return true;
        }
    }

    uint32_t key_hash = key->hash;
    uint32_t index = key_hash & (dict->capacity - 1);
    for (;;) {
        int32_t slot = dict->indices[index];
        if (slot == 0) return false;
        if (slot > 0) {
            DictEntry *entry = &dict->entries[slot - 1];
            if (entry->hash == key_hash && entry->key != TOMBSTONE_KEY && dict_key_equal_fast(entry->key, key)) {
                ic->dict = dict;
                ic->key = key;
                ic->version = dict->version;
                ic->entry = entry;
                ic->index = (int)index;
                *out = entry->value;
                return true;
            }
        }
        index = (index + 1) & (dict->capacity - 1);
    }
}

static inline bool dict_get_cached(VM *vm, ObjDict *dict, ObjString *key, Value *out) {
    uint32_t slot = (((uint32_t)(uintptr_t)dict >> 4) ^ key->hash) & (DICT_IC_SIZE - 1);
    return dict_get_cached_at(vm, dict, key, slot, out);
}

static void coroutine_ensure_stack(ObjCoroutine *co, int needed) {
    if (co->stack_capacity >= needed) return;
    while (co->stack_capacity < needed) {
        int old_capacity = co->stack_capacity;
        co->stack_capacity *= 2;
        Value *new_stack = (Value *)realloc(co->stack, sizeof(Value) * (size_t)co->stack_capacity);
        if (!new_stack) {
            fprintf(stderr, "Out of memory growing coroutine stack.\n");
            exit(1);
        }
        if (new_stack != co->stack) {
            for (int i = 0; i < co->frame_count; i++) {
                co->frames[i].slots = new_stack + (co->frames[i].slots - co->stack);
            }
        }
        co->stack = new_stack;
        for (int i = old_capacity; i < co->stack_capacity; i++) {
            co->stack[i] = NULL_VAL;
        }
    }
}

static void save_vm_context(VM *vm, SavedVMContext *ctx) {
    ctx->stack = vm->stack;
    ctx->stack_capacity = vm->stack_capacity;
    ctx->stack_size = vm->stack_size;
    ctx->stack_top = vm->stack_top;
    memcpy(ctx->frames, vm->frames, sizeof(CallFrame) * (size_t)vm->frame_count);
    ctx->frame_count = vm->frame_count;
    ctx->open_upvalues = vm->open_upvalues;
    ctx->defer_items = vm->defer_stack.items;
    ctx->defer_count = vm->defer_stack.count;
    ctx->defer_capacity = vm->defer_stack.capacity;
    ctx->current_coroutine = vm->current_coroutine;
    ctx->yield_requested = vm->yield_requested;
    ctx->yield_count = vm->yield_count;
    if (ctx->yield_count > 0) {
        memcpy(ctx->yield_values, vm->yield_values,
               sizeof(Value) * (size_t)ctx->yield_count);
    }
    ctx->native_return_count = vm->native_return_count;
    if (ctx->native_return_count > 0) {
        memcpy(ctx->native_return_values, vm->native_return_values,
               sizeof(Value) * (size_t)ctx->native_return_count);
    }
    ctx->next = vm->saved_contexts;
}

static void restore_vm_context(VM *vm, SavedVMContext *ctx) {
    vm->stack = ctx->stack;
    vm->stack_capacity = ctx->stack_capacity;
    vm->stack_size = ctx->stack_size;
    vm->stack_top = ctx->stack_top;
    memcpy(vm->frames, ctx->frames, sizeof(CallFrame) * (size_t)ctx->frame_count);
    vm->frame_count = ctx->frame_count;
    vm->open_upvalues = ctx->open_upvalues;
    vm->defer_stack.items = ctx->defer_items;
    vm->defer_stack.count = ctx->defer_count;
    vm->defer_stack.capacity = ctx->defer_capacity;
    vm->current_coroutine = ctx->current_coroutine;
    vm->yield_requested = ctx->yield_requested;
    vm->yield_count = ctx->yield_count;
    if (vm->yield_count > 0) {
        memcpy(vm->yield_values, ctx->yield_values,
               sizeof(Value) * (size_t)vm->yield_count);
    }
    vm->native_return_count = ctx->native_return_count;
    if (vm->native_return_count > 0) {
        memcpy(vm->native_return_values, ctx->native_return_values,
               sizeof(Value) * (size_t)vm->native_return_count);
    }
    clear_upvalue_cache(vm);
}

static void load_coroutine_context(VM *vm, ObjCoroutine *co) {
    vm->stack = co->stack;
    vm->stack_capacity = co->stack_capacity;
    vm->stack_size = co->stack_size;
    vm->stack_top = co->stack_top;
    memcpy(vm->frames, co->frames, sizeof(CallFrame) * (size_t)co->frame_count);
    vm->frame_count = co->frame_count;
    vm->open_upvalues = co->open_upvalues;
    vm->defer_stack.items = co->defer_items;
    vm->defer_stack.count = co->defer_count;
    vm->defer_stack.capacity = co->defer_capacity;
    vm->current_coroutine = co;
    vm->yield_requested = false;
    vm->yield_count = 0;
    vm->native_return_count = 0;
    clear_upvalue_cache(vm);
}

static void save_coroutine_context(VM *vm, ObjCoroutine *co) {
    co->stack = vm->stack;
    co->stack_capacity = vm->stack_capacity;
    co->stack_size = vm->stack_size;
    co->stack_top = vm->stack_top;
    memcpy(co->frames, vm->frames, sizeof(CallFrame) * (size_t)vm->frame_count);
    co->frame_count = vm->frame_count;
    co->open_upvalues = vm->open_upvalues;
    co->defer_items = vm->defer_stack.items;
    co->defer_count = vm->defer_stack.count;
    co->defer_capacity = vm->defer_stack.capacity;
    co->yield_count = vm->yield_count;
    if (co->yield_count > 0) {
        memcpy(co->yield_values, vm->yield_values,
               sizeof(Value) * (size_t)co->yield_count);
    }
}

static void start_coroutine_context(ObjCoroutine *co) {
    ObjFunction *function = co->closure->function;
    int reg_need = function->reg_count > 0 ? function->reg_count : MAX_REGISTERS;
    coroutine_ensure_stack(co, reg_need);
    for (int i = 0; i < reg_need; i++) co->stack[i] = NULL_VAL;
    co->stack[0] = OBJ_VAL(co->closure);
    co->stack_size = reg_need;
    co->stack_top = reg_need;
    co->frame_count = 1;
    co->open_upvalues = NULL;
    co->frames[0].closure = co->closure;
    co->frames[0].ip = function->chunk.code;
    co->frames[0].slots = co->stack;
    co->frames[0].call_dest = 0;
    co->frames[0].expected_returns = 1;
}

static const char *coroutine_state_name(CoroutineState state) {
    switch (state) {
        case COROUTINE_NEW: return "new";
        case COROUTINE_RUNNING: return "running";
        case COROUTINE_SUSPENDED: return "suspended";
        case COROUTINE_DEAD: return "dead";
    }
    return "unknown";
}

static Value native_coroutine_create(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_CLOSURE(args[0])) {
        return make_error_value(vm, "TypeError", "coroutine.create expected a function",
                                "Call coroutine.create(fn() ... end).");
    }
    ObjCoroutine *co = new_coroutine(vm, AS_CLOSURE(args[0]));
    return OBJ_VAL(co);
}

static Value native_coroutine_yield(VM *vm, int arg_count, Value *args) {
    if (!vm->current_coroutine || vm->current_coroutine->state != COROUTINE_RUNNING) {
        return make_error_value(vm, "StateError", "coroutine.yield called outside a running coroutine",
                                "Only call coroutine.yield from inside a coroutine function.");
    }
    int count = arg_count > 256 ? 256 : arg_count;
    vm->yield_count = count;
    for (int i = 0; i < count; i++) {
        vm->yield_values[i] = args[i];
    }
    vm->yield_requested = true;
    return NULL_VAL;
}

static Value native_coroutine_status(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_COROUTINE(args[0])) {
        return make_error_value(vm, "TypeError", "coroutine.status expected a coroutine",
                                "Pass a value returned by coroutine.create(fn).");
    }
    const char *status = coroutine_state_name(AS_COROUTINE(args[0])->state);
    return OBJ_VAL(copy_string(vm, status, (int)strlen(status)));
}

static Value native_coroutine_resume(VM *vm, int arg_count, Value *args) {
    if (arg_count != 1 || !IS_COROUTINE(args[0])) {
        return make_error_value(vm, "TypeError", "coroutine.resume expected a coroutine",
                                "Pass a value returned by coroutine.create(fn).");
    }
    ObjCoroutine *co = AS_COROUTINE(args[0]);
    if (co->state == COROUTINE_DEAD) {
        return make_error_value(vm, "StateError", "Cannot resume a dead coroutine",
                                "Create a new coroutine after one has finished.");
    }
    if (co->state == COROUTINE_RUNNING) {
        return make_error_value(vm, "StateError", "Cannot resume a running coroutine",
                                "A coroutine cannot recursively resume itself.");
    }

    if (co->state == COROUTINE_NEW) {
        start_coroutine_context(co);
    }

    SavedVMContext caller;
    save_vm_context(vm, &caller);
    vm->saved_contexts = &caller;

    co->state = COROUTINE_RUNNING;
    load_coroutine_context(vm, co);
    InterpretResult result = vm_execute(vm);
    int produced_count = vm->yield_count;
    Value produced[256];
    if (produced_count > 0) {
        memcpy(produced, vm->yield_values, sizeof(Value) * (size_t)produced_count);
    }
    save_coroutine_context(vm, co);

    if (result == INTERPRET_YIELD) {
        co->state = COROUTINE_SUSPENDED;
    } else {
        co->state = COROUTINE_DEAD;
        co->yield_count = 0;
        co->stack_top = 0;
        co->stack_size = 0;
        co->frame_count = 0;
        co->open_upvalues = NULL;
        co->defer_count = 0;
    }

    vm->saved_contexts = caller.next;
    restore_vm_context(vm, &caller);

    if (result == INTERPRET_YIELD || result == INTERPRET_OK) {
        if (produced_count == 0) return TRUE_VAL;
        vm->native_return_count = produced_count;
        memcpy(vm->native_return_values, produced, sizeof(Value) * (size_t)produced_count);
        return produced[0];
    }
    return make_error_value(vm, "RuntimeError", "Coroutine failed while running",
                            "Run the coroutine body directly for the runtime diagnostic.");
}

static inline double number_mod(double a, double b) {
    if (__builtin_expect(b != 0.0, 1)) {
        long long ia = (long long)a;
        long long ib = (long long)b;
        if ((double)ia == a && (double)ib == b && ib != 0) {
            return (double)(ia % ib);
        }
    }
    return fmod(a, b);
}

static inline double number_mod_i(double a, int b) {
    if (__builtin_expect(b != 0, 1)) {
        long long ia = (long long)a;
        if ((double)ia == a) {
            return (double)(ia % b);
        }
    }
    return fmod(a, (double)b);
}

static inline __attribute__((always_inline)) Value int_or_number(long long value) {
    if (__builtin_expect(value >= INT32_MIN && value <= INT32_MAX, 1)) {
        return INT_VAL((int32_t)value);
    }
    return NUMBER_VAL((double)value);
}

/* Capture an upvalue with direct-mapped cache */
static ObjUpvalue *capture_upvalue(VM *vm, Value *local) {
    /* Compute stack slot index for cache lookup */
    int slot_idx = (int)(local - vm->stack);

    /* Check cache first (O(1)) */
    if (slot_idx >= 0 && slot_idx < vm->upvalue_cache_capacity) {
        ObjUpvalue *cached = vm->upvalue_cache[slot_idx];
        if (cached && cached->location == local) {
            return cached;
        }
    }

    /* Cache miss: fall back to linked list walk */
    ObjUpvalue *prev = NULL;
    ObjUpvalue *upvalue = vm->open_upvalues;
    while (upvalue && upvalue->location > local) {
        prev = upvalue;
        upvalue = upvalue->next;
    }
    if (upvalue && upvalue->location == local) {
        /* Found it, populate cache */
        if (slot_idx >= 0) {
            if (slot_idx >= vm->upvalue_cache_capacity) {
                int new_cap = slot_idx + 64;
                vm->upvalue_cache = realloc(vm->upvalue_cache, sizeof(ObjUpvalue *) * new_cap);
                memset(vm->upvalue_cache + vm->upvalue_cache_capacity, 0,
                       sizeof(ObjUpvalue *) * (new_cap - vm->upvalue_cache_capacity));
                vm->upvalue_cache_capacity = new_cap;
            }
            vm->upvalue_cache[slot_idx] = upvalue;
        }
        return upvalue;
    }

    ObjUpvalue *created = new_upvalue(vm, local);
    created->next = upvalue;
    if (prev) {
        prev->next = created;
    } else {
        vm->open_upvalues = created;
    }

    /* Cache the new upvalue */
    if (slot_idx >= 0) {
        if (slot_idx >= vm->upvalue_cache_capacity) {
            int new_cap = slot_idx + 64;
            vm->upvalue_cache = realloc(vm->upvalue_cache, sizeof(ObjUpvalue *) * new_cap);
            memset(vm->upvalue_cache + vm->upvalue_cache_capacity, 0,
                   sizeof(ObjUpvalue *) * (new_cap - vm->upvalue_cache_capacity));
            vm->upvalue_cache_capacity = new_cap;
        }
        vm->upvalue_cache[slot_idx] = created;
    }

    return created;
}

/* Close upvalues at or above a given stack slot */
static void close_upvalues(VM *vm, Value *last) {
    while (vm->open_upvalues && vm->open_upvalues->location >= last) {
        ObjUpvalue *upvalue = vm->open_upvalues;
        /* Invalidate cache entry */
        int slot_idx = (int)(upvalue->location - vm->stack);
        if (slot_idx >= 0 && slot_idx < vm->upvalue_cache_capacity) {
            vm->upvalue_cache[slot_idx] = NULL;
        }
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        gc_write_barrier(vm, (Obj *)upvalue, upvalue->closed);
        vm->open_upvalues = upvalue->next;
    }
}

/* ========================================================================
 * VM Execution Loop (Computed Goto Dispatch)
 * ======================================================================== */
static InterpretResult vm_execute(VM *vm) {
    CallFrame *frame = &vm->frames[vm->frame_count - 1];
    Value * restrict slots = frame->slots;
    Instruction * restrict ip = frame->ip;
    const Value * restrict k = frame->closure->function->chunk.constants;
    uint32_t cancel_counter = vm->loop_cancel_counter;

#ifdef __GNUC__
    static void *dispatch_table[] = {
        &&LBL_LOADK, &&LBL_LOADBOOL, &&LBL_LOADNIL, &&LBL_MOVE,
        &&LBL_GETGLOBAL, &&LBL_SETGLOBAL, &&LBL_GETUPVAL, &&LBL_SETUPVAL,
        &&LBL_ADD, &&LBL_SUB, &&LBL_MUL, &&LBL_DIV, &&LBL_MOD, &&LBL_NEG,
        &&LBL_ADDK, &&LBL_SUBK, &&LBL_MULK, &&LBL_DIVK, &&LBL_MODK,
        &&LBL_ADDI, &&LBL_SUBI, &&LBL_MULI, &&LBL_DIVI, &&LBL_MODI,
        &&LBL_EQ, &&LBL_NEQ, &&LBL_LT, &&LBL_LE,
        &&LBL_EQI, &&LBL_NEQI, &&LBL_LTI, &&LBL_LEI, &&LBL_GTI, &&LBL_GEI,
        &&LBL_EQI_TEST, &&LBL_NEQI_TEST, &&LBL_LTI_TEST,
        &&LBL_LEI_TEST, &&LBL_GTI_TEST, &&LBL_GEI_TEST,
        &&LBL_MODI_EQI_TEST, &&LBL_MODI_NEQI_TEST,
        &&LBL_NOT, &&LBL_TEST, &&LBL_TESTSET, &&LBL_TESTJMP, &&LBL_TESTERRJMP,
        &&LBL_CONCAT, &&LBL_TOSTRING, &&LBL_LEN,
        &&LBL_JMP, &&LBL_LOOP,
        &&LBL_FORPREP, &&LBL_FORPREP_NUM, &&LBL_FORLOOP, &&LBL_FORLOOP_INC,
        &&LBL_FORLOOP_NUM, &&LBL_FORLOOP_INC_NUM,
        &&LBL_FORADDLOCAL_FIELD_PROP, &&LBL_FORADDLOCAL_FIELD_PROP_INC,
        &&LBL_FORADDGLOBAL_FIELD_PROP, &&LBL_FORADDGLOBAL_FIELD_PROP_INC,
        &&LBL_FOR_MODI_ACCUM, &&LBL_FOR_MODI_ACCUM_INC,
        &&LBL_FOR_FIELD2_ACCUM, &&LBL_FOR_FIELD2_ACCUM_INC,
        &&LBL_ARRAY_MARK_FALSE_STRIDE,
        &&LBL_CLOSURE, &&LBL_CALL, &&LBL_CALLG, &&LBL_MCALL, &&LBL_CALLR, &&LBL_CALLSELF,
        &&LBL_ADDUP, &&LBL_ADDLOCAL, &&LBL_SUBLOCAL,
        &&LBL_ADDLOCAL_FIELD_PROP, &&LBL_SUBLOCAL_FIELD_PROP,
        &&LBL_ADDLOCAL_LEN, &&LBL_SUBLOCAL_LEN,
        &&LBL_ADDLOCAL_MULI, &&LBL_SUBLOCAL_MULI, &&LBL_ADDLOCAL_MULK, &&LBL_SUBLOCAL_MULK,
        &&LBL_ADDLOCAL_DIVI, &&LBL_SUBLOCAL_DIVI, &&LBL_ADDLOCAL_MODI, &&LBL_SUBLOCAL_MODI,
        &&LBL_RETURN,
        &&LBL_NEWARRAY, &&LBL_SETARRAY, &&LBL_ARRAY_PUSH, &&LBL_GETINDEX, &&LBL_GETINDEX_TRY, &&LBL_SETINDEX, &&LBL_NEWDICT,
        &&LBL_GETFIELD, &&LBL_GETFIELD_TRY, &&LBL_GETFIELD_PROP, &&LBL_SETFIELD,
        &&LBL_GETFIELD_IDX, &&LBL_SETFIELD_IDX,
        &&LBL_ADDSUB, &&LBL_SUBADD, &&LBL_MULADD, &&LBL_MULSUB,
        &&LBL_ADD_GT_TEST, &&LBL_ADD_GE_TEST, &&LBL_SUB_GT_TEST, &&LBL_SUB_GE_TEST,
        &&LBL_MULLOCAL_ADD, &&LBL_MULLOCAL_SUB,
        &&LBL_ITER_PREP, &&LBL_ITER_NEXT,
        &&LBL_AUX, &&LBL_CLOSE_UPVAL, &&LBL_DEFER, &&LBL_NEWSTRUCT,
        &&LBL_MCALLFIELD, &&LBL_MCALLFIELD0,
        &&LBL_ADDI_LOOP
    };

    #define READ_INST() (*ip++)
    #define DISPATCH()  goto *dispatch_table[GET_OPCODE(*ip)]
    #define NEXT()      do { ip++; DISPATCH(); } while(0)
    #define R(i)        slots[i]
    #define K(i)        k[i]

    #define SAVE_FRAME()   do { frame->ip = ip; } while(0)
    #define LOAD_FRAME()   do { frame = &vm->frames[vm->frame_count-1]; \
                                slots = frame->slots; ip = frame->ip; \
                                k = frame->closure->function->chunk.constants; } while(0)
    #define CHECK_CANCEL() do { \
        if (__builtin_expect(MG_ATOMIC_LOAD_BOOL(vm->cancel_requested) || \
            (vm->deadline_ms != 0 && monotonic_ms() >= vm->deadline_ms), 0)) { \
            MG_ATOMIC_STORE_BOOL(vm->cancel_requested, true); \
            SAVE_FRAME(); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
    } while(0)

    DISPATCH();
#else
    #define READ_INST() (*ip++)
    #define DISPATCH()  goto dispatch_switch
    #define NEXT()      do { ip++; DISPATCH(); } while(0)
    #define R(i)        slots[i]
    #define K(i)        k[i]

    #define SAVE_FRAME()   do { frame->ip = ip; } while(0)
    #define LOAD_FRAME()   do { frame = &vm->frames[vm->frame_count-1]; \
                                slots = frame->slots; ip = frame->ip; \
                                k = frame->closure->function->chunk.constants; } while(0)
    #define CHECK_CANCEL() do { \
        if (MG_ATOMIC_LOAD_BOOL(vm->cancel_requested) || \
            (vm->deadline_ms != 0 && monotonic_ms() >= vm->deadline_ms)) { \
            MG_ATOMIC_STORE_BOOL(vm->cancel_requested, true); \
            SAVE_FRAME(); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
    } while(0)

    dispatch_switch:
    switch (GET_OPCODE(*ip)) {
#endif

    #define CHECK_LOOP_CANCEL() do { \
        if (__builtin_expect(cancel_counter == 0, 0)) { \
            vm->loop_cancel_counter = 1024; \
            cancel_counter = 1024; \
            CHECK_CANCEL(); \
        } \
        cancel_counter--; \
    } while(0)

#define RETURN_VALUES(first_reg, return_count) do {                                      \
    int _first = (first_reg);                                                            \
    int _ret_count = (return_count);                                                      \
    if (vm->open_upvalues) {                                                             \
        close_upvalues(vm, slots);                                                       \
    }                                                                                    \
    vm->frame_count--;                                                                   \
    if (vm->frame_count == 0) {                                                          \
        if (vm->current_coroutine) {                                                     \
            vm->yield_count = _ret_count > 256 ? 256 : _ret_count;                       \
            for (int _i = 0; _i < vm->yield_count; _i++) {                               \
                vm->yield_values[_i] = R(_first + _i);                                   \
            }                                                                            \
        }                                                                                \
        vm->loop_cancel_counter = cancel_counter;                                           \
        return INTERPRET_OK;                                                             \
    }                                                                                    \
    if (_ret_count == 1) {                                                               \
        Value _result = R(_first);                                                       \
        int _call_dest = frame->call_dest;                                               \
        int _expected = frame->expected_returns;                                         \
        frame = &vm->frames[vm->frame_count - 1];                                        \
        slots = frame->slots;                                                            \
        ip = frame->ip;                                                                  \
        k = frame->closure->function->chunk.constants;                                   \
        if (_expected > 0) {                                                             \
            slots[_call_dest] = _result;                                                 \
            for (int _i = 1; _i < _expected; _i++) {                                     \
                slots[_call_dest + _i] = NULL_VAL;                                       \
            }                                                                            \
        }                                                                                \
        DISPATCH();                                                                      \
    }                                                                                    \
    Value _ret_vals[256];                                                                \
    for (int _i = 0; _i < _ret_count; _i++) {                                            \
        _ret_vals[_i] = R(_first + _i);                                                   \
    }                                                                                    \
    {                                                                                    \
        int _call_dest = frame->call_dest;                                               \
        int _expected = frame->expected_returns;                                         \
        LOAD_FRAME();                                                                    \
        int _write_count = (_ret_count < _expected) ? _ret_count : _expected;            \
        for (int _i = 0; _i < _write_count; _i++) {                                      \
            R(_call_dest + _i) = _ret_vals[_i];                                          \
        }                                                                                \
        for (int _i = _write_count; _i < _expected; _i++) {                              \
            R(_call_dest + _i) = NULL_VAL;                                               \
        }                                                                                \
    }                                                                                    \
    DISPATCH();                                                                          \
} while (0)

#define STORE_SINGLE_RESULT(base_reg, expected_count, result_value) do {                 \
    int _base = (base_reg);                                                              \
    int _expected = (expected_count);                                                     \
    if (_expected <= 0) {                                                                \
        break;                                                                           \
    }                                                                                    \
    R(_base) = (result_value);                                                           \
    if (_expected > 1 && IS_ERROR(R(_base))) {                                           \
        R(_base + 1) = R(_base);                                                         \
        R(_base) = NULL_VAL;                                                             \
        for (int _i = 2; _i < _expected; _i++) {                                         \
            R(_base + _i) = NULL_VAL;                                                    \
        }                                                                                \
        break;                                                                           \
    }                                                                                    \
    for (int _i = 1; _i < _expected; _i++) {                                             \
        R(_base + _i) = NULL_VAL;                                                        \
    }                                                                                    \
} while (0)

#define STORE_NATIVE_RESULT(base_reg, expected_count, result_value) do {                 \
    int _native_base = (base_reg);                                                       \
    int _native_expected = (expected_count);                                             \
    if (vm->native_return_count > 0) {                                                   \
        if (_native_expected <= 0) {                                                     \
            vm->native_return_count = 0;                                                 \
            break;                                                                       \
        }                                                                                \
        int _write_count = vm->native_return_count < _native_expected                    \
            ? vm->native_return_count : _native_expected;                                \
        for (int _i = 0; _i < _write_count; _i++) {                                      \
            R(_native_base + _i) = vm->native_return_values[_i];                         \
        }                                                                                \
        for (int _i = _write_count; _i < _native_expected; _i++) {                       \
            R(_native_base + _i) = NULL_VAL;                                             \
        }                                                                                \
        vm->native_return_count = 0;                                                     \
        break;                                                                           \
    }                                                                                    \
    STORE_SINGLE_RESULT(_native_base, _native_expected, (result_value));                 \
} while (0)

#ifndef __GNUC__
    case OP_LOADK:
#endif
LBL_LOADK: {
    Instruction inst = READ_INST();
    R(GET_A(inst)) = K(GET_Bx(inst));
    DISPATCH();
}

#ifndef __GNUC__
    case OP_LOADBOOL:
#endif
LBL_LOADBOOL: {
    Instruction inst = READ_INST();
    R(GET_A(inst)) = BOOL_VAL(GET_B(inst) != 0);
    if (GET_C(inst)) ip++;
    DISPATCH();
}

#ifndef __GNUC__
    case OP_LOADNIL:
#endif
LBL_LOADNIL: {
    Instruction inst = READ_INST();
    int a = GET_A(inst);
    R(a) = NULL_VAL;
    DISPATCH();
}

#ifndef __GNUC__
    case OP_MOVE:
#endif
LBL_MOVE: {
    Instruction inst = READ_INST();
    R(GET_A(inst)) = R(GET_B(inst));
    DISPATCH();
}

#ifndef __GNUC__
    case OP_GETGLOBAL:
#endif
LBL_GETGLOBAL: {
    Instruction inst = READ_INST();
    ObjString *name = AS_STRING(K(GET_Bx(inst)));
    /* Check inline cache first (O(1) pointer comparison) */
    uint32_t ic_slot = ((uint32_t)(uintptr_t)name) >> 4 & (GLOBAL_IC_SIZE - 1);
    GlobalICEntry *ic = &vm->global_ic[ic_slot];
    if (ic->name == name) {
        R(GET_A(inst)) = ic->value;
        DISPATCH();
    }
    /* IC miss: fall back to hash table */
    Value value;
    if (!table_get(&vm->globals, name, &value)) {
        SAVE_FRAME();
        vm_runtime_error(vm, "Undefined variable '%s'.", name->chars);
        return INTERPRET_RUNTIME_ERROR;
    }
    /* Populate IC */
    ic->name = name;
    ic->value = value;
    R(GET_A(inst)) = value;
    DISPATCH();
}

#ifndef __GNUC__
    case OP_SETGLOBAL:
#endif
LBL_SETGLOBAL: {
    Instruction inst = READ_INST();
    ObjString *name = AS_STRING(K(GET_Bx(inst)));
    Value val = R(GET_A(inst));
    table_set(&vm->globals, name, val);
    /* Update inline cache */
    uint32_t ic_slot = ((uint32_t)(uintptr_t)name) >> 4 & (GLOBAL_IC_SIZE - 1);
    vm->global_ic[ic_slot].name = name;
    vm->global_ic[ic_slot].value = val;
    DISPATCH();
}

#ifndef __GNUC__
    case OP_GETUPVAL:
#endif
LBL_GETUPVAL: {
    Instruction inst = READ_INST();
    R(GET_A(inst)) = *frame->closure->upvalues[GET_B(inst)]->location;
    DISPATCH();
}

#ifndef __GNUC__
    case OP_SETUPVAL:
#endif
LBL_SETUPVAL: {
    Instruction inst = READ_INST();
    *frame->closure->upvalues[GET_B(inst)]->location = R(GET_A(inst));
    DISPATCH();
}

/* Arithmetic */
/* Pure numeric ADD (string concat handled by OP_CONCAT) */
#ifndef __GNUC__
    case OP_ADD:
#endif
LBL_ADD: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    Value vc = R(GET_C(inst));
    if (__builtin_expect(IS_INT(vb) && IS_INT(vc), 1)) {
        R(GET_A(inst)) = int_or_number((long long)AS_INT(vb) + (long long)AS_INT(vc));
    } else if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc), 1)) {
        R(GET_A(inst)) = NUMBER_VAL(AS_DOUBLE(vb) + AS_DOUBLE(vc));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {
        R(GET_A(inst)) = NUMBER_VAL(AS_NUMBER(vb) + AS_NUMBER(vc));
    } else if (IS_OBJ(vb) && IS_OBJ(vc) &&
               AS_OBJ(vb)->type == OBJ_STRING && AS_OBJ(vc)->type == OBJ_STRING) {
        SAVE_FRAME();
        R(GET_A(inst)) = OBJ_VAL(concat_strings(vm, (ObjString*)AS_OBJ(vb), (ObjString*)AS_OBJ(vc)));
        LOAD_FRAME();
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm,
                         "Operator '+' expected two numbers or two strings, got %s and %s.",
                         value_type_name(vb), value_type_name(vc));
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#define INT_BINARY_NUM_OP(op_name, operator)                               \
op_name: {                                                                 \
    Instruction inst = READ_INST();                                        \
    Value vb = R(GET_B(inst));                                             \
    Value vc = R(GET_C(inst));                                             \
    if (__builtin_expect(IS_INT(vb) && IS_INT(vc), 1)) {                   \
        R(GET_A(inst)) = int_or_number((long long)AS_INT(vb) operator (long long)AS_INT(vc)); \
    } else if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc), 1)) {     \
        R(GET_A(inst)) = NUMBER_VAL(AS_DOUBLE(vb) operator AS_DOUBLE(vc)); \
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {                         \
        R(GET_A(inst)) = NUMBER_VAL(AS_NUMBER(vb) operator AS_NUMBER(vc)); \
    } else {                                                               \
        SAVE_FRAME();                                                      \
        vm_runtime_error(vm,                                               \
                         "Operator '%s' expected two numbers, got %s and %s.", \
                         #operator, value_type_name(vb), value_type_name(vc)); \
        return INTERPRET_RUNTIME_ERROR;                                     \
    }                                                                      \
    DISPATCH();                                                            \
}

#define BINARY_NUM_OP(op_name, operator)                                   \
op_name: {                                                                 \
    Instruction inst = READ_INST();                                        \
    Value vb = R(GET_B(inst));                                             \
    Value vc = R(GET_C(inst));                                             \
    if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc), 1)) {             \
        R(GET_A(inst)) = NUMBER_VAL(AS_DOUBLE(vb) operator AS_DOUBLE(vc)); \
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {                         \
        R(GET_A(inst)) = NUMBER_VAL(AS_NUMBER(vb) operator AS_NUMBER(vc)); \
    } else {                                                               \
        SAVE_FRAME();                                                      \
        vm_runtime_error(vm,                                               \
                         "Operator '%s' expected two numbers, got %s and %s.", \
                         #operator, value_type_name(vb), value_type_name(vc)); \
        return INTERPRET_RUNTIME_ERROR;                                     \
    }                                                                      \
    DISPATCH();                                                            \
}

INT_BINARY_NUM_OP(LBL_SUB, -)
INT_BINARY_NUM_OP(LBL_MUL, *)
BINARY_NUM_OP(LBL_DIV, /)

#ifndef __GNUC__
    case OP_MOD:
#endif
LBL_MOD: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst)), vc = R(GET_C(inst));
    if (IS_INT(vb) && IS_INT(vc) && AS_INT(vc) != 0) {
        R(GET_A(inst)) = INT_VAL(AS_INT(vb) % AS_INT(vc));
    } else if (IS_NUMBER(vb) && IS_NUMBER(vc)) {
        R(GET_A(inst)) = NUMBER_VAL(number_mod(AS_NUMBER(vb), AS_NUMBER(vc)));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {
        R(GET_A(inst)) = NUMBER_VAL(number_mod(AS_NUMBER(vb), AS_NUMBER(vc)));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm,
                         "Operator '%%' expected two numbers, got %s and %s.",
                         value_type_name(vb), value_type_name(vc));
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_NEG:
#endif
LBL_NEG: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    if (IS_INT(vb)) {
        R(GET_A(inst)) = int_or_number(-(long long)AS_INT(vb));
    } else if (__builtin_expect(IS_NUMBER(vb), 1)) {
        R(GET_A(inst)) = NUMBER_VAL(-AS_DOUBLE(vb));
    } else if (IS_NUMERIC(vb)) {
        R(GET_A(inst)) = NUMBER_VAL(-AS_NUMBER(vb));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Unary '-' expected a number, got %s.",
                         value_type_name(vb));
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

/* Fused arithmetic with INT+INT fast path (for ADDK, SUBK, MULK) */
#define FUSED_NUM_OP(op_name, operator)                                    \
op_name: {                                                                 \
    Instruction inst = READ_INST();                                        \
    Value vb = R(GET_B(inst));                                             \
    Value vc = K(GET_C(inst));                                             \
    if (__builtin_expect(IS_INT(vb) && IS_INT(vc), 1)) {                   \
        R(GET_A(inst)) = int_or_number((long long)AS_INT(vb) operator (long long)AS_INT(vc)); \
    } else if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc), 1)) {     \
        R(GET_A(inst)) = NUMBER_VAL(AS_DOUBLE(vb) operator AS_DOUBLE(vc)); \
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {                         \
        R(GET_A(inst)) = NUMBER_VAL(AS_NUMBER(vb) operator AS_NUMBER(vc)); \
    } else {                                                               \
        SAVE_FRAME();                                                      \
        vm_runtime_error(vm,                                               \
                         "Operator '%s' expected two numbers, got %s and %s.", \
                         #operator, value_type_name(vb), value_type_name(vc)); \
        return INTERPRET_RUNTIME_ERROR;                                     \
    }                                                                      \
    DISPATCH();                                                            \
}

/* Fused arithmetic without INT+INT fast path (for DIVK - division must stay double) */
#define FUSED_NUM_OP_DOUBLE(op_name, operator)                            \
op_name: {                                                                 \
    Instruction inst = READ_INST();                                        \
    Value vb = R(GET_B(inst));                                             \
    Value vc = K(GET_C(inst));                                             \
    if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc), 1)) {             \
        R(GET_A(inst)) = NUMBER_VAL(AS_DOUBLE(vb) operator AS_DOUBLE(vc)); \
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {                         \
        R(GET_A(inst)) = NUMBER_VAL(AS_NUMBER(vb) operator AS_NUMBER(vc)); \
    } else {                                                               \
        SAVE_FRAME();                                                      \
        vm_runtime_error(vm,                                               \
                         "Operator '%s' expected two numbers, got %s and %s.", \
                         #operator, value_type_name(vb), value_type_name(vc)); \
        return INTERPRET_RUNTIME_ERROR;                                     \
    }                                                                      \
    DISPATCH();                                                            \
}

FUSED_NUM_OP(LBL_ADDK, +)
FUSED_NUM_OP(LBL_SUBK, -)
FUSED_NUM_OP(LBL_MULK, *)
FUSED_NUM_OP_DOUBLE(LBL_DIVK, /)
#ifndef __GNUC__
    case OP_MODK:
#endif
LBL_MODK: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    Value vc = K(GET_C(inst));
    if (IS_NUMBER(vb) && IS_NUMBER(vc)) {
        R(GET_A(inst)) = NUMBER_VAL(number_mod(AS_NUMBER(vb), AS_NUMBER(vc)));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {
        R(GET_A(inst)) = NUMBER_VAL(number_mod(AS_NUMBER(vb), AS_NUMBER(vc)));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm,
                         "Operator '%%' expected two numbers, got %s and %s.",
                         value_type_name(vb), value_type_name(vc));
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

/* Immediate arithmetic (register + signed 8-bit integer) */
#define INT_IMMEDIATE_NUM_OP(op_name, operator)                           \
op_name: {                                                                \
    Instruction inst = READ_INST();                                       \
    Value vb = R(GET_B(inst));                                            \
    int imm = GET_sC(inst);                                                \
    if (__builtin_expect(IS_INT(vb), 1)) {                                \
        R(GET_A(inst)) = int_or_number((long long)AS_INT(vb) operator (long long)imm); \
    } else if (__builtin_expect(IS_NUMBER(vb), 1)) {                     \
        R(GET_A(inst)) = NUMBER_VAL(AS_DOUBLE(vb) operator (double)imm); \
    } else if (IS_NUMERIC(vb)) {                                          \
        R(GET_A(inst)) = NUMBER_VAL(AS_NUMBER(vb) operator (double)imm);  \
    } else {                                                              \
        SAVE_FRAME();                                                     \
        vm_runtime_error(vm, "Operator '%s' expected a number, got %s.",   \
                         #operator, value_type_name(vb));                 \
        return INTERPRET_RUNTIME_ERROR;                                    \
    }                                                                     \
    DISPATCH();                                                           \
}

#define IMMEDIATE_NUM_OP(op_name, operator)                               \
op_name: {                                                                \
    Instruction inst = READ_INST();                                       \
    Value vb = R(GET_B(inst));                                            \
    if (__builtin_expect(IS_NUMBER(vb), 1)) {                             \
        R(GET_A(inst)) = NUMBER_VAL(AS_DOUBLE(vb) operator (double)GET_sC(inst)); \
    } else if (IS_NUMERIC(vb)) {                                          \
        R(GET_A(inst)) = NUMBER_VAL(AS_NUMBER(vb) operator (double)GET_sC(inst)); \
    } else {                                                              \
        SAVE_FRAME();                                                     \
        vm_runtime_error(vm, "Operator '%s' expected a number, got %s.",   \
                         #operator, value_type_name(vb));                 \
        return INTERPRET_RUNTIME_ERROR;                                    \
    }                                                                     \
    DISPATCH();                                                           \
}

INT_IMMEDIATE_NUM_OP(LBL_ADDI, +)
INT_IMMEDIATE_NUM_OP(LBL_SUBI, -)
INT_IMMEDIATE_NUM_OP(LBL_MULI, *)
IMMEDIATE_NUM_OP(LBL_DIVI, /)
#ifndef __GNUC__
    case OP_MODI:
#endif
LBL_MODI: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    int imm = GET_sC(inst);
    if (__builtin_expect(IS_INT(vb) && imm != 0, 1)) {
        R(GET_A(inst)) = INT_VAL(AS_INT(vb) % imm);
    } else if (__builtin_expect(IS_NUMBER(vb), 1)) {
        R(GET_A(inst)) = NUMBER_VAL(number_mod_i(AS_DOUBLE(vb), GET_sC(inst)));
    } else if (IS_NUMERIC(vb)) {
        R(GET_A(inst)) = NUMBER_VAL(number_mod_i(AS_DOUBLE(vb), GET_sC(inst)));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operator '%%' expected a number, got %s.",
                         value_type_name(vb));
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

/* Comparison */
#ifndef __GNUC__
    case OP_EQ:
#endif
LBL_EQ: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    Value vc = R(GET_C(inst));
    bool equal = vb == vc ||
        (((IS_NUMERIC(vb) && IS_NUMERIC(vc)) || (IS_STRING(vb) && IS_STRING(vc))) &&
         values_equal(vb, vc));
    R(GET_A(inst)) = BOOL_VAL(equal);
    DISPATCH();
}

#ifndef __GNUC__
    case OP_NEQ:
#endif
LBL_NEQ: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    Value vc = R(GET_C(inst));
    bool equal = vb == vc ||
        (((IS_NUMERIC(vb) && IS_NUMERIC(vc)) || (IS_STRING(vb) && IS_STRING(vc))) &&
         values_equal(vb, vc));
    R(GET_A(inst)) = BOOL_VAL(!equal);
    DISPATCH();
}

#ifndef __GNUC__
    case OP_LT:
#endif
LBL_LT: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    Value vc = R(GET_C(inst));
    if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc), 1)) {
        R(GET_A(inst)) = BOOL_VAL(AS_DOUBLE(vb) < AS_DOUBLE(vc));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {
        R(GET_A(inst)) = BOOL_VAL(AS_NUMBER(vb) < AS_NUMBER(vc));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm,
                         "Operator '<' expected two numbers, got %s and %s.",
                         value_type_name(vb), value_type_name(vc));
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_LE:
#endif
LBL_LE: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    Value vc = R(GET_C(inst));
    if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc), 1)) {
        R(GET_A(inst)) = BOOL_VAL(AS_DOUBLE(vb) <= AS_DOUBLE(vc));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {
        R(GET_A(inst)) = BOOL_VAL(AS_NUMBER(vb) <= AS_NUMBER(vc));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm,
                         "Operator '<=' expected two numbers, got %s and %s.",
                         value_type_name(vb), value_type_name(vc));
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_EQI:
#endif
LBL_EQI: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    R(GET_A(inst)) = BOOL_VAL(IS_INT(vb) ? AS_INT(vb) == GET_sC(inst) :
        (IS_NUMBER(vb) && AS_DOUBLE(vb) == (double)GET_sC(inst)));
    DISPATCH();
}

#ifndef __GNUC__
    case OP_NEQI:
#endif
LBL_NEQI: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    R(GET_A(inst)) = BOOL_VAL(IS_INT(vb) ? AS_INT(vb) != GET_sC(inst) :
        (!IS_NUMBER(vb) || AS_DOUBLE(vb) != (double)GET_sC(inst)));
    DISPATCH();
}

#ifndef __GNUC__
    case OP_LTI:
#endif
LBL_LTI: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    if (__builtin_expect(IS_INT(vb), 1)) {
        R(GET_A(inst)) = BOOL_VAL(AS_INT(vb) < GET_sC(inst));
    } else if (__builtin_expect(IS_NUMBER(vb), 1)) {
        R(GET_A(inst)) = BOOL_VAL(AS_DOUBLE(vb) < (double)GET_sC(inst));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operator '<' expected a number, got %s.",
                         value_type_name(vb));
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_LEI:
#endif
LBL_LEI: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    if (__builtin_expect(IS_INT(vb), 1)) {
        R(GET_A(inst)) = BOOL_VAL(AS_INT(vb) <= GET_sC(inst));
    } else if (__builtin_expect(IS_NUMBER(vb), 1)) {
        R(GET_A(inst)) = BOOL_VAL(AS_DOUBLE(vb) <= (double)GET_sC(inst));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operator '<=' expected a number, got %s.",
                         value_type_name(vb));
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_GTI:
#endif
LBL_GTI: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    if (__builtin_expect(IS_INT(vb), 1)) {
        R(GET_A(inst)) = BOOL_VAL(AS_INT(vb) > GET_sC(inst));
    } else if (__builtin_expect(IS_NUMBER(vb), 1)) {
        R(GET_A(inst)) = BOOL_VAL(AS_DOUBLE(vb) > (double)GET_sC(inst));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operator '>' expected a number, got %s.",
                         value_type_name(vb));
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_GEI:
#endif
LBL_GEI: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    if (__builtin_expect(IS_INT(vb), 1)) {
        R(GET_A(inst)) = BOOL_VAL(AS_INT(vb) >= GET_sC(inst));
    } else if (__builtin_expect(IS_NUMBER(vb), 1)) {
        R(GET_A(inst)) = BOOL_VAL(AS_DOUBLE(vb) >= (double)GET_sC(inst));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operator '>=' expected a number, got %s.",
                         value_type_name(vb));
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#define INLINE_TEST_RESULT(result) do {                                    \
    if (result) {                                                          \
        ip++;                                                              \
    } else {                                                              \
        Instruction jmp_inst = *ip++;                                      \
        ip += GET_sBx(jmp_inst);                                           \
    }                                                                     \
    DISPATCH();                                                           \
} while (0)

#ifndef __GNUC__
    case OP_EQI_TEST:
#endif
LBL_EQI_TEST: {
    Instruction inst = READ_INST();
    Value value = R(GET_A(inst));
    INLINE_TEST_RESULT(IS_INT(value) ? AS_INT(value) == GET_sB(inst) :
        (IS_NUMBER(value) && AS_DOUBLE(value) == (double)GET_sB(inst)));
}

#ifndef __GNUC__
    case OP_NEQI_TEST:
#endif
LBL_NEQI_TEST: {
    Instruction inst = READ_INST();
    Value value = R(GET_A(inst));
    INLINE_TEST_RESULT(IS_INT(value) ? AS_INT(value) != GET_sB(inst) :
        (!IS_NUMBER(value) || AS_DOUBLE(value) != (double)GET_sB(inst)));
}

#ifndef __GNUC__
    case OP_LTI_TEST:
#endif
LBL_LTI_TEST: {
    Instruction inst = READ_INST();
    Value value = R(GET_A(inst));
    if (__builtin_expect(IS_INT(value), 1)) {
        INLINE_TEST_RESULT(AS_INT(value) < GET_sB(inst));
    }
    if (__builtin_expect(IS_NUMBER(value), 1)) {
        INLINE_TEST_RESULT(AS_DOUBLE(value) < (double)GET_sB(inst));
    }
    SAVE_FRAME();
    vm_runtime_error(vm, "Operator '<' expected a number, got %s.",
                     value_type_name(value));
    return INTERPRET_RUNTIME_ERROR;
}

#ifndef __GNUC__
    case OP_LEI_TEST:
#endif
LBL_LEI_TEST: {
    Instruction inst = READ_INST();
    Value value = R(GET_A(inst));
    if (__builtin_expect(IS_INT(value), 1)) {
        INLINE_TEST_RESULT(AS_INT(value) <= GET_sB(inst));
    }
    if (__builtin_expect(IS_NUMBER(value), 1)) {
        INLINE_TEST_RESULT(AS_DOUBLE(value) <= (double)GET_sB(inst));
    }
    SAVE_FRAME();
    vm_runtime_error(vm, "Operator '<=' expected a number, got %s.",
                     value_type_name(value));
    return INTERPRET_RUNTIME_ERROR;
}

#ifndef __GNUC__
    case OP_GTI_TEST:
#endif
LBL_GTI_TEST: {
    Instruction inst = READ_INST();
    Value value = R(GET_A(inst));
    if (__builtin_expect(IS_INT(value), 1)) {
        INLINE_TEST_RESULT(AS_INT(value) > GET_sB(inst));
    }
    if (__builtin_expect(IS_NUMBER(value), 1)) {
        INLINE_TEST_RESULT(AS_DOUBLE(value) > (double)GET_sB(inst));
    }
    SAVE_FRAME();
    vm_runtime_error(vm, "Operator '>' expected a number, got %s.",
                     value_type_name(value));
    return INTERPRET_RUNTIME_ERROR;
}

#ifndef __GNUC__
    case OP_GEI_TEST:
#endif
LBL_GEI_TEST: {
    Instruction inst = READ_INST();
    Value value = R(GET_A(inst));
    if (__builtin_expect(IS_INT(value), 1)) {
        INLINE_TEST_RESULT(AS_INT(value) >= GET_sB(inst));
    }
    if (__builtin_expect(IS_NUMBER(value), 1)) {
        INLINE_TEST_RESULT(AS_DOUBLE(value) >= (double)GET_sB(inst));
    }
    SAVE_FRAME();
    vm_runtime_error(vm, "Operator '>=' expected a number, got %s.",
                     value_type_name(value));
    return INTERPRET_RUNTIME_ERROR;
}

#ifndef __GNUC__
    case OP_MODI_EQI_TEST:
#endif
LBL_MODI_EQI_TEST: {
    Instruction inst = READ_INST();
    Value value = R(GET_A(inst));
    int divisor = GET_sB(inst);
    if (__builtin_expect(IS_INT(value) && divisor != 0, 1)) {
        INLINE_TEST_RESULT((AS_INT(value) % divisor) == GET_sC(inst));
    }
    if (__builtin_expect(IS_NUMBER(value), 1)) {
        double mod = number_mod_i(AS_DOUBLE(value), GET_sB(inst));
        INLINE_TEST_RESULT(mod == (double)GET_sC(inst));
    }
    SAVE_FRAME();
    vm_runtime_error(vm, "Operands must be numbers for '%%'.");
    return INTERPRET_RUNTIME_ERROR;
}

#ifndef __GNUC__
    case OP_MODI_NEQI_TEST:
#endif
LBL_MODI_NEQI_TEST: {
    Instruction inst = READ_INST();
    Value value = R(GET_A(inst));
    int divisor = GET_sB(inst);
    if (__builtin_expect(IS_INT(value) && divisor != 0, 1)) {
        INLINE_TEST_RESULT((AS_INT(value) % divisor) != GET_sC(inst));
    }
    if (__builtin_expect(IS_NUMBER(value), 1)) {
        double mod = number_mod_i(AS_DOUBLE(value), GET_sB(inst));
        INLINE_TEST_RESULT(mod != (double)GET_sC(inst));
    }
    SAVE_FRAME();
    vm_runtime_error(vm, "Operands must be numbers for '%%'.");
    return INTERPRET_RUNTIME_ERROR;
}

/* --- Logical --- */
#ifndef __GNUC__
    case OP_NOT:
#endif
LBL_NOT: {
    Instruction inst = READ_INST();
    R(GET_A(inst)) = BOOL_VAL(IS_FALSEY(R(GET_B(inst))));
    DISPATCH();
}

#ifndef __GNUC__
    case OP_TEST:
#endif
LBL_TEST: {
    Instruction inst = READ_INST();
    bool truthy = !IS_FALSEY(R(GET_A(inst)));
    if (truthy != (GET_C(inst) != 0)) {
        ip++; /* skip the jump instruction */
    } else {
        /* Inline the jump execution to avoid a dispatch cycle */
        Instruction jmp_inst = *ip++;
        ip += GET_sBx(jmp_inst);
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_TESTSET:
#endif
LBL_TESTSET: {
    Instruction inst = READ_INST();
    Value vb = R(GET_B(inst));
    bool truthy = !IS_FALSEY(vb);
    if (truthy == (GET_C(inst) != 0)) {
        R(GET_A(inst)) = vb;
        /* Inline the jump execution */
        Instruction jmp_inst = *ip++;
        ip += GET_sBx(jmp_inst);
    } else {
        ip++; /* skip the jump instruction */
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_TESTJMP:
#endif
LBL_TESTJMP: {
    Instruction inst = READ_INST();
    if (IS_FALSEY(R(GET_AsBx_A(inst)))) {
        ip += GET_AsBx_sBx(inst);
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_TESTERRJMP:
#endif
LBL_TESTERRJMP: {
    Instruction inst = READ_INST();
    if (!IS_ERROR(R(GET_AsBx_A(inst)))) {
        ip += GET_AsBx_sBx(inst);
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_CONCAT:
#endif
LBL_CONCAT: {
    Instruction inst = READ_INST();
    /* Concatenate R[B] through R[C] - single-pass O(n) */
    int b = GET_B(inst);
    int c = GET_C(inst);
    SAVE_FRAME();
    /* First pass: validate and compute total length */
    int total_len = 0;
    for (int i = b; i <= c; i++) {
        if (!IS_STRING(R(i))) {
            vm_runtime_error(vm, "Can only concatenate strings.");
            return INTERPRET_RUNTIME_ERROR;
        }
        total_len += AS_STRING(R(i))->length;
    }
    /* Second pass: allocate once and copy all segments */
    char *buf = (char *)malloc(total_len + 1);
    int pos = 0;
    for (int i = b; i <= c; i++) {
        ObjString *s = AS_STRING(R(i));
        memcpy(buf + pos, string_chars(vm, s), s->length);
        pos += s->length;
    }
    buf[total_len] = '\0';
    R(GET_A(inst)) = OBJ_VAL(take_string(vm, buf, total_len));
    LOAD_FRAME();
    DISPATCH();
}

#ifndef __GNUC__
    case OP_TOSTRING:
#endif
LBL_TOSTRING: {
    Instruction inst = READ_INST();
    Value val = R(GET_B(inst));
    if (IS_STRING(val)) {
        R(GET_A(inst)) = val;
        DISPATCH();
    }
    if (IS_INT(val)) {
        int32_t num = AS_INT(val);
        if ((uint32_t)num < INT_STR_CACHE_SIZE && vm->int_str_cache[num] != NULL) {
            R(GET_A(inst)) = OBJ_VAL(vm->int_str_cache[num]);
            DISPATCH();
        }
    }
    SAVE_FRAME();
    R(GET_A(inst)) = value_to_string(vm, val);
    LOAD_FRAME();
    DISPATCH();
}

#ifndef __GNUC__
    case OP_LEN:
#endif
LBL_LEN: {
    Instruction inst = READ_INST();
    Value value = R(GET_B(inst));
    if (IS_STRING(value)) {
        R(GET_A(inst)) = INT_VAL(AS_STRING(value)->length);
    } else if (IS_ARRAY(value)) {
        R(GET_A(inst)) = INT_VAL(AS_ARRAY(value)->count);
    } else if (IS_DICT(value)) {
        R(GET_A(inst)) = INT_VAL(AS_DICT(value)->count);
    } else {
        R(GET_A(inst)) = NULL_VAL;
    }
    DISPATCH();
}

/* --- Jumps --- */
#ifndef __GNUC__
    case OP_JMP:
#endif
LBL_JMP: {
    Instruction inst = READ_INST();
    int offset = GET_sBx(inst);
    ip += offset;
    DISPATCH();
}

#ifndef __GNUC__
    case OP_LOOP:
#endif
LBL_LOOP: {
    CHECK_LOOP_CANCEL();
    Instruction inst = READ_INST();
    int offset = GET_sBx(inst);
    ip -= offset;
    DISPATCH();
}

LBL_ADDI_LOOP: {
    CHECK_LOOP_CANCEL();
    Instruction inst = READ_INST();
    int target = GET_A(inst);
    int imm = GET_sB(inst);
    int offset = GET_C(inst);
    Value vt = R(target);
    if (__builtin_expect(IS_INT(vt), 1)) {
        R(target) = int_or_number((long long)AS_INT(vt) + (long long)imm);
        ip -= offset;
        DISPATCH();
    }
    if (__builtin_expect(IS_NUMBER(vt), 1)) {
        R(target) = NUMBER_VAL(AS_DOUBLE(vt) + (double)imm);
        ip -= offset;
        DISPATCH();
    }
    SAVE_FRAME();
    vm_runtime_error(vm, "Operator '+' expected a number, got %s.",
                     value_type_name(vt));
    return INTERPRET_RUNTIME_ERROR;
}

#ifndef __GNUC__
    case OP_FORPREP:
#endif
LBL_FORPREP: {
    Instruction inst = READ_INST();
    int a = GET_A(inst);
    Value iter = R(a);
    Value limit = R(a + 1);
    if (__builtin_expect(IS_INT(iter) && IS_INT(limit), 1)) {
        if (__builtin_expect(AS_INT(limit) == INT32_MAX, 0)) {
            R(a) = NUMBER_VAL((double)AS_INT(iter) - 1.0);
            R(a + 1) = NUMBER_VAL((double)AS_INT(limit));
            DISPATCH();
        }
        R(a) = int_or_number((long long)AS_INT(iter) - 1);
    } else if (__builtin_expect(IS_NUMBER(iter) && IS_NUMBER(limit), 1) ||
               (IS_NUMERIC(iter) && IS_NUMERIC(limit))) {
        double iter_num = AS_NUMBER(iter);
        double limit_num = AS_NUMBER(limit);
        if (iter_num >= (double)INT32_MIN + 1.0 && iter_num <= (double)INT32_MAX &&
            limit_num >= (double)INT32_MIN && limit_num <= (double)INT32_MAX) {
            int32_t iter_int = (int32_t)iter_num;
            int32_t limit_int = (int32_t)limit_num;
            if (limit_int != INT32_MAX &&
                (double)iter_int == iter_num && (double)limit_int == limit_num) {
                R(a) = INT_VAL(iter_int - 1);
                R(a + 1) = INT_VAL(limit_int);
                DISPATCH();
            }
        }
        R(a) = NUMBER_VAL(iter_num - 1.0);
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Range bounds must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_FORPREP_NUM:
#endif
LBL_FORPREP_NUM: {
    Instruction inst = READ_INST();
    int a = GET_A(inst);
    Value iter = R(a);
    Value limit = R(a + 1);
    if (__builtin_expect(IS_NUMERIC(iter) && IS_NUMERIC(limit), 1)) {
        R(a) = NUMBER_VAL(AS_NUMBER(iter) - 1.0);
        R(a + 1) = NUMBER_VAL(AS_NUMBER(limit));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Range bounds must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_FORLOOP:
#endif
LBL_FORLOOP: {
    CHECK_LOOP_CANCEL();
    Instruction inst = READ_INST();
    int a = GET_AsBx_A(inst);
    int offset = GET_AsBx_sBx(inst);
    Value iter_val = R(a);
    Value limit_val = R(a + 1);
    if (__builtin_expect(IS_INT(iter_val) && IS_INT(limit_val), 1)) {
        int32_t next = AS_INT(iter_val) + 1;
        R(a) = INT_VAL(next);
        if (next >= AS_INT(limit_val)) {
            ip += offset;
        }
        DISPATCH();
    }
    double val = AS_DOUBLE(iter_val) + 1.0;
    R(a) = NUMBER_VAL(val);
    if (val >= AS_DOUBLE(limit_val)) {
        ip += offset;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_FORLOOP_INC:
#endif
LBL_FORLOOP_INC: {
    CHECK_LOOP_CANCEL();
    Instruction inst = READ_INST();
    int a = GET_AsBx_A(inst);
    int offset = GET_AsBx_sBx(inst);
    Value iter_val = R(a);
    Value limit_val = R(a + 1);
    if (__builtin_expect(IS_INT(iter_val) && IS_INT(limit_val), 1)) {
        int32_t next = AS_INT(iter_val) + 1;
        R(a) = INT_VAL(next);
        if (next > AS_INT(limit_val)) {
            ip += offset;
        }
        DISPATCH();
    }
    double val = AS_DOUBLE(iter_val) + 1.0;
    R(a) = NUMBER_VAL(val);
    if (val > AS_DOUBLE(limit_val)) {
        ip += offset;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_FORLOOP_NUM:
#endif
LBL_FORLOOP_NUM: {
    CHECK_LOOP_CANCEL();
    Instruction inst = READ_INST();
    int a = GET_AsBx_A(inst);
    int offset = GET_AsBx_sBx(inst);
    Value iter_val = R(a);
    Value limit_val = R(a + 1);
    double val = AS_DOUBLE(iter_val) + 1.0;
    R(a) = NUMBER_VAL(val);
    if (val >= AS_DOUBLE(limit_val)) {
        ip += offset;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_FORLOOP_INC_NUM:
#endif
LBL_FORLOOP_INC_NUM: {
    CHECK_LOOP_CANCEL();
    Instruction inst = READ_INST();
    int a = GET_AsBx_A(inst);
    int offset = GET_AsBx_sBx(inst);
    Value iter_val = R(a);
    Value limit_val = R(a + 1);
    double val = AS_DOUBLE(iter_val) + 1.0;
    R(a) = NUMBER_VAL(val);
    if (val > AS_DOUBLE(limit_val)) {
        ip += offset;
    }
    DISPATCH();
}

#define FORADD_FIELD_PROP_RUN(label, inclusive, error_label, global_target) do {    \
label: {                                                                            \
    Instruction inst = READ_INST();                                                 \
    Instruction aux = READ_INST();                                                  \
    int iter_reg = GET_A(inst);                                                     \
    int target = GET_B(inst);                                                       \
    int obj_reg = GET_C(inst);                                                      \
    ObjString *target_name = NULL;                                                  \
    if (global_target) {                                                            \
        target_name = AS_STRING(K(GET_Bx(aux)));                                    \
        aux = READ_INST();                                                          \
    }                                                                               \
    ObjString *name = AS_STRING(K(GET_Bx(aux)));                                    \
    Value start_v = R(iter_reg);                                                    \
    Value limit_v = R(iter_reg + 1);                                                 \
    long long count = 0;                                                            \
    bool int_bounds = false;                                                        \
    long long final_int = 0;                                                        \
    double final_num = 0.0;                                                         \
    if (__builtin_expect(IS_INT(start_v) && IS_INT(limit_v), 1)) {                  \
        long long start = (long long)AS_INT(start_v);                               \
        long long limit = (long long)AS_INT(limit_v);                               \
        if (inclusive) {                                                            \
            count = start <= limit ? (limit - start + 1) : 0;                       \
        } else {                                                                    \
            count = start < limit ? (limit - start) : 0;                            \
        }                                                                           \
        final_int = start + count;                                                  \
        int_bounds = true;                                                          \
    } else if (IS_NUMERIC(start_v) && IS_NUMERIC(limit_v)) {                        \
        double start = AS_NUMBER(start_v);                                          \
        double limit = AS_NUMBER(limit_v);                                          \
        if (inclusive) {                                                            \
            count = start <= limit ? (long long)floor(limit - start) + 1 : 0;       \
        } else {                                                                    \
            count = start < limit ? (long long)ceil(limit - start) : 0;             \
        }                                                                           \
        if (count < 0) count = 0;                                                   \
        final_num = start + (double)count;                                          \
    } else {                                                                        \
        SAVE_FRAME();                                                               \
        vm_runtime_error(vm, "Range bounds must be numbers.");                      \
        return INTERPRET_RUNTIME_ERROR;                                             \
    }                                                                               \
    if (count <= 0) {                                                               \
        R(iter_reg) = int_bounds ? int_or_number(final_int) : NUMBER_VAL(final_num);\
        DISPATCH();                                                                 \
    }                                                                               \
    if (global_target) {                                                            \
        uint32_t global_ic_slot = ((uint32_t)(uintptr_t)target_name) >> 4 &          \
                                  (GLOBAL_IC_SIZE - 1);                             \
        GlobalICEntry *global_ic = &vm->global_ic[global_ic_slot];                  \
        Value acc_value;                                                            \
        if (__builtin_expect(global_ic->name == target_name, 1)) {                  \
            acc_value = global_ic->value;                                           \
        } else {                                                                    \
            if (!table_get(&vm->globals, target_name, &acc_value)) {                \
                SAVE_FRAME();                                                       \
                vm_runtime_error(vm, "Undefined variable '%s'.", target_name->chars);\
                return INTERPRET_RUNTIME_ERROR;                                     \
            }                                                                       \
            global_ic->name = target_name;                                          \
            global_ic->value = acc_value;                                           \
        }                                                                           \
        R(target) = acc_value;                                                      \
    }                                                                               \
    Value obj = R(obj_reg);                                                         \
    Value rhs = NULL_VAL;                                                           \
    const char *kind = "LookupError";                                               \
    const char *message = "Field not found";                                        \
    const char *hint = NULL;                                                        \
    char message_buf[256];                                                          \
    if (__builtin_expect(IS_DICT(obj), 1)) {                                        \
        if (!dict_get_mono_cached(AS_DICT(obj), name, &rhs)) {                      \
            kind = "KeyError";                                                     \
            snprintf(message_buf, sizeof(message_buf), "Key not found: %.*s",       \
                     name->length, name->chars);                                    \
            message = message_buf;                                                  \
            hint = "Check dict.has(dict, key) or provide a default value.";         \
            goto error_label;                                                       \
        }                                                                           \
    } else if (IS_INSTANCE(obj)) {                                                   \
        ObjInstance *inst_obj = AS_INSTANCE(obj);                                   \
        ObjStruct *klass = inst_obj->klass;                                         \
        Value idx_val;                                                              \
        if (table_get(&klass->field_index, name, &idx_val)) {                       \
            rhs = inst_obj->fields[(int)AS_NUMBER(idx_val)];                        \
        } else {                                                                    \
            kind = "FieldError";                                                   \
            snprintf(message_buf, sizeof(message_buf), "Field not found: %.*s",     \
                     name->length, name->chars);                                    \
            message = message_buf;                                                  \
            hint = "Check the struct field name.";                                  \
            goto error_label;                                                       \
        }                                                                           \
    } else {                                                                        \
        kind = "TypeError";                                                        \
        message = "Value has no fields";                                            \
        hint = "Use field access on structs, dicts, or error values.";              \
        goto error_label;                                                           \
    }                                                                               \
    if (__builtin_expect(IS_NUMERIC(R(target)) && IS_NUMERIC(rhs), 1)) {             \
        double acc = AS_NUMBER(R(target));                                          \
        double add = AS_NUMBER(rhs);                                                \
        for (long long n = 0; n < count; n++) {                                     \
            if (__builtin_expect((MG_ATOMIC_LOAD_BOOL(vm->cancel_requested) || vm->deadline_ms != 0) && \
                                 ((n & 0xfff) == 0), 0)) {                          \
                CHECK_CANCEL();                                                     \
            }                                                                       \
            acc += add;                                                             \
        }                                                                           \
        R(target) = NUMBER_VAL(acc);                                                \
        R(iter_reg) = int_bounds ? int_or_number(final_int) : NUMBER_VAL(final_num);\
        if (global_target) {                                                        \
            table_set(&vm->globals, target_name, R(target));                        \
            uint32_t global_ic_slot = ((uint32_t)(uintptr_t)target_name) >> 4 &      \
                                      (GLOBAL_IC_SIZE - 1);                         \
            vm->global_ic[global_ic_slot].name = target_name;                       \
            vm->global_ic[global_ic_slot].value = R(target);                        \
        }                                                                           \
        DISPATCH();                                                                 \
    }                                                                               \
    if (IS_OBJ(R(target)) && IS_OBJ(rhs) &&                                         \
        AS_OBJ(R(target))->type == OBJ_STRING && AS_OBJ(rhs)->type == OBJ_STRING) { \
        for (long long n = 0; n < count; n++) {                                     \
            if (__builtin_expect((MG_ATOMIC_LOAD_BOOL(vm->cancel_requested) || vm->deadline_ms != 0) && \
                                 ((n & 0xfff) == 0), 0)) {                          \
                CHECK_CANCEL();                                                     \
            }                                                                       \
            SAVE_FRAME();                                                           \
            R(target) = OBJ_VAL(concat_strings(vm, AS_STRING(R(target)), AS_STRING(rhs))); \
            LOAD_FRAME();                                                           \
        }                                                                           \
        R(iter_reg) = int_bounds ? int_or_number(final_int) : NUMBER_VAL(final_num);\
        if (global_target) {                                                        \
            table_set(&vm->globals, target_name, R(target));                        \
            uint32_t global_ic_slot = ((uint32_t)(uintptr_t)target_name) >> 4 &      \
                                      (GLOBAL_IC_SIZE - 1);                         \
            vm->global_ic[global_ic_slot].name = target_name;                       \
            vm->global_ic[global_ic_slot].value = R(target);                        \
        }                                                                           \
        DISPATCH();                                                                 \
    }                                                                               \
    SAVE_FRAME();                                                                   \
    vm_runtime_error(vm, "Operands must be numbers.");                              \
    return INTERPRET_RUNTIME_ERROR;                                                 \
error_label:                                                                        \
    SAVE_FRAME();                                                                   \
    Value err = make_error_value(vm, kind, message, hint);                          \
    LOAD_FRAME();                                                                   \
    R(target) = NULL_VAL;                                                           \
    R(target + 1) = err;                                                            \
    RETURN_VALUES(target, 2);                                                       \
}                                                                                   \
} while (0)

#ifndef __GNUC__
    case OP_FORADDLOCAL_FIELD_PROP:
#endif
FORADD_FIELD_PROP_RUN(LBL_FORADDLOCAL_FIELD_PROP, false,
                      LBL_FORADDLOCAL_FIELD_PROP_ERROR, false);

#ifndef __GNUC__
    case OP_FORADDLOCAL_FIELD_PROP_INC:
#endif
FORADD_FIELD_PROP_RUN(LBL_FORADDLOCAL_FIELD_PROP_INC, true,
                      LBL_FORADDLOCAL_FIELD_PROP_INC_ERROR, false);

#ifndef __GNUC__
    case OP_FORADDGLOBAL_FIELD_PROP:
#endif
FORADD_FIELD_PROP_RUN(LBL_FORADDGLOBAL_FIELD_PROP, false,
                      LBL_FORADDGLOBAL_FIELD_PROP_ERROR, true);

#ifndef __GNUC__
    case OP_FORADDGLOBAL_FIELD_PROP_INC:
#endif
FORADD_FIELD_PROP_RUN(LBL_FORADDGLOBAL_FIELD_PROP_INC, true,
                      LBL_FORADDGLOBAL_FIELD_PROP_INC_ERROR, true);

#undef FORADD_FIELD_PROP_RUN

#define FOR_MODI_ACCUM_RUN(label, inclusive) do {                                  \
label: {                                                                           \
    Instruction inst = READ_INST();                                                \
    Instruction aux0 = READ_INST();                                                \
    Instruction aux1 = READ_INST();                                                \
    int iter_reg = GET_A(inst);                                                    \
    int target = GET_B(inst);                                                      \
    int divisor = GET_sC(inst);                                                    \
    int residue0 = (int8_t)GET_A(aux0);                                            \
    int delta0 = GET_sB(aux0);                                                     \
    int residue1 = GET_sC(aux0);                                                   \
    int delta1 = (int8_t)GET_A(aux1);                                              \
    int delta_else = GET_sB(aux1);                                                 \
    if (__builtin_expect(divisor == 0, 0)) {                                       \
        SAVE_FRAME();                                                              \
        vm_runtime_error(vm, "Operands must be numbers for '%%'.");                \
        return INTERPRET_RUNTIME_ERROR;                                            \
    }                                                                              \
    Value start_v = R(iter_reg);                                                   \
    Value limit_v = R(iter_reg + 1);                                               \
    if (__builtin_expect(IS_INT(start_v) && IS_INT(limit_v), 1)) {                 \
        long long iter = (long long)AS_INT(start_v);                               \
        long long limit = (long long)AS_INT(limit_v);                              \
        if (!(inclusive ? (iter <= limit) : (iter < limit))) {                     \
            R(iter_reg) = int_or_number(iter);                                     \
            DISPATCH();                                                            \
        }                                                                          \
        Value acc_v = R(target);                                                   \
        if (__builtin_expect(IS_INT(acc_v), 1)) {                                  \
            long long acc = (long long)AS_INT(acc_v);                              \
            while (inclusive ? (iter <= limit) : (iter < limit)) {                 \
                CHECK_LOOP_CANCEL();                                               \
                int residue = (int)(iter % (long long)divisor);                    \
                acc += residue == residue0 ? delta0                                \
                     : residue == residue1 ? delta1                                \
                     : delta_else;                                                 \
                iter++;                                                            \
            }                                                                      \
            R(iter_reg) = int_or_number(iter);                                     \
            R(target) = int_or_number(acc);                                        \
            DISPATCH();                                                            \
        }                                                                          \
        if (__builtin_expect(IS_NUMBER(acc_v), 1)) {                               \
            double acc = AS_DOUBLE(acc_v);                                         \
            while (inclusive ? (iter <= limit) : (iter < limit)) {                 \
                CHECK_LOOP_CANCEL();                                               \
                int residue = (int)(iter % (long long)divisor);                    \
                acc += residue == residue0 ? (double)delta0                        \
                     : residue == residue1 ? (double)delta1                        \
                     : (double)delta_else;                                         \
                iter++;                                                            \
            }                                                                      \
            R(iter_reg) = int_or_number(iter);                                     \
            R(target) = NUMBER_VAL(acc);                                           \
            DISPATCH();                                                            \
        }                                                                          \
    } else if (__builtin_expect(IS_NUMERIC(start_v) && IS_NUMERIC(limit_v), 1)) {  \
        double iter = AS_NUMBER(start_v);                                          \
        double limit = AS_NUMBER(limit_v);                                         \
        if (!(inclusive ? (iter <= limit) : (iter < limit))) {                     \
            R(iter_reg) = NUMBER_VAL(iter);                                        \
            DISPATCH();                                                            \
        }                                                                          \
        Value acc_v = R(target);                                                   \
        if (__builtin_expect(IS_NUMERIC(acc_v), 1)) {                              \
            double acc = AS_NUMBER(acc_v);                                         \
            while (inclusive ? (iter <= limit) : (iter < limit)) {                 \
                CHECK_LOOP_CANCEL();                                               \
                double residue = number_mod_i(iter, divisor);                      \
                acc += residue == (double)residue0 ? (double)delta0                \
                     : residue == (double)residue1 ? (double)delta1                \
                     : (double)delta_else;                                         \
                iter += 1.0;                                                       \
            }                                                                      \
            R(iter_reg) = NUMBER_VAL(iter);                                        \
            R(target) = NUMBER_VAL(acc);                                           \
            DISPATCH();                                                            \
        }                                                                          \
    } else {                                                                       \
        SAVE_FRAME();                                                              \
        vm_runtime_error(vm, "Range bounds must be numbers.");                     \
        return INTERPRET_RUNTIME_ERROR;                                            \
    }                                                                              \
    SAVE_FRAME();                                                                  \
    vm_runtime_error(vm, "Operands must be numbers.");                             \
    return INTERPRET_RUNTIME_ERROR;                                                \
}                                                                                  \
} while (0)

#ifndef __GNUC__
    case OP_FOR_MODI_ACCUM:
#endif
FOR_MODI_ACCUM_RUN(LBL_FOR_MODI_ACCUM, false);

#ifndef __GNUC__
    case OP_FOR_MODI_ACCUM_INC:
#endif
FOR_MODI_ACCUM_RUN(LBL_FOR_MODI_ACCUM_INC, true);

#undef FOR_MODI_ACCUM_RUN

#define FOR_FIELD2_ACCUM_RUN(label, inclusive) do {                                \
label: {                                                                           \
    Instruction inst = READ_INST();                                                \
    Instruction aux0 = READ_INST();                                                \
    Instruction aux1 = READ_INST();                                                \
    int iter_reg = GET_A(inst);                                                    \
    int receiver_reg = GET_B(inst);                                                \
    ObjString *struct_name = AS_STRING(K(GET_C(inst)));                            \
    ObjString *field0 = AS_STRING(K(GET_A(aux0)));                                 \
    int delta0 = GET_sB(aux0);                                                     \
    ObjString *field1 = AS_STRING(K(GET_C(aux0)));                                 \
    int delta1 = (int8_t)GET_A(aux1);                                              \
    Value start_v = R(iter_reg);                                                   \
    Value limit_v = R(iter_reg + 1);                                               \
    long long count = 0;                                                           \
    long long final_int = 0;                                                       \
    double final_num = 0.0;                                                        \
    bool int_bounds = false;                                                       \
    if (__builtin_expect(IS_INT(start_v) && IS_INT(limit_v), 1)) {                 \
        long long start = (long long)AS_INT(start_v);                              \
        long long limit = (long long)AS_INT(limit_v);                              \
        if (inclusive) {                                                           \
            count = start <= limit ? (limit - start + 1) : 0;                      \
        } else {                                                                   \
            count = start < limit ? (limit - start) : 0;                           \
        }                                                                          \
        final_int = start + count;                                                 \
        int_bounds = true;                                                         \
    } else if (__builtin_expect(IS_NUMERIC(start_v) && IS_NUMERIC(limit_v), 1)) {  \
        double start = AS_NUMBER(start_v);                                         \
        double limit = AS_NUMBER(limit_v);                                         \
        if (inclusive) {                                                           \
            count = start <= limit ? (long long)floor(limit - start) + 1 : 0;      \
        } else {                                                                   \
            count = start < limit ? (long long)ceil(limit - start) : 0;            \
        }                                                                          \
        if (count < 0) count = 0;                                                  \
        final_num = start + (double)count;                                         \
    } else {                                                                       \
        SAVE_FRAME();                                                              \
        vm_runtime_error(vm, "Range bounds must be numbers.");                     \
        return INTERPRET_RUNTIME_ERROR;                                            \
    }                                                                              \
    if (count <= 0) {                                                              \
        R(iter_reg) = int_bounds ? int_or_number(final_int) : NUMBER_VAL(final_num);\
        DISPATCH();                                                                \
    }                                                                              \
    Value receiver = R(receiver_reg);                                              \
    if (!IS_INSTANCE(receiver) || AS_INSTANCE(receiver)->klass->name != struct_name) { \
        SAVE_FRAME();                                                              \
        vm_runtime_error(vm, "Optimized method receiver changed type.");           \
        return INTERPRET_RUNTIME_ERROR;                                            \
    }                                                                              \
    ObjInstance *obj = AS_INSTANCE(receiver);                                      \
    Value idx0_v;                                                                  \
    Value idx1_v;                                                                  \
    if (!table_get(&obj->klass->field_index, field0, &idx0_v) ||                  \
        !table_get(&obj->klass->field_index, field1, &idx1_v)) {                  \
        SAVE_FRAME();                                                              \
        vm_runtime_error(vm, "Field not found.");                                  \
        return INTERPRET_RUNTIME_ERROR;                                            \
    }                                                                              \
    int idx0 = (int)AS_NUMBER(idx0_v);                                             \
    int idx1 = (int)AS_NUMBER(idx1_v);                                             \
    Value value0 = obj->fields[idx0];                                              \
    Value value1 = obj->fields[idx1];                                              \
    if (!IS_NUMERIC(value0) || !IS_NUMERIC(value1)) {                              \
        SAVE_FRAME();                                                              \
        vm_runtime_error(vm, "Operands must be numbers.");                         \
        return INTERPRET_RUNTIME_ERROR;                                            \
    }                                                                              \
    for (long long n = 0; n < count; n += 4096) {                                  \
        if (__builtin_expect(MG_ATOMIC_LOAD_BOOL(vm->cancel_requested) || vm->deadline_ms != 0, 0)) { \
            CHECK_CANCEL();                                                        \
        }                                                                          \
    }                                                                              \
    obj->fields[idx0] = NUMBER_VAL(AS_NUMBER(value0) + (double)delta0 * (double)count); \
    obj->fields[idx1] = NUMBER_VAL(AS_NUMBER(value1) + (double)delta1 * (double)count); \
    R(iter_reg) = int_bounds ? int_or_number(final_int) : NUMBER_VAL(final_num);   \
    DISPATCH();                                                                    \
}                                                                                  \
} while (0)

#ifndef __GNUC__
    case OP_FOR_FIELD2_ACCUM:
#endif
FOR_FIELD2_ACCUM_RUN(LBL_FOR_FIELD2_ACCUM, false);

#ifndef __GNUC__
    case OP_FOR_FIELD2_ACCUM_INC:
#endif
FOR_FIELD2_ACCUM_RUN(LBL_FOR_FIELD2_ACCUM_INC, true);

#undef FOR_FIELD2_ACCUM_RUN

#ifndef __GNUC__
    case OP_ARRAY_MARK_FALSE_STRIDE:
#endif
LBL_ARRAY_MARK_FALSE_STRIDE: {
    Instruction inst = READ_INST();
    Instruction aux = READ_INST();
    int array_reg = GET_A(inst);
    int index_reg = GET_B(inst);
    int limit_reg = GET_C(inst);
    int step_reg = GET_Bx(aux);
    Value array_value = R(array_reg);
    Value index_value = R(index_reg);
    Value limit_value = R(limit_reg);
    Value step_value = R(step_reg);
    if (__builtin_expect(IS_ARRAY(array_value) &&
                         IS_NUMERIC(index_value) &&
                         IS_NUMERIC(limit_value) &&
                         IS_NUMERIC(step_value), 1)) {
        ObjArray *arr = AS_ARRAY(array_value);
        long long index = (long long)AS_NUMBER(index_value);
        long long limit = (long long)AS_NUMBER(limit_value);
        long long step = (long long)AS_NUMBER(step_value);
        if (__builtin_expect(step <= 0, 0)) {
            SAVE_FRAME();
            vm_runtime_error(vm, "Loop step must be positive.");
            return INTERPRET_RUNTIME_ERROR;
        }
        while (index <= limit) {
            CHECK_LOOP_CANCEL();
            if (index >= 0) {
                while (arr->count <= index) {
                    array_push(vm, arr, NULL_VAL);
                }
                arr->items[index] = FALSE_VAL;
            }
            index += step;
        }
        R(index_reg) = int_or_number(index);
        DISPATCH();
    }
    SAVE_FRAME();
    vm_runtime_error(vm, "Optimized array stride loop expected array and numeric bounds.");
    return INTERPRET_RUNTIME_ERROR;
}

/* Closures */
#ifndef __GNUC__
    case OP_CLOSURE:
#endif
LBL_CLOSURE: {
    Instruction inst = READ_INST();
    ObjFunction *fn = AS_FUNCTION(K(GET_Bx(inst)));
    ObjClosure *closure = new_closure(vm, fn);
    R(GET_A(inst)) = OBJ_VAL(closure);

    for (int i = 0; i < closure->upvalue_count; i++) {
        Instruction upval_inst = READ_INST();
        int is_local = GET_OPCODE(upval_inst);
        int index = GET_A(upval_inst);
        if (is_local) {
            closure->upvalues[i] = capture_upvalue(vm, &slots[index]);
        } else {
            closure->upvalues[i] = frame->closure->upvalues[index];
        }
    }
    DISPATCH();
}

#define CALL_CLOSURE_FAST(closure_value, base_value, arg_count_value, expected_value) do { \
    ObjClosure *call_closure__ = (closure_value);                               \
    ObjFunction *call_fn__ = call_closure__->function;                          \
    int call_base__ = (base_value);                                             \
    int call_arg_count__ = (arg_count_value);                                   \
    int call_expected__ = (expected_value);                                     \
    CHECK_CANCEL();                                                             \
    if (__builtin_expect(call_fn__->arity != call_arg_count__, 0)) {            \
        SAVE_FRAME();                                                           \
        vm_runtime_error(vm, "Expected %d arguments but got %d.",               \
                         call_fn__->arity, call_arg_count__);                   \
        return INTERPRET_RUNTIME_ERROR;                                         \
    }                                                                           \
    if (__builtin_expect(vm->frame_count >= MAX_CALL_FRAMES, 0)) {              \
        SAVE_FRAME();                                                           \
        vm_runtime_error(vm, "Stack overflow.");                                \
        return INTERPRET_RUNTIME_ERROR;                                         \
    }                                                                           \
    int call_reg_need__ = call_fn__->reg_count > 0 ? call_fn__->reg_count : MAX_REGISTERS; \
    int call_needed__ = (int)(slots - vm->stack) + call_base__ + call_reg_need__; \
    if (__builtin_expect(vm->stack_capacity < call_needed__, 0)) {              \
        SAVE_FRAME();                                                           \
        ensure_stack(vm, call_needed__);                                        \
        slots = frame->slots;                                                   \
    }                                                                           \
    frame->ip = ip;                                                             \
    CallFrame *call_new_frame__ = &vm->frames[vm->frame_count++];               \
    call_new_frame__->closure = call_closure__;                                 \
    call_new_frame__->call_dest = call_base__;                                  \
    call_new_frame__->expected_returns = call_expected__;                       \
    frame = call_new_frame__;                                                  \
    slots = slots + call_base__;                                                \
    ip = call_fn__->chunk.code;                                                 \
    k = call_fn__->chunk.constants;                                             \
    frame->ip = ip;                                                             \
    frame->slots = slots;                                                       \
    int call_new_top__ = (int)(slots - vm->stack) + call_reg_need__;            \
    if (call_new_top__ > vm->stack_top) vm->stack_top = call_new_top__;         \
    if (call_new_top__ > vm->stack_size) vm->stack_size = call_new_top__;       \
    DISPATCH();                                                                 \
} while (0)

#define CALL_NON_CLOSURE_SLOW(callee_value, base_value, arg_count_value) do {   \
    Value slow_callee__ = (callee_value);                                       \
    int slow_base__ = (base_value);                                             \
    int slow_arg_count__ = (arg_count_value);                                   \
    SAVE_FRAME();                                                               \
    if (IS_NATIVE(slow_callee__)) {                                             \
        ObjNative *native__ = AS_NATIVE(slow_callee__);                         \
        vm->native_return_count = 0;                                             \
        Value result__ = native__->function(vm, slow_arg_count__, &slots[slow_base__ + 1]); \
        frame = &vm->frames[vm->frame_count - 1];                               \
        slots = frame->slots;                                                   \
        ip = frame->ip;                                                         \
        k = frame->closure->function->chunk.constants;                          \
        STORE_NATIVE_RESULT(slow_base__, 1, result__);                          \
        if (vm->yield_requested) return INTERPRET_YIELD;                         \
        DISPATCH();                                                             \
    }                                                                           \
    if (IS_FFI(slow_callee__)) {                                                \
        ObjFFI *ffi__ = AS_FFI(slow_callee__);                                  \
        if (ffi__->arity != slow_arg_count__) {                                 \
            vm_runtime_error(vm, "FFI %s expected %d arguments but got %d.",     \
                             ffi__->name, ffi__->arity, slow_arg_count__);       \
            return INTERPRET_RUNTIME_ERROR;                                     \
        }                                                                       \
        double (*fn1__)(double) = (double (*)(double))ffi__->c_function;         \
        double (*fn2__)(double, double) = (double (*)(double, double))ffi__->c_function; \
        Value result__ = NULL_VAL;                                              \
        if (ffi__->arity == 1) {                                                 \
            double v1__;                                                        \
            if (!ffi_read_number_arg(vm, ffi__, 0, slots[slow_base__ + 1], &v1__)) return INTERPRET_RUNTIME_ERROR; \
            result__ = NUMBER_VAL(fn1__(v1__));                                 \
        } else if (ffi__->arity == 2) {                                          \
            double v1__;                                                        \
            double v2__;                                                        \
            if (!ffi_read_number_arg(vm, ffi__, 0, slots[slow_base__ + 1], &v1__)) return INTERPRET_RUNTIME_ERROR; \
            if (!ffi_read_number_arg(vm, ffi__, 1, slots[slow_base__ + 2], &v2__)) return INTERPRET_RUNTIME_ERROR; \
            result__ = NUMBER_VAL(fn2__(v1__, v2__));                           \
        } else {                                                                \
            vm_runtime_error(vm, "FFI %s: Unsupported arity %d", ffi__->name, ffi__->arity); \
            return INTERPRET_RUNTIME_ERROR;                                     \
        }                                                                       \
        frame = &vm->frames[vm->frame_count - 1];                               \
        slots = frame->slots;                                                   \
        ip = frame->ip;                                                         \
        k = frame->closure->function->chunk.constants;                          \
        STORE_SINGLE_RESULT(slow_base__, 1, result__);                          \
        DISPATCH();                                                             \
    }                                                                           \
    if (IS_STRUCT(slow_callee__)) {                                             \
        ObjStruct *klass__ = AS_STRUCT_OBJ(slow_callee__);                      \
        ObjInstance *inst_obj__ = new_instance(vm, klass__);                    \
        for (int i__ = 0; i__ < slow_arg_count__ && i__ < klass__->field_count; i__++) { \
            inst_obj__->fields[i__] = slots[slow_base__ + 1 + i__];             \
        }                                                                       \
        STORE_SINGLE_RESULT(slow_base__, 1, OBJ_VAL(inst_obj__));               \
        LOAD_FRAME();                                                           \
        DISPATCH();                                                             \
    }                                                                           \
    vm_runtime_error(vm, "Cannot call value of type %s. Only functions and structs are callable.", \
                     value_type_name(slow_callee__));                           \
    return INTERPRET_RUNTIME_ERROR;                                             \
} while (0)

/* Function Calls */
#ifndef __GNUC__
    case OP_CALLG:
#endif
LBL_CALLG: {
    Instruction inst = READ_INST();
    int base = GET_A(inst);
    int arg_count = GET_B(inst);
    int expected = 1;
    ObjString *name = AS_STRING(K(GET_C(inst)));

    uint32_t ic_slot = ((uint32_t)(uintptr_t)name) >> 4 & (GLOBAL_IC_SIZE - 1);
    GlobalICEntry *ic = &vm->global_ic[ic_slot];
    Value callee;
    if (__builtin_expect(ic->name == name, 1)) {
        callee = ic->value;
    } else {
        if (!table_get(&vm->globals, name, &callee)) {
            SAVE_FRAME();
            vm_runtime_error(vm, "Undefined variable '%s'.", name->chars);
            return INTERPRET_RUNTIME_ERROR;
        }
        ic->name = name;
        ic->value = callee;
    }
    R(base) = callee;

    if (__builtin_expect(IS_CLOSURE(callee), 1)) {
        ObjClosure *closure = AS_CLOSURE(callee);
        ObjFunction *fn = closure->function;
        CHECK_CANCEL();

        if (__builtin_expect(fn->arity != arg_count, 0)) {
            SAVE_FRAME();
            vm_runtime_error(vm, "Expected %d arguments but got %d.", fn->arity, arg_count);
            return INTERPRET_RUNTIME_ERROR;
        }
        if (__builtin_expect(vm->frame_count >= MAX_CALL_FRAMES, 0)) {
            SAVE_FRAME();
            vm_runtime_error(vm, "Stack overflow.");
            return INTERPRET_RUNTIME_ERROR;
        }

        int reg_need = fn->reg_count > 0 ? fn->reg_count : MAX_REGISTERS;
        int needed = (int)(slots - vm->stack) + base + reg_need;
        if (__builtin_expect(vm->stack_capacity < needed, 0)) {
            SAVE_FRAME();
            ensure_stack(vm, needed);
            slots = frame->slots;
        }
        frame->ip = ip;

        CallFrame *new_frame = &vm->frames[vm->frame_count++];
        new_frame->closure = closure;
        new_frame->call_dest = base;
        new_frame->expected_returns = expected;

        frame = new_frame;
        slots = slots + base;
        ip = fn->chunk.code;
        k = fn->chunk.constants;

        frame->ip = ip;
        frame->slots = slots;

        { int new_top = (int)(slots - vm->stack) + reg_need;
          if (new_top > vm->stack_top) vm->stack_top = new_top;
          if (new_top > vm->stack_size) vm->stack_size = new_top; }

        DISPATCH();
    }

    SAVE_FRAME();

    if (IS_NATIVE(callee)) {
        ObjNative *native = AS_NATIVE(callee);
        vm->native_return_count = 0;
        vm->calling_native_userdata = native->userdata;
        Value result = native->function(vm, arg_count, &slots[base + 1]);
        vm->calling_native_userdata = NULL;
        frame = &vm->frames[vm->frame_count - 1];
        slots = frame->slots;
        ip = frame->ip;
        k = frame->closure->function->chunk.constants;
        STORE_NATIVE_RESULT(base, expected, result);
        if (vm->yield_requested) return INTERPRET_YIELD;
        DISPATCH();
    }

    if (IS_FFI(callee)) {
        ObjFFI *ffi = AS_FFI(callee);
        if (ffi->arity != arg_count) {
            vm_runtime_error(vm, "FFI %s expected %d arguments but got %d.", ffi->name, ffi->arity, arg_count);
            return INTERPRET_RUNTIME_ERROR;
        }
        double (*fn1)(double) = (double (*)(double))ffi->c_function;
        double (*fn2)(double, double) = (double (*)(double, double))ffi->c_function;
        Value result = NULL_VAL;
        if (ffi->arity == 1) {
            double v1;
            if (!ffi_read_number_arg(vm, ffi, 0, slots[base + 1], &v1)) return INTERPRET_RUNTIME_ERROR;
            result = NUMBER_VAL(fn1(v1));
        } else if (ffi->arity == 2) {
            double v1;
            double v2;
            if (!ffi_read_number_arg(vm, ffi, 0, slots[base + 1], &v1)) return INTERPRET_RUNTIME_ERROR;
            if (!ffi_read_number_arg(vm, ffi, 1, slots[base + 2], &v2)) return INTERPRET_RUNTIME_ERROR;
            result = NUMBER_VAL(fn2(v1, v2));
        } else {
            vm_runtime_error(vm, "FFI %s: Unsupported arity %d", ffi->name, ffi->arity);
            return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm->frames[vm->frame_count - 1];
        slots = frame->slots;
        ip = frame->ip;
        k = frame->closure->function->chunk.constants;
        STORE_SINGLE_RESULT(base, expected, result);
        if (vm->yield_requested) return INTERPRET_YIELD;
        DISPATCH();
    }

    if (IS_STRUCT(callee)) {
        ObjStruct *klass = AS_STRUCT_OBJ(callee);
        ObjInstance *inst_obj = new_instance(vm, klass);
        for (int i = 0; i < arg_count && i < klass->field_count; i++) {
            inst_obj->fields[i] = slots[base + 1 + i];
        }
        STORE_SINGLE_RESULT(base, expected, OBJ_VAL(inst_obj));
        LOAD_FRAME();
        DISPATCH();
    }

    vm_runtime_error(vm, "Cannot call global '%s' because it is a %s. Only functions and structs are callable.",
                     name->chars, value_type_name(callee));
    return INTERPRET_RUNTIME_ERROR;
}

#define MCALL_DISPATCH_BODY(base, arg_count, expected) do {                       \
    Value callee = R(base);                                                        \
    if (__builtin_expect(IS_CLOSURE(callee), 1)) {                                 \
        ObjClosure *closure = AS_CLOSURE(callee);                                  \
        ObjFunction *fn = closure->function;                                       \
        CHECK_CANCEL();                                                            \
        if (__builtin_expect(fn->arity != arg_count, 0)) {                         \
            SAVE_FRAME();                                                          \
            vm_runtime_error(vm, "Expected %d arguments but got %d.",              \
                             fn->arity, arg_count);                                \
            return INTERPRET_RUNTIME_ERROR;                                        \
        }                                                                          \
        if (__builtin_expect(vm->frame_count >= MAX_CALL_FRAMES, 0)) {             \
            SAVE_FRAME();                                                          \
            vm_runtime_error(vm, "Stack overflow.");                               \
            return INTERPRET_RUNTIME_ERROR;                                        \
        }                                                                          \
        int reg_need = fn->reg_count > 0 ? fn->reg_count : MAX_REGISTERS;          \
        int needed = (int)(slots - vm->stack) + base + reg_need;                   \
        if (__builtin_expect(vm->stack_capacity < needed, 0)) {                    \
            SAVE_FRAME();                                                          \
            ensure_stack(vm, needed);                                              \
            slots = frame->slots;                                                  \
        }                                                                          \
        frame->ip = ip;                                                            \
        CallFrame *new_frame = &vm->frames[vm->frame_count++];                     \
        new_frame->closure = closure;                                              \
        new_frame->call_dest = base;                                               \
        new_frame->expected_returns = expected;                                    \
        frame = new_frame;                                                         \
        slots = slots + base;                                                      \
        ip = fn->chunk.code;                                                       \
        k = fn->chunk.constants;                                                   \
        frame->ip = ip;                                                            \
        frame->slots = slots;                                                      \
        { int new_top = (int)(slots - vm->stack) + reg_need;                       \
          if (new_top > vm->stack_top) vm->stack_top = new_top;                    \
          if (new_top > vm->stack_size) vm->stack_size = new_top; }                \
        DISPATCH();                                                                \
    }                                                                              \
    SAVE_FRAME();                                                                  \
    if (IS_NATIVE(callee)) {                                                       \
        ObjNative *native = AS_NATIVE(callee);                                     \
        vm->native_return_count = 0;                                               \
        vm->calling_native_userdata = native->userdata;                            \
        Value result = native->function(vm, arg_count, &slots[base + 1]);          \
        vm->calling_native_userdata = NULL;                                        \
        frame = &vm->frames[vm->frame_count - 1];                                  \
        slots = frame->slots;                                                      \
        ip = frame->ip;                                                            \
        k = frame->closure->function->chunk.constants;                             \
        STORE_NATIVE_RESULT(base, expected, result);                               \
        if (vm->yield_requested) return INTERPRET_YIELD;                           \
        DISPATCH();                                                                \
    }                                                                              \
    if (IS_FFI(callee)) {                                                          \
        ObjFFI *ffi = AS_FFI(callee);                                              \
        if (ffi->arity != arg_count) {                                             \
            vm_runtime_error(vm, "FFI %s expected %d arguments but got %d.",      \
                             ffi->name, ffi->arity, arg_count);                    \
            return INTERPRET_RUNTIME_ERROR;                                        \
        }                                                                          \
        double (*fn1)(double) = (double (*)(double))ffi->c_function;               \
        double (*fn2)(double, double) = (double (*)(double, double))ffi->c_function; \
        Value result = NULL_VAL;                                                   \
        if (ffi->arity == 1) {                                                     \
            double v1;                                                             \
            if (!ffi_read_number_arg(vm, ffi, 0, slots[base + 1], &v1))            \
                return INTERPRET_RUNTIME_ERROR;                                    \
            result = NUMBER_VAL(fn1(v1));                                          \
        } else if (ffi->arity == 2) {                                              \
            double v1, v2;                                                         \
            if (!ffi_read_number_arg(vm, ffi, 0, slots[base + 1], &v1) ||          \
                !ffi_read_number_arg(vm, ffi, 1, slots[base + 2], &v2))            \
                return INTERPRET_RUNTIME_ERROR;                                    \
            result = NUMBER_VAL(fn2(v1, v2));                                      \
        } else {                                                                   \
            vm_runtime_error(vm, "FFI %s: Unsupported arity %d", ffi->name, ffi->arity); \
            return INTERPRET_RUNTIME_ERROR;                                        \
        }                                                                          \
        frame = &vm->frames[vm->frame_count - 1];                                  \
        slots = frame->slots;                                                      \
        ip = frame->ip;                                                            \
        k = frame->closure->function->chunk.constants;                             \
        STORE_SINGLE_RESULT(base, expected, result);                               \
        DISPATCH();                                                                \
    }                                                                              \
    if (IS_STRUCT(callee)) {                                                       \
        ObjStruct *klass = AS_STRUCT_OBJ(callee);                                  \
        ObjInstance *inst_obj = new_instance(vm, klass);                           \
        for (int i = 0; i < arg_count && i < klass->field_count; i++) {            \
            inst_obj->fields[i] = slots[base + 1 + i];                             \
        }                                                                          \
        STORE_SINGLE_RESULT(base, expected, OBJ_VAL(inst_obj));                    \
        LOAD_FRAME();                                                              \
        DISPATCH();                                                                \
    }                                                                              \
    vm_runtime_error(vm, "Cannot call value of type %s. Only functions and structs are callable.", \
                     value_type_name(callee));                                     \
    return INTERPRET_RUNTIME_ERROR;                                                \
} while(0)

#ifndef __GNUC__
    case OP_MCALL:
#endif
LBL_MCALL: {
    Instruction inst = READ_INST();
    int base = GET_A(inst);
    int arg_count = GET_B(inst);
    int expected = GET_C(inst) - 1;

    Value self = R(base + 1);
    if (!IS_INSTANCE(self) && !IS_NATIVE_HANDLE(self)) {
        arg_count--;
        for(int i = 0; i < arg_count; i++) {
            R(base + 1 + i) = R(base + 2 + i);
        }
    }

    MCALL_DISPATCH_BODY(base, arg_count, expected);
}

#define MCALLFIELD_RUN(expected_value) do {                                           \
    Instruction inst = READ_INST();                                                   \
    int base = GET_A(inst);                                                           \
    int arg_count = GET_B(inst);                                                       \
    int expected = (expected_value);                                                   \
    ObjString *name = AS_STRING(K(GET_C(inst)));                                      \
    Value self = R(base + 1);                                                          \
    Value callee = NULL_VAL;                                                           \
    if (IS_INSTANCE(self)) {                                                           \
        ObjInstance *inst_obj = AS_INSTANCE(self);                                     \
        ObjStruct *klass = inst_obj->klass;                                            \
        uint32_t method_slot = (((uint32_t)(uintptr_t)klass >> 3) ^                    \
                                ((uint32_t)(uintptr_t)name >> 5)) &                    \
                               (METHOD_IC_SIZE - 1);                                  \
        MethodICEntry *method_ic = &vm->method_ic[method_slot];                       \
        if (__builtin_expect(method_ic->klass == klass && method_ic->name == name, 1)) { \
            callee = method_ic->method;                                                \
        } else if (dict_get(klass->methods, name, &callee)) {                          \
            method_ic->klass = klass;                                                  \
            method_ic->name = name;                                                    \
            method_ic->method = callee;                                                \
        }                                                                              \
    } else if (IS_DICT(self)) {                                                        \
        dict_get_mono_cached(AS_DICT(self), name, &callee);                            \
    } else if (IS_NATIVE_HANDLE(self)) {                                                \
        dict_get(AS_NATIVE_HANDLE(self)->methods, name, &callee);                      \
    }                                                                                  \
    if (IS_NULL(callee)) {                                                             \
        SAVE_FRAME();                                                                  \
        vm_runtime_error(vm, "Method '%s' not found.", name->chars);                   \
        return INTERPRET_RUNTIME_ERROR;                                                \
    }                                                                                  \
    if (!IS_INSTANCE(self) && !IS_NATIVE_HANDLE(self)) {                               \
        arg_count--;                                                                   \
        for (int i = 0; i < arg_count; i++) {                                          \
            R(base + 1 + i) = R(base + 2 + i);                                         \
        }                                                                              \
    }                                                                                  \
    R(base) = callee;                                                                  \
    MCALL_DISPATCH_BODY(base, arg_count, expected);                                    \
} while (0)

#ifndef __GNUC__
    case OP_MCALLFIELD:
#endif
LBL_MCALLFIELD: {
    MCALLFIELD_RUN(1);
}

#ifndef __GNUC__
    case OP_MCALLFIELD0:
#endif
LBL_MCALLFIELD0: {
    MCALLFIELD_RUN(0);
}

#undef MCALLFIELD_RUN
#undef MCALL_DISPATCH_BODY

#ifndef __GNUC__
    case OP_CALLR:
#endif
LBL_CALLR: {
    Instruction inst = READ_INST();
    int base = GET_A(inst);
    int arg_count = GET_C(inst);
    Value callee = R(GET_B(inst));
    R(base) = callee;

    if (__builtin_expect(IS_CLOSURE(callee), 1)) {
        CALL_CLOSURE_FAST(AS_CLOSURE(callee), base, arg_count, 1);
    }

    CALL_NON_CLOSURE_SLOW(callee, base, arg_count);
}

#ifndef __GNUC__
    case OP_CALLSELF:
#endif
LBL_CALLSELF: {
    Instruction inst = READ_INST();
    int base = GET_A(inst);
    int arg_count = GET_B(inst);
    int expected = GET_C(inst) - 1;
    ObjClosure *closure = frame->closure;
    R(base) = OBJ_VAL(closure);
    CALL_CLOSURE_FAST(closure, base, arg_count, expected);
}

#ifndef __GNUC__
    case OP_ADDUP:
#endif
LBL_ADDUP: {
    Instruction inst = READ_INST();
    ObjUpvalue *upvalue = frame->closure->upvalues[GET_A(inst)];
    Value *loc = upvalue->location;
    Value vb = *loc;
    Value vc = R(GET_B(inst));
    if (__builtin_expect(IS_INT(vb) && IS_INT(vc), 1)) {
        *loc = int_or_number((long long)AS_INT(vb) + (long long)AS_INT(vc));
    } else if (__builtin_expect(IS_NUMERIC(vb) && IS_NUMERIC(vc), 1)) {
        *loc = NUMBER_VAL(AS_NUMBER(vb) + AS_NUMBER(vc));
    } else if (IS_OBJ(vb) && IS_OBJ(vc) &&
               AS_OBJ(vb)->type == OBJ_STRING && AS_OBJ(vc)->type == OBJ_STRING) {
        SAVE_FRAME();
        *loc = OBJ_VAL(concat_strings(vm, (ObjString*)AS_OBJ(vb), (ObjString*)AS_OBJ(vc)));
        LOAD_FRAME();
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_ADDLOCAL:
#endif
LBL_ADDLOCAL: {
    Instruction inst = READ_INST();
    int target = GET_A(inst);
    Value vb = R(target);
    Value vc = R(GET_B(inst));
    if (__builtin_expect(IS_INT(vb) && IS_INT(vc), 1)) {
        R(target) = int_or_number((long long)AS_INT(vb) + (long long)AS_INT(vc));
    } else if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc), 1)) {
        R(target) = NUMBER_VAL(AS_DOUBLE(vb) + AS_DOUBLE(vc));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {
        R(target) = NUMBER_VAL(AS_NUMBER(vb) + AS_NUMBER(vc));
    } else if (IS_OBJ(vb) && IS_OBJ(vc) &&
               AS_OBJ(vb)->type == OBJ_STRING && AS_OBJ(vc)->type == OBJ_STRING) {
        SAVE_FRAME();
        R(target) = OBJ_VAL(concat_strings(vm, AS_STRING(vb), AS_STRING(vc)));
        LOAD_FRAME();
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_SUBLOCAL:
#endif
LBL_SUBLOCAL: {
    Instruction inst = READ_INST();
    int target = GET_A(inst);
    Value vb = R(target);
    Value vc = R(GET_B(inst));
    if (__builtin_expect(IS_INT(vb) && IS_INT(vc), 1)) {
        R(target) = int_or_number((long long)AS_INT(vb) - (long long)AS_INT(vc));
    } else if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc), 1)) {
        R(target) = NUMBER_VAL(AS_DOUBLE(vb) - AS_DOUBLE(vc));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {
        R(target) = NUMBER_VAL(AS_NUMBER(vb) - AS_NUMBER(vc));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#define LOCAL_FIELD_PROP_UPDATE(label, operator, allow_concat) do {                     \
label: {                                                                                \
    Instruction inst = READ_INST();                                                     \
    int target = GET_A(inst);                                                           \
    Value obj = R(GET_B(inst));                                                         \
    ObjString *name = AS_STRING(K(GET_C(inst)));                                        \
    Value rhs = NULL_VAL;                                                               \
    const char *kind = "LookupError";                                                   \
    const char *message = "Field not found";                                            \
    const char *hint = NULL;                                                            \
    char message_buf[256];                                                              \
    if (__builtin_expect(IS_DICT(obj), 1)) {                                            \
        if (dict_get_mono_cached(AS_DICT(obj), name, &rhs)) {                           \
            Value lhs = R(target);                                                       \
            if (__builtin_expect(IS_NUMBER(lhs) && IS_NUMBER(rhs), 1)) {                \
                R(target) = NUMBER_VAL(AS_DOUBLE(lhs) operator AS_DOUBLE(rhs));         \
                DISPATCH();                                                             \
            } else if (IS_NUMERIC(lhs) && IS_NUMERIC(rhs)) {                            \
                R(target) = NUMBER_VAL(AS_NUMBER(lhs) operator AS_NUMBER(rhs));         \
                DISPATCH();                                                             \
            } else if ((allow_concat) && IS_OBJ(lhs) && IS_OBJ(rhs) &&                  \
                       AS_OBJ(lhs)->type == OBJ_STRING && AS_OBJ(rhs)->type == OBJ_STRING) { \
                SAVE_FRAME();                                                           \
                R(target) = OBJ_VAL(concat_strings(vm, AS_STRING(lhs), AS_STRING(rhs))); \
                LOAD_FRAME();                                                           \
                DISPATCH();                                                             \
            }                                                                            \
            SAVE_FRAME();                                                               \
            vm_runtime_error(vm, "Operands must be numbers.");                          \
            return INTERPRET_RUNTIME_ERROR;                                             \
        }                                                                                \
        kind = "KeyError";                                                             \
        snprintf(message_buf, sizeof(message_buf), "Key not found: %.*s",               \
                 name->length, name->chars);                                            \
        message = message_buf;                                                          \
        hint = "Check dict.has(dict, key) or provide a default value.";                 \
    } else if (IS_INSTANCE(obj)) {                                                       \
        ObjInstance *inst_obj = AS_INSTANCE(obj);                                       \
        ObjStruct *klass = inst_obj->klass;                                             \
        Value idx_val;                                                                  \
        if (table_get(&klass->field_index, name, &idx_val)) {                           \
            rhs = inst_obj->fields[(int)AS_NUMBER(idx_val)];                            \
            Value lhs = R(target);                                                       \
            if (__builtin_expect(IS_NUMBER(lhs) && IS_NUMBER(rhs), 1)) {                \
                R(target) = NUMBER_VAL(AS_DOUBLE(lhs) operator AS_DOUBLE(rhs));         \
                DISPATCH();                                                             \
            } else if (IS_NUMERIC(lhs) && IS_NUMERIC(rhs)) {                            \
                R(target) = NUMBER_VAL(AS_NUMBER(lhs) operator AS_NUMBER(rhs));         \
                DISPATCH();                                                             \
            } else if ((allow_concat) && IS_OBJ(lhs) && IS_OBJ(rhs) &&                  \
                       AS_OBJ(lhs)->type == OBJ_STRING && AS_OBJ(rhs)->type == OBJ_STRING) { \
                SAVE_FRAME();                                                           \
                R(target) = OBJ_VAL(concat_strings(vm, AS_STRING(lhs), AS_STRING(rhs))); \
                LOAD_FRAME();                                                           \
                DISPATCH();                                                             \
            }                                                                            \
            SAVE_FRAME();                                                               \
            vm_runtime_error(vm, "Operands must be numbers.");                          \
            return INTERPRET_RUNTIME_ERROR;                                             \
        }                                                                                \
        kind = "FieldError";                                                           \
        snprintf(message_buf, sizeof(message_buf), "Field not found: %.*s",             \
                 name->length, name->chars);                                            \
        message = message_buf;                                                          \
        hint = "Check the struct field name.";                                          \
    } else {                                                                             \
        kind = "TypeError";                                                            \
        message = "Value has no fields";                                                \
        hint = "Use field access on structs, dicts, or error values.";                  \
    }                                                                                    \
    SAVE_FRAME();                                                                        \
    Value err = make_error_value(vm, kind, message, hint);                              \
    LOAD_FRAME();                                                                        \
    R(target) = NULL_VAL;                                                                \
    R(target + 1) = err;                                                                 \
    RETURN_VALUES(target, 2);                                                           \
}                                                                                        \
} while (0)

#ifndef __GNUC__
    case OP_ADDLOCAL_FIELD_PROP:
#endif
LOCAL_FIELD_PROP_UPDATE(LBL_ADDLOCAL_FIELD_PROP, +, true);

#ifndef __GNUC__
    case OP_SUBLOCAL_FIELD_PROP:
#endif
LOCAL_FIELD_PROP_UPDATE(LBL_SUBLOCAL_FIELD_PROP, -, false);

#undef LOCAL_FIELD_PROP_UPDATE

#define LOCAL_LEN_UPDATE(label, operator)                                      \
label: {                                                                       \
    Instruction inst = READ_INST();                                            \
    int target = GET_A(inst);                                                  \
    Value value = R(GET_B(inst));                                              \
    int32_t length = 0;                                                        \
    if (IS_STRING(value)) {                                                    \
        length = AS_STRING(value)->length;                                     \
    } else if (IS_ARRAY(value)) {                                              \
        length = AS_ARRAY(value)->count;                                       \
    } else if (IS_DICT(value)) {                                               \
        length = AS_DICT(value)->count;                                        \
    }                                                                          \
    Value lhs = R(target);                                                     \
    if (__builtin_expect(IS_INT(lhs), 1)) {                                    \
        R(target) = int_or_number((long long)AS_INT(lhs) operator (long long)length); \
    } else if (__builtin_expect(IS_NUMBER(lhs), 1)) {                          \
        R(target) = NUMBER_VAL(AS_DOUBLE(lhs) operator (double)length);        \
    } else if (IS_NUMERIC(lhs)) {                                              \
        R(target) = NUMBER_VAL(AS_NUMBER(lhs) operator (double)length);        \
    } else {                                                                   \
        SAVE_FRAME();                                                          \
        vm_runtime_error(vm, "Operands must be numbers.");                     \
        return INTERPRET_RUNTIME_ERROR;                                        \
    }                                                                          \
    DISPATCH();                                                                \
}

#ifndef __GNUC__
    case OP_ADDLOCAL_LEN:
#endif
LOCAL_LEN_UPDATE(LBL_ADDLOCAL_LEN, +)

#ifndef __GNUC__
    case OP_SUBLOCAL_LEN:
#endif
LOCAL_LEN_UPDATE(LBL_SUBLOCAL_LEN, -)

#undef LOCAL_LEN_UPDATE

#define LOCAL_MULI_UPDATE(label, op)                                      \
label: {                                                                  \
    Instruction inst = READ_INST();                                        \
    int target = GET_A(inst);                                              \
    Value vt = R(target);                                                  \
    Value vs = R(GET_B(inst));                                             \
    int imm = GET_sC(inst);                                                \
    if (__builtin_expect(IS_INT(vt), 1)) {                                 \
        long long _base = (long long)AS_INT(vt);                           \
        if (__builtin_expect(IS_INT(vs), 1)) {                             \
            R(target) = int_or_number(_base op ((long long)AS_INT(vs) * (long long)imm)); \
            DISPATCH();                                                    \
        } else if (IS_NUMBER(vs)) {                                        \
            R(target) = NUMBER_VAL((double)_base op (AS_DOUBLE(vs) * (double)imm)); \
            DISPATCH();                                                    \
        }                                                                  \
    } else if (__builtin_expect(IS_NUMBER(vt), 1)) {                       \
        double base = AS_DOUBLE(vt);                                       \
        if (__builtin_expect(IS_INT(vs), 1)) {                             \
            R(target) = NUMBER_VAL(base op ((double)AS_INT(vs) * (double)imm)); \
            DISPATCH();                                                    \
        } else if (IS_NUMBER(vs)) {                                        \
            R(target) = NUMBER_VAL(base op (AS_DOUBLE(vs) * (double)imm)); \
            DISPATCH();                                                    \
        }                                                                  \
    }                                                                      \
    SAVE_FRAME();                                                          \
    vm_runtime_error(vm, "Operands must be numbers.");                      \
    return INTERPRET_RUNTIME_ERROR;                                         \
}

#define LOCAL_MULK_UPDATE(label, op)                                      \
label: {                                                                  \
    Instruction inst = READ_INST();                                        \
    int target = GET_A(inst);                                              \
    Value vt = R(target);                                                  \
    Value vs = R(GET_B(inst));                                             \
    Value vk = K(GET_C(inst));                                             \
    if (__builtin_expect(IS_INT(vt) && IS_INT(vk), 1)) {                   \
        long long _base = (long long)AS_INT(vt);                           \
        long long _scale = (long long)AS_INT(vk);                          \
        if (__builtin_expect(IS_INT(vs), 1)) {                             \
            R(target) = int_or_number(_base op ((long long)AS_INT(vs) * _scale)); \
            DISPATCH();                                                    \
        } else if (IS_NUMBER(vs)) {                                        \
            R(target) = NUMBER_VAL((double)_base op (AS_DOUBLE(vs) * (double)_scale)); \
            DISPATCH();                                                    \
        }                                                                  \
    } else if (__builtin_expect(IS_NUMBER(vt) && IS_NUMBER(vk), 1)) {       \
        double base = AS_DOUBLE(vt);                                       \
        double scale = AS_DOUBLE(vk);                                      \
        if (__builtin_expect(IS_INT(vs), 1)) {                             \
            R(target) = NUMBER_VAL(base op ((double)AS_INT(vs) * scale));  \
            DISPATCH();                                                    \
        } else if (IS_NUMBER(vs)) {                                        \
            R(target) = NUMBER_VAL(base op (AS_DOUBLE(vs) * scale));       \
            DISPATCH();                                                    \
        }                                                                  \
    } else if (IS_NUMERIC(vt) && IS_NUMERIC(vs) && IS_NUMERIC(vk)) {      \
        R(target) = NUMBER_VAL(AS_NUMBER(vt) op (AS_NUMBER(vs) * AS_NUMBER(vk))); \
        DISPATCH();                                                        \
    }                                                                      \
    SAVE_FRAME();                                                          \
    vm_runtime_error(vm, "Operands must be numbers.");                      \
    return INTERPRET_RUNTIME_ERROR;                                         \
}

#define LOCAL_DIVI_UPDATE(label, op)                                      \
label: {                                                                  \
    Instruction inst = READ_INST();                                        \
    int target = GET_A(inst);                                              \
    Value vt = R(target);                                                  \
    Value vs = R(GET_B(inst));                                             \
    double divisor = (double)GET_sC(inst);                                 \
    if (__builtin_expect(IS_INT(vt), 1)) {                                \
        double base = (double)AS_INT(vt);                                  \
        if (__builtin_expect(IS_INT(vs), 1)) {                             \
            R(target) = NUMBER_VAL(base op ((double)AS_INT(vs) / divisor)); \
            DISPATCH();                                                    \
        }                                                                  \
    } else if (__builtin_expect(IS_NUMBER(vt), 1)) {                      \
        double base = AS_DOUBLE(vt);                                       \
        if (__builtin_expect(IS_INT(vs), 1)) {                             \
            R(target) = NUMBER_VAL(base op ((double)AS_INT(vs) / divisor)); \
            DISPATCH();                                                    \
        } else if (IS_NUMBER(vs)) {                                        \
            R(target) = NUMBER_VAL(base op (AS_DOUBLE(vs) / divisor));     \
            DISPATCH();                                                    \
        }                                                                  \
    } else if (IS_NUMERIC(vt) && IS_NUMERIC(vs)) {                         \
        R(target) = NUMBER_VAL(AS_NUMBER(vt) op (AS_NUMBER(vs) / divisor)); \
        DISPATCH();                                                        \
    }                                                                      \
    SAVE_FRAME();                                                          \
    vm_runtime_error(vm, "Operands must be numbers.");                      \
    return INTERPRET_RUNTIME_ERROR;                                         \
}

#define LOCAL_MODI_UPDATE(label, op)                                      \
label: {                                                                  \
    Instruction inst = READ_INST();                                        \
    int target = GET_A(inst);                                              \
    Value vt = R(target);                                                  \
    Value vs = R(GET_B(inst));                                             \
    int divisor = GET_sC(inst);                                            \
    if (__builtin_expect(IS_INT(vt) && IS_INT(vs) && divisor != 0, 1)) {  \
        int32_t mod_result = AS_INT(vs) % divisor;                         \
        R(target) = int_or_number((long long)AS_INT(vt) op (long long)mod_result); \
        DISPATCH();                                                        \
    }                                                                      \
    double rhs;                                                            \
    if (__builtin_expect(IS_INT(vs) && divisor != 0, 1)) {                 \
        rhs = (double)(AS_INT(vs) % divisor);                              \
    } else if (IS_NUMERIC(vs)) {                                           \
        rhs = number_mod_i(AS_NUMBER(vs), divisor);                        \
    } else {                                                               \
        SAVE_FRAME();                                                      \
        vm_runtime_error(vm, "Operands must be numbers.");                  \
        return INTERPRET_RUNTIME_ERROR;                                     \
    }                                                                      \
    if (__builtin_expect(IS_NUMBER(vt), 1)) {                              \
        R(target) = NUMBER_VAL(AS_DOUBLE(vt) op rhs);                      \
    } else if (IS_INT(vt)) {                                               \
        R(target) = NUMBER_VAL((double)AS_INT(vt) op rhs);                 \
    } else {                                                               \
        SAVE_FRAME();                                                      \
        vm_runtime_error(vm, "Operands must be numbers.");                  \
        return INTERPRET_RUNTIME_ERROR;                                     \
    }                                                                      \
    DISPATCH();                                                            \
}

#ifndef __GNUC__
    case OP_ADDLOCAL_MULI:
#endif
LOCAL_MULI_UPDATE(LBL_ADDLOCAL_MULI, +)

#ifndef __GNUC__
    case OP_SUBLOCAL_MULI:
#endif
LOCAL_MULI_UPDATE(LBL_SUBLOCAL_MULI, -)

#ifndef __GNUC__
    case OP_ADDLOCAL_MULK:
#endif
LOCAL_MULK_UPDATE(LBL_ADDLOCAL_MULK, +)

#ifndef __GNUC__
    case OP_SUBLOCAL_MULK:
#endif
LOCAL_MULK_UPDATE(LBL_SUBLOCAL_MULK, -)

#ifndef __GNUC__
    case OP_ADDLOCAL_DIVI:
#endif
LOCAL_DIVI_UPDATE(LBL_ADDLOCAL_DIVI, +)

#ifndef __GNUC__
    case OP_SUBLOCAL_DIVI:
#endif
LOCAL_DIVI_UPDATE(LBL_SUBLOCAL_DIVI, -)

#ifndef __GNUC__
    case OP_ADDLOCAL_MODI:
#endif
LOCAL_MODI_UPDATE(LBL_ADDLOCAL_MODI, +)

#ifndef __GNUC__
    case OP_SUBLOCAL_MODI:
#endif
LOCAL_MODI_UPDATE(LBL_SUBLOCAL_MODI, -)

#undef LOCAL_MULI_UPDATE
#undef LOCAL_MULK_UPDATE
#undef LOCAL_DIVI_UPDATE
#undef LOCAL_MODI_UPDATE

#ifndef __GNUC__
    case OP_CALL:
#endif
LBL_CALL: {
    Instruction inst = READ_INST();
    int base = GET_A(inst);
    int arg_count = GET_B(inst);
    int expected = GET_C(inst) - 1;
    Value callee = R(base);

    if (__builtin_expect(IS_CLOSURE(callee), 1)) {
        ObjClosure *closure = AS_CLOSURE(callee);
        ObjFunction *fn = closure->function;
        CHECK_CANCEL();

        if (__builtin_expect(fn->arity != arg_count, 0)) {
            SAVE_FRAME();
            vm_runtime_error(vm, "Expected %d arguments but got %d.", fn->arity, arg_count);
            return INTERPRET_RUNTIME_ERROR;
        }
        if (__builtin_expect(vm->frame_count >= MAX_CALL_FRAMES, 0)) {
            SAVE_FRAME();
            vm_runtime_error(vm, "Stack overflow.");
            return INTERPRET_RUNTIME_ERROR;
        }

        int reg_need = fn->reg_count > 0 ? fn->reg_count : MAX_REGISTERS;
        int needed = (int)(slots - vm->stack) + base + reg_need;
        if (__builtin_expect(vm->stack_capacity < needed, 0)) {
            SAVE_FRAME();
            ensure_stack(vm, needed);
            slots = frame->slots;
        }
        frame->ip = ip;

        CallFrame *new_frame = &vm->frames[vm->frame_count++];
        new_frame->closure = closure;
        new_frame->call_dest = base;
        new_frame->expected_returns = expected;

        frame = new_frame;
        slots = slots + base;
        ip = fn->chunk.code;
        k = fn->chunk.constants;

        frame->ip = ip;
        frame->slots = slots;

        /* Update stack_top for GC scanning */
        { int new_top = (int)(slots - vm->stack) + reg_need;
          if (new_top > vm->stack_top) vm->stack_top = new_top;
          if (new_top > vm->stack_size) vm->stack_size = new_top; }

        DISPATCH();
    }

    SAVE_FRAME();

    if (IS_NATIVE(callee)) {
        ObjNative *native = AS_NATIVE(callee);
        vm->native_return_count = 0;
        vm->calling_native_userdata = native->userdata;
        Value result = native->function(vm, arg_count, &slots[base + 1]);
        vm->calling_native_userdata = NULL;
        frame = &vm->frames[vm->frame_count - 1];
        slots = frame->slots;
        ip = frame->ip;
        k = frame->closure->function->chunk.constants;
        STORE_NATIVE_RESULT(base, expected, result);
        if (vm->yield_requested) return INTERPRET_YIELD;
        DISPATCH();
    }

    if (IS_FFI(callee)) {
        ObjFFI *ffi = AS_FFI(callee);
        if (ffi->arity != arg_count) {
            vm_runtime_error(vm, "FFI %s expected %d arguments but got %d.", ffi->name, ffi->arity, arg_count);
            return INTERPRET_RUNTIME_ERROR;
        }
        double (*fn1)(double) = (double (*)(double))ffi->c_function;
        double (*fn2)(double, double) = (double (*)(double, double))ffi->c_function;
        Value result = NULL_VAL;
        if (ffi->arity == 1) {
            double v1;
            if (!ffi_read_number_arg(vm, ffi, 0, slots[base + 1], &v1)) return INTERPRET_RUNTIME_ERROR;
            result = NUMBER_VAL(fn1(v1));
        } else if (ffi->arity == 2) {
            double v1;
            double v2;
            if (!ffi_read_number_arg(vm, ffi, 0, slots[base + 1], &v1)) return INTERPRET_RUNTIME_ERROR;
            if (!ffi_read_number_arg(vm, ffi, 1, slots[base + 2], &v2)) return INTERPRET_RUNTIME_ERROR;
            result = NUMBER_VAL(fn2(v1, v2));
        } else {
            vm_runtime_error(vm, "FFI %s: Unsupported arity %d", ffi->name, ffi->arity);
            return INTERPRET_RUNTIME_ERROR;
        }
        frame = &vm->frames[vm->frame_count - 1];
        slots = frame->slots;
        ip = frame->ip;
        k = frame->closure->function->chunk.constants;
        STORE_SINGLE_RESULT(base, expected, result);
        DISPATCH();
    }

    if (IS_STRUCT(callee)) {
        ObjStruct *klass = AS_STRUCT_OBJ(callee);
        ObjInstance *inst_obj = new_instance(vm, klass);
        for (int i = 0; i < arg_count && i < klass->field_count; i++) {
            inst_obj->fields[i] = slots[base + 1 + i];
        }
        STORE_SINGLE_RESULT(base, expected, OBJ_VAL(inst_obj));
        LOAD_FRAME();
        DISPATCH();
    }

    vm_runtime_error(vm, "Cannot call value of type %s. Only functions and structs are callable.",
                     value_type_name(callee));
    return INTERPRET_RUNTIME_ERROR;
}

/* --- Return --- */
#ifndef __GNUC__
    case OP_RETURN:
#endif
LBL_RETURN: {
    Instruction inst = READ_INST();
    int first = GET_A(inst);
    int ret_count = GET_B(inst);
    RETURN_VALUES(first, ret_count);
}

/* --- Data Structures --- */
#ifndef __GNUC__
    case OP_NEWARRAY:
#endif
LBL_NEWARRAY: {
    Instruction inst = READ_INST();
    int hint = GET_B(inst);
    ObjArray *arr = new_array(vm);
    if (hint > 0) {
        /* Pre-allocate capacity to avoid repeated growth from SETARRAY */
        int cap = 8;
        while (cap < hint) cap *= 2;
        arr->capacity = cap;
        arr->items = realloc(arr->items, sizeof(Value) * cap);
        vm->bytes_allocated += sizeof(Value) * cap;
        for (int i = 0; i < cap; i++) arr->items[i] = NULL_VAL;
    }
    R(GET_A(inst)) = OBJ_VAL(arr);
    DISPATCH();
}

#ifndef __GNUC__
    case OP_SETARRAY:
#endif
LBL_SETARRAY: {
    Instruction inst = READ_INST();
    Value arr_val = R(GET_A(inst));
    if (IS_ARRAY(arr_val)) {
        ObjArray *arr = AS_ARRAY(arr_val);
        int idx = GET_B(inst);
        Value val = R(GET_C(inst));
        /* Grow array if needed */
        while (arr->count <= idx) {
            array_push(vm, arr, NULL_VAL);
        }
        arr->items[idx] = val;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_ARRAY_PUSH:
#endif
LBL_ARRAY_PUSH: {
    Instruction inst = READ_INST();
    Value arr_val = R(GET_A(inst));
    if (IS_ARRAY(arr_val)) {
        ObjArray *arr = AS_ARRAY(arr_val);
        if (__builtin_expect(arr->capacity >= arr->count + 1, 1)) {
            arr->items[arr->count++] = R(GET_B(inst));
        } else {
            array_push(vm, arr, R(GET_B(inst)));
        }
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_GETINDEX:
#endif
LBL_GETINDEX: {
    Instruction inst = READ_INST();
    Value obj = R(GET_B(inst));
    Value idx = R(GET_C(inst));

    if (IS_ARRAY(obj) && IS_NUMERIC(idx)) {
        int i = IS_INT(idx) ? AS_INT(idx) : (int)AS_DOUBLE(idx);
        ObjArray *arr = AS_ARRAY(obj);
        R(GET_A(inst)) = (i >= 0 && i < arr->count) ? arr->items[i] : NULL_VAL;
    } else if (IS_DICT(obj) && IS_STRING(idx)) {
        Value val;
        if (dict_get_mono_cached(AS_DICT(obj), AS_STRING(idx), &val)) {
            R(GET_A(inst)) = val;
        } else {
            R(GET_A(inst)) = NULL_VAL;
        }
    } else if (IS_STRING(obj) && IS_NUMERIC(idx)) {
        ObjString *str = AS_STRING(obj);
        int i = (int)AS_NUMBER(idx);
        if (i >= 0 && i < str->length) {
            SAVE_FRAME();
            char ch = string_char_at(str, i);
            R(GET_A(inst)) = OBJ_VAL(copy_string(vm, &ch, 1));
            LOAD_FRAME();
        } else {
            R(GET_A(inst)) = NULL_VAL;
        }
    } else {
        R(GET_A(inst)) = NULL_VAL;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_GETINDEX_TRY:
#endif
LBL_GETINDEX_TRY: {
    Instruction inst = READ_INST();
    int dest = GET_A(inst);
    Value obj = R(GET_B(inst));
    Value idx = R(GET_C(inst));
    Value result = NULL_VAL;
    char message[256];
    const char *kind = "LookupError";
    const char *hint = NULL;
    bool ok = false;

    if (IS_ARRAY(obj)) {
        if (IS_NUMERIC(idx)) {
            int i = IS_INT(idx) ? AS_INT(idx) : (int)AS_DOUBLE(idx);
            ObjArray *arr = AS_ARRAY(obj);
            if (i >= 0 && i < arr->count) {
                result = arr->items[i];
                ok = true;
            } else {
                kind = "IndexError";
                snprintf(message, sizeof(message), "Index out of range: %d", i);
                hint = "Check the array length before reading this index.";
            }
        } else {
            kind = "TypeError";
            snprintf(message, sizeof(message), "Array index must be a number");
        }
    } else if (IS_DICT(obj)) {
        if (IS_STRING(idx)) {
            ObjString *key = AS_STRING(idx);
            if (dict_get_cached(vm, AS_DICT(obj), key, &result)) {
                ok = true;
            } else if (key->chars) {
                kind = "KeyError";
                snprintf(message, sizeof(message), "Key not found: %.*s", key->length, key->chars);
                hint = "Check dict.has(dict, key) or provide a default value.";
            } else {
                kind = "KeyError";
                snprintf(message, sizeof(message), "Key not found");
            }
        } else {
            kind = "TypeError";
            snprintf(message, sizeof(message), "Dictionary key must be a string");
        }
    } else if (IS_STRING(obj)) {
        if (IS_NUMERIC(idx)) {
            ObjString *str = AS_STRING(obj);
            int i = IS_INT(idx) ? AS_INT(idx) : (int)AS_DOUBLE(idx);
            if (i >= 0 && i < str->length) {
                SAVE_FRAME();
                char ch = string_char_at(str, i);
                result = OBJ_VAL(copy_string(vm, &ch, 1));
                LOAD_FRAME();
                ok = true;
            } else {
                kind = "IndexError";
                snprintf(message, sizeof(message), "Index out of range: %d", i);
                hint = "Check the string length before reading this index.";
            }
        } else {
            kind = "TypeError";
            snprintf(message, sizeof(message), "String index must be a number");
        }
    } else {
        kind = "TypeError";
        snprintf(message, sizeof(message), "Value is not indexable");
    }

    if (ok) {
        R(dest) = result;
        R(dest + 1) = NULL_VAL;
        DISPATCH();
    }

    SAVE_FRAME();
    Value err = make_error_value(vm, kind, message, hint);
    LOAD_FRAME();
    R(dest) = NULL_VAL;
    R(dest + 1) = err;
    DISPATCH();
}

#ifndef __GNUC__
    case OP_SETINDEX:
#endif
LBL_SETINDEX: {
    Instruction inst = READ_INST();
    Value obj = R(GET_A(inst));
    Value idx = R(GET_B(inst));
    Value val = R(GET_C(inst));

    if (IS_ARRAY(obj) && IS_NUMERIC(idx)) {
        int i = IS_INT(idx) ? AS_INT(idx) : (int)AS_DOUBLE(idx);
        ObjArray *arr = AS_ARRAY(obj);
        while (arr->count <= i) {
            array_push(vm, arr, NULL_VAL);
        }
        arr->items[i] = val;
    } else if (IS_DICT(obj) && IS_STRING(idx)) {
        dict_set(vm, AS_DICT(obj), AS_STRING(idx), val);
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_NEWDICT:
#endif
LBL_NEWDICT: {
    Instruction inst = READ_INST();
    int hint = GET_B(inst);
    ObjDict *dict = new_dict(vm);
    if (hint > 0) {
        int cap = 8;
        while (cap < hint * 4 / 3) cap *= 2;
        dict_adjust_capacity(vm, dict, cap);
    }
    R(GET_A(inst)) = OBJ_VAL(dict);
    DISPATCH();
}

/* --- Field Access --- */
#ifndef __GNUC__
    case OP_GETFIELD:
#endif
LBL_GETFIELD: {
    Instruction inst = READ_INST();
    Value obj = R(GET_B(inst));
    ObjString *name = AS_STRING(K(GET_C(inst)));

    if (IS_INSTANCE(obj)) {
        ObjInstance *inst_obj = AS_INSTANCE(obj);
        ObjStruct *klass = inst_obj->klass;
        uint32_t field_slot = (((uint32_t)(uintptr_t)klass) ^ ((uint32_t)(uintptr_t)name >> 4)) &
                              (FIELD_IC_SIZE - 1);
        FieldICEntry *field_ic = &vm->field_ic[field_slot];
        if (__builtin_expect(field_ic->klass == klass && field_ic->name == name, 1)) {
            R(GET_A(inst)) = inst_obj->fields[field_ic->index];
            DISPATCH();
        }
        Value idx_val;
        if (table_get(&klass->field_index, name, &idx_val)) {
            int idx = (int)AS_NUMBER(idx_val);
            field_ic->klass = klass;
            field_ic->name = name;
            field_ic->index = idx;
            R(GET_A(inst)) = inst_obj->fields[idx];
            DISPATCH();
        }
        uint32_t method_slot = (((uint32_t)(uintptr_t)klass >> 3) ^ ((uint32_t)(uintptr_t)name >> 5)) &
                               (METHOD_IC_SIZE - 1);
        MethodICEntry *method_ic = &vm->method_ic[method_slot];
        if (__builtin_expect(method_ic->klass == klass && method_ic->name == name, 1)) {
            R(GET_A(inst)) = method_ic->method;
            DISPATCH();
        }
        Value method;
        if (dict_get(klass->methods, name, &method)) {
            method_ic->klass = klass;
            method_ic->name = name;
            method_ic->method = method;
            R(GET_A(inst)) = method;
            DISPATCH();
        }
        R(GET_A(inst)) = NULL_VAL;
    } else if (IS_DICT(obj)) {
        Value val;
        if (dict_get_mono_cached(AS_DICT(obj), name, &val)) {
            R(GET_A(inst)) = val;
        } else {
            R(GET_A(inst)) = NULL_VAL;
        }
    } else if (IS_ERROR(obj)) {
        ObjError *err = AS_ERROR(obj);
        if (name->length == 4 && memcmp(name->chars, "kind", 4) == 0) {
            R(GET_A(inst)) = err->kind ? OBJ_VAL(err->kind) : NULL_VAL;
        } else if (name->length == 7 && memcmp(name->chars, "message", 7) == 0) {
            R(GET_A(inst)) = err->message ? OBJ_VAL(err->message) : NULL_VAL;
        } else if (name->length == 4 && memcmp(name->chars, "file", 4) == 0) {
            R(GET_A(inst)) = err->file ? OBJ_VAL(err->file) : NULL_VAL;
        } else if (name->length == 4 && memcmp(name->chars, "line", 4) == 0) {
            R(GET_A(inst)) = INT_VAL(err->line);
        } else if (name->length == 8 && memcmp(name->chars, "function", 8) == 0) {
            R(GET_A(inst)) = err->function ? OBJ_VAL(err->function) : NULL_VAL;
        } else if (name->length == 4 && memcmp(name->chars, "hint", 4) == 0) {
            R(GET_A(inst)) = err->hint ? OBJ_VAL(err->hint) : NULL_VAL;
        } else if (name->length == 6 && memcmp(name->chars, "report", 6) == 0) {
            SAVE_FRAME();
            R(GET_A(inst)) = error_to_string(vm, err);
            LOAD_FRAME();
        } else {
            R(GET_A(inst)) = NULL_VAL;
        }
    } else if (IS_NATIVE_HANDLE(obj)) {
        Value method;
        if (dict_get(AS_NATIVE_HANDLE(obj)->methods, name, &method)) {
            R(GET_A(inst)) = method;
        } else {
            R(GET_A(inst)) = NULL_VAL;
        }
    } else {
        R(GET_A(inst)) = NULL_VAL;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_GETFIELD_TRY:
#endif
LBL_GETFIELD_TRY: {
    Instruction inst = READ_INST();
    int dest = GET_A(inst);
    Value obj = R(GET_B(inst));
    ObjString *name = AS_STRING(K(GET_C(inst)));
    Value result = NULL_VAL;
    const char *kind = "LookupError";
    const char *message = "Field not found";
    const char *hint = NULL;
    char message_buf[256];

    if (__builtin_expect(IS_DICT(obj), 1)) {
        if (dict_get_mono_cached(AS_DICT(obj), name, &result)) {
            R(dest) = result;
            R(dest + 1) = NULL_VAL;
            DISPATCH();
        }
        kind = "KeyError";
        snprintf(message_buf, sizeof(message_buf), "Key not found: %.*s", name->length, name->chars);
        message = message_buf;
        hint = "Check dict.has(dict, key) or provide a default value.";
    } else if (IS_INSTANCE(obj)) {
        ObjInstance *inst_obj = AS_INSTANCE(obj);
        ObjStruct *klass = inst_obj->klass;
        uint32_t field_slot = (((uint32_t)(uintptr_t)klass) ^ ((uint32_t)(uintptr_t)name >> 4)) &
                              (FIELD_IC_SIZE - 1);
        FieldICEntry *field_ic = &vm->field_ic[field_slot];
        if (__builtin_expect(field_ic->klass == klass && field_ic->name == name, 1)) {
            R(dest) = inst_obj->fields[field_ic->index];
            R(dest + 1) = NULL_VAL;
            DISPATCH();
        }
        Value idx_val;
        if (table_get(&klass->field_index, name, &idx_val)) {
            int idx = (int)AS_NUMBER(idx_val);
            field_ic->klass = klass;
            field_ic->name = name;
            field_ic->index = idx;
            R(dest) = inst_obj->fields[idx];
            R(dest + 1) = NULL_VAL;
            DISPATCH();
        }
        kind = "FieldError";
        snprintf(message_buf, sizeof(message_buf), "Field not found: %.*s", name->length, name->chars);
        message = message_buf;
        hint = "Check the struct field name.";
    } else if (IS_NATIVE_HANDLE(obj)) {
        if (dict_get(AS_NATIVE_HANDLE(obj)->methods, name, &result)) {
            R(dest) = result;
            R(dest + 1) = NULL_VAL;
            DISPATCH();
        }
        kind = "FieldError";
        snprintf(message_buf, sizeof(message_buf), "Native handle '%s' has no method '%.*s'",
                  AS_NATIVE_HANDLE(obj)->type_name->chars, name->length, name->chars);
        message = message_buf;
        hint = "Check the native handle method name registered by the host.";
    } else {
        kind = "TypeError";
        message = "Value has no fields";
        hint = "Use field access on structs, dicts, errors, or native handles.";
    }

    SAVE_FRAME();
    Value err = make_error_value(vm, kind, message, hint);
    LOAD_FRAME();
    R(dest) = NULL_VAL;
    R(dest + 1) = err;
    DISPATCH();
}

#ifndef __GNUC__
    case OP_GETFIELD_PROP:
#endif
LBL_GETFIELD_PROP: {
    Instruction inst = READ_INST();
    int dest = GET_A(inst);
    Value obj = R(GET_B(inst));
    ObjString *name = AS_STRING(K(GET_C(inst)));
    Value result = NULL_VAL;
    const char *kind = "LookupError";
    const char *message = "Field not found";
    const char *hint = NULL;
    char message_buf[256];

    if (__builtin_expect(IS_DICT(obj), 1)) {
        if (dict_get_mono_cached(AS_DICT(obj), name, &result)) {
            R(dest) = result;
            DISPATCH();
        }
        kind = "KeyError";
        snprintf(message_buf, sizeof(message_buf), "Key not found: %.*s", name->length, name->chars);
        message = message_buf;
        hint = "Check dict.has(dict, key) or provide a default value.";
    } else if (IS_INSTANCE(obj)) {
        ObjInstance *inst_obj = AS_INSTANCE(obj);
        ObjStruct *klass = inst_obj->klass;
        uint32_t field_slot = (((uint32_t)(uintptr_t)klass) ^ ((uint32_t)(uintptr_t)name >> 4)) &
                              (FIELD_IC_SIZE - 1);
        FieldICEntry *field_ic = &vm->field_ic[field_slot];
        if (__builtin_expect(field_ic->klass == klass && field_ic->name == name, 1)) {
            R(dest) = inst_obj->fields[field_ic->index];
            DISPATCH();
        }
        Value idx_val;
        if (table_get(&klass->field_index, name, &idx_val)) {
            int idx = (int)AS_NUMBER(idx_val);
            field_ic->klass = klass;
            field_ic->name = name;
            field_ic->index = idx;
            R(dest) = inst_obj->fields[idx];
            DISPATCH();
        }
        kind = "FieldError";
        snprintf(message_buf, sizeof(message_buf), "Field not found: %.*s", name->length, name->chars);
        message = message_buf;
        hint = "Check the struct field name.";
    } else if (IS_NATIVE_HANDLE(obj)) {
        if (dict_get(AS_NATIVE_HANDLE(obj)->methods, name, &result)) {
            R(dest) = result;
            DISPATCH();
        }
        kind = "FieldError";
        snprintf(message_buf, sizeof(message_buf), "Native handle '%s' has no method '%.*s'",
                 AS_NATIVE_HANDLE(obj)->type_name->chars, name->length, name->chars);
        message = message_buf;
        hint = "Check the native handle method name registered by the host.";
    } else {
        kind = "TypeError";
        message = "Value has no fields";
        hint = "Use field access on structs, dicts, errors, or native handles.";
    }

    SAVE_FRAME();
    Value err = make_error_value(vm, kind, message, hint);
    LOAD_FRAME();
    R(dest) = NULL_VAL;
    R(dest + 1) = err;
    RETURN_VALUES(dest, 2);
}

#ifndef __GNUC__
    case OP_SETFIELD:
#endif
LBL_SETFIELD: {
    Instruction inst = READ_INST();
    Value obj = R(GET_A(inst));
    ObjString *name = AS_STRING(K(GET_B(inst)));
    Value val = R(GET_C(inst));

    if (IS_INSTANCE(obj)) {
        ObjInstance *inst_obj = AS_INSTANCE(obj);
        ObjStruct *klass = inst_obj->klass;
        uint32_t field_slot = (((uint32_t)(uintptr_t)klass) ^ ((uint32_t)(uintptr_t)name >> 4)) &
                              (FIELD_IC_SIZE - 1);
        FieldICEntry *field_ic = &vm->field_ic[field_slot];
        if (__builtin_expect(field_ic->klass == klass && field_ic->name == name, 1)) {
            inst_obj->fields[field_ic->index] = val;
            gc_write_barrier(vm, (Obj *)inst_obj, val);
            DISPATCH();
        }
        Value idx_val;
        if (table_get(&klass->field_index, name, &idx_val)) {
            int idx = (int)AS_NUMBER(idx_val);
            field_ic->klass = klass;
            field_ic->name = name;
            field_ic->index = idx;
            inst_obj->fields[idx] = val;
            gc_write_barrier(vm, (Obj *)inst_obj, val);
            DISPATCH();
        }
        SAVE_FRAME();
        vm_runtime_error(vm, "Struct '%s' has no field '%s'.", klass->name->chars, name->chars);
        return INTERPRET_RUNTIME_ERROR;
    } else if (IS_STRUCT(obj)) {
        /* Setting a method on a struct definition */
        ObjStruct *klass = AS_STRUCT_OBJ(obj);
        dict_set(vm, klass->methods, name, val);
        uint32_t method_slot = (((uint32_t)(uintptr_t)klass >> 3) ^ ((uint32_t)(uintptr_t)name >> 5)) &
                               (METHOD_IC_SIZE - 1);
        vm->method_ic[method_slot].klass = klass;
        vm->method_ic[method_slot].name = name;
        vm->method_ic[method_slot].method = val;
    } else if (IS_DICT(obj)) {
        dict_set(vm, AS_DICT(obj), name, val);
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_GETFIELD_IDX:
#endif
LBL_GETFIELD_IDX: {
    Instruction inst = READ_INST();
    Value obj = R(GET_B(inst));
    int idx = GET_C(inst);
    if (IS_INSTANCE(obj) && idx >= 0 && idx < AS_INSTANCE(obj)->klass->field_count) {
        R(GET_A(inst)) = AS_INSTANCE(obj)->fields[idx];
    } else {
        R(GET_A(inst)) = NULL_VAL;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_SETFIELD_IDX:
#endif
LBL_SETFIELD_IDX: {
    Instruction inst = READ_INST();
    Value obj = R(GET_A(inst));
    int idx = GET_C(inst);
    Value val = R(GET_B(inst));
    if (IS_INSTANCE(obj) && idx >= 0 && idx < AS_INSTANCE(obj)->klass->field_count) {
        AS_INSTANCE(obj)->fields[idx] = val;
        gc_write_barrier(vm, AS_OBJ(obj), val);
    }
    DISPATCH();
}

/* --- Iterators --- */
#ifndef __GNUC__
    case OP_ITER_PREP:
#endif
LBL_ITER_PREP: {
    Instruction inst = READ_INST();
    /* Store iterator state: R[A] = 0 (index counter) */
    R(GET_A(inst)) = NUMBER_VAL(0);
    DISPATCH();
}

#ifndef __GNUC__
    case OP_ITER_NEXT:
#endif
LBL_ITER_NEXT: {
    Instruction inst = READ_INST();
    int var_reg = GET_A(inst);
    int iter_reg = GET_B(inst);
    int jump_dist = GET_C(inst);
    int idx = (int)AS_NUMBER(R(iter_reg));

    /* The iterable is in the register before the iterator state */
    Value iterable = R(iter_reg - 1);

    if (IS_ARRAY(iterable)) {
        ObjArray *arr = AS_ARRAY(iterable);
        if (idx >= arr->count) {
            ip += jump_dist;
            DISPATCH();
        }
        R(var_reg) = arr->items[idx];
        R(iter_reg) = NUMBER_VAL(idx + 1);
    } else if (IS_DICT(iterable)) {
        ObjDict *dict = AS_DICT(iterable);
        while (idx < dict->entry_count &&
               (dict->entries[idx].key == NULL || dict->entries[idx].key == TOMBSTONE_KEY)) {
            idx++;
        }
        if (idx >= dict->entry_count) {
            ip += jump_dist;
            DISPATCH();
        }
        R(var_reg) = OBJ_VAL(dict->entries[idx].key);
        R(var_reg + 1) = dict->entries[idx].value;
        R(iter_reg) = NUMBER_VAL(idx + 1);
    } else {
        ip += jump_dist;
    }
    DISPATCH();
}

/* --- Fused Ternary Arithmetic --- */
#ifndef __GNUC__
    case OP_ADDSUB:
#endif
LBL_ADDSUB: {
    Instruction inst = READ_INST();
    Instruction aux = READ_INST();
    int a = GET_A(inst);
    Value vb = R(GET_B(inst));
    Value vc = R(GET_C(inst));
    Value vd = R(GET_Bx(aux));
    if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc) && IS_NUMBER(vd), 1)) {
        R(a) = NUMBER_VAL(AS_DOUBLE(vb) + AS_DOUBLE(vc) - AS_DOUBLE(vd));
    } else if (IS_INT(vb) && IS_INT(vc) && IS_INT(vd)) {
        R(a) = int_or_number((long long)AS_INT(vb) + (long long)AS_INT(vc) - (long long)AS_INT(vd));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc) && IS_NUMERIC(vd)) {
        R(a) = NUMBER_VAL(AS_NUMBER(vb) + AS_NUMBER(vc) - AS_NUMBER(vd));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_SUBADD:
#endif
LBL_SUBADD: {
    Instruction inst = READ_INST();
    Instruction aux = READ_INST();
    int a = GET_A(inst);
    Value vb = R(GET_B(inst));
    Value vc = R(GET_C(inst));
    Value vd = R(GET_Bx(aux));
    if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc) && IS_NUMBER(vd), 1)) {
        R(a) = NUMBER_VAL(AS_DOUBLE(vb) - AS_DOUBLE(vc) + AS_DOUBLE(vd));
    } else if (IS_INT(vb) && IS_INT(vc) && IS_INT(vd)) {
        R(a) = int_or_number((long long)AS_INT(vb) - (long long)AS_INT(vc) + (long long)AS_INT(vd));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc) && IS_NUMERIC(vd)) {
        R(a) = NUMBER_VAL(AS_NUMBER(vb) - AS_NUMBER(vc) + AS_NUMBER(vd));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_MULADD:
#endif
LBL_MULADD: {
    Instruction inst = READ_INST();
    Instruction aux = READ_INST();
    int a = GET_A(inst);
    Value vb = R(GET_B(inst));
    Value vc = R(GET_C(inst));
    Value vd = R(GET_Bx(aux));
    if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc) && IS_NUMBER(vd), 1)) {
        R(a) = NUMBER_VAL(AS_DOUBLE(vb) * AS_DOUBLE(vc) + AS_DOUBLE(vd));
    } else if (IS_INT(vb) && IS_INT(vc) && IS_INT(vd)) {
        R(a) = int_or_number((long long)AS_INT(vb) * (long long)AS_INT(vc) + (long long)AS_INT(vd));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc) && IS_NUMERIC(vd)) {
        R(a) = NUMBER_VAL(AS_NUMBER(vb) * AS_NUMBER(vc) + AS_NUMBER(vd));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_MULSUB:
#endif
LBL_MULSUB: {
    Instruction inst = READ_INST();
    Instruction aux = READ_INST();
    int a = GET_A(inst);
    Value vb = R(GET_B(inst));
    Value vc = R(GET_C(inst));
    Value vd = R(GET_Bx(aux));
    if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc) && IS_NUMBER(vd), 1)) {
        R(a) = NUMBER_VAL(AS_DOUBLE(vb) * AS_DOUBLE(vc) - AS_DOUBLE(vd));
    } else if (IS_INT(vb) && IS_INT(vc) && IS_INT(vd)) {
        R(a) = int_or_number((long long)AS_INT(vb) * (long long)AS_INT(vc) - (long long)AS_INT(vd));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc) && IS_NUMERIC(vd)) {
        R(a) = NUMBER_VAL(AS_NUMBER(vb) * AS_NUMBER(vc) - AS_NUMBER(vd));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

/* --- Fused Comparison-of-Sum --- */
#ifndef __GNUC__
    case OP_ADD_GT_TEST:
#endif
LBL_ADD_GT_TEST: {
    Instruction inst = READ_INST();
    Value va = R(GET_A(inst));
    Value vb = R(GET_B(inst));
    int imm = GET_sC(inst);
    bool result;
    if (__builtin_expect(IS_NUMBER(va) && IS_NUMBER(vb), 1)) {
        result = (AS_DOUBLE(va) + AS_DOUBLE(vb)) > (double)imm;
    } else if (IS_INT(va) && IS_INT(vb)) {
        result = (long long)AS_INT(va) + (long long)AS_INT(vb) > (long long)imm;
    } else if (IS_NUMERIC(va) && IS_NUMERIC(vb)) {
        result = (AS_NUMBER(va) + AS_NUMBER(vb)) > (double)imm;
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    if (result) {
        ip++;
    } else {
        Instruction jmp = *ip++;
        ip += GET_sBx(jmp);
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_ADD_GE_TEST:
#endif
LBL_ADD_GE_TEST: {
    Instruction inst = READ_INST();
    Value va = R(GET_A(inst));
    Value vb = R(GET_B(inst));
    int imm = GET_sC(inst);
    bool result;
    if (__builtin_expect(IS_NUMBER(va) && IS_NUMBER(vb), 1)) {
        result = (AS_DOUBLE(va) + AS_DOUBLE(vb)) >= (double)imm;
    } else if (IS_INT(va) && IS_INT(vb)) {
        result = (long long)AS_INT(va) + (long long)AS_INT(vb) >= (long long)imm;
    } else if (IS_NUMERIC(va) && IS_NUMERIC(vb)) {
        result = (AS_NUMBER(va) + AS_NUMBER(vb)) >= (double)imm;
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    if (result) {
        ip++;
    } else {
        Instruction jmp = *ip++;
        ip += GET_sBx(jmp);
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_SUB_GT_TEST:
#endif
LBL_SUB_GT_TEST: {
    Instruction inst = READ_INST();
    Value va = R(GET_A(inst));
    Value vb = R(GET_B(inst));
    int imm = GET_sC(inst);
    bool result;
    if (__builtin_expect(IS_NUMBER(va) && IS_NUMBER(vb), 1)) {
        result = (AS_DOUBLE(va) - AS_DOUBLE(vb)) > (double)imm;
    } else if (IS_INT(va) && IS_INT(vb)) {
        result = (long long)AS_INT(va) - (long long)AS_INT(vb) > (long long)imm;
    } else if (IS_NUMERIC(va) && IS_NUMERIC(vb)) {
        result = (AS_NUMBER(va) - AS_NUMBER(vb)) > (double)imm;
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    if (result) {
        ip++;
    } else {
        Instruction jmp = *ip++;
        ip += GET_sBx(jmp);
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_SUB_GE_TEST:
#endif
LBL_SUB_GE_TEST: {
    Instruction inst = READ_INST();
    Value va = R(GET_A(inst));
    Value vb = R(GET_B(inst));
    int imm = GET_sC(inst);
    bool result;
    if (__builtin_expect(IS_NUMBER(va) && IS_NUMBER(vb), 1)) {
        result = (AS_DOUBLE(va) - AS_DOUBLE(vb)) >= (double)imm;
    } else if (IS_INT(va) && IS_INT(vb)) {
        result = (long long)AS_INT(va) - (long long)AS_INT(vb) >= (long long)imm;
    } else if (IS_NUMERIC(va) && IS_NUMERIC(vb)) {
        result = (AS_NUMBER(va) - AS_NUMBER(vb)) >= (double)imm;
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    if (result) {
        ip++;
    } else {
        Instruction jmp = *ip++;
        ip += GET_sBx(jmp);
    }
    DISPATCH();
}

/* --- Fused Multiply-Accumulate into Local --- */
#ifndef __GNUC__
    case OP_MULLOCAL_ADD:
#endif
LBL_MULLOCAL_ADD: {
    Instruction inst = READ_INST();
    int target = GET_A(inst);
    Value vb = R(GET_B(inst));
    Value vc = R(GET_C(inst));
    if (__builtin_expect(IS_INT(vb) && IS_INT(vc), 1)) {
        R(target) = int_or_number((long long)AS_INT(vb) * (long long)AS_INT(vc) + (IS_INT(R(target)) ? (long long)AS_INT(R(target)) : (long long)AS_NUMBER(R(target))));
    } else if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc), 1)) {
        double acc = IS_NUMBER(R(target)) ? AS_DOUBLE(R(target)) : AS_NUMBER(R(target));
        R(target) = NUMBER_VAL(AS_DOUBLE(vb) * AS_DOUBLE(vc) + acc);
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {
        double acc = AS_NUMBER(R(target));
        R(target) = NUMBER_VAL(AS_NUMBER(vb) * AS_NUMBER(vc) + acc);
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_MULLOCAL_SUB:
#endif
LBL_MULLOCAL_SUB: {
    Instruction inst = READ_INST();
    int target = GET_A(inst);
    Value vb = R(GET_B(inst));
    Value vc = R(GET_C(inst));
    if (__builtin_expect(IS_INT(vb) && IS_INT(vc), 1)) {
        R(target) = int_or_number((IS_INT(R(target)) ? (long long)AS_INT(R(target)) : (long long)AS_NUMBER(R(target))) - (long long)AS_INT(vb) * (long long)AS_INT(vc));
    } else if (__builtin_expect(IS_NUMBER(vb) && IS_NUMBER(vc), 1)) {
        double acc = IS_NUMBER(R(target)) ? AS_DOUBLE(R(target)) : AS_NUMBER(R(target));
        R(target) = NUMBER_VAL(acc - AS_DOUBLE(vb) * AS_DOUBLE(vc));
    } else if (IS_NUMERIC(vb) && IS_NUMERIC(vc)) {
        double acc = AS_NUMBER(R(target));
        R(target) = NUMBER_VAL(acc - AS_NUMBER(vb) * AS_NUMBER(vc));
    } else {
        SAVE_FRAME();
        vm_runtime_error(vm, "Operands must be numbers.");
        return INTERPRET_RUNTIME_ERROR;
    }
    DISPATCH();
}

/* --- Misc --- */
#ifndef __GNUC__
    case OP_AUX:
#endif
LBL_AUX: {
    (void)READ_INST();
    DISPATCH();
}

#ifndef __GNUC__
    case OP_CLOSE_UPVAL:
#endif
LBL_CLOSE_UPVAL: {
    Instruction inst = READ_INST();
    close_upvalues(vm, &slots[GET_A(inst)]);
    DISPATCH();
}

#ifndef __GNUC__
    case OP_DEFER:
#endif
LBL_DEFER: {
    Instruction inst = READ_INST();
    Value val = R(GET_A(inst));
    if (IS_CLOSURE(val)) {
        if (vm->defer_stack.count >= vm->defer_stack.capacity) {
            vm->defer_stack.capacity = vm->defer_stack.capacity < 8 ? 8 : vm->defer_stack.capacity * 2;
            vm->defer_stack.items = realloc(vm->defer_stack.items,
                sizeof(ObjClosure *) * vm->defer_stack.capacity);
        }
        vm->defer_stack.items[vm->defer_stack.count++] = AS_CLOSURE(val);
    }
    DISPATCH();
}

#ifndef __GNUC__
    case OP_NEWSTRUCT:
#endif
LBL_NEWSTRUCT: {
    Instruction inst = READ_INST();
    ObjString *name = AS_STRING(K(GET_Bx(inst)));
    ObjStruct *s = new_struct(vm, name);

    /* Read field count from next pseudo-instruction */
    Instruction fc_inst = READ_INST();
    int field_count = GET_OPCODE(fc_inst);
    s->field_count = field_count;
    s->field_names = (ObjString **)malloc(sizeof(ObjString *) * field_count);

    /* Read field name constant indices */
    for (int i = 0; i < field_count; i++) {
        Instruction fn_inst = READ_INST();
        int fk = GET_OPCODE(fn_inst);
        s->field_names[i] = AS_STRING(K(fk));
        table_set(&s->field_index, s->field_names[i], NUMBER_VAL(i));
    }

    R(GET_A(inst)) = OBJ_VAL(s);
    DISPATCH();
}

    /* End of dispatch - should never reach here */
#ifndef __GNUC__
    default:
        return INTERPRET_RUNTIME_ERROR;
    } /* end switch */
#endif
    return INTERPRET_RUNTIME_ERROR;

    #undef READ_INST
    #undef DISPATCH
    #undef NEXT
    #undef R
    #undef K
    #undef SAVE_FRAME
    #undef LOAD_FRAME
    #undef CHECK_CANCEL
    #undef STORE_SINGLE_RESULT
    #undef STORE_NATIVE_RESULT
}

/* ========================================================================
 * Public API
 * ======================================================================== */
VM *vm_new(void) {
    VM *vm = (VM *)malloc(sizeof(VM));
    if (!vm) return NULL;
    vm_init(vm);
    return vm;
}

void vm_delete(VM *vm) {
    if (!vm) return;
    vm_free(vm);
    free(vm);
}

bool vm_set_global_value(VM *vm, const char *name, Value value) {
    if (!vm || !name) return false;
    vm_push(vm, value);
    ObjString *key = copy_string(vm, name, (int)strlen(name));
    bool ok = table_set(&vm->globals, key, value);
    vm_pop(vm);
    memset(vm->global_ic, 0, sizeof(vm->global_ic));
    return ok;
}

bool vm_get_global_value(VM *vm, const char *name, Value *out) {
    if (!vm || !name || !out) return false;
    ObjString *key = copy_string(vm, name, (int)strlen(name));
    return table_get(&vm->globals, key, out);
}

void vm_register_native(VM *vm, const char *name, NativeFn function, int arity,
                        void *userdata, void (*userdata_finalizer)(void *)) {
    if (!vm || !name || !function) return;
    define_native_with_userdata(vm, name, function, arity, userdata, userdata_finalizer);
    memset(vm->global_ic, 0, sizeof(vm->global_ic));
}

ObjFFI *vm_register_ffi(VM *vm, const char *name, void *c_func, int arity) {
    if (!vm || !name || !c_func) return NULL;
    ObjString *key = copy_string(vm, name, (int)strlen(name));
    ObjFFI *ffi = new_ffi(vm, c_func, key->chars, arity);
    vm_push(vm, OBJ_VAL(ffi));
    table_set(&vm->globals, key, OBJ_VAL(ffi));
    vm_pop(vm);
    memset(vm->global_ic, 0, sizeof(vm->global_ic));
    return ffi;
}

ObjNativeHandle *vm_new_native_handle(VM *vm, const char *type_name, void *data,
                                      NativeHandleFinalizer finalizer) {
    if (!vm || !type_name) return NULL;
    ObjString *type = copy_string(vm, type_name, (int)strlen(type_name));
    vm_push(vm, OBJ_VAL(type));
    ObjNativeHandle *handle = new_native_handle(vm, type, data, finalizer);
    vm_pop(vm);
    return handle;
}

bool vm_native_handle_set_method(VM *vm, ObjNativeHandle *handle, const char *name,
                                 NativeFn function, int arity,
                                 void *userdata, void (*userdata_finalizer)(void *)) {
    if (!vm || !handle || !name || !function) return false;
    vm_push(vm, OBJ_VAL(handle));
    ObjString *key = copy_string(vm, name, (int)strlen(name));
    ObjNative *native = new_native(vm, function, key->chars, arity);
    native->userdata = userdata;
    native->userdata_finalizer = userdata_finalizer;
    vm_push(vm, OBJ_VAL(native));
    bool ok = dict_set(vm, handle->methods, key, OBJ_VAL(native));
    vm_pop(vm);
    vm_pop(vm);
    return ok;
}

void *vm_native_handle_data(Value value, const char *type_name) {
    if (!IS_NATIVE_HANDLE(value)) return NULL;
    ObjNativeHandle *handle = AS_NATIVE_HANDLE(value);
    if (type_name && handle->type_name &&
        strcmp(handle->type_name->chars, type_name) != 0) {
        return NULL;
    }
    return handle->data;
}

Value vm_native_handle_value(ObjNativeHandle *handle) {
    return handle ? OBJ_VAL(handle) : NULL_VAL;
}

ObjFunction *vm_compile_named(VM *vm, const char *source, const char *name) {
    vm_clear_error(vm);
    /* Parse */
    bool had_error = false;
    ASTNode *ast = parse(source, &had_error);
    if (had_error) {
        ast_free(ast);
        return NULL;
    }

    if (!mg_typecheck_ast(ast, false)) {
        ast_free(ast);
        return NULL;
    }

    /* Optimize (constant folding) */
    optimize_ast(ast);

    /* Compile */
    const char *source_name = name ? name : "<script>";
    ObjFunction *function = compile_named(vm, ast, source_name, (int)strlen(source_name));
    ast_free(ast);
    return function;
}

ObjFunction *vm_compile(VM *vm, const char *source) {
    return vm_compile_named(vm, source, "<script>");
}

static InterpretResult vm_run_closure(VM *vm, ObjClosure *closure) {
    ObjFunction *function = closure->function;
    int reg_need = function->reg_count > 0 ? function->reg_count : MAX_REGISTERS;

    ensure_stack(vm, reg_need);
    clear_stack_range(vm, 0, reg_need);
    vm->stack[0] = OBJ_VAL(closure);
    vm->stack_size = reg_need;
    vm->stack_top = reg_need;

    CallFrame *frame = &vm->frames[0];
    frame->closure = closure;
    frame->ip = function->chunk.code;
    frame->slots = vm->stack;
    frame->call_dest = 0;
    frame->expected_returns = 1;
    vm->frame_count = 1;

    return vm_execute(vm);
}

InterpretResult vm_run_function(VM *vm, ObjFunction *function) {
    vm_clear_error(vm);
    /* Root the function while allocating its closure. */
    vm_push(vm, OBJ_VAL(function));
    ObjClosure *closure = new_closure(vm, function);
    vm_pop(vm);

    InterpretResult result = vm_run_closure(vm, closure);

    while (result == INTERPRET_OK && vm->task_queue.count > 0) {
        ObjClosure *task = vm->task_queue.items[0];
        memmove(vm->task_queue.items, vm->task_queue.items + 1,
            sizeof(ObjClosure *) * (vm->task_queue.count - 1));
        vm->task_queue.count--;
        result = vm_run_closure(vm, task);
    }

    clear_stack_range(vm, 0, vm->stack_top);
    vm->stack_size = 0;
    vm->stack_top = 0;
    vm->frame_count = 0;
    return result;
}

InterpretResult vm_interpret(VM *vm, const char *source) {
    return vm_interpret_named(vm, source, "<script>");
}

InterpretResult vm_interpret_named(VM *vm, const char *source, const char *name) {
    ObjFunction *function = vm_compile_named(vm, source, name);
    if (!function) return INTERPRET_COMPILE_ERROR;
    return vm_run_function(vm, function);
}
