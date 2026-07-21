# Global Functions

Functions available without importing or using a namespace.

| Function | Args | Returns | Description |
|----------|------|---------|-------------|
| `print(...)` | `any...` | - | Print to stdout |
| `len(x)` | `array/string/dict` | `number` | Length |
| `push(arr, val)` | `array, any` | - | Append to array |
| `type(x)` | `any` | `string` | Type name |
| `tostring(x)` | `any` | `string` | Convert to string |
| `tonumber(x)` | `string/number` | `number` | Convert to number |
| `Err(kind, message, hint?)` | `string, string, string?` | `error` | Create structured error |
| `Error(kind, message, hint?)` | `string, string, string?` | `error` | Alias for `Err` |
| `assert(cond, msg?)` | `bool, string?` | - | Fatal if false |
| `error(msg)` | `string` | - | Fatal error |
| `input(prompt?)` | `string?` | `string` | Read stdin line |

## print(...)

Print values to stdout followed by a newline. Multiple arguments are separated by tabs.

```magnesium
print("hello")
print("score", 10)
print("x =" + tostring(42))
```

## len(x)

Return the length of a string, array, or dict.

```magnesium
print(len("hello"))    // 5
print(len([1,2,3]))    // 3
print(len(&<a=1>))     // 1
```

## push(arr, val)

Append `val` to the end of `arr`.

```magnesium
let items = ["sword"]
push(items, "shield")
print(items)  // ["sword", "shield"]
```

## type(x)

Return the type name of `x` as a string.

```magnesium
print(type(42))      // number
print(type("hi"))    // string
print(type([]))      // array
print(type(null))    // null
print(type(print))   // function
```

## tostring(x)

Convert any value to a string.

```magnesium
let score = 10
print("score: " + tostring(score))
```

## tonumber(x)

Convert a string or number to a number. Strings that cannot be parsed return `0`.

```magnesium
print(tonumber("42"))    // 42
print(tonumber("3.14"))  // 3.14
print(tonumber(7))       // 7
```

## Err(kind, message, hint?)

Create a structured error value. `Error` is an alias for `Err`.

```magnesium
let err = Err("KeyError", "Key not found: name", "Check dict.has first.")
print(err.kind)      // KeyError
print(err.message)   // Key not found: name
print(err.hint)       // Check dict.has first.
print(err.report)    // formatted diagnostic
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

Use error values with Lua-style multi-return:

```magnesium
return null, Err("ConfigError", "Missing port")
```

## assert(cond, msg?)

Halt execution if `cond` is falsey. Optional `msg` replaces the default message.

```magnesium
fn divide(a, b)
    assert(b != 0, "division by zero")
    return a / b
end
```

`assert` is for programmer mistakes. For recoverable failures, return `Err` instead.

## error(msg)

Halt execution with a fatal error message.

```magnesium
fn validate(data)
    if data == null then error("data is required") end
    return data
end
```

`error` is unrecoverable. Use `Err` and multi-return for expected failures.

## input(prompt?)

Read one line from stdin. Optional `prompt` is written to stdout without a newline.

```magnesium
let name = input("Name: ")
print("Hello, " + name)
```

`io.read_line()` is the namespace form of the same function.

---

`print`, `len`, `push`, `type`, `tostring`, and `tonumber` also have internal `__builtin_*` names used by compiler lowering. User code should call the normal public names.
