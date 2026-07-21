#pragma once

#include <mg/value.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace mg {

class Vm;

class MgString {
public:
    explicit MgString(::ObjString* r) : raw(r) {}
    static MgString from_raw(::ObjString* r) { return MgString(r); }

    ::ObjString* raw;

    std::optional<std::string_view> as_str() const {
        if (!raw || raw->is_rope) return std::nullopt;
        return std::string_view(raw->chars, static_cast<size_t>(raw->length));
    }

    std::optional<std::string> resolve(Vm& vm) const;

    int32_t length() const { return raw ? raw->length : 0; }
    uint32_t hash() const { return raw ? raw->hash : 0; }
    bool is_rope() const { return raw ? raw->is_rope : false; }

    std::optional<char> char_at(int32_t index) const {
        if (!raw || index < 0 || index >= raw->length) return std::nullopt;
        if (raw->is_rope) return std::nullopt;
        return raw->chars[index];
    }
};

} // namespace mg
