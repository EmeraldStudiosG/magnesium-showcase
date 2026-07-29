# Changelog

## v1.1.1 - 2026-07-30

### Repository links

- Replaced the old repository name `magnesium-dev` with the new name `magnesium-showcase` in `install.sh`, `install.ps1`, build instructions, the `mt` toolchain manager, documentation links, and Visual Studio Code extension links.

## v1.1.0 - 2026-07-29

### Compatibility

- Changed the public `CallFrame`, `ObjDict`, `ObjNative`, `ObjFFI`, and `VM` layouts. Native clients that use these layouts must rebuild.
- Changed `array_set(ObjArray *, int, Value)` to `array_set(VM *, ObjArray *, int, Value)` so writes can run the GC barrier.
- Changed the low-level public `new_native` and `new_ffi` name arguments from `const char *` to GC-managed `ObjString *` values.
- Changed the Windows shared library name to `libmagnesium.dll`. Windows dynamic consumers must define `MG_USE_SHARED`.
- Changed `vm_set_global_value` to return true for any successful write, including replacement of an existing global.
- Changed `and` and `or` type inference to join operand types instead of always returning `bool`.
- Changed numeric output to use the shortest round-trippable form and to print integral doubles without scientific notation.
- Tightened type checking for shapes, dict keys, struct fields, exports, and range loop variables.
- Kept the bytecode magic, opcode numbers, and opcode count unchanged. Legacy batch opcodes still decode, but the compiler no longer emits them.

### Frontend and compiler

- Fixed physical line tracking for escaped newlines and interpolated strings.
- Parsed interpolation expressions with the normal parser, including nested braces, quoted strings, and escaped braces.
- Separated lexical identifier resolution from explicit `@global` resolution.
- Included built-ins, modules, extern declarations, and exports in global collection and type propagation.
- Added function declaration expression types and function body checking.
- Checked shape compatibility by required field name and type; checked dict literals field by field when passed as shapes.
- Reported unknown structs, unknown fields, incompatible exports, invalid dict keys, and unsupported two-variable range loops.
- Fixed chained comparison parsing and single-evaluation semantics for compound field and index assignments.
- Fixed shared AST ownership that could double-free compound assignment nodes.
- Fixed multi-value register reservation and null filling for missing, non-call, and fallible results.
- Kept division and modulo by zero as runtime errors instead of folding them to zero.
- Replaced compiler aborts and operand truncation with diagnostics for register, local, upvalue, constant, argument, return, and struct field limits.
- Disabled the fused fallible field-update form when its field constant does not fit the instruction operand.
- Fixed temporary register restoration, per-iteration cleanup, defer execution, and captured upvalue closing on non-local exits.
- Rooted active compiler functions, global names, imported functions, and object constants across collection.
- Added write barriers when promoted functions receive names, source names, or object constants.
- Removed compiler source-shape recognition for the legacy batch opcodes.

### VM, GC, objects, and standard library

- Reduced the initial stack from 65536 values to 1024 values and retained dynamic growth.
- Fixed frame slots and open upvalue pointers after stack relocation.
- Restored caller stack bounds, cleared dead return windows, and closed transient upvalues on every VM exit path.
- Added `vm_root_value`, `vm_root_set`, `vm_root_get`, and `vm_unroot_value`; minor and major GC trace these host roots.
- Copied host callback arguments into stable GC-visible snapshots for each callback.
- Stored native and FFI names as GC-managed strings instead of borrowed pointers.
- Fixed major/minor collection ordering and stopped minor GC from traversing unrelated old-to-old edges.
- Added missing tracing and barriers for native objects, coroutine snapshots, arrays, upvalues, dict keys, compiler objects, and deserialized values.
- Cleared weak caches and unmarked interned strings before reclamation.
- Made the integer-to-string cache lazy and retained dictionary-owned keys in the monomorphic cache.
- Stored monomorphic dictionary hit values directly, updated them on replacement, and invalidated them on deletion, clearing, and resize.
- Added allocation and length overflow checks to stacks, arrays, strings, paths, tasks, defers, coroutines, and structs.
- Rejected NaN, infinity, fractions, and out-of-range values where integer indices or counts are required.
- Fixed `INT32_MIN % -1`, floating-point modulo, and non-finite or overflowing range counts.
- Fixed full-string and `ERANGE` checks in numeric conversion.
- Fixed bounds handling in string and array operations, including array self-extension.
- Preserved embedded NUL bytes in `fs.read`; fixed directory detection in `fs.exists` and overflow checks in path joining.
- Fixed coroutine snapshots, stack accounting, terminal upvalues, yielded state, task cancellation, and timeout propagation.
- Cancelled and joined child tasks before VM teardown and prevented duplicate child error output.
- Preserved structured error kind, message, file, line, function, trace, and hint data through rope-safe access.
- Made runtime and string introspection null-safe; `vm_string_length` now reports logical rope length.

### Bytecode

- Added graph preflight before opening or truncating a bytecode destination.
- Rejected malformed pointers, counts, capacities, register metadata, unsupported constants, cyclic functions, and cyclic ropes during save.
- Added depth, count, string, rope, instruction, constant, and aggregate allocation limits.
- Preserved an existing destination when save preflight fails.
- Checked bytecode read and write counts, input bounds, and the save-path close result.
- Rejected truncated input, trailing data, unknown tags, and zero-count allocation edge cases.
- Validated instruction boundaries before control flow.
- Rejected standalone auxiliary words, truncated multi-word instructions, bad opcodes, bad operands, invalid captures, invalid fallthrough, and jumps into metadata.
- Rooted and barriered loaded functions, strings, constants, closures, and metadata across allocation.

### Language server

- Read exact `Content-Length` payloads and replaced fixed response buffers with dynamic buffers.
- Added bounds checks for truncated escapes, Unicode escapes, and surrogate pairs.
- Fixed JSON escaping and unescaping for URI, source text, diagnostics, and responses.
- Converted positions between UTF-8 byte offsets and LSP UTF-16 code units.
- Fixed Unicode positions for hover, definition, rename, completion, diagnostics, and semantic tokens.
- Fixed `didOpen`, `didChange`, and `didClose` document ownership and cleanup.
- Fixed long diagnostics, large formatting responses, malformed request handling, and large-document behavior.
- Expanded smoke coverage for malformed JSON, escaped input, Unicode positions, formatting, completion, hover, definition, rename, symbols, and document lifecycle.

### Embedding and bindings

- Added `MG_API`, `MG_BUILD_SHARED`, and `MG_USE_SHARED` handling for the exported C surface.
- Added host-root, error-value, native-userdata, stack, allocation, and rope-safe string accessors.
- Added native C API coverage for roots, barriers, callback arguments, userdata, globals, errors, strings, stack growth, coroutines, tasks, bytecode, files, and teardown.
- C++ now shares VM lifetime state across values, errors, callbacks, and roots; errors own their text and location snapshots.
- C++ roots are move-safe and become inert after VM release; typed native handles retain closure state until finalization.
- Rust raw layouts and declarations now match the C ABI; safe wrappers retain VM ownership and reject cross-VM values.
- Rust native callbacks use retained closure userdata, contain panics, and preserve error and return ownership.
- Rust vendored/system builds and Windows library naming were fixed; both Rust crates are now `publish = false`.
- C# now has explicit package version `1.1.0`, host roots, global access, native registration, error snapshots, rope-safe strings, and allocation metrics.
- C# now retains delegates and callback contexts until finalization and contains managed exceptions at reverse P/Invoke boundaries.

### Build, toolchain, CI, and tests

- Corrected platform flags and names for Windows DLLs, Darwin dylibs, POSIX PIC, `mt.exe`, test executables, and `ws2_32` linkage.
- Added sanitized native API binaries and ran sanitizer modes sequentially to avoid output collisions.
- Fixed Windows Make test and sanitizer recipes to use PowerShell runners, Windows paths, and platform-supported sanitizer options.
- Updated install targets to build and install the interpreter, shared library, toolchain, header, and pkg-config file with platform-correct names.
- Updated `install.sh` to require `make`, select one C compiler, and pass it to build and install.
- Fixed `mt` argument, prefix, quoting, path, package, binding, extension, install, and uninstall handling on Windows and POSIX.
- Fixed the Windows `mt remove_tree` long-path heap overflow by measuring the quoted command before allocation.
- Added refusal checks for empty, dot, navigation, root, drive-root, and current-directory uninstall targets.
- Added Linux and Windows CI jobs for core builds, source tests, native API tests, bytecode tests, and release benchmark gates.
- Expanded the Windows source, bytecode, and benchmark runners with timeouts, abnormal-exit checks, and output validation.
- Added cancellation regression coverage for range entry, fused loop backedges, and conditional negative-jump backedges.
- Fixed PowerShell process environment normalization so benchmark launches return one `ProcessStartInfo` object.
- Added generated embedded-NUL source coverage.
- Removed LuaJIT from the benchmark image, added Perl timeout support, and added a `luac` smoke check.
- Synchronized the VS Code extension lockfile with its `3.6.2` manifest.
- Added LF rules for shell scripts, Makefiles, and Dockerfiles.

### Performance and benchmark integrity

- Added guarded runtime fusion for comparison plus `TESTJMP` and local add/subtract plus `LOOP`.
- Split dictionary cache hits from cold probe paths and loaded monomorphic hit values without a second entry lookup.
- Outlined uncommon propagated field-update lookup, type, concatenation, and error paths.
- Moved numeric range cancellation polling to range entry and loop backedges while retaining bounded cancellation latency.
- Direct-threaded existing numeric range and loop instructions for general local add and subtract updates.
- Direct-threaded successful range-loop continuations to existing immediate comparison handlers.
- Checked negative `JMP` backedges for cancellation and direct-threaded them to existing range-loop and immediate comparison handlers.
- Direct-threaded adjacent existing immediate and three-input arithmetic handlers.
- Executed guarded adjacent `MUL`, `MULI`, `MULADD`, `SUBADD`, comparison, and loop sequences without changing emitted bytecode; type and layout mismatches use the original handlers.
- Added an inline reciprocal remainder path for small nonzero immediate integer divisors with signed remainder semantics and a general fallback.
- Used remaining-word checks before arithmetic lookahead and required the expected `JMP` word before consuming comparison sequences.
- Fused adjacent instance `GETFIELD`, numeric `ADD`, and matching `SETFIELD` instructions after a field-cache hit, with the original handlers retained as the fallback.
- Added direct numeric array `GETINDEX` and `SETINDEX` paths with generic fallbacks.
- Corrected integer accumulator and mixed-number handling in existing local arithmetic fast paths.
- Added dispatch-table size and opcode bounds checks; made the Clang computed-goto condition explicit.
- Kept loop cancellation checks on fused paths.
- Reduced startup work through the smaller initial stack and lazy integer-to-string cache.
- Removed compiler emission of the exact-shape batch opcode families without adding replacement opcodes.
- Kept all 48 benchmark source files content-equivalent to `HEAD`; no benchmark name, path, or workload detection was added.
- Benchmark runners now use isolated source copies, validate every warmup and timed result, reject stderr/nonzero/timeout, and compare five-run medians.
- The release gate requires Magnesium to beat Lua 5.4 and CPython for all 16 workloads in source and bytecode modes.

### Validation

- Windows and POSIX source/CLI suites: 60 of 60 passed.
- Windows and POSIX malformed-bytecode suites: 10 of 10 passed.
- Native C API, ASan, and UBSan suites passed.
- Rust: 118 tests passed; `rustfmt` and `clippy` passed.
- C++: 50 of 50 passed on Windows and POSIX.
- C#: 51 of 51 passed.
- Windows and POSIX LSP smoke suites passed.
- Windows and POSIX release benchmark gates passed all 16 workloads in source and bytecode modes.

## v1.0.0

Initial release.
