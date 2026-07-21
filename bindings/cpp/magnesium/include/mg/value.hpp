#pragma once

#include <cstdint>
#include <cstring>
#include <cmath>
#include <optional>
#include <string>
#include <stdexcept>

extern "C" {
#include "magnesium.h"
}

namespace mg {

class Value {
public:
    constexpr Value() : bits_(QNAN | TAG_NULL_VAL) {}
    constexpr explicit Value(uint64_t bits) : bits_(bits) {}

    static constexpr Value null() { return Value(QNAN | TAG_NULL_VAL); }
    static constexpr Value false_val() { return Value(QNAN | TAG_FALSE_VAL); }
    static constexpr Value true_val() { return Value(QNAN | TAG_TRUE_VAL); }
    static constexpr Value bool_val(bool b) { return b ? true_val() : false_val(); }
    static constexpr Value int_val(int32_t n) {
        return Value(QNAN | TAG_INT_VAL | (static_cast<uint64_t>(static_cast<uint32_t>(n)) << 3));
    }
    static Value number_val(double n) {
        uint64_t bits;
        std::memcpy(&bits, &n, sizeof(bits));
        if ((bits & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT)) {
            bits = 0;
            std::memcpy(&n, &bits, sizeof(n));
            std::memcpy(&bits, &n, sizeof(bits));
        }
        return Value(bits);
    }
    static Value obj_val(const void* ptr) {
        uint64_t ptr_bits = reinterpret_cast<uint64_t>(ptr);
        return Value(SIGN_BIT | QNAN | ptr_bits);
    }
    static Value auto_val(double n) {
        double int_part;
        if (std::modf(n, &int_part) == 0.0 && n >= INT32_MIN && n <= INT32_MAX) {
            auto i = static_cast<int32_t>(n);
            if (static_cast<double>(i) == n) return int_val(i);
        }
        return number_val(n);
    }
    static constexpr Value from_bits(uint64_t bits) { return Value(bits); }
    static Value from_raw(::Value raw) { return Value(static_cast<uint64_t>(raw)); }

    constexpr uint64_t to_bits() const { return bits_; }
    ::Value to_raw() const { return static_cast<::Value>(bits_); }

    constexpr bool is_null() const { return bits_ == (QNAN | TAG_NULL_VAL); }
    constexpr bool is_bool() const { return bits_ == (QNAN | TAG_FALSE_VAL) || bits_ == (QNAN | TAG_TRUE_VAL); }
    constexpr bool is_int() const { return (bits_ & (QNAN | 0x7)) == (QNAN | TAG_INT_VAL); }
    bool is_number() const {
        if ((bits_ & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT)) return false;
        double d;
        uint64_t b = bits_;
        std::memcpy(&d, &b, sizeof(d));
        return !std::isnan(d);
    }
    constexpr bool is_numeric() const { return is_int() || is_number(); }
    constexpr bool is_obj() const {
        return (bits_ & (SIGN_BIT | QNAN)) == (SIGN_BIT | QNAN) && (bits_ & 0x7) == 0;
    }

    bool is_string() const { return is_obj() && as_obj()->type == OBJ_STRING; }
    bool is_array() const { return is_obj() && as_obj()->type == OBJ_ARRAY; }
    bool is_dict() const { return is_obj() && as_obj()->type == OBJ_DICT; }
    bool is_function() const { return is_obj() && as_obj()->type == OBJ_FUNCTION; }
    bool is_closure() const { return is_obj() && as_obj()->type == OBJ_CLOSURE; }
    bool is_native() const { return is_obj() && as_obj()->type == OBJ_NATIVE; }
    bool is_ffi() const { return is_obj() && as_obj()->type == OBJ_FFI; }
    bool is_native_handle() const { return is_obj() && as_obj()->type == OBJ_NATIVE_HANDLE; }
    bool is_struct() const { return is_obj() && as_obj()->type == OBJ_STRUCT; }
    bool is_instance() const { return is_obj() && as_obj()->type == OBJ_INSTANCE; }
    bool is_error() const { return is_obj() && as_obj()->type == OBJ_ERROR; }

    bool is_callable() const {
        if (!is_obj()) return false;
        auto t = as_obj()->type;
        return t == OBJ_FUNCTION || t == OBJ_CLOSURE ||
               t == OBJ_NATIVE || t == OBJ_FFI || t == OBJ_STRUCT;
    }

    constexpr bool is_falsey() const { return is_null() || (bits_ == (QNAN | TAG_FALSE_VAL)); }

    bool as_bool() const { return bits_ == (QNAN | TAG_TRUE_VAL); }
    int32_t as_int() const { return static_cast<int32_t>((bits_ & ~((QNAN | 0x7))) >> 3); }
    double as_number() const {
        double d;
        uint64_t b = bits_;
        std::memcpy(&d, &b, sizeof(d));
        return d;
    }
    double as_double() const { return as_number(); }

    double as_numeric() const {
        if (is_int()) return static_cast<double>(as_int());
        return as_number();
    }

    ::Obj* as_obj() const { return reinterpret_cast<::Obj*>(bits_ & ~(SIGN_BIT | QNAN)); }
    ::ObjNativeHandle* as_native_handle() const { return reinterpret_cast<::ObjNativeHandle*>(as_obj()); }

    std::optional<ObjType> obj_type() const {
        if (!is_obj()) return std::nullopt;
        return as_obj()->type;
    }

    std::optional<bool> try_as_bool() const { return is_bool() ? std::optional<bool>(as_bool()) : std::nullopt; }
    std::optional<int32_t> try_as_int() const { return is_int() ? std::optional<int32_t>(as_int()) : std::nullopt; }
    std::optional<double> try_as_number() const { return is_number() ? std::optional<double>(as_number()) : std::nullopt; }
    std::optional<double> try_as_numeric() const { return is_numeric() ? std::optional<double>(as_numeric()) : std::nullopt; }
    std::optional<::Obj*> try_as_obj() const { return is_obj() ? std::optional<::Obj*>(as_obj()) : std::nullopt; }

    std::optional<std::string_view> try_as_string() const {
        if (!is_string()) return std::nullopt;
        auto* s = reinterpret_cast<::ObjString*>(as_obj());
        if (s->is_rope) return std::nullopt;
        return std::string_view(s->chars, static_cast<size_t>(s->length));
    }

    bool expect_bool() const { if (!is_bool()) throw std::runtime_error("value is not a bool"); return as_bool(); }
    int32_t expect_int() const { if (!is_int()) throw std::runtime_error("value is not an int"); return as_int(); }
    double expect_number() const { if (!is_number()) throw std::runtime_error("value is not a number"); return as_number(); }
    double expect_numeric() const { if (!is_numeric()) throw std::runtime_error("value is not numeric"); return as_numeric(); }

    constexpr bool operator==(const Value& other) const { return bits_ == other.bits_; }
    constexpr bool operator!=(const Value& other) const { return bits_ != other.bits_; }

private:
    static constexpr uint64_t TAG_NULL_VAL  = 1;
    static constexpr uint64_t TAG_FALSE_VAL = 2;
    static constexpr uint64_t TAG_TRUE_VAL  = 3;
    static constexpr uint64_t TAG_INT_VAL   = 4;

    uint64_t bits_;
};

static_assert(sizeof(Value) == sizeof(uint64_t), "Value must be 8 bytes");

} // namespace mg
