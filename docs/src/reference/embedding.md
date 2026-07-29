# Embedding and FFI

Magnesium is designed to be embedded by a host application. The recommended integration path is:

1. The host creates and owns the VM.
2. The host registers native functions, native handles, and global constants.
3. Script files declare those host-provided names with `extern` when they want static diagnostics and LSP support.
4. Runtime behavior still comes from the host registration. `extern` does not emit bytecode and does not create a value by itself.

This keeps script code dynamic by default while allowing production hosts to document their API surface precisely.

## Script Declarations

Use `extern` to tell the checker and LSP about host-provided symbols:

```mg
!strict

extern fn read_score(player: string): number
extern fn save_score(player: string, score: number): null
extern const MAX_SCORE: number

let score: number = read_score("Ada")
save_score("Ada", score)
```

Rules:

- `extern fn` declares a callable symbol and its parameter/return types.
- `extern const` declares a read-only host-provided value.
- `extern` declarations are accepted in every mode.
- `!strict` reports type errors against extern signatures.
- `!nocheck` suppresses type diagnostics even when the host or LSP requests checking.
- The host must still register or provide the symbol before runtime use.

## C Host API

The C API is the low-level embedding boundary. It is intentionally explicit and should be treated as unsafe by host code.

When a native C or C++ host links `libmagnesium.dll`, define
`MG_USE_SHARED` before including `magnesium.h`. The library build defines
`MG_BUILD_SHARED` itself so `MG_API` exports the supported embedding ABI.
Neither definition is needed for static builds or on Unix-like platforms.

Native functions use this shape:

```c
typedef Value (*NativeFn)(VM *vm, int arg_count, Value *args);
```

Register a native function with:

```c
void vm_register_native(
    VM *vm,
    const char *name,
    NativeFn function,
    int arity,
    void *userdata,
    void (*userdata_finalizer)(void *)
);
```

Pass `NULL, NULL` when the callback has no userdata. Native functions receive
raw VM values. They are responsible for checking argument count and value kinds
before reading arguments. The `args` array remains stable and GC-rooted for the
duration of the callback, including across `vm_push()` calls and collections.
Do not retain the pointer or its unrooted object values after the callback
returns; use `vm_root_value()` for values the host must keep.

Native handles attach host-owned data to script-visible values:

```c
ObjNativeHandle *vm_new_native_handle(
    VM *vm,
    const char *type_name,
    void *data,
    NativeHandleFinalizer finalizer
);

bool vm_native_handle_set_method(
    VM *vm,
    ObjNativeHandle *handle,
    const char *name,
    NativeFn function,
    int arity,
    void *userdata,
    void (*userdata_finalizer)(void *)
);
```

Use native handles for engine objects, resources, file handles, scene nodes, sockets, ECS handles, or any value whose lifetime is owned by the host.

Raw numeric FFI exists for simple C function pointers:

```c
ObjFFI *vm_register_ffi(VM *vm, const char *name, void *c_func, int arity);
```

This is a narrow path for numeric C calls. Prefer registered native functions when you need validation, multiple value kinds, handles, structured errors, or stable host behavior.

## Rust Bindings

The Rust bindings are split into two crates:

- `magnesium-sys`: raw `repr(C)` bindings to the C API.
- `magnesium`: safer wrappers for VM lifecycle, interpretation, compilation, globals, native functions, numeric FFI, native handles, arrays, dicts, strings, errors, and static checking.

Use the safe wrapper by default:

```rust
use magnesium::Vm;

let mut vm = Vm::new();
vm.try_set_global_number("MAX_SCORE", 100.0)?;
vm.try_interpret_named(r#"
!strict
extern const MAX_SCORE: number
print(MAX_SCORE)
"#, "game.mg")?;
```

The safe wrapper exposes checked operations for common host values:

```rust
vm.try_set_global_number("MAX_SCORE", 100.0)?;
vm.try_set_global_bool("debug", true)?;
vm.try_set_global_string("title", "Magnesium")?;
let arr = vm.new_array_value();
let dict = vm.new_dict_value();
```

Static checking can be run directly by the host:

```rust
let ok = vm.try_type_check(source, true)?;
```

Use the safe closure API for native functions:

```rust
use magnesium::Value;

vm.register_native_fn("host_add", 2, |ctx| {
    Value::auto_val(ctx.expect_numeric(0) + ctx.expect_numeric(1))
})?;
```

The raw `try_register_native` and `register_native` methods are `unsafe`: the caller must provide a callback with the exact C ABI and uphold the raw value and lifetime contracts.

Use numeric FFI only for simple numeric functions:

```rust
extern "C" fn square(x: f64) -> f64 {
    x * x
}

vm.register_ffi1("square", square)?;
```

`register_ffi_raw_unchecked` is unsafe because Rust cannot verify the function pointer ABI, arity, or value contract.

## C++ Bindings

The C++ bindings are header-only and require C++17. They live at `bindings/cpp/` and build with CMake.

Structure:

- `magnesium-sys/`: compiles the C source and produces `libmagnesium`.
- `magnesium/include/mg/`: header-only wrapper with `mg::Value`, `mg::Vm`, `mg::NativeContext`, collection types, and error types.

Use the wrapper:

```cpp
#include <mg/magnesium.hpp>

int main() {
    mg::Vm vm;
    vm.set_global_number("MAX_SCORE", 100.0);
    auto result = vm.interpret(R"mg(
!strict
extern const MAX_SCORE: number
print(MAX_SCORE)
)mg");
    if (result.is_err()) {
        std::cerr << result.error().to_string() << "\n";
        return 1;
    }
}
```

`mg::Value` is a NaN-boxed value class. Construct values with `mg::Value::null()`, `mg::Value::bool_val()`, `mg::Value::int_val()`, `mg::Value::number_val()`, `mg::Value::obj_val()`, and `mg::Value::auto_val()`.

Register native functions with closures:

```cpp
vm.register_native_fn("double", 1, [](mg::NativeContext& ctx) {
    return mg::Value::auto_val(ctx.expect_numeric(0) * 2.0);
});
```

Native handles wrap host-owned data:

```cpp
struct Entity { double x, y; };
auto handle = vm.new_native_handle_t<Entity>(
    "Entity", std::make_unique<Entity>(Entity{10.0, 20.0}));
vm.set_native_handle_method_fn<Entity>(
    handle, "Entity", "get_x", 1,
    [](Entity& entity, mg::NativeContext&) {
        return mg::Value::number_val(entity.x);
    });
```

Keep host-held object values alive across collections with `mg::GcRoot`:

```cpp
auto array = vm.new_array_value();
mg::GcRoot root(vm, array);
mg::gc_collect(vm);
assert(root.get().is_array());
```

Roots follow their VM through move construction or move assignment. If a root
outlives the VM, it becomes inert: `get()` returns null, `set()` returns false,
and destruction never calls into the released VM.

CMake integration:

```cmake
add_subdirectory("/path/to/magnesium/bindings/cpp" magnesium-bindings)
target_link_libraries(your_target PRIVATE magnesium)
```

## C# Bindings

The C# bindings target .NET 8 and use P/Invoke to call `libmagnesium`. They live at `bindings/csharp/`.

Structure:

- `Magnesium/`: the binding library (`Magnesium.csproj`).
- `Magnesium.Tests/`: integration tests.

Use the wrapper:

```csharp
using Magnesium;

using var vm = new Vm();
vm.SetGlobalNumber("MAX_SCORE", 100.0);
var result = vm.Interpret(@"
!strict
extern const MAX_SCORE: number
print(MAX_SCORE)
");
if (result.IsErr) {
    Console.WriteLine(result.Error.Message);
    return;
}
```

`Magnesium.Value` is a readonly struct with NaN-boxed representation. Construct with `Value.Null`, `Value.BoolVal()`, `Value.IntVal()`, `Value.NumberVal()`, `Value.ObjVal()`, `Value.AutoVal()`.

Register native functions:

```csharp
vm.RegisterNativeFn("double", 1, ctx => Value.AutoVal(ctx.ExpectNumeric(0) * 2.0));
```

Native handles use `GCHandle` to retain managed objects until the VM releases the handle:

```csharp
var obj = new CounterObj { Value = 42 };
var handle = vm.NewNativeHandle<CounterObj>("Counter", obj);
```

Project reference:

```bash
dotnet add reference /path/to/bindings/csharp/Magnesium/Magnesium.csproj
```

Ensure the directory containing `libmagnesium.so` (or `.dll` on Windows) is on your `LD_LIBRARY_PATH` (or `PATH` on Windows).

## Header Imports

Script-level C header/library imports are experimental integration surface, not the preferred production embedding path. Production hosts should register APIs explicitly and pair them with `extern` declarations for tooling.

## Runtime And Type Boundaries

Types are optional and tooling-facing. They do not change runtime semantics.

That means this:

```mg
let score = read_score("Ada")
```

and this:

```mg
let score: number = read_score("Ada")
```

must run the same way once compiled. In strict mode, the second form gives the checker more information. In dynamic mode, the annotation is accepted and ignored by default.

## Recommended Host API Style

Use small, explicit host APIs:

```mg
extern fn entity_position(id: number): &< x: number, y: number >
extern fn set_entity_position(id: number, x: number, y: number): null
extern fn log(message: string): null
```

Prefer data values over object-heavy host surfaces:

- arrays for ordered homogeneous or mixed sequences
- dicts for dynamic string-keyed data
- structs for Magnesium-owned fixed records
- native handles for host-owned resources

Do not expose a public `table` abstraction. The VM may use hash tables internally, but scripts should see arrays, dicts, structs, functions, errors, and native handles.
