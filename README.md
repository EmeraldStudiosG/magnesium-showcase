# Magnesium

![Magnesium Header](docs/src/assets/images/MagnesiumMainHeader.png)

Magnesium is an embeddable scripting language implemented from scratch in C11: lexer, recursive-descent parser, register-bytecode compiler, type checker, generational garbage collector, computed-goto VM, bytecode serializer with a static verifier, language server, VS Code extension, and safe bindings for Rust, C++, and C#. Roughly **17,000 lines of C** with no runtime dependencies beyond libc, libm, and pthreads.

It is at **v1.1.1**, ships with a 59-program exact-output regression suite plus generated CLI edge cases, a strict 16-benchmark cross-language gate (Magnesium vs. Lua 5.4 and CPython), a 10-variant bytecode-loader fuzzer, and an end-to-end LSP smoke test covering every advertised capability.

The design goal is **predictable performance without a JIT**: aggressive static bytecode specialization, typed inline caches, a generational collector, and a verifier that makes loading untrusted `.mgc` files safe. Lua is the embedding reference point, but Magnesium keeps arrays, dicts, structs, and enums as distinct types instead of one public table.

---

## Implementation

| Component | File | Lines | Description |
| --- | --- | --- | --- |
| VM | `src/vm.c` | 6,551 | Computed-goto dispatch loop, 121 opcodes, 4 inline caches, native/FFI calls, threads, coroutines |
| Compiler | `src/compiler.c` | 4,204 | AST to bytecode, register allocator, peephole fusion, leaf-function inliner, control-flow lowering |
| Parser | `src/parser.c` | 1,631 | Recursive descent + Pratt precedence, sub-scan for interpolated strings, panic-mode recovery |
| Header | `src/magnesium.h` | 1,286 | NaN-boxing, opcode encoding, object model, GC layout, public C API |
| LSP server | `src/lsp.c` | 1,756 | Hand-written LSP base protocol, no JSON library |
| Type checker | `src/typecheck.c` | 1,072 | Opt-in checker, type aliases, structural shapes, forward references |
| GC | `src/gc.c` | 633 | Generational tri-color mark-and-sweep, write barriers, slab allocator |
| Serializer | `src/serialize.c` | 632 | `.mgc` format and bytecode validator |
| Lexer | `src/lexer.c` | 344 | Table-free scanner with hand-coded keyword trie |
| Toolchain | `src/mt.c` | 539 | `mt` package manager: core, bindings, VS Code extension |

### Notable implementation details

- **General-purpose bytecode specialization.** The compiler lowers language-level arithmetic, collection, field, call, and control-flow operations into register bytecode using semantic and type information. The same lowering rules apply to every source file; the benchmark gate does not enable benchmark-only opcodes or runtime paths.

- **Four direct-mapped inline caches with two different hash functions.** Globals (128 slots), struct fields (128), method dispatch (128), and dict keys (256). Field IC hashes `(klass ^ name>>4) & mask`; method IC hashes `(klass>>3 ^ name>>5) & mask`. Different hash functions on adjacent caches avoid same-slot collisions under workloads that touch fields and methods together. Dict gets an additional per-instance monomorphic slot checked before the VM-wide cache. Weak cache entries are cleared before collections that could reclaim their objects.

- **NaN-boxing with auto-promoting integers.** All values fit in 64 bits (`src/magnesium.h:128-171`). Numbers that happen to be integral and in `int32_t` range silently tag as `TAG_INT`, so the int+int fast path skips floating-point work entirely. `int_or_number` (`src/vm.c:2784`) promotes only on overflow: a chain of `int + int + int` stays int-register-resident until something overflows, then the whole chain degrades to float. No silent truncation, no heap traffic.

- **Bytecode verifier.** `validate_function` (`src/serialize.c`) walks every instruction and checks register spans, constant-table indices with type tags, jump-target bounds, arity limits, and multi-word instruction integrity. A `.mgc` file whose opcode or auxiliary word is rewritten to an invalid instruction is rejected before any code executes. Size caps on every chunk prevent malformed files from requesting unbounded allocations.

- **Compile-time name resolution for globals.** An undefined global is a *compile* error, not a runtime one. The compiler seeds its global table from builtins and the host-registered globals (`src/compiler.c:427-435`) and rejects unknown identifiers upfront (`src/compiler.c:662`). It also tracks whether the program has shadowed `len`, `push`, `tostring`, etc. so the fast-path opcodes for those builtins can be safely disabled when the global has been redefined (`src/compiler.c:466`).

- **Leaf-function inliner that reuses argument registers.** Side-effect-free single-return functions with <=8 parameters are inlined by binding their parameters directly to the caller's already-allocated argument registers: no `OP_CALL`, no frame setup, no `OP_MOVE` (`src/compiler.c:1144-1356`).

- **Type-directed opcode specialization in a dynamic language.** When a local is assigned a struct literal, the compiler records the struct type, and subsequent field accesses on that local compile to `OP_GETFIELD_IDX`: a single O(1) opcode that reads the field by index with no name lookup (`src/compiler.c:2341`).

- **Generational GC with remembered sets and write barriers.** Young/old generations, tri-color mark-and-sweep, a `gc_write_barrier` inlined at exactly the mutating opcodes that can create old-to-young edges (`src/magnesium.h:1241`), a 14-class slab allocator for small objects, dedicated free-lists for closures and upvalues (the highest-churn objects), and a `protect_newborn_young` pass that handles the classic hazard of an object allocated *during* a minor collection (`src/gc.c:200`). ICs are cleared on major collection only: minor collection is IC-stable.

- **Coroutines via context save/restore, not `setjmp`/`longjmp`.** A small `SavedVMContext` struct captures the entire register/stack world (`src/magnesium.h:574-592`); `save_vm_context`/`restore_vm_context` (`src/vm.c:2558`) swap to/from `ObjCoroutine`. YIELD propagates as an in-band `InterpretResult` enum value through nested native calls without abnormal C stack unwinding.

- **`vm.spawn` for isolated VMs on native threads.** Each child gets its own GC, stack, globals, and modules, plus cooperative cancel propagation (loop-back opcodes check an atomic flag every 1,024 iterations via `CHECK_LOOP_CANCEL`, `src/vm.c:2960`), deadlines via monotonic-clock timeouts, and proper GC rooting through the parent's `vm_tasks` list so the parent collector cannot free a task still owned by a worker thread (`src/magnesium.h:1108-1115`).

- **Per-call frame caching with `restrict` pointers.** The dispatch loop hoists `slots`, `ip`, `k` into `Value * restrict slots`, `Instruction * restrict ip`, `const Value * restrict k` (`src/vm.c:2870-2872`). Combined with ~140 `__builtin_expect` annotations on the hot path, the compiler keeps the interpreter state in registers across opcode handlers.

- **MSVC is intentionally not supported.** The dispatch loop relies on labels-as-values (computed `goto`), supported by GCC and Clang but not MSVC. The `Makefile` detects an unsupported Windows compiler early and points to a Clang-based toolchain.

---

## A small example

```magnesium
// hello.mg
let name = input("Name: ")
print(_"Hello, {name}!")
```

```bash
magnesium hello.mg
```

A larger tour lives in [`examples/showcase.mg`](examples/showcase.mg). The full language walk is [`docs/src/getting-started/quick-start.md`](docs/src/getting-started/quick-start.md).

### Embedding example

```magnesium
!strict

// Host-provided symbols are declared, not defined. The declaration goes to
// the checker and LSP; the host still has to register the runtime value.
extern fn read_score(player: string): number
extern const MAX_SCORE: number

// Multi-return errors, with `?` propagation for fallible lookups.
fn get_score(scores, name)
    return scores<name>?
end

let scores = &< Ada = 10 >
let value, err = get_score(scores, "Grace")
if err then print(err) end
```

---

## Build

Requires `git`, `make`, and a C compiler: `gcc` or `clang` on Linux/macOS, **LLVM-MinGW (clang)** on Windows (MSVC is not supported; the dispatch loop uses labels-as-values).

```bash
git clone https://github.com/EmeraldStudiosG/magnesium-showcase.git
cd magnesium-showcase
make                       # builds `magnesium` and `mt`
magnesium --version
```

One-line installers also exist for Linux/macOS and Windows PowerShell. See [`docs/src/getting-started/installation.md`](docs/src/getting-started/installation.md). `make install PREFIX=...` lays down the interpreter, `mt`, the shared library (`libmagnesium.{so,dylib,dll}`), and the C header (`magnesium.h`).

### The `mt` toolchain manager

```bash
mt install              # core runtime + shared library + headers + VS Code extension
mt install rust         # Rust bindings (magnesium-sys + safe magnesium crate)
mt install cpp          # C++17 header-only bindings
mt install csharp       # .NET 8 bindings via P/Invoke
mt install extension    # VS Code extension only
mt list                 # show installed components
```

Default prefix is `~/.magnesium/`; override with `MAGNESIUM_PREFIX` or `mt --prefix=<path> install`.

---

## Embedding from a host language

The C API is the low-level boundary:

```c
typedef Value (*NativeFn)(VM *vm, int arg_count, Value *args);
void vm_register_native(
    VM *vm, const char *name, NativeFn function, int arity,
    void *userdata, void (*userdata_finalizer)(void *)
);
```

Three layers above it: native functions (registered by the host), native handles (for host-owned resources like file handles, scene nodes, or ECS handles, with optional finalizers), and raw numeric FFI for narrow C function-pointer calls.

**Rust**: `magnesium-sys` mirrors every C type as `#[repr(C)]` (1,103 lines of `types.rs`) and provides three build modes via `build.rs`: vendored (default; compiles the C source in-tree via the `cc` crate), `system` (pkg-config), and `MAGNESIUM_REPO_ROOT` (developer mode). The safe `magnesium` crate wraps it: `Vm` holds a `NonNull<sys::VM>` and is `!Send + !Sync` by construction; host errors surface as `HostError::InteriorNul`; native-handle data is boxed with an `extern "C" fn drop_box<T>` finalizer. 47 binary tests + 28 in-process integration tests + 16 unsafe raw-FFI tests.

> **Note (Windows):** The Rust library builds cleanly with LLVM-MinGW on `x86_64-pc-windows-gnu`. Running `cargo test` or `cargo run --example` additionally requires a full mingw-w64 GCC installation, because the `windows-gnu` target spec hardcodes `-lgcc -lgcc_eh` from the GCC runtime. On Linux and macOS, `cargo build` and `cargo test` work out of the box.

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

**C++**: Header-only (`#include <mg/magnesium.hpp>`), C++17, built with CMake. `mg::Value` is `static_assert(sizeof(Value) == sizeof(uint64_t))`-checked. `mg::Vm` supports move semantics; native functions register via `std::function`-backed trampolines; native methods are template-parameterized by the host type so `set_native_handle_method_fn<Entity>` reuses a single `MethodClosure<T>` specialization.

**C#**: .NET 8, P/Invoke, `AllowUnsafeBlocks`. `Value` is a `readonly struct` over a `ulong`. Native delegates are retained for the VM lifetime, managed exceptions are contained at the reverse P/Invoke boundary, and `GcRoot` keeps host-held values alive across VM collections. `~Vm()` is the safety net for unmanaged lifetime.

Full embedding reference: [`docs/src/reference/embedding.md`](docs/src/reference/embedding.md).

---

## Tooling

> **Note:** The language server and VS Code extension are a work in progress and may have issues. They are usable but not yet at feature parity with the core language.

**Language server** (`src/lsp.c`, 1,756 lines) is built into the interpreter itself: `magnesium --lsp` boots an LSP server over stdio. It implements 14 methods (initialize, didOpen/didChange/didClose, completion, hover, signatureHelp, documentSymbol, definition, rename, formatting, semanticTokens/full, shutdown, exit) with **no JSON library**. Base protocol framing is hand-written. Completion returns Markdown docs from 14 hand-curated `LspItem` tables covering keywords, builtins, types, and every stdlib module.

Diagnostics spawn a fresh VM, compile the open document, and parse the compiler's stderr line-by-line into typed `Diagnostic` objects; `didChange` immediately republishes on every keystroke.

**VS Code extension** (`lsp/magnesium-vscode/`) is a thin `vscode-languageclient` wrapper (~150 lines of TypeScript). It ships a TextMate grammar covering interpolated strings, `&<...>` dict literals, `@globals`, struct/enum declarations, and the `?` fallible-lookup operator; a 35-token Magnesium Dark theme; auto-indent rules that recognize the language's `end`-terminated blocks; and a status-bar item that shows error count live.

**`scripts/lsp_smoke.py`** is an end-to-end JSON-RPC integration test that opens a document exercising structs, `extern fn`, dict lookup, and `math.sqrt`, then verifies all 10 advertised LSP capabilities round-trip, including strict-mode type diagnostics (`"Expected number"`, `"Cannot call number"`) published after a `didChange` to a deliberately-wrong `!strict` source.

---

## Testing and benchmarks

```bash
make test                # 59 .mg tests + CLI/native-API regressions + bytecode fuzzer
make test-verbose        # same, with per-test diff
make test-ubsan         # full suite under UBSan with halt_on_error=1
make test-asan          # targeted ASan/leak smoke test
make test-asan-full     # full suite under ASan + leak detection + UBSan
make bench              # strict 16-benchmark Magnesium / Lua 5.4 / CPython gate
```

The **test harness** (`scripts/run_tests.sh`) runs every `tests/test_*.mg`, captures combined stdout+stderr, strips CRLF for cross-platform stability, and asserts exact-output match against `tests/expected/<name>.expected`. Negative tests are written to fail at runtime; the assertion is on error *text*, not exit code, so deterministic error messages are part of the spec.

The **bytecode fuzzer** (`scripts/run_bytecode_tests.sh`) compiles two valid `.mgc` files, then mutates ten specific fields: magic bytes, trailing bytes, truncation, register count forced to 257, invalid opcodes, invalid register indices, invalid value tags, invalid constant indices, invalid jump targets. It asserts the loader rejects each variant with `Could not load bytecode` before executing any code.

The **benchmark suite** covers both source and bytecode modes across Magnesium, Lua 5.4, and CPython 3. It first checks equivalent output, stages the runtime and sources on the native temporary filesystem, performs one warmup, and measures five runs. The release gate compares medians and exits nonzero unless Magnesium is strictly faster than both reference runtimes for every benchmark in both modes. Sixteen benchmarks: `fib35`, `binary_trees` (depth-18 GC pressure), `sieve` (1M-element array), `mandelbrot` (400x400), `dict_bench` (250k insert/lookup), `call_loop` (10M function calls), `closure_loop` (5M upvalue mutations), `object_fields` (1M struct-field dispatches), `gc_alloc` (50kx50 arrays), `fallible_lookup` (3M `?` operator), `native_len` (5M native calls), `coroutine_switch` (200k resume/yield), and four more. `Dockerfile.bench` provides a clean Ubuntu 24.04 environment with Lua 5.4, CPython, and the low-overhead timeout dependency.

---

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | Interpreter, compiler, VM, GC, lexer, parser, type checker, serializer, LSP server, FFI glue. ~17k lines of C. |
| `bindings/rust/` | `magnesium-sys` (raw `repr(C)` mirror) + `magnesium` (safe `NonNull<VM>` wrapper). |
| `bindings/cpp/` | Header-only C++17 wrapper; CMake-built `magnesium-sys` static lib. |
| `bindings/csharp/` | .NET 8 P/Invoke bindings with hand-rolled marshalling. |
| `lsp/magnesium-vscode/` | VS Code extension: TextMate grammar, dark theme, language client. |
| `docs/` | mdBook source for *The Magnesium Book*. |
| `examples/` | Sample `.mg` programs including a full feature showcase. |
| `tests/` + `tests/expected/` | 59 end-to-end `.mg` tests with exact expected outputs. |
| `benchmark/` | 16 cross-language benchmarks in source and compiled form. |
| `scripts/` | `run_tests.sh`, `run_bytecode_tests.sh`, `run_bench.sh` / `.ps1`, `serve_docs.sh` / `.ps1`, `lsp_smoke.py`. |
| `Makefile`, `install.sh`, `install.ps1` | Build, installers. |
| `Dockerfile.bench` | Clean-room benchmark environment. |

---

## Status

Magnesium is at v1.1.1 and actively maintained. The docs describe the current implementation unless a section explicitly states it is a design note. See [`docs/src/CHANGELOG.md`](docs/src/CHANGELOG.md) for release notes.

## Contributors

- **Ismail Kuzey Pulat** - Developed and mapped out the entire codebase from scratch.
- thekingofdespair - Created the mascot Flow.
- regretted - Testing the language.
- The authors of the various research papers and institutions whose work on VM and interpreter performance informed Magnesium's design.

## License

MIT. See [`LICENSE`](LICENSE). Copyright (c) 2026 Ismail Kuzey Pulat.
