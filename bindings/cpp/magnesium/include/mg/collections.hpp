#pragma once

#include <mg/string.hpp>
#include <mg/vm.hpp>
#include <optional>

namespace mg {

class MgArray {
public:
    explicit MgArray(::ObjArray* r) : raw(r) {}
    static MgArray from_raw(::ObjArray* r) { return MgArray(r); }

    ::ObjArray* raw;

    int32_t len() const { return raw ? raw->count : 0; }
    bool is_empty() const { return len() == 0; }
    int32_t capacity() const { return raw ? raw->capacity : 0; }

    std::optional<Value> get(int32_t index) const {
        if (!raw || index < 0 || index >= raw->count) return std::nullopt;
        return Value::from_raw(raw->items[index]);
    }

    bool set(Vm& vm, int32_t index, Value v) {
        if (!raw || index < 0 || index >= raw->count) return false;
        array_set(vm.raw_mut(), raw, index, v.to_raw());
        return true;
    }

    void push(Vm& vm, Value v) {
        if (raw) array_push(vm.raw_mut(), raw, v.to_raw());
    }
};

class MgDict {
public:
    explicit MgDict(::ObjDict* r) : raw(r) {}
    static MgDict from_raw(::ObjDict* r) { return MgDict(r); }

    ::ObjDict* raw;

    int32_t len() const { return raw ? raw->count : 0; }
    bool is_empty() const { return len() == 0; }
    int32_t capacity() const { return raw ? raw->capacity : 0; }
    uint32_t version() const { return raw ? raw->version : 0; }

    std::optional<Value> get(const MgString& key) const {
        if (!raw || !key.raw) return std::nullopt;
        ::Value out;
        if (dict_get(raw, key.raw, &out)) return Value::from_raw(out);
        return std::nullopt;
    }

    bool set(Vm& vm, const MgString& key, Value v) {
        if (!raw || !key.raw) return false;
        dict_set(vm.raw_mut(), raw, key.raw, v.to_raw());
        return true;
    }

    bool del(const MgString& key) {
        if (!raw || !key.raw) return false;
        return dict_delete(raw, key.raw);
    }
};

} // namespace mg
