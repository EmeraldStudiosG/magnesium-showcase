# Syntax Reference

This page describes the syntax implemented by the current compiler.

## Comments

```mg
// Line comment

//[[
Block comment
]]
```

## File Directives

Directives are real syntax at the top level. They are not comments.

```mg
!strict
```

```mg
!nocheck
```

No directive means dynamic mode. `!strict` enables static diagnostics. `!nocheck` suppresses static diagnostics for the file.

## Bindings

```mg
let x = 10
const limit = 100
let a, b = pair()
```

`let` creates a mutable binding. `const` creates a binding that cannot be reassigned.

Global declarations use `@`:

```mg
let @shared = 1
@shared = 2
```

## Optional Type Annotations

Annotations are optional and valid in all checking modes:

```mg
let score: number = 10
const name: string = "Ada"

fn add(a: number, b: number): number
    return a + b
end
```

In dynamic mode, annotations are ignored by runtime execution. In `!strict`, they are checked.

## Type Aliases

Type aliases are static-only:

```mg
type Scores = &<string, number>
type Callback = fn(number): null
```

Data shape aliases describe dict-shaped data:

```mg
type PlayerData = &<
    name: string,
    score: number
>
```

Shapes are open. Extra fields are allowed.

## Extern Declarations

Extern declarations describe host-provided globals for the checker and LSP.

```mg
extern fn read_score(): number
extern fn save_score(score: number): null
extern const MAX_SCORE: number
```

Extern declarations emit no bytecode. The host must provide the actual runtime value.

## Runtime Value Syntax

| Value | Syntax |
| --- | --- |
| Number | `42`, `3.14` |
| String | `"hello"`, `_"Hello, {name}"` |
| Bool | `true`, `false` |
| Null | `null` |
| Array | `[1, 2, 3]` |
| Dict | `&< name = "Ada", hp = 100 >` |
| Struct literal | `Point { x = 1, y = 2 }` |
| Function literal | `fn(x) return x end` |
| Error | `Err("Kind", "message")` |

## Operators

```mg
+  -  *  /  %          // arithmetic
==  !=  <  >  <=  >=   // comparison
and  or  not           // logical
!                      // logical not
..  ..=                // exclusive and inclusive ranges
?                      // fallible propagation
```

## Control Flow

```mg
if x > 5 then
    print("big")
elseif x > 0 then
    print("small")
else
    print("zero")
end
```

```mg
loop
    if done then
        break
    end
end
```

```mg
for i in 0..10
    print(i)
end

for i in 0..=10
    print(i)
end
```

Collection iteration:

```mg
for item in items
    print(item)
end

for key, value in settings
    print(key, value)
end
```

## Functions

```mg
fn add(a, b)
    return a + b
end
```

With optional annotations:

```mg
fn divide(a: number, b: number): number
    return a / b
end
```

Methods attach to a struct name:

```mg
fn Player.move(self, nx, ny)
    self.x = nx
    self.y = ny
end
```

## Arrays

Arrays use `[]`.

```mg
let arr = [1, 2, 3]
arr[0]
arr[0]?
push(arr, 4)
array.pop(arr)
```

Indexes are zero-based.

## Dicts

Dict literals and lookup are intentionally separate from arrays:

```mg
let d = &< a = 1, b = 2 >
d<"a">
d<"a">?
d<"c"> = 3
```

`[]` is not dict syntax. Dict lookup is `dict<key>`.

## Structs

Struct declarations list field names and end with `end`:

```mg
struct Point
    x
    y
end
```

Construction:

```mg
let p = Point(1, 2)
let q = Point { x = 1, y = 2 }
```

Field access:

```mg
p.x
p.x = 10
```

## Modules

```mg
import "math_utils"
import "math_utils" as math_utils

export fn add(a, b)
    return a + b
end
```

Imports return an export dict. `as name` binds that export dict to a local or global according to normal scope rules.

## Fallible Propagation

```mg
fn get_value(dict, key)
    return dict<key>?
end
```

`?` supports:

```mg
dict<key>?
array[index]?
string[index]?
fallible_call()?
```

On success it unwraps the value. On failure it returns `null, err` from the current function.

## Try/Catch

```mg
try
    let port = dict.require(config, "port")?
    print(port)
catch err
    print(err.report)
end
```

## Defer

```mg
defer cleanup()
```

Deferred calls run when the current scope exits.

## Enums

```mg
enum Status { Ok, Error, Pending }
let s = Status<"Ok">
```
