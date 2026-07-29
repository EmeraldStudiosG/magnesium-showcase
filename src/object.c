/* Heap-allocated objects: strings, arrays, dicts, functions, closures, structs.
 * Also contains the hash table implementation used for globals/string interning. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include "magnesium.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

static void print_string(ObjString *string);

int format_number(char *buffer, size_t capacity, double number) {
    if (!buffer || capacity == 0) return -1;
    if (isfinite(number) && trunc(number) == number &&
        number >= -9007199254740991.0 && number <= 9007199254740991.0) {
        return snprintf(buffer, capacity, "%.0f", number);
    }
    if (!isfinite(number)) {
        return snprintf(buffer, capacity, "%g", number);
    }

    /* Pick the shortest %g representation that parses back to the same
       double. This keeps output readable without the six-digit truncation
       of bare %g. */
    for (int precision = 1; precision <= 17; precision++) {
        int length = snprintf(buffer, capacity, "%.*g", precision, number);
        if (length < 0 || (size_t)length >= capacity) return length;
        char *end = NULL;
        double parsed = strtod(buffer, &end);
        if (end && *end == '\0' && parsed == number) return length;
    }
    return snprintf(buffer, capacity, "%.17g", number);
}

/* ========================================================================
 * Value helpers
 * ======================================================================== */
void print_value(Value value) {
    if (IS_NULL(value)) {
        printf("null");
    } else if (IS_BOOL(value)) {
        printf(AS_BOOL(value) ? "true" : "false");
    } else if (IS_INT(value)) {
        printf("%d", AS_INT(value));
    } else if (IS_NUMBER(value)) {
        char buffer[64];
        int length = format_number(buffer, sizeof(buffer), AS_NUMBER(value));
        if (length > 0 && (size_t)length < sizeof(buffer)) {
            fwrite(buffer, 1, (size_t)length, stdout);
        }
    } else if (IS_OBJ(value)) {
            Obj *obj = AS_OBJ(value);
            switch (obj->type) {
                case OBJ_STRING:
                    print_string(AS_STRING(value));
                    break;
                case OBJ_FUNCTION: {
                    ObjFunction *fn = AS_FUNCTION(value);
                    if (fn->name) printf("<fn %s>", fn->name->chars);
                    else printf("<script>");
                    break;
                }
                case OBJ_CLOSURE:
                    if (AS_CLOSURE(value)->function->name)
                        printf("<fn %s>", AS_CLOSURE(value)->function->name->chars);
                    else printf("<script>");
                    break;
                case OBJ_NATIVE:
                    printf("<native %s>", AS_NATIVE(value)->name->chars);
                    break;
                case OBJ_FFI:
                    printf("<ffi %s>", AS_FFI(value)->name->chars);
                    break;
                case OBJ_NATIVE_HANDLE:
                    printf("<native-handle %s>", AS_NATIVE_HANDLE(value)->type_name->chars);
                    break;
                case OBJ_ARRAY:
                    printf("<array[%d]>", AS_ARRAY(value)->count);
                    break;
                case OBJ_DICT:
                    printf("<dict>");
                    break;
                case OBJ_STRUCT:
                    printf("<struct %s>", AS_STRUCT_OBJ(value)->name->chars);
                    break;
                case OBJ_INSTANCE:
                    printf("<instance %s>", AS_INSTANCE(value)->klass->name->chars);
                    break;
                case OBJ_ERROR: {
                    ObjError *err = AS_ERROR(value);
                    printf("error[");
                    print_string(err->kind);
                    printf("]: ");
                    print_string(err->message);
                    if (err->file && err->line > 0) {
                        printf("\n  at ");
                        print_string(err->file);
                        printf(":%d", err->line);
                    }
                    if (err->hint && err->hint->length > 0) {
                        printf("\n  hint: ");
                        print_string(err->hint);
                    }
                    break;
                }
                case OBJ_VM_TASK:
                    printf("<vm-task>");
                    break;
                case OBJ_COROUTINE:
                    printf("<coroutine>");
                    break;
                case OBJ_UPVALUE:
                    printf("<upvalue>");
                    break;
            }
    }
}

static void print_string(ObjString *string) {
    if (!string) return;
    int capacity = 64;
    int count = 0;
    ObjString **stack = (ObjString **)malloc(sizeof(ObjString *) * capacity);
    if (!stack) {
        fprintf(stderr, "Out of memory printing string.\n");
        exit(1);
    }

    stack[count++] = string;
    while (count > 0) {
        ObjString *current = stack[--count];
        if (current->is_rope) {
            if (count + 2 > capacity) {
                capacity *= 2;
                ObjString **new_stack = (ObjString **)realloc(stack, sizeof(ObjString *) * capacity);
                if (!new_stack) {
                    free(stack);
                    fprintf(stderr, "Out of memory printing string.\n");
                    exit(1);
                }
                stack = new_stack;
            }
            stack[count++] = current->right;
            stack[count++] = current->left;
        } else {
            fwrite(current->chars, sizeof(char), current->length, stdout);
        }
    }
    free(stack);
}

static void copy_string_bytes(ObjString *string, char *dest, int *offset) {
    int capacity = 64;
    int count = 0;
    ObjString **stack = (ObjString **)malloc(sizeof(ObjString *) * capacity);
    if (!stack) {
        fprintf(stderr, "Out of memory flattening string.\n");
        exit(1);
    }

    stack[count++] = string;
    while (count > 0) {
        ObjString *current = stack[--count];
        if (current->is_rope) {
            if (count + 2 > capacity) {
                capacity *= 2;
                ObjString **new_stack = (ObjString **)realloc(stack, sizeof(ObjString *) * capacity);
                if (!new_stack) {
                    free(stack);
                    fprintf(stderr, "Out of memory flattening string.\n");
                    exit(1);
                }
                stack = new_stack;
            }
            stack[count++] = current->right;
            stack[count++] = current->left;
        } else {
            memcpy(dest + *offset, current->chars, current->length);
            *offset += current->length;
        }
    }
    free(stack);
}

const char *string_chars(VM *vm, ObjString *string) {
    if (!string->is_rope) return string->chars;

    char *chars = (char *)malloc((size_t)string->length + 1);
    if (!chars) {
        fprintf(stderr, "Out of memory flattening string.\n");
        exit(1);
    }

    int offset = 0;
    copy_string_bytes(string, chars, &offset);
    chars[string->length] = '\0';

    string->chars = chars;
    string->capacity = string->length;
    string->is_rope = false;
    string->left = NULL;
    string->right = NULL;
    vm->bytes_allocated += (size_t)string->length + 1;
    return string->chars;
}

char string_char_at(ObjString *string, int index) {
    while (string->is_rope) {
        if (index < string->left->length) {
            string = string->left;
        } else {
            index -= string->left->length;
            string = string->right;
        }
    }
    return string->chars[index];
}

bool values_equal(Value a, Value b) {
    if (a == b) return true;
    if (IS_NUMERIC(a) && IS_NUMERIC(b)) return AS_NUMBER(a) == AS_NUMBER(b);
    if (!IS_STRING(a) || !IS_STRING(b)) return false;

    ObjString *sa = AS_STRING(a);
    ObjString *sb = AS_STRING(b);
    if (sa->length != sb->length || sa->hash != sb->hash) return false;
    for (int i = 0; i < sa->length; i++) {
        if (string_char_at(sa, i) != string_char_at(sb, i)) return false;
    }
    return true;
}

/* ========================================================================
 * Slab allocator for small heap objects
 * ======================================================================== */
static const size_t SLAB_CLASS_SIZES[SLAB_CLASS_COUNT] = {
    16, 24, 32, 48, 64, 80, 96, 128, 192, 256, 384, 512, 768, 1024
};

static uint8_t size_class_for_size(size_t size) {
    if (size <= 16) return 0;
    if (size <= 24) return 1;
    if (size <= 32) return 2;
    if (size <= 48) return 3;
    if (size <= 64) return 4;
    if (size <= 80) return 5;
    if (size <= 96) return 6;
    if (size <= 128) return 7;
    if (size <= 192) return 8;
    if (size <= 256) return 9;
    if (size <= 384) return 10;
    if (size <= 512) return 11;
    if (size <= 768) return 12;
    if (size <= 1024) return 13;
    return 255; /* larger than max slab size */
}

static void slab_refill(VM *vm, uint8_t class_idx) {
    size_t chunk_size = SLAB_CLASS_SIZES[class_idx];
    size_t usable = SLAB_SIZE / chunk_size * chunk_size;

    uint8_t *memory = (uint8_t *)malloc(SLAB_SIZE);
    if (!memory) {
        fprintf(stderr, "Out of memory allocating slab.\n");
        exit(1);
    }

    Slab *slab = (Slab *)malloc(sizeof(Slab));
    if (!slab) {
        fprintf(stderr, "Out of memory allocating slab header.\n");
        exit(1);
    }
    slab->memory = memory;
    slab->next = vm->slab_blocks;
    vm->slab_blocks = slab;

    for (size_t offset = 0; offset + chunk_size <= usable; offset += chunk_size) {
        Obj *obj = (Obj *)(memory + offset);
        obj->size_class = class_idx + 1; /* pre-stamp for later reuse */
        obj->next = vm->slab_free_lists[class_idx];
        vm->slab_free_lists[class_idx] = obj;
    }
}

Obj *allocate_object(VM *vm, size_t size, ObjType type) {
    const size_t nursery_limit = (size_t)8 * 1024 * 1024;
    bool needs_major = size > vm->next_gc ||
                       vm->bytes_allocated > vm->next_gc - size;
    bool needs_minor = size > nursery_limit ||
                       vm->young_bytes > nursery_limit - size;

    /*
     * A major collection already collects both generations. Running a minor
     * first would age unrooted newborns held by in-progress C constructors,
     * then let the immediately following major collection sweep them.
     */
    if (needs_major) {
        gc_major_collect(vm);
    } else if (needs_minor) {
        gc_minor_collect(vm);
    }

    Obj *obj;
    uint8_t class_idx = size_class_for_size(size);

    if (class_idx < SLAB_CLASS_COUNT) {
        if (__builtin_expect(vm->slab_free_lists[class_idx] == NULL, 0)) {
            slab_refill(vm, class_idx);
        }
        obj = vm->slab_free_lists[class_idx];
        vm->slab_free_lists[class_idx] = obj->next;
    } else {
        obj = (Obj *)malloc(size);
        if (!obj) {
            fprintf(stderr, "Out of memory.\n");
            exit(1);
        }
        obj->size_class = 0;
    }

    obj->type = type;
    obj->is_marked = false;
    obj->is_old = false;
    obj->gc_age = 0;
    obj->alloc_size = size;
    obj->next = vm->young_objects;
    vm->young_objects = obj;
    vm->bytes_allocated += size;
    vm->young_bytes += size;
    return obj;
}

/* ========================================================================
 * Strings (interned via hash table)
 * ======================================================================== */
static uint32_t hash_bytes_continue(uint32_t hash, const char *key, int length) {
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

static uint32_t hash_string(const char *key, int length) {
    return hash_bytes_continue(2166136261u, key, length);
}

static uint32_t hash_string_object_continue(uint32_t hash, ObjString *string) {
    if (string->is_rope) {
        hash = hash_string_object_continue(hash, string->left);
        return hash_string_object_continue(hash, string->right);
    }
    return hash_bytes_continue(hash, string->chars, string->length);
}

static ObjString *allocate_flat_string(VM *vm, char *chars, int length, uint32_t hash) {
    ObjString *string = (ObjString *)allocate_object(vm, sizeof(ObjString), OBJ_STRING);
    string->length = length;
    string->capacity = length;
    string->hash = hash;
    string->is_rope = false;
    string->chars = chars;
    string->left = NULL;
    string->right = NULL;
    vm->bytes_allocated += (size_t)length + 1;
    table_set(&vm->strings, string, NULL_VAL);
    vm->strings_interned_since_minor++;
    return string;
}

ObjString *copy_string(VM *vm, const char *chars, int length) {
    uint32_t hash = hash_string(chars, length);
    ObjString *interned = table_find_string(&vm->strings, chars, length, hash);
    if (interned) return interned;

    char *heap = (char *)malloc(length + 1);
    if (!heap) {
        fprintf(stderr, "Out of memory allocating string.\n");
        exit(1);
    }
    memcpy(heap, chars, length);
    heap[length] = '\0';
    return allocate_flat_string(vm, heap, length, hash);
}

ObjString *take_string(VM *vm, char *chars, int length) {
    uint32_t hash = hash_string(chars, length);
    ObjString *interned = table_find_string(&vm->strings, chars, length, hash);
    if (interned) {
        free(chars);
        return interned;
    }
    return allocate_flat_string(vm, chars, length, hash);
}

ObjString *concat_strings(VM *vm, ObjString *a, ObjString *b) {
    if (!vm || !a || !b) return NULL;
    if (b->length > INT_MAX - a->length) {
        vm_runtime_error(vm, "Concatenated string is too large.");
        return NULL;
    }
    int length = a->length + b->length;
    ObjString *string = (ObjString *)allocate_object(vm, sizeof(ObjString), OBJ_STRING);
    string->length = length;
    string->capacity = 0;
    string->hash = hash_string_object_continue(a->hash, b);
    string->is_rope = true;
    string->chars = NULL;
    string->left = a;
    string->right = b;
    return string;
}

/* ========================================================================
 * Functions, Closures, Upvalues, Natives
 * ======================================================================== */
ObjFunction *new_function(VM *vm) {
    ObjFunction *fn = (ObjFunction *)allocate_object(vm, sizeof(ObjFunction), OBJ_FUNCTION);
    fn->arity = 0;
    fn->upvalue_count = 0;
    fn->reg_count = 0;
    fn->name = NULL;
    fn->source_name = NULL;
    chunk_init(&fn->chunk);
    return fn;
}

ObjClosure *new_closure(VM *vm, ObjFunction *function) {
    int uv_count = function->upvalue_count;
    size_t size = sizeof(ObjClosure) + sizeof(ObjUpvalue *) * uv_count;

    /* Try free-list first for closures with matching upvalue count */
    ObjClosure *closure = NULL;
    ObjClosure **prev = &vm->closure_free_list;
    for (ObjClosure *c = vm->closure_free_list; c != NULL; c = (ObjClosure *)c->obj.next) {
        if (c->upvalue_count == uv_count) {
            *prev = (ObjClosure *)c->obj.next;
            vm->closure_free_list_count--;
            /* Re-link into live objects list */
            c->obj.next = vm->young_objects;
            vm->young_objects = (Obj *)c;
            c->obj.is_marked = false;
            c->obj.is_old = false;
            c->obj.gc_age = 0;
            c->obj.alloc_size = size;
            closure = c;
            vm->young_bytes += size;
            break;
        }
        prev = (ObjClosure **)&c->obj.next;
    }

    if (!closure) {
        closure = (ObjClosure *)allocate_object(vm, size, OBJ_CLOSURE);
    }

    closure->function = function;
    closure->upvalue_count = uv_count;
    for (int i = 0; i < uv_count; i++) {
        closure->upvalues[i] = NULL;
    }
    return closure;
}

ObjUpvalue *new_upvalue(VM *vm, Value *slot) {
    ObjUpvalue *upvalue = NULL;

    /* Try free-list first */
    if (vm->upvalue_free_list) {
        upvalue = vm->upvalue_free_list;
        vm->upvalue_free_list = upvalue->next;
        vm->upvalue_free_list_count--;
        /* Re-link into live objects list */
        upvalue->obj.next = vm->young_objects;
        vm->young_objects = (Obj *)upvalue;
        upvalue->obj.is_marked = false;
        upvalue->obj.is_old = false;
        upvalue->obj.gc_age = 0;
        upvalue->obj.alloc_size = sizeof(ObjUpvalue);
        vm->young_bytes += sizeof(ObjUpvalue);
    } else {
        upvalue = (ObjUpvalue *)allocate_object(vm, sizeof(ObjUpvalue), OBJ_UPVALUE);
    }

    upvalue->location = slot;
    upvalue->closed = NULL_VAL;
    upvalue->next = NULL;
    return upvalue;
}

ObjNative *new_native(VM *vm, NativeFn function, ObjString *name, int arity) {
    ObjNative *native = (ObjNative *)allocate_object(vm, sizeof(ObjNative), OBJ_NATIVE);
    native->function = function;
    native->name = name;
    native->arity = arity;
    native->copy_args = true;
    native->userdata = NULL;
    native->userdata_finalizer = NULL;
    return native;
}

ObjFFI *new_ffi(VM *vm, void *c_func, ObjString *name, int arity) {
    ObjFFI *ffi = (ObjFFI *)allocate_object(vm, sizeof(ObjFFI), OBJ_FFI);
    ffi->c_function = c_func;
    ffi->name = name;
    ffi->arity = arity;
    return ffi;
}

ObjNativeHandle *new_native_handle(VM *vm, ObjString *type_name, void *data,
                                   NativeHandleFinalizer finalizer) {
    ObjNativeHandle *handle = (ObjNativeHandle *)allocate_object(vm, sizeof(ObjNativeHandle),
                                                                OBJ_NATIVE_HANDLE);
    handle->data = data;
    handle->type_name = type_name;
    handle->methods = NULL;
    handle->finalizer = finalizer;
    vm_push(vm, OBJ_VAL(handle));
    handle->methods = new_dict(vm);
    gc_write_barrier(vm, (Obj *)handle, OBJ_VAL(handle->methods));
    vm_pop(vm);
    return handle;
}

/* ========================================================================
 * Arrays
 * ======================================================================== */
ObjArray *new_array(VM *vm) {
    ObjArray *array = (ObjArray *)allocate_object(vm, sizeof(ObjArray), OBJ_ARRAY);
    array->count = 0;
    array->capacity = 0;
    array->items = NULL;
    return array;
}

void array_push(VM *vm, ObjArray *array, Value value) {
    if (!vm || !array) return;
    if (array->count == INT_MAX) {
        fprintf(stderr, "Array is too large.\n");
        exit(1);
    }
    if (array->capacity < array->count + 1) {
        int old_cap = array->capacity;
        if (old_cap > INT_MAX / 2) {
            fprintf(stderr, "Array is too large.\n");
            exit(1);
        }
        int new_cap = array->capacity < 8 ? 8 : array->capacity * 2;
        if ((size_t)new_cap > SIZE_MAX / sizeof(Value)) {
            fprintf(stderr, "Array is too large.\n");
            exit(1);
        }
        Value *items = realloc(array->items, sizeof(Value) * new_cap);
        if (!items) {
            fprintf(stderr, "Out of memory growing array.\n");
            exit(1);
        }
        array->items = items;
        array->capacity = new_cap;
        vm->bytes_allocated += sizeof(Value) * (new_cap - old_cap);
    }
    array->items[array->count++] = value;
    if (((Obj *)array)->is_old && IS_OBJ(value)) gc_write_barrier(vm, (Obj *)array, value);
}

Value array_get(ObjArray *array, int index) {
    if (index < 0 || index >= array->count) return NULL_VAL;
    return array->items[index];
}

void array_set(VM *vm, ObjArray *array, int index, Value value) {
    if (!vm || !array) return;
    if (index < 0 || index >= array->count) return;
    array->items[index] = value;
    gc_write_barrier(vm, (Obj *)array, value);
}

/* ========================================================================
 * Dictionaries
 * ======================================================================== */
ObjDict *new_dict(VM *vm) {
    ObjDict *dict = (ObjDict *)allocate_object(vm, sizeof(ObjDict), OBJ_DICT);
    dict->count = 0;
    dict->capacity = 0;
    dict->version = 1;
    dict->mono_cache_key = NULL;
    dict->mono_cache_entry = NULL;
    dict->mono_cache_value = NULL_VAL;
    dict->indices = NULL;
    dict->entry_count = 0;
    dict->entry_capacity = 0;
    dict->entries = NULL;
    return dict;
}

static bool strings_equal(ObjString *a, ObjString *b) {
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

/* Probe the index table and return the matching entry, or NULL.
   On return, *indices_slot is always set to the index-table position
   where the key belongs (empty, tombstone, or matching slot). */
DictEntry *dict_find_entry(ObjDict *dict, ObjString *key, int *entry_index, int *indices_slot) {
    if (dict->capacity == 0) {
        *entry_index = -1;
        *indices_slot = -1;
        return NULL;
    }
    uint32_t key_hash = key->hash;
    uint32_t idx = key_hash & (dict->capacity - 1);
    int tombstone_slot = -1;
    for (;;) {
        int32_t slot = dict->indices[idx];
        if (slot == 0) {
            *indices_slot = (tombstone_slot >= 0) ? tombstone_slot : (int)idx;
            *entry_index = -1;
            return NULL;
        } else if (slot == TOMBSTONE_IDX) {
            if (tombstone_slot < 0) tombstone_slot = (int)idx;
        } else {
            DictEntry *entry = &dict->entries[slot - 1];
            if (entry->key == key || (entry->hash == key_hash && strings_equal(entry->key, key))) {
                *entry_index = slot - 1;
                *indices_slot = (int)idx;
                return entry;
            }
        }
        idx = (idx + 1) & (dict->capacity - 1);
    }
}

void dict_adjust_capacity(VM *vm, ObjDict *dict, int capacity) {
    size_t old_indices_size = sizeof(int32_t) * dict->capacity;
    size_t old_entries_size = sizeof(DictEntry) * dict->entry_capacity;
    size_t new_indices_size = sizeof(int32_t) * capacity;
    size_t new_entries_size = sizeof(DictEntry) * capacity;

    int32_t *indices = (int32_t *)calloc(capacity, sizeof(int32_t));
    if (!indices) {
        fprintf(stderr, "Out of memory growing dictionary indices.\n");
        exit(1);
    }
    DictEntry *entries = (DictEntry *)malloc(new_entries_size);
    if (!entries) {
        fprintf(stderr, "Out of memory growing dictionary entries.\n");
        exit(1);
    }

    int new_count = 0;
    for (int i = 0; i < dict->entry_count; i++) {
        DictEntry *entry = &dict->entries[i];
        if (entry->key == NULL || entry->key == TOMBSTONE_KEY) continue;
        uint32_t idx = entry->hash & (capacity - 1);
        while (indices[idx] != 0) idx = (idx + 1) & (capacity - 1);
        entries[new_count] = *entry;
        indices[idx] = new_count + 1;
        new_count++;
    }

    free(dict->indices);
    free(dict->entries);
    dict->indices = indices;
    dict->entries = entries;
    dict->capacity = capacity;
    dict->entry_count = new_count;
    dict->entry_capacity = capacity;
    dict->count = new_count;
    dict->version++;
    dict->mono_cache_key = NULL;
    dict->mono_cache_entry = NULL;
    dict->mono_cache_value = NULL_VAL;
    vm->bytes_allocated += (new_indices_size + new_entries_size) -
                           (old_indices_size + old_entries_size);
}

bool dict_get(ObjDict *dict, ObjString *key, Value *value) {
    int entry_index, indices_slot;
    DictEntry *entry = dict_find_entry(dict, key, &entry_index, &indices_slot);
    if (!entry) return false;
    *value = entry->value;
    return true;
}

bool dict_set(VM *vm, ObjDict *dict, ObjString *key, Value value) {
    /* Resize when the active load is too high or the indices table is
       full of active+deleted entries (which would make probing loop). */
    if (dict->count + 1 > dict->capacity * 3 / 4 ||
        dict->entry_count + 1 > dict->capacity) {
        int capacity = dict->capacity < 8 ? 8 : dict->capacity * 2;
        dict_adjust_capacity(vm, dict, capacity);
    }
    int entry_index, indices_slot;
    DictEntry *entry = dict_find_entry(dict, key, &entry_index, &indices_slot);
    bool is_new = (entry == NULL);
    if (is_new) {
        /* Append a new entry. entry_capacity always equals capacity. */
        entry_index = dict->entry_count++;
        dict->indices[indices_slot] = entry_index + 1;
        dict->count++;
        dict->version++;
        entry = &dict->entries[entry_index];
        entry->key = key;
        entry->hash = key->hash;
    }
    entry->value = value;
    /*
     * Cache the dictionary-owned key, not an equal temporary lookup key such
     * as a rope. Cache pointers are weak and are not traced by GC.
     */
    ObjString *stored_key = entry->key;
    dict->mono_cache_key = stored_key;
    dict->mono_cache_entry = entry;
    dict->mono_cache_value = value;
    if (((Obj *)dict)->is_old) {
        if (IS_OBJ(value)) gc_write_barrier(vm, (Obj *)dict, value);
        if (!((Obj *)stored_key)->is_old) {
            gc_write_barrier(vm, (Obj *)dict, OBJ_VAL(stored_key));
        }
    }
    return is_new;
}

bool dict_delete(ObjDict *dict, ObjString *key) {
    if (dict->count == 0) return false;
    int entry_index, indices_slot;
    DictEntry *entry = dict_find_entry(dict, key, &entry_index, &indices_slot);
    if (!entry) return false;
    dict->indices[indices_slot] = TOMBSTONE_IDX;
    entry->key = TOMBSTONE_KEY;
    entry->hash = 0;
    entry->value = TRUE_VAL;
    dict->count--;
    dict->version++;
    dict->mono_cache_key = NULL;
    dict->mono_cache_entry = NULL;
    dict->mono_cache_value = NULL_VAL;
    return true;
}

/* ========================================================================
 * Structs and Instances
 * ======================================================================== */
ObjStruct *new_struct(VM *vm, ObjString *name) {
    ObjStruct *s = (ObjStruct *)allocate_object(vm, sizeof(ObjStruct), OBJ_STRUCT);
    s->name = name;
    s->methods = NULL;
    s->field_names = NULL;
    s->field_count = 0;
    table_init(&s->field_index);
    vm_push(vm, OBJ_VAL(s));
    s->methods = new_dict(vm);
    gc_write_barrier(vm, (Obj *)s, OBJ_VAL(s->methods));
    vm_pop(vm);
    return s;
}

ObjInstance *new_instance(VM *vm, ObjStruct *klass) {
    ObjInstance *inst = (ObjInstance *)allocate_object(vm, sizeof(ObjInstance) + sizeof(Value) * klass->field_count, OBJ_INSTANCE);
    inst->klass = klass;
    for (int i = 0; i < klass->field_count; i++) {
        inst->fields[i] = NULL_VAL;
    }
    return inst;
}

ObjError *new_error(VM *vm, ObjString *kind, ObjString *message,
                    ObjString *file, int line, ObjString *function,
                    ObjString *hint) {
    vm_push(vm, kind ? OBJ_VAL(kind) : NULL_VAL);
    vm_push(vm, message ? OBJ_VAL(message) : NULL_VAL);
    vm_push(vm, file ? OBJ_VAL(file) : NULL_VAL);
    vm_push(vm, function ? OBJ_VAL(function) : NULL_VAL);
    vm_push(vm, hint ? OBJ_VAL(hint) : NULL_VAL);
    ObjError *err = (ObjError *)allocate_object(vm, sizeof(ObjError), OBJ_ERROR);
    err->kind = kind;
    err->message = message;
    err->file = file;
    err->function = function;
    err->hint = hint;
    err->line = line;
    for (int i = 0; i < 5; i++) {
        vm_pop(vm);
    }
    return err;
}

ObjVMTask *new_vm_task(VM *vm, const char *path) {
    ObjVMTask *task = (ObjVMTask *)allocate_object(vm, sizeof(ObjVMTask), OBJ_VM_TASK);
    size_t path_len = strlen(path);
    task->path = (char *)malloc(path_len + 1);
    if (!task->path) {
        fprintf(stderr, "Out of memory allocating VM task path.\n");
        exit(1);
    }
    memcpy(task->path, path, path_len + 1);
    vm->bytes_allocated += path_len + 1;
    task->state = VM_TASK_RUNNING;
    task->result = INTERPRET_RUNTIME_ERROR;
    task->started = false;
    task->joined = false;
    atomic_init(&task->cancel_requested, false);
    task->child_vm = NULL;
    task->error_kind[0] = '\0';
    task->error_message[0] = '\0';
    task->error_hint[0] = '\0';
#ifdef _WIN32
    task->thread = NULL;
    CRITICAL_SECTION *lock = (CRITICAL_SECTION *)malloc(sizeof(CRITICAL_SECTION));
    if (!lock) {
        fprintf(stderr, "Out of memory allocating VM task lock.\n");
        exit(1);
    }
    InitializeCriticalSection(lock);
    task->lock = lock;
#else
    if (pthread_mutex_init(&task->lock, NULL) != 0) {
        fprintf(stderr, "Could not initialize VM task lock.\n");
        exit(1);
    }
#endif
    return task;
}

ObjCoroutine *new_coroutine(VM *vm, ObjClosure *closure) {
    ObjCoroutine *co = (ObjCoroutine *)allocate_object(vm, sizeof(ObjCoroutine), OBJ_COROUTINE);
    co->closure = closure;
    co->state = COROUTINE_NEW;
    co->stack_capacity = 1024;
    co->stack_size = 0;
    co->stack_top = 0;
    co->stack = (Value *)malloc(sizeof(Value) * (size_t)co->stack_capacity);
    if (!co->stack) {
        fprintf(stderr, "Out of memory allocating coroutine stack.\n");
        exit(1);
    }
    for (int i = 0; i < co->stack_capacity; i++) {
        co->stack[i] = NULL_VAL;
    }
    vm->bytes_allocated += sizeof(Value) * (size_t)co->stack_capacity;
    memset(co->frames, 0, sizeof(co->frames));
    co->frame_count = 0;
    co->open_upvalues = NULL;
    co->defer_items = NULL;
    co->defer_count = 0;
    co->defer_capacity = 0;
    co->yield_count = 0;
    for (int i = 0; i < 256; i++) {
        co->yield_values[i] = NULL_VAL;
    }
    return co;
}

/* ========================================================================
 * Hash Table (used for globals, string interning)
 * ======================================================================== */
void table_init(Table *table) {
    table->count = 0;
    table->capacity = 0;
    table->entries = NULL;
}

void table_free(Table *table) {
    free(table->entries);
    table_init(table);
}

static TableEntry *table_find(TableEntry *entries, int capacity, ObjString *key) {
    uint32_t index = key->hash & (capacity - 1);
    uint32_t key_hash = key->hash;
    TableEntry *tombstone = NULL;
    for (;;) {
        TableEntry *entry = &entries[index];
        if (entry->key == NULL) {
            return tombstone != NULL ? tombstone : entry;
        } else if (entry->key == TOMBSTONE_KEY) {
            if (tombstone == NULL) tombstone = entry;
        } else if (entry->hash == key_hash && entry->key == key) {
            return entry;
        }
        index = (index + 1) & (capacity - 1);
    }
}

static void table_adjust(Table *table, int capacity) {
    TableEntry *entries = (TableEntry *)malloc(sizeof(TableEntry) * capacity);
    if (!entries) {
        fprintf(stderr, "Out of memory growing table.\n");
        exit(1);
    }
    for (int i = 0; i < capacity; i++) {
        entries[i].key = NULL;
        entries[i].hash = 0;
        entries[i].value = NULL_VAL;
    }
    table->count = 0;
    for (int i = 0; i < table->capacity; i++) {
        TableEntry *entry = &table->entries[i];
        if (entry->key == NULL || entry->key == TOMBSTONE_KEY) continue;
        TableEntry *dest = table_find(entries, capacity, entry->key);
        dest->key = entry->key;
        dest->hash = entry->hash;
        dest->value = entry->value;
        table->count++;
    }
    free(table->entries);
    table->entries = entries;
    table->capacity = capacity;
}

bool table_get(Table *table, ObjString *key, Value *value) {
    if (table->count == 0) return false;
    TableEntry *entry = table_find(table->entries, table->capacity, key);
    if (entry->key == NULL || entry->key == TOMBSTONE_KEY) return false;
    *value = entry->value;
    return true;
}

bool table_set(Table *table, ObjString *key, Value value) {
    if (table->count + 1 > table->capacity * 3 / 4) {
        int capacity = table->capacity < 8 ? 8 : table->capacity * 2;
        table_adjust(table, capacity);
    }
    TableEntry *entry = table_find(table->entries, table->capacity, key);
    bool is_new = (entry->key == NULL || entry->key == TOMBSTONE_KEY);
    if (is_new && entry->key != TOMBSTONE_KEY) table->count++;
    entry->key = key;
    entry->hash = key->hash;
    entry->value = value;
    return is_new;
}

bool table_delete(Table *table, ObjString *key) {
    if (table->count == 0) return false;
    TableEntry *entry = table_find(table->entries, table->capacity, key);
    if (entry->key == NULL || entry->key == TOMBSTONE_KEY) return false;
    entry->key = TOMBSTONE_KEY;
    entry->hash = 0;
    return true;
}

ObjString *table_find_string(Table *table, const char *chars, int length, uint32_t hash) {
    if (table->count == 0) return NULL;
    uint32_t index = hash & (table->capacity - 1);
    for (;;) {
        TableEntry *entry = &table->entries[index];
        if (entry->key == NULL) {
            return NULL;
        } else if (entry->key == TOMBSTONE_KEY) {
            /* skip tombstones */
        } else if (entry->hash == hash &&
                   !entry->key->is_rope &&
                   entry->key->length == length &&
                   memcmp(entry->key->chars, chars, length) == 0) {
            return entry->key;
        }
        index = (index + 1) & (table->capacity - 1);
    }
}
