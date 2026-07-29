#pragma once

#include <stdexcept>
#include <string>
#include <cstring>
#include <variant>
#include <optional>
#include <utility>
#include <mg/value.hpp>

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
    explicit MgError(const ::ObjError* raw) : MgError(nullptr, raw) {}

    MgError(::VM* vm, const ::ObjError* raw)
        : kind_(snapshot_string(vm, raw ? raw->kind : nullptr)),
          message_(snapshot_string(vm, raw ? raw->message : nullptr)),
          file_(snapshot_string(vm, raw ? raw->file : nullptr)),
          line_(raw ? raw->line : 0),
          function_(snapshot_string(vm, raw ? raw->function : nullptr)),
          hint_(snapshot_string(vm, raw ? raw->hint : nullptr)) {}

    MgError(std::string kind, std::string message, std::string file,
            int line, std::string function, std::string hint)
        : kind_(std::move(kind)),
          message_(std::move(message)),
          file_(std::move(file)),
          line_(line),
          function_(std::move(function)),
          hint_(std::move(hint)) {}

    std::string kind() const { return kind_; }
    std::string message() const { return message_; }
    std::string file() const { return file_; }
    int line() const { return line_; }
    std::string function() const { return function_; }
    std::string hint() const { return hint_; }

    std::string to_string() const {
        return "[" + kind() + ":" + std::to_string(line()) + "] " + message();
    }

private:
    static std::string snapshot_string(::VM* vm, ::ObjString* raw) {
        if (!raw || raw->length < 0) return "";
        if (vm) {
            size_t required = 0;
            ::Value value = Value::obj_val(raw).to_raw();
            vm_string_copy(vm, value, nullptr, 0, &required);
            if (required > 0) {
                std::string copy(required, '\0');
                if (vm_string_copy(vm, value, copy.data(), copy.size(), nullptr)) {
                    copy.resize(required - 1);
                    return copy;
                }
            }
        }
        if (!raw->is_rope && raw->chars) {
            return std::string(raw->chars, static_cast<size_t>(raw->length));
        }
        return "";
    }

    std::string kind_;
    std::string message_;
    std::string file_;
    int line_;
    std::string function_;
    std::string hint_;
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
        ::Value raw_error = Value::null().to_raw();
        if (raw == ::InterpretResult::INTERPRET_RUNTIME_ERROR &&
                vm_last_error_value(vm, &raw_error)) {
            Value error_value = Value::from_raw(raw_error);
            if (error_value.is_error()) {
                auto* err_obj = reinterpret_cast<::ObjError*>(error_value.as_obj());
                return InterpretResult(InterpretError(
                    InterpretErrorKind::RuntimeError, MgError(vm, err_obj)));
            }
        }
        if (raw == ::InterpretResult::INTERPRET_RUNTIME_ERROR && vm) {
            return InterpretResult(InterpretError(
                InterpretErrorKind::RuntimeError,
                MgError("RuntimeError",
                        vm_last_error(vm),
                        vm_last_error_file(vm),
                        vm_last_error_line(vm),
                        vm_last_error_function(vm),
                        "")));
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
