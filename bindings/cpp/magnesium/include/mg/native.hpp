#pragma once

#include <mg/value.hpp>
#include <mg/error.hpp>
#include <functional>
#include <exception>
#include <memory>
#include <string>

namespace mg {

class Vm;

class NativeContext {
public:
    NativeContext(Vm& vm, const ::Value* args, size_t count)
        : vm_(&vm), args_(args), count_(count) {}

    Vm& vm() { return *vm_; }

    std::optional<Value> arg(size_t index) const {
        if (index >= count_) return std::nullopt;
        return Value::from_raw(args_[index]);
    }
    size_t arg_count() const { return count_; }

    std::optional<bool> arg_bool(size_t i) const { auto v = arg(i); return v ? v->try_as_bool() : std::nullopt; }
    std::optional<int32_t> arg_int(size_t i) const { auto v = arg(i); return v ? v->try_as_int() : std::nullopt; }
    std::optional<double> arg_number(size_t i) const { auto v = arg(i); return v ? v->try_as_number() : std::nullopt; }
    std::optional<double> arg_numeric(size_t i) const { auto v = arg(i); return v ? v->try_as_numeric() : std::nullopt; }
    std::optional<std::string_view> arg_string(size_t i) const { auto v = arg(i); return v ? v->try_as_string() : std::nullopt; }

    Value expect_arg(size_t i) const {
        auto v = arg(i);
        if (!v) throw std::runtime_error("argument index out of range");
        return *v;
    }
    bool expect_bool(size_t i) const { auto v = arg_bool(i); if (!v) throw std::runtime_error("not a bool"); return *v; }
    int32_t expect_int(size_t i) const { auto v = arg_int(i); if (!v) throw std::runtime_error("not an int"); return *v; }
    double expect_number(size_t i) const { auto v = arg_number(i); if (!v) throw std::runtime_error("not a number"); return *v; }
    double expect_numeric(size_t i) const { auto v = arg_numeric(i); if (!v) throw std::runtime_error("not numeric"); return *v; }
    std::string_view expect_string(size_t i) const { auto v = arg_string(i); if (!v) throw std::runtime_error("not a string"); return *v; }

private:
    Vm* vm_;
    const ::Value* args_;
    size_t count_;
};

using NativeClosure = std::function<Value(NativeContext&)>;

struct VmControl {
    Vm* owner;
    ::VM* raw;
};

struct NativeClosureWrap {
    std::shared_ptr<VmControl> vm_control;
    NativeClosure fn;
};

inline void native_closure_finalizer(void* data) {
    if (data) delete static_cast<NativeClosureWrap*>(data);
}

inline ::Value native_trampoline(::VM* vm, int32_t arg_count, ::Value* args) {
    void* userdata = mg_get_native_userdata(vm);
    if (!userdata) return Value::null().to_raw();

    auto* wrap = static_cast<NativeClosureWrap*>(userdata);
    if (!wrap->vm_control || !wrap->vm_control->owner ||
        !wrap->vm_control->raw) {
        return Value::null().to_raw();
    }

    size_t count = (arg_count > 0 && args) ? static_cast<size_t>(arg_count) : 0;

    try {
        NativeContext ctx(*wrap->vm_control->owner, args, count);
        return wrap->fn(ctx).to_raw();
    } catch (const std::exception& error) {
        vm_runtime_error(vm, "native callback failed: %s", error.what());
    } catch (...) {
        vm_runtime_error(vm, "native callback failed with an unknown C++ exception");
    }
    return Value::null().to_raw();
}

template<typename T>
struct MethodClosure {
    std::shared_ptr<VmControl> vm_control;
    std::string type_name_str;
    std::function<Value(T&, NativeContext&)> fn;
};

template<typename T>
void method_finalizer(void* data) {
    if (data) delete static_cast<MethodClosure<T>*>(data);
}

template<typename T>
::Value method_trampoline(::VM* vm, int32_t arg_count, ::Value* args) {
    if (!args || arg_count < 1) return Value::null().to_raw();

    void* userdata = mg_get_native_userdata(vm);
    if (!userdata) return Value::null().to_raw();

    auto* mc = static_cast<MethodClosure<T>*>(userdata);
    if (!mc->vm_control || !mc->vm_control->owner || !mc->vm_control->raw) {
        return Value::null().to_raw();
    }

    Value self_val = Value::from_raw(args[0]);
    void* data = vm_native_handle_data(self_val.to_raw(), mc->type_name_str.c_str());
    if (!data) return Value::null().to_raw();
    auto* typed = static_cast<T*>(data);

    auto* val_args = args + 1;
    size_t count = (arg_count > 1) ? static_cast<size_t>(arg_count - 1) : 0;

    try {
        NativeContext ctx(*mc->vm_control->owner, val_args, count);
        return mc->fn(*typed, ctx).to_raw();
    } catch (const std::exception& error) {
        vm_runtime_error(vm, "native method failed: %s", error.what());
    } catch (...) {
        vm_runtime_error(vm, "native method failed with an unknown C++ exception");
    }
    return Value::null().to_raw();
}

} // namespace mg
