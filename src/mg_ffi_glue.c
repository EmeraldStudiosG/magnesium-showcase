#include "magnesium.h"
#include <string.h>

void mg_runtime_error_simple(VM *vm, const char *message) {
    if (!vm) return;
    vm_runtime_error(vm, "%s", message ? message : "Runtime error.");
}

void *mg_get_native_userdata(VM *vm) {
    return vm ? vm->calling_native_userdata : NULL;
}

int vm_frame_count(VM *vm) {
    return vm ? vm->frame_count : 0;
}

int vm_stack_top(VM *vm) {
    return vm ? vm->stack_top : 0;
}

size_t vm_bytes_allocated(VM *vm) {
    return vm ? vm->bytes_allocated : 0;
}

bool vm_last_error_value(VM *vm, Value *out) {
    if (out) *out = NULL_VAL;
    if (!vm || !out || vm->stack_top <= 0) return false;
    Value value = vm->stack[vm->stack_top - 1];
    if (!IS_ERROR(value)) return false;
    *out = value;
    return true;
}

const char *vm_string_chars(Value value) {
    if (!IS_STRING(value)) return NULL;
    ObjString *s = AS_STRING(value);
    if (s->is_rope) return NULL;
    return s->chars;
}

int vm_string_length(Value value) {
    if (!IS_STRING(value)) return 0;
    ObjString *s = AS_STRING(value);
    return s->length;
}

const char *vm_string_chars_resolved(VM *vm, Value value) {
    if (!vm || !IS_STRING(value)) return NULL;
    vm_push(vm, value);
    const char *chars = string_chars(vm, AS_STRING(value));
    vm_pop(vm);
    return chars;
}

bool vm_string_copy(VM *vm, Value value, char *buffer, size_t capacity,
                    size_t *required) {
    if (!vm || !IS_STRING(value)) {
        if (required) *required = 0;
        return false;
    }
    ObjString *string = AS_STRING(value);
    size_t needed = (size_t)string->length + 1;
    if (required) *required = needed;
    if (!buffer || capacity < needed) return false;
    vm_push(vm, value);
    memcpy(buffer, string_chars(vm, string), (size_t)string->length);
    vm_pop(vm);
    buffer[string->length] = '\0';
    return true;
}

