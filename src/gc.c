/* ========================================================================
 * Generational Mark-and-Sweep with tri-color marking.
 *
 * Young generation: recently allocated objects on vm->young_objects list.
 *   Minor collections only scan this list + remembered set roots.
 *   Survivors are promoted to the old generation.
 *
 * Old generation: long-lived objects on vm->old_objects list.
 *   Major collections scan both lists (full stop-the-world).
 *   Inline caches are only cleared on major collections.
 *
 * Write barrier: when an old-gen object stores a reference to a young-gen
 *   object, the old object is added to vm->remembered_set so minor GC
 *   can find the young object through the old one.
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "magnesium.h"

/* ========================================================================
 * Gray Stack (for tri-color marking)
 * ======================================================================== */
static void push_gray(VM *vm, Obj *obj) {
    if (vm->gray_count >= vm->gray_capacity) {
        vm->gray_capacity = vm->gray_capacity < 8 ? 8 : vm->gray_capacity * 2;
        vm->gray_stack = realloc(vm->gray_stack, sizeof(Obj *) * vm->gray_capacity);
        if (!vm->gray_stack) {
            fprintf(stderr, "GC: out of memory for gray stack.\n");
            exit(1);
        }
    }
    vm->gray_stack[vm->gray_count++] = obj;
}

/* ========================================================================
 * Mark Phase
 * ======================================================================== */
void gc_mark_object(VM *vm, Obj *object) {
    if (!object) return;
    if (vm->minor_gc_active && object->is_old) return;
    if (object->is_marked) return;
    object->is_marked = true;
    push_gray(vm, object);
}

void gc_mark_value(VM *vm, Value value) {
    if (IS_OBJ(value)) gc_mark_object(vm, AS_OBJ(value));
}

static void mark_table(VM *vm, Table *table) {
    for (int i = 0; i < table->capacity; i++) {
        TableEntry *entry = &table->entries[i];
        if (entry->key != NULL && entry->key != TOMBSTONE_KEY) {
            gc_mark_object(vm, (Obj *)entry->key);
            gc_mark_value(vm, entry->value);
        }
    }
}

static void table_remove_unmarked_strings(Table *table, bool minor) {
    for (int i = 0; i < table->capacity; i++) {
        TableEntry *entry = &table->entries[i];
        if (entry->key == NULL || entry->key == TOMBSTONE_KEY) continue;
        Obj *string = (Obj *)entry->key;
        bool collectable = !minor || (!string->is_old && string->gc_age != 0);
        if (collectable && !string->is_marked) {
            table_delete(table, entry->key);
        }
    }
}

static void blacken_object(VM *vm, Obj *object);

static void mark_roots(VM *vm) {
    for (VMRoot *root = vm->host_roots; root; root = root->next) {
        gc_mark_value(vm, root->value);
    }
    for (NativeArgRoot *root = vm->native_arg_roots; root; root = root->previous) {
        for (int i = 0; i < root->count; i++) {
            gc_mark_value(vm, root->values[i]);
        }
    }

    int top = vm->stack_top > vm->stack_capacity ? vm->stack_capacity : vm->stack_top;
    for (int i = 0; i < top; i++) {
        gc_mark_value(vm, vm->stack[i]);
    }

    for (int i = 0; i < vm->frame_count; i++) {
        gc_mark_object(vm, (Obj *)vm->frames[i].closure);
    }

    for (ObjUpvalue *upvalue = vm->open_upvalues; upvalue; upvalue = upvalue->next) {
        gc_mark_object(vm, (Obj *)upvalue);
    }

    if (vm->current_coroutine) {
        gc_mark_object(vm, (Obj *)vm->current_coroutine);
    }
    for (int i = 0; i < vm->yield_count; i++) {
        gc_mark_value(vm, vm->yield_values[i]);
    }
    for (int i = 0; i < vm->native_return_count; i++) {
        gc_mark_value(vm, vm->native_return_values[i]);
    }

    for (SavedVMContext *ctx = vm->saved_contexts; ctx; ctx = ctx->next) {
        int saved_top = ctx->stack_top > ctx->stack_capacity ? ctx->stack_capacity : ctx->stack_top;
        for (int i = 0; i < saved_top; i++) {
            gc_mark_value(vm, ctx->stack[i]);
        }
        for (int i = 0; i < ctx->frame_count; i++) {
            gc_mark_object(vm, (Obj *)ctx->frames[i].closure);
        }
        for (ObjUpvalue *upvalue = ctx->open_upvalues; upvalue; upvalue = upvalue->next) {
            gc_mark_object(vm, (Obj *)upvalue);
        }
        for (int i = 0; i < ctx->defer_count; i++) {
            gc_mark_object(vm, (Obj *)ctx->defer_items[i]);
        }
        if (ctx->current_coroutine) {
            gc_mark_object(vm, (Obj *)ctx->current_coroutine);
        }
        for (int i = 0; i < ctx->yield_count; i++) {
            gc_mark_value(vm, ctx->yield_values[i]);
        }
        for (int i = 0; i < ctx->native_return_count; i++) {
            gc_mark_value(vm, ctx->native_return_values[i]);
        }
    }

    mark_table(vm, &vm->globals);
    mark_table(vm, &vm->modules);
    for (int i = 0; i < vm->int_str_cache_used_count; i++) {
        uint32_t index = vm->int_str_cache_used[i];
        gc_mark_object(vm, (Obj *)vm->int_str_cache[index]);
    }

    for (int i = 0; i < vm->defer_stack.count; i++) {
        gc_mark_object(vm, (Obj *)vm->defer_stack.items[i]);
    }

    for (int i = 0; i < vm->task_queue.count; i++) {
        gc_mark_object(vm, (Obj *)vm->task_queue.items[i]);
    }

    for (int i = 0; i < vm->vm_tasks.count; i++) {
        gc_mark_object(vm, (Obj *)vm->vm_tasks.items[i]);
    }
}

static void mark_roots_minor(VM *vm) {
    for (VMRoot *root = vm->host_roots; root; root = root->next) {
        gc_mark_value(vm, root->value);
    }
    for (NativeArgRoot *root = vm->native_arg_roots; root; root = root->previous) {
        for (int i = 0; i < root->count; i++) {
            gc_mark_value(vm, root->values[i]);
        }
    }

    int top = vm->stack_top > vm->stack_capacity ? vm->stack_capacity : vm->stack_top;
    for (int i = 0; i < top; i++) {
        gc_mark_value(vm, vm->stack[i]);
    }

    for (int i = 0; i < vm->frame_count; i++) {
        gc_mark_object(vm, (Obj *)vm->frames[i].closure);
    }

    for (ObjUpvalue *upvalue = vm->open_upvalues; upvalue; upvalue = upvalue->next) {
        gc_mark_object(vm, (Obj *)upvalue);
    }

    if (vm->current_coroutine) {
        gc_mark_object(vm, (Obj *)vm->current_coroutine);
    }
    for (int i = 0; i < vm->yield_count; i++) {
        gc_mark_value(vm, vm->yield_values[i]);
    }
    for (int i = 0; i < vm->native_return_count; i++) {
        gc_mark_value(vm, vm->native_return_values[i]);
    }

    for (SavedVMContext *ctx = vm->saved_contexts; ctx; ctx = ctx->next) {
        int saved_top = ctx->stack_top > ctx->stack_capacity ? ctx->stack_capacity : ctx->stack_top;
        for (int i = 0; i < saved_top; i++) {
            gc_mark_value(vm, ctx->stack[i]);
        }
        for (int i = 0; i < ctx->frame_count; i++) {
            gc_mark_object(vm, (Obj *)ctx->frames[i].closure);
        }
        for (ObjUpvalue *upvalue = ctx->open_upvalues; upvalue; upvalue = upvalue->next) {
            gc_mark_object(vm, (Obj *)upvalue);
        }
        for (int i = 0; i < ctx->defer_count; i++) {
            gc_mark_object(vm, (Obj *)ctx->defer_items[i]);
        }
        if (ctx->current_coroutine) {
            gc_mark_object(vm, (Obj *)ctx->current_coroutine);
        }
        for (int i = 0; i < ctx->yield_count; i++) {
            gc_mark_value(vm, ctx->yield_values[i]);
        }
        for (int i = 0; i < ctx->native_return_count; i++) {
            gc_mark_value(vm, ctx->native_return_values[i]);
        }
    }

    for (int i = 0; i < vm->defer_stack.count; i++) {
        gc_mark_object(vm, (Obj *)vm->defer_stack.items[i]);
    }

    for (int i = 0; i < vm->task_queue.count; i++) {
        gc_mark_object(vm, (Obj *)vm->task_queue.items[i]);
    }

    for (int i = 0; i < vm->vm_tasks.count; i++) {
        gc_mark_object(vm, (Obj *)vm->vm_tasks.items[i]);
    }

    mark_table(vm, &vm->globals);
    mark_table(vm, &vm->modules);
    for (int i = 0; i < vm->int_str_cache_used_count; i++) {
        uint32_t index = vm->int_str_cache_used[i];
        gc_mark_object(vm, (Obj *)vm->int_str_cache[index]);
    }

    for (int i = 0; i < vm->remembered_count; i++) {
        blacken_object(vm, vm->remembered_set[i]);
    }
}

static void protect_newborn_young(VM *vm) {
    for (Obj *object = vm->young_objects; object; object = object->next) {
        if (object->gc_age == 0) {
            blacken_object(vm, object);
        }
    }
}

static void mark_newborn_young(VM *vm) {
    for (Obj *object = vm->young_objects; object; object = object->next) {
        if (object->gc_age == 0) {
            gc_mark_object(vm, object);
        }
    }
}

static void blacken_object(VM *vm, Obj *object) {
    switch (object->type) {
        case OBJ_STRING:
            if (((ObjString *)object)->is_rope) {
                gc_mark_object(vm, (Obj *)((ObjString *)object)->left);
                gc_mark_object(vm, (Obj *)((ObjString *)object)->right);
            }
            break;

        case OBJ_UPVALUE:
            gc_mark_value(vm, ((ObjUpvalue *)object)->closed);
            break;

        case OBJ_FUNCTION: {
            ObjFunction *fn = (ObjFunction *)object;
            gc_mark_object(vm, (Obj *)fn->name);
            gc_mark_object(vm, (Obj *)fn->source_name);
            for (int i = 0; i < fn->chunk.const_count; i++) {
                gc_mark_value(vm, fn->chunk.constants[i]);
            }
            break;
        }

        case OBJ_CLOSURE: {
            ObjClosure *closure = (ObjClosure *)object;
            gc_mark_object(vm, (Obj *)closure->function);
            for (int i = 0; i < closure->upvalue_count; i++) {
                gc_mark_object(vm, (Obj *)closure->upvalues[i]);
            }
            break;
        }

        case OBJ_NATIVE:
            gc_mark_object(vm, (Obj *)((ObjNative *)object)->name);
            break;

        case OBJ_FFI:
            gc_mark_object(vm, (Obj *)((ObjFFI *)object)->name);
            break;

        case OBJ_NATIVE_HANDLE: {
            ObjNativeHandle *handle = (ObjNativeHandle *)object;
            gc_mark_object(vm, (Obj *)handle->type_name);
            gc_mark_object(vm, (Obj *)handle->methods);
            break;
        }

        case OBJ_ARRAY: {
            ObjArray *array = (ObjArray *)object;
            for (int i = 0; i < array->count; i++) {
                gc_mark_value(vm, array->items[i]);
            }
            break;
        }

        case OBJ_DICT: {
            ObjDict *dict = (ObjDict *)object;
            for (int i = 0; i < dict->entry_count; i++) {
                if (dict->entries[i].key != NULL && dict->entries[i].key != TOMBSTONE_KEY) {
                    gc_mark_object(vm, (Obj *)dict->entries[i].key);
                    gc_mark_value(vm, dict->entries[i].value);
                }
            }
            break;
        }

        case OBJ_STRUCT: {
            ObjStruct *s = (ObjStruct *)object;
            gc_mark_object(vm, (Obj *)s->name);
            gc_mark_object(vm, (Obj *)s->methods);
            for (int i = 0; i < s->field_count; i++) {
                gc_mark_object(vm, (Obj *)s->field_names[i]);
            }
            mark_table(vm, &s->field_index);
            break;
        }

        case OBJ_INSTANCE: {
            ObjInstance *inst = (ObjInstance *)object;
            gc_mark_object(vm, (Obj *)inst->klass);
            for (int i = 0; i < inst->klass->field_count; i++) {
                gc_mark_value(vm, inst->fields[i]);
            }
            break;
        }

        case OBJ_ERROR: {
            ObjError *err = (ObjError *)object;
            gc_mark_object(vm, (Obj *)err->kind);
            gc_mark_object(vm, (Obj *)err->message);
            gc_mark_object(vm, (Obj *)err->file);
            gc_mark_object(vm, (Obj *)err->function);
            gc_mark_object(vm, (Obj *)err->hint);
            break;
        }

        case OBJ_VM_TASK:
            break;

        case OBJ_COROUTINE: {
            ObjCoroutine *co = (ObjCoroutine *)object;
            gc_mark_object(vm, (Obj *)co->closure);
            int top = co->stack_top > co->stack_capacity ? co->stack_capacity : co->stack_top;
            for (int i = 0; i < top; i++) {
                gc_mark_value(vm, co->stack[i]);
            }
            for (int i = 0; i < co->frame_count; i++) {
                gc_mark_object(vm, (Obj *)co->frames[i].closure);
            }
            for (ObjUpvalue *upvalue = co->open_upvalues; upvalue; upvalue = upvalue->next) {
                gc_mark_object(vm, (Obj *)upvalue);
            }
            for (int i = 0; i < co->defer_count; i++) {
                gc_mark_object(vm, (Obj *)co->defer_items[i]);
            }
            for (int i = 0; i < co->yield_count; i++) {
                gc_mark_value(vm, co->yield_values[i]);
            }
            break;
        }
    }
}

static void trace_references(VM *vm) {
    while (vm->gray_count > 0) {
        Obj *object = vm->gray_stack[--vm->gray_count];
        blacken_object(vm, object);
    }
}

/* ========================================================================
 * Sweep Phase
 * ======================================================================== */
static size_t free_object(VM *vm, Obj *object) {
    size_t freed = 0;
    switch (object->type) {
        case OBJ_STRING: {
            ObjString *string = (ObjString *)object;
            if (!string->is_rope) {
                freed += (size_t)string->length + 1;
                free(string->chars);
            }
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction *fn = (ObjFunction *)object;
            chunk_free(&fn->chunk);
            break;
        }
        case OBJ_CLOSURE: {
            break;
        }
        case OBJ_UPVALUE:
            break;
        case OBJ_NATIVE: {
            ObjNative *native = (ObjNative *)object;
            if (native->userdata_finalizer && native->userdata)
                native->userdata_finalizer(native->userdata);
            break;
        }
        case OBJ_FFI:
            break;
        case OBJ_NATIVE_HANDLE: {
            ObjNativeHandle *handle = (ObjNativeHandle *)object;
            if (handle->finalizer && handle->data) {
                handle->finalizer(handle->data);
            }
            break;
        }
        case OBJ_ARRAY: {
            ObjArray *array = (ObjArray *)object;
            freed += sizeof(Value) * (size_t)array->capacity;
            free(array->items);
            break;
        }
        case OBJ_DICT: {
            ObjDict *dict = (ObjDict *)object;
            freed += sizeof(DictEntry) * (size_t)dict->entry_capacity;
            free(dict->entries);
            freed += sizeof(int32_t) * (size_t)dict->capacity;
            free(dict->indices);
            break;
        }
        case OBJ_STRUCT: {
            ObjStruct *s = (ObjStruct *)object;
            freed += sizeof(ObjString *) * (size_t)s->field_count;
            free(s->field_names);
            table_free(&s->field_index);
            break;
        }
        case OBJ_INSTANCE: {
            break;
        }
        case OBJ_ERROR:
            break;
        case OBJ_VM_TASK: {
            ObjVMTask *task = (ObjVMTask *)object;
            if (task->path) freed += strlen(task->path) + 1;
            vm_task_destroy(task);
            break;
        }
        case OBJ_COROUTINE: {
            ObjCoroutine *co = (ObjCoroutine *)object;
            freed += sizeof(Value) * (size_t)co->stack_capacity;
            freed += sizeof(ObjClosure *) * (size_t)co->defer_capacity;
            free(co->stack);
            free(co->defer_items);
            break;
        }
    }

    freed += object->alloc_size;
    if (object->size_class != 0) {
        /* Return small object to its slab free list instead of freeing. */
        uint8_t class_idx = object->size_class - 1;
        object->next = vm->slab_free_lists[class_idx];
        vm->slab_free_lists[class_idx] = object;
    } else {
        free(object);
    }
    return freed;
}

#define FREE_LIST_MAX 256

static void sweep(VM *vm) {
    Obj *previous = NULL;
    Obj *object = vm->old_objects;
    while (object) {
        if (object->is_marked) {
            object->is_marked = false;
            previous = object;
            object = object->next;
        } else {
            Obj *unreached = object;
            object = object->next;
            if (previous) {
                previous->next = object;
            } else {
                vm->old_objects = object;
            }
            if (unreached->type == OBJ_CLOSURE && vm->closure_free_list_count < FREE_LIST_MAX) {
                unreached->next = (Obj *)vm->closure_free_list;
                vm->closure_free_list = (ObjClosure *)unreached;
                vm->closure_free_list_count++;
            } else if (unreached->type == OBJ_UPVALUE && vm->upvalue_free_list_count < FREE_LIST_MAX) {
                ObjUpvalue *uv = (ObjUpvalue *)unreached;
                uv->next = vm->upvalue_free_list;
                vm->upvalue_free_list = uv;
                vm->upvalue_free_list_count++;
            } else {
                vm->bytes_allocated -= free_object(vm, unreached);
            }
        }
    }
}

/* ========================================================================
 * Remembered Set
 * ======================================================================== */
void remembered_set_add(VM *vm, Obj *old_obj) {
    for (int i = 0; i < vm->remembered_count; i++) {
        if (vm->remembered_set[i] == old_obj) return;
    }
    if (vm->remembered_count >= vm->remembered_capacity) {
        vm->remembered_capacity = vm->remembered_capacity < 16 ? 16 : vm->remembered_capacity * 2;
        vm->remembered_set = realloc(vm->remembered_set, sizeof(Obj *) * vm->remembered_capacity);
        if (!vm->remembered_set) {
            fprintf(stderr, "GC: out of memory for remembered set.\n");
            exit(1);
        }
    }
    vm->remembered_set[vm->remembered_count++] = old_obj;
}

/* ========================================================================
 * Public API: Minor Collection
 * ======================================================================== */
void gc_minor_collect(VM *vm) {
    vm->minor_gc_active = true;

    /* Inline caches are weak accelerators. They must never keep stale
       addresses to young objects across a collection. */
    memset(vm->global_ic, 0, sizeof(vm->global_ic));
    memset(vm->field_ic, 0, sizeof(vm->field_ic));
    memset(vm->method_ic, 0, sizeof(vm->method_ic));
    memset(vm->dict_ic, 0, sizeof(vm->dict_ic));

    protect_newborn_young(vm);
    trace_references(vm);
    mark_roots_minor(vm);
    trace_references(vm);
    table_remove_unmarked_strings(&vm->strings, true);

    Obj *previous = NULL;
    Obj *object = vm->young_objects;
    size_t remaining_young_bytes = 0;
    while (object) {
        if (object->is_marked) {
            object->is_marked = false;
            Obj *next = object->next;
            if (previous) {
                previous->next = next;
            } else {
                vm->young_objects = next;
            }
            object->is_old = true;
            object->gc_age = 0;
            object->next = vm->old_objects;
            vm->old_objects = object;
            object = next;
        } else if (object->gc_age == 0) {
            object->gc_age = 1;
            remaining_young_bytes += object->alloc_size;
            previous = object;
            object = object->next;
        } else {
            Obj *unreached = object;
            object = object->next;
            if (previous) {
                previous->next = object;
            } else {
                vm->young_objects = object;
            }
            if (unreached->type == OBJ_CLOSURE && vm->closure_free_list_count < FREE_LIST_MAX) {
                unreached->next = (Obj *)vm->closure_free_list;
                vm->closure_free_list = (ObjClosure *)unreached;
                vm->closure_free_list_count++;
            } else if (unreached->type == OBJ_UPVALUE && vm->upvalue_free_list_count < FREE_LIST_MAX) {
                ObjUpvalue *uv = (ObjUpvalue *)unreached;
                uv->next = vm->upvalue_free_list;
                vm->upvalue_free_list = uv;
                vm->upvalue_free_list_count++;
            } else {
                vm->bytes_allocated -= free_object(vm, unreached);
            }
        }
    }

    vm->young_bytes = remaining_young_bytes;

    vm->remembered_count = 0;
    vm->strings_interned_since_minor = 0;
    vm->minor_gc_active = false;
}

/* ========================================================================
 * Public API: Major Collection
 * ======================================================================== */
void gc_major_collect(VM *vm) {
    vm->minor_gc_active = false;
    memset(vm->global_ic, 0, sizeof(vm->global_ic));
    memset(vm->field_ic, 0, sizeof(vm->field_ic));
    memset(vm->method_ic, 0, sizeof(vm->method_ic));
    memset(vm->dict_ic, 0, sizeof(vm->dict_ic));

    mark_newborn_young(vm);

    while (vm->young_objects) {
        Obj *obj = vm->young_objects;
        vm->young_objects = obj->next;
        obj->next = vm->old_objects;
        vm->old_objects = obj;
        obj->is_old = true;
        obj->gc_age = 0;
    }
    vm->young_bytes = 0;

    mark_roots(vm);
    trace_references(vm);
    table_remove_unmarked_strings(&vm->strings, false);
    sweep(vm);
    vm->remembered_count = 0;
    vm->next_gc = vm->bytes_allocated * GC_HEAP_GROW_FACTOR;
}

/* ========================================================================
 * Backward-compatible alias
 * ======================================================================== */
void gc_collect(VM *vm) {
    gc_major_collect(vm);
}

/* Free ALL objects (called at VM shutdown) */
void gc_free_all(VM *vm) {
    Obj *obj = vm->young_objects;
    while (obj) {
        Obj *next = obj->next;
        free_object(vm, obj);
        obj = next;
    }
    vm->young_objects = NULL;

    obj = vm->old_objects;
    while (obj) {
        Obj *next = obj->next;
        free_object(vm, obj);
        obj = next;
    }
    vm->old_objects = NULL;

    /* Closure/upvalue free-list objects are either slab-allocated (handled
       when the slabs are freed below) or, rarely, malloc-allocated for very
       large closure sizes. Free any malloc-allocated ones individually. */
    while (vm->closure_free_list) {
        ObjClosure *next = (ObjClosure *)vm->closure_free_list->obj.next;
        if (vm->closure_free_list->obj.size_class == 0) {
            free(vm->closure_free_list);
        }
        vm->closure_free_list = next;
    }
    while (vm->upvalue_free_list) {
        ObjUpvalue *next = vm->upvalue_free_list->next;
        if (vm->upvalue_free_list->obj.size_class == 0) {
            free(vm->upvalue_free_list);
        }
        vm->upvalue_free_list = next;
    }

    /* Free every backing slab block. Individual slab chunks are already
       accounted for through the free lists or live object lists above. */
    Slab *slab = vm->slab_blocks;
    while (slab) {
        Slab *next = slab->next;
        free(slab->memory);
        free(slab);
        slab = next;
    }
    vm->slab_blocks = NULL;
    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        vm->slab_free_lists[i] = NULL;
    }

    free(vm->remembered_set);
    vm->remembered_set = NULL;
    vm->remembered_count = 0;
    vm->remembered_capacity = 0;
}
