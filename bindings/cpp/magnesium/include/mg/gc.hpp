#pragma once

#include <mg/vm.hpp>
#include <new>

namespace mg {

class GcRoot {
public:
    GcRoot(Vm& vm, Value value)
        : control_(vm.control_), root_(nullptr) {
        auto* raw = control_ ? control_->raw : nullptr;
        if (!raw) throw std::runtime_error("cannot root a value in an invalid VM");
        root_ = vm_root_value(raw, value.to_raw());
        if (!root_) throw std::bad_alloc();
    }
    ~GcRoot() {
        auto* raw = control_ ? control_->raw : nullptr;
        if (raw && root_) vm_unroot_value(raw, root_);
    }

    GcRoot(const GcRoot&) = delete;
    GcRoot& operator=(const GcRoot&) = delete;
    GcRoot(GcRoot&& other) noexcept
        : control_(std::move(other.control_)), root_(other.root_) {
        other.root_ = nullptr;
    }
    GcRoot& operator=(GcRoot&&) = delete;

    Value get() const {
        auto* raw = control_ ? control_->raw : nullptr;
        return Value::from_raw(
            raw && root_ ? vm_root_get(root_) : Value::null().to_raw());
    }
    bool set(Value value) {
        auto* raw = control_ ? control_->raw : nullptr;
        return raw && root_ && vm_root_set(raw, root_, value.to_raw());
    }

private:
    std::shared_ptr<VmControl> control_;
    ::VMRoot* root_;
};

inline void gc_collect(Vm& vm) { ::gc_major_collect(vm.raw_mut()); }
inline void gc_mark_value(Vm& vm, Value v) { ::gc_mark_value(vm.raw_mut(), v.to_raw()); }
inline void gc_mark_object(Vm& vm, ::Obj* obj) { ::gc_mark_object(vm.raw_mut(), obj); }

} // namespace mg
