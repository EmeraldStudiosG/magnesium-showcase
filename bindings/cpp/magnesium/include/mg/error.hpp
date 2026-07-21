#pragma once

#include <stdexcept>
#include <string>
#include <cstring>
#include <variant>
#include <optional>

extern "C" {
#include "magnesium.h"
}

namespace mg {

enum class HostErrorKind {
    InteriorNul,
    AllocationFailed,
    InvalidFunctionHandle,
    InvalidNativeHandle,
};

class HostError : public std::runtime_error {
public:
    explicit HostError(HostErrorKind kind, const std::string& msg)
        : std::runtime_error(msg), kind_(kind) {}

    HostErrorKind kind() const { return kind_; }

    static HostError interior_nul(const char* field) {
        return HostError(HostErrorKind::InteriorNul,
            std::string("interior nul byte in field: ") + field);
    }
    static HostError allocation_failed(const char* op) {
        return HostError(HostErrorKind::AllocationFailed,
            std::string("allocation failed: ") + op);
    }
    static HostError invalid_function_handle() {
        return HostError(HostErrorKind::InvalidFunctionHandle, "invalid function handle");
    }
    static HostError invalid_native_handle() {
        return HostError(HostErrorKind::InvalidNativeHandle, "invalid native handle");
    }

private:
    HostErrorKind kind_;
};

class MgError {
public:
    explicit MgError(::ObjError* raw) : raw_(raw) {}

    ::ObjError* raw() const { return raw_; }

    std::string kind() const {
        return raw_->kind ? std::string(raw_->kind->chars, raw_->kind->length) : "";
    }
    std::string message() const {
        return raw_->message ? std::string(raw_->message->chars, raw_->message->length) : "";
    }
    std::string file() const {
        return raw_->file ? std::string(raw_->file->chars, raw_->file->length) : "";
    }
    int line() const { return raw_->line; }
    std::string function() const {
        return raw_->function ? std::string(raw_->function->chars, raw_->function->length) : "";
    }
    std::string hint() const {
        return raw_->hint ? std::string(raw_->hint->chars, raw_->hint->length) : "";
    }

    std::string to_string() const {
        return "[" + kind() + ":" + std::to_string(line()) + "] " + message();
    }

private:
    ::ObjError* raw_;
};

enum class InterpretErrorKind {
    CompileError,
    RuntimeError,
    Yield,
    HostError,
};

class InterpretError {
public:
    explicit InterpretError(InterpretErrorKind kind) : kind_(kind), mg_error_(std::nullopt), host_(std::nullopt) {}
    InterpretError(InterpretErrorKind kind, MgError err) : kind_(kind), mg_error_(std::move(err)), host_(std::nullopt) {}
    InterpretError(InterpretErrorKind kind, HostError err) : kind_(kind), mg_error_(std::nullopt), host_(std::move(err)) {}

    InterpretErrorKind kind() const { return kind_; }
    const std::optional<MgError>& mg_error() const { return mg_error_; }
    const std::optional<HostError>& host_error() const { return host_; }

    std::string to_string() const {
        switch (kind_) {
            case InterpretErrorKind::CompileError: return "compile error";
            case InterpretErrorKind::RuntimeError:
                return mg_error_ ? mg_error_->to_string() : "runtime error";
            case InterpretErrorKind::Yield: return "yield";
            case InterpretErrorKind::HostError:
                return host_ ? host_->what() : "host error";
        }
        return "unknown error";
    }

private:
    InterpretErrorKind kind_;
    std::optional<MgError> mg_error_;
    std::optional<HostError> host_;
};

class InterpretResult {
public:
    static InterpretResult ok() { return InterpretResult(OkTag{}); }
    static InterpretResult err(InterpretError e) { return InterpretResult(std::move(e)); }

    static InterpretResult from_raw(::InterpretResult raw) {
        switch (raw) {
            case ::InterpretResult::INTERPRET_OK: return ok();
            case ::InterpretResult::INTERPRET_COMPILE_ERROR:
                return InterpretResult(InterpretError(InterpretErrorKind::CompileError));
            case ::InterpretResult::INTERPRET_RUNTIME_ERROR:
                return InterpretResult(InterpretError(InterpretErrorKind::RuntimeError));
            case ::InterpretResult::INTERPRET_YIELD:
                return InterpretResult(InterpretError(InterpretErrorKind::Yield));
        }
        return ok();
    }

    static InterpretResult from_raw_with_vm(::InterpretResult raw, ::VM* vm) {
        if (raw == ::InterpretResult::INTERPRET_RUNTIME_ERROR && vm) {
            auto* err_obj = reinterpret_cast<::ObjError*>(vm->stack[vm->stack_top - 1]);
            if (err_obj && err_obj->obj.type == OBJ_ERROR) {
                return InterpretResult(InterpretError(InterpretErrorKind::RuntimeError, MgError(err_obj)));
            }
        }
        return from_raw(raw);
    }

    static InterpretResult host_error(HostError e) {
        return InterpretResult(InterpretError(InterpretErrorKind::HostError, std::move(e)));
    }

    bool is_ok() const { return std::holds_alternative<OkTag>(result_); }
    bool is_err() const { return !is_ok(); }

    const InterpretError& error() const { return std::get<InterpretError>(result_); }

private:
    struct OkTag {};
    std::variant<OkTag, InterpretError> result_;

    explicit InterpretResult(OkTag) : result_(OkTag{}) {}
    explicit InterpretResult(InterpretError e) : result_(std::move(e)) {}
};

} // namespace mg
