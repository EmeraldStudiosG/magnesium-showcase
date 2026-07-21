# VM Architecture

Overview of Magnesium internals. You don't need to know this to use the language.

## Register-Based VM

Most scripting languages use stack-based VMs. Magnesium uses 256 local registers instead.

- Fewer instructions per operation
- Better cache locality
- No push/pop overhead

The compiler assigns temporary values and locals to registers. A function call writes return values back into the caller's destination registers. If the caller asks for more return values than the callee returns, the VM fills the remaining destination registers with `null`.

## NaN Boxing

All values fit in 64 bits using quiet-NaN tagging:
- **Numbers**: Raw IEEE 754 double
- **Integers**: Tagged with `TAG_INT`
- **Booleans/Null**: Tagged sentinels
- **Pointers**: Object addresses in NaN payload space

Pure number math requires no type checks.

## Garbage Collector

Generational, tri-color mark-and-sweep:
- Young objects collected frequently
- Survivors promoted to old generation
- Runs incrementally at `next_gc` threshold

## Inline Caches

Four direct-mapped caches for hot-path lookups:
- **Global** (128 entries): `OP_GETGLOBAL`/`OP_SETGLOBAL`
- **Field** (128 entries): Struct field access
- **Method** (128 entries): Method dispatch
- **Dict** (256 entries): Dict string-key lookups, with an additional per-instance monomorphic slot checked first

Miss = fallback to hash table. Caches are cleared on major GC only; minor GC leaves them intact.

## Bytecode Format

Source compiles to `.mgc` files:
```bash
magnesium build script.mg     // Compile to script.mgc
magnesium script.mgc          // Run bytecode
```

The bytecode header has a magic/version number. When opcodes or validation rules change, old `.mgc` files must be rebuilt.

The bytecode validator checks register spans, constant references, jump targets, closure metadata, and opcode-specific operands before running loaded bytecode.

## Execution Pipeline

```
Source → Lexer → Parser → AST → Compiler → Bytecode → VM
                              ↑___Optimizer_____|
```

1. **Lexer**: Token stream
2. **Parser**: Recursive descent → AST
3. **Compiler**: AST walker → register bytecode
4. **VM**: Computed-goto dispatch

## Error Propagation Bytecode

Postfix `?` on a lookup compiles to a fallible lookup opcode instead of a normal lookup:

```magnesium
return dict<key>?
```

On success, the opcode writes the found value to the destination register. On failure, it writes `null, err` and returns from the current function immediately.

Plain lookup still uses the nullable lookup opcode:

```magnesium
dict<key>       // value or null
dict<key>?      // value or propagated error
```

This keeps the fast nullable path simple and makes fallible behavior explicit in bytecode.

## FFI

Host integration has three layers:

- native functions registered by the host
- native handles for host-owned resources
- raw numeric FFI for narrow C function-pointer calls

Scripts can describe host-provided symbols with `extern` declarations. Those declarations feed the checker and LSP only; the host registration is still the runtime source of truth.

Script-level C header/library imports are experimental and should not be treated as the primary embedding path.
