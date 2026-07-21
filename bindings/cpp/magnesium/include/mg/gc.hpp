#pragma once

#include <mg/value.hpp>

namespace mg {

class Vm;

class GcRoot {
public:
    GcRoot(Vm& vm, Value v);
    ~GcRoot();

    GcRoot(const GcRoot&) = delete;
    GcRoot& operator=(const GcRoot&) = delete;

private:
    Vm& vm_;
};

inline void gc_collect(Vm& vm);
inline void gc_mark_value(Vm& vm, Value v);
inline void gc_mark_object(Vm& vm, ::Obj* obj);

} // namespace mg
