#include "magnesium.h"

void mg_runtime_error_simple(VM *vm, const char *message) {
    vm_runtime_error(vm, "%s", message);
}

void *mg_get_native_userdata(VM *vm) {
    return vm->calling_native_userdata;
}

int vm_frame_count(VM *vm) {
    return vm->frame_count;
}

size_t vm_bytes_allocated(VM *vm) {
    return vm->bytes_allocated;
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
    if (s->is_rope) return 0;
    return s->length;
}

