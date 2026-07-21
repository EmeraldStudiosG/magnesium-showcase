#pragma once

#include <mg/value.hpp>
#include <mg/error.hpp>
#include <mg/native.hpp>

#include <cstring>
#include <string>
#include <memory>
#include <optional>

namespace mg {

class Vm {
public:
    Vm() : raw_(vm_new()) {
        if (!raw_) throw std::runtime_error("failed to allocate VM");
    }

    ~Vm() {
        if (raw_) vm_delete(raw_);
    }

    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;
    Vm(Vm&& other) noexcept : raw_(other.raw_) { other.raw_ = nullptr; }
    Vm& operator=(Vm&& other) noexcept {
        if (this != &other) {
            if (raw_) vm_delete(raw_);
            raw_ = other.raw_;
            other.raw_ = nullptr;
        }
        return *this;
    }

    ::VM* raw_mut() { return raw_; }
    const ::VM* raw() const { return raw_; }

    InterpretResult interpret(const std::string& source) {
        auto cs = cstr(source);
        if (!cs) return InterpretResult::host_error(HostError::interior_nul("source"));
        auto r = vm_interpret(raw_, cs->c_str());
        return InterpretResult::from_raw(r);
    }

    InterpretResult interpret_named(const std::string& source, const std::string& name) {
        auto cs = cstr(source);
        if (!cs) return InterpretResult::host_error(HostError::interior_nul("source"));
        auto cn = cstr(name);
        if (!cn) return InterpretResult::host_error(HostError::interior_nul("name"));
        auto r = vm_interpret_named(raw_, cs->c_str(), cn->c_str());
        return InterpretResult::from_raw(r);
    }

    std::optional<::ObjFunction*> compile(const std::string& source) {
        auto cs = cstr(source);
        if (!cs) return std::nullopt;
        auto* fn = vm_compile(raw_, cs->c_str());
        return fn ? std::optional<::ObjFunction*>(fn) : std::nullopt;
    }

    std::optional<::ObjFunction*> compile_named(const std::string& source, const std::string& name) {
        auto cs = cstr(source);
        if (!cs) return std::nullopt;
        auto cn = cstr(name);
        if (!cn) return std::nullopt;
        auto* fn = vm_compile_named(raw_, cs->c_str(), cn->c_str());
        return fn ? std::optional<::ObjFunction*>(fn) : std::nullopt;
    }

    InterpretResult run_function(::ObjFunction* fn) {
        auto r = vm_run_function(raw_, fn);
        return InterpretResult::from_raw(r);
    }

    bool save_bytecode(::ObjFunction* fn, const std::string& path) {
        auto cp = cstr(path);
        if (!cp) return false;
        return vm_save_bytecode(raw_, fn, cp->c_str());
    }

    std::optional<::ObjFunction*> load_bytecode(const std::string& path) {
        auto cp = cstr(path);
        if (!cp) return std::nullopt;
        auto* fn = vm_load_bytecode(raw_, cp->c_str());
        return fn ? std::optional<::ObjFunction*>(fn) : std::nullopt;
    }

    void push(Value v) { vm_push(raw_, v.to_raw()); }
    Value pop() { return Value::from_raw(vm_pop(raw_)); }

    void set_global(const std::string& name, Value v) {
        auto cn = cstr(name);
        if (!cn) return;
        vm_set_global_value(raw_, cn->c_str(), v.to_raw());
    }

    std::optional<Value> get_global(const std::string& name) {
        auto cn = cstr(name);
        if (!cn) return std::nullopt;
        ::Value out;
        if (vm_get_global_value(raw_, cn->c_str(), &out))
            return Value::from_raw(out);
        return std::nullopt;
    }

    void set_global_number(const std::string& name, double v) { set_global(name, Value::auto_val(v)); }
    void set_global_bool(const std::string& name, bool v) { set_global(name, Value::bool_val(v)); }

    void set_global_string(const std::string& name, const std::string& v) {
        auto cn = cstr(name);
        if (!cn) return;
        auto* s = copy_string(raw_, v.c_str(), static_cast<int>(v.size()));
        if (!s) return;
        vm_set_global_value(raw_, cn->c_str(), Value::obj_val(s).to_raw());
    }

    void register_native(const std::string& name, ::NativeFn func, int32_t arity) {
        auto cn = cstr(name);
        if (!cn) return;
        vm_register_native(raw_, cn->c_str(), func, arity, nullptr, nullptr);
    }

    void register_native_fn(const std::string& name, int32_t arity, NativeClosure f) {
        auto cn = cstr(name);
        if (!cn) return;

        auto* wrap = new NativeClosureWrap{this, std::move(f)};
        vm_register_native(raw_, cn->c_str(), native_trampoline, arity,
                           static_cast<void*>(wrap), native_closure_finalizer);
    }

    Value new_native_handle(const std::string& type_name, void* data, void (*finalizer)(void*)) {
        auto cn = cstr(type_name);
        if (!cn) return Value::null();
        auto* handle = vm_new_native_handle(raw_, cn->c_str(), data, finalizer);
        if (!handle) return Value::null();
        return Value::from_raw(vm_native_handle_value(handle));
    }

    template<typename T>
    Value new_native_handle_t(const std::string& type_name, std::unique_ptr<T> data) {
        auto cn = cstr(type_name);
        if (!cn) return Value::null();
        auto* ptr = data.release();
        auto* handle = vm_new_native_handle(raw_, cn->c_str(), ptr,
            [](void* d) { delete static_cast<T*>(d); });
        if (!handle) { delete ptr; return Value::null(); }
        return Value::from_raw(vm_native_handle_value(handle));
    }

    void set_native_handle_method(Value handle, const std::string& name,
                                   ::NativeFn func, int32_t arity) {
        if (!handle.is_native_handle()) return;
        auto cn = cstr(name);
        if (!cn) return;
        vm_native_handle_set_method(raw_, handle.as_native_handle(), cn->c_str(),
                                     func, arity, nullptr, nullptr);
    }

    template<typename T>
    void set_native_handle_method_fn(Value handle, const std::string& type_name,
                                       const std::string& method_name, int32_t arity,
                                       std::function<Value(T&, NativeContext&)> f) {
        if (!handle.is_native_handle()) return;
        auto cm = cstr(method_name);
        if (!cm) return;

        auto* mc = new MethodClosure<T>{this, type_name, std::move(f)};
        vm_native_handle_set_method(raw_, handle.as_native_handle(), cm->c_str(),
                                     method_trampoline<T>, arity,
                                     static_cast<void*>(mc), method_finalizer<T>);
    }

    template<typename T>
    T* native_handle_data(Value value, const std::string& type_name) {
        if (!value.is_native_handle()) return nullptr;
        auto cn = cstr(type_name);
        if (!cn) return nullptr;
        void* data = vm_native_handle_data(value.to_raw(), cn->c_str());
        return static_cast<T*>(data);
    }

    Value new_array_value() {
        auto* arr = new_array(raw_);
        return arr ? Value::obj_val(arr) : Value::null();
    }

    Value new_dict_value() {
        auto* d = new_dict(raw_);
        return d ? Value::obj_val(d) : Value::null();
    }

    std::string last_error_message() const {
        return raw_->last_error_message ? std::string(raw_->last_error_message) : "";
    }
    std::string last_error_trace() const {
        return raw_->last_error_trace ? std::string(raw_->last_error_trace) : "";
    }
    int32_t last_error_line() const { return raw_->last_error_line; }
    void clear_error() { vm_clear_error(raw_); }

    int32_t frame_count() const { return raw_->frame_count; }
    int32_t stack_top() const { return raw_->stack_top; }
    size_t bytes_allocated() const { return raw_->bytes_allocated; }

private:
    std::optional<std::string> cstr(const std::string& s) {
        if (s.find('\0') != std::string::npos) return std::nullopt;
        return s;
    }

    ::VM* raw_;
};

} // namespace mg
