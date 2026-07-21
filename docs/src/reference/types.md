# Types Reference

Magnesium is dynamically typed at runtime. Values carry their runtime kind, and normal scripts do not have to write type annotations.

Optional static types exist for tooling and strict checking. They are parsed in every mode, but they do not change runtime semantics or emitted bytecode.

## Runtime Values

### Number

Integers and floating-point values are both `number`.

```mg
let n = 42
let pi = 3.14159
```

The VM uses an internal integer representation for exact 32-bit integers and falls back to double precision for non-integers.

### String

Strings use double quotes and are immutable.

```mg
let s = "hello"
let msg = _"Hello, {name}!"
```

Use `+` for string concatenation:

```mg
print("hp: " + tostring(hp))
```

### Bool

Boolean values are `true` and `false`.

```mg
let ready = true
```

Only `false` and `null` are falsey.

### Null

`null` means no useful value.

```mg
let data = null
```

Functions that do not return a meaningful value return `null`.

### Array

Arrays are ordered, mutable, and zero-indexed with `[]`.

```mg
let items = ["a", "b", "c"]
print(items[0])      // a
push(items, "d")
print(len(items))    // 4
```

Use arrays when order matters.

### Dict

Dicts are dynamic string-keyed maps. Dict literals use `&< >`.

```mg
let settings = &< width = 1280, height = 720 >
print(settings<"width">)
settings<"fullscreen"> = true
```

Identifier-style literal keys become string keys:

```mg
let player = &< name = "Ada", hp = 100 >
print(player<"name">)
```

Dict lookup uses angle brackets so it cannot be confused with array indexing:

```mg
array[0]
dict<"key">
```

Magnesium does not have a public Lua-style table type. Use arrays, dicts, and structs directly.

### Struct

Structs are named fixed-field records owned by Magnesium code.

```mg
struct Point
    x
    y
end

let p = Point { x = 1, y = 2 }
print(p.x)
```

Structs are better than dicts when the fields are known by the program.

### Function

Functions are first-class callable values.

```mg
fn add(a, b)
    return a + b
end

let f = add
print(f(2, 3))
```

Functions can return multiple values:

```mg
fn pair()
    return "left", "right"
end

let a, b = pair()
```

### Error

Recoverable errors are structured values.

```mg
let err = Err("KeyError", "Key not found: name", "Check dict.has first.")
print(err.kind)
print(err.message)
print(err.hint)
print(err.report)
```

Error fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `kind` | `string` | Stable category |
| `message` | `string` | Human-readable failure |
| `hint` | `string/null` | Optional next step |
| `file` | `string` | Source file known to the VM |
| `line` | `number` | Source line |
| `function` | `string` | Function name or `script` |
| `report` | `string` | Formatted diagnostic |

Use `?` with fallible calls and lookups when you want to propagate `null, err`.

## Static Type Modes

A file has one of three checking modes:

| Directive | Meaning |
| --- | --- |
| no directive | Dynamic mode. Type annotations parse, but no type diagnostics are reported by default. |
| `!strict` | Static diagnostics are enabled for this file. |
| `!nocheck` | Static diagnostics are suppressed even if tooling or the host asks for checking. |

The directive must be written directly, not as a comment:

```mg
!strict
```

Do not write `--!strict`.

## Optional Annotations

Annotations are always optional:

```mg
let score: number = 10
const name: string = "Ada"

fn add(a: number, b: number): number
    return a + b
end
```

In dynamic mode, these annotations are metadata only. In `!strict`, the checker reports obvious mismatches.

## Static Type Syntax

Primitive static types:

```mg
number
string
bool
null
any
unknown
```

Array types:

```mg
let scores: [number] = [10, 20, 30]
```

Dict types:

```mg
let prices: &<string, number> = &<
    apple = 2,
    banana = 3
>
```

Function types:

```mg
type ScoreCallback = fn(number): null
```

`null` is the return type for functions with no useful result.

## `any` And `unknown`

`any` is an escape hatch. It can be used as anything.

```mg
let value: any = read_value()
value + 1        // allowed in strict mode
value()          // allowed in strict mode
```

`unknown` is safe opaque data. Anything can be assigned to `unknown`, but strict mode will not let you use it as a number, string, function, array, or dict without first converting or checking it.

```mg
let value: unknown = read_value()
value + 1        // strict error
value()          // strict error
```

## Data Shape Aliases

Data shape aliases describe dict-shaped data. They are static-only and do not create structs, classes, objects, or tables.

```mg
type PlayerData = &<
    name: string,
    score: number
>

let player: PlayerData = &<
    name = "Ada",
    score = 10,
    level = 2
>
```

Shapes are open by default. Required fields must exist, but extra fields are allowed. This matches dynamic data from config files, parsed payloads, and host APIs.

Use a struct instead when Magnesium owns the fixed runtime record:

```mg
struct Player
    name
    score
end
```

## Extern Declarations

`extern` declares host-provided symbols for the checker and LSP.

```mg
extern fn read_score(): number
extern fn save_score(score: number): null
extern const MAX_SCORE: number
```

Extern declarations emit no bytecode and do not create runtime values. The host must register or set the matching global before the script uses it.

## Runtime Type Inspection

Use the built-in `type(value)` when runtime logic needs to branch by value kind:

```mg
print(type(42))       // number
print(type("hi"))     // string
print(type([]))       // array
print(type(&<>))      // dict
```
