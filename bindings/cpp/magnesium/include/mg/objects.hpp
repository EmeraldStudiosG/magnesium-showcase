#pragma once

#include <mg/value.hpp>
#include <optional>
#include <string>

namespace mg {

class MgFunction {
public:
    explicit MgFunction(::ObjFunction* r) : raw(r) {}
    static MgFunction from_raw(::ObjFunction* r) { return MgFunction(r); }

    ::ObjFunction* raw;

    int32_t arity() const { return raw ? raw->arity : 0; }
    int32_t upvalue_count() const { return raw ? raw->upvalue_count : 0; }
    int32_t reg_count() const { return raw ? raw->reg_count : 0; }

    std::string name() const {
        if (!raw || !raw->name) return "<unnamed>";
        return std::string(raw->name->chars, raw->name->length);
    }
};

class MgClosure {
public:
    explicit MgClosure(::ObjClosure* r) : raw(r) {}
    static MgClosure from_raw(::ObjClosure* r) { return MgClosure(r); }

    ::ObjClosure* raw;

    MgFunction function() const { return MgFunction(raw ? raw->function : nullptr); }
    int32_t upvalue_count() const { return raw ? raw->upvalue_count : 0; }

    ::ObjUpvalue* upvalue(size_t index) const {
        if (!raw || index >= static_cast<size_t>(raw->upvalue_count)) return nullptr;
        return raw->upvalues[index];
    }
};

class MgStruct {
public:
    explicit MgStruct(::ObjStruct* r) : raw(r) {}
    static MgStruct from_raw(::ObjStruct* r) { return MgStruct(r); }

    ::ObjStruct* raw;

    std::string name() const {
        if (!raw || !raw->name) return "";
        return std::string(raw->name->chars, raw->name->length);
    }
    int32_t field_count() const { return raw ? raw->field_count : 0; }
    ::ObjDict* methods() const { return raw ? raw->methods : nullptr; }
};

class MgInstance {
public:
    explicit MgInstance(::ObjInstance* r) : raw(r) {}
    static MgInstance from_raw(::ObjInstance* r) { return MgInstance(r); }

    ::ObjInstance* raw;

    MgStruct klass() const { return MgStruct(raw ? raw->klass : nullptr); }

    Value field(size_t index) const {
        if (!raw) return Value::null();
        auto* fields = reinterpret_cast<const ::Value*>(raw + 1);
        return Value::from_raw(fields[index]);
    }

    void set_field(size_t index, Value v) {
        if (!raw) return;
        auto* fields = reinterpret_cast<::Value*>(raw + 1);
        fields[index] = v.to_raw();
    }
};

class MgCoroutine {
public:
    explicit MgCoroutine(::ObjCoroutine* r) : raw(r) {}
    static MgCoroutine from_raw(::ObjCoroutine* r) { return MgCoroutine(r); }

    ::ObjCoroutine* raw;

    CoroutineState state() const { return raw ? raw->state : COROUTINE_DEAD; }
    MgClosure closure() const { return MgClosure(raw ? raw->closure : nullptr); }
    int32_t frame_count() const { return raw ? raw->frame_count : 0; }
};

class MgFfi {
public:
    explicit MgFfi(::ObjFFI* r) : raw(r) {}
    static MgFfi from_raw(::ObjFFI* r) { return MgFfi(r); }

    ::ObjFFI* raw;

    std::string name() const { return raw && raw->name ? std::string(raw->name) : ""; }
    int32_t arity() const { return raw ? raw->arity : 0; }
};

} // namespace mg
