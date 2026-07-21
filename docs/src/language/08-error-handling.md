# Error Handling

![Flow is concerned](../assets/images/FlowAngry.png)

Magnesium uses Lua-style multiple returns for recoverable failures:

```magnesium
return value, err
```

When the operation succeeds, `err` is `null`. When it fails, the value is usually `null` and `err` is an error object.

## Error Objects

Create an error with `Err(kind, message)` or `Err(kind, message, hint)`:

```magnesium
let err = Err("ConfigError", "Missing port", "Set config.port before starting.")
print(err.kind)     // ConfigError
print(err.message)  // Missing port
print(err.hint)     // Set config.port before starting.
```

An error also has `file`, `line`, `function`, and `report` fields. `report` is a formatted diagnostic string.

## Manual Returns

You can always write the full multiple-return form:

```magnesium
fn require_port(config)
    if !dict.has(config, "port") then
        return null, Err("KeyError", "Key not found: port")
    end

    return config<"port">
end
```

Callers bind both values:

```magnesium
let port, err = require_port(config)
if err then
    print(err.report)
end
```

If a function returns fewer values than the caller asks for, Magnesium fills the missing slots with `null`.

## The `?` Operator

![Flow is happy](../assets/images/FlowHappy.png)

`?` is shorthand for: if there is an `Err`, return it from the current function. It works on fallible lookups and fallible calls, and it only propagates actual `Err` objects. Other returned values are treated as ordinary values.

For lookups:

```magnesium
fn require_name(player)
    return player<"name">?
end
```

For calls:

```magnesium
fn load_port(config)
    let port = dict.require(config, "port")?
    return port
end
```

`dict.require`, `array.get`, and `string.at` are standard-library functions that return either a value or an `Err` object. Because native functions can participate in `value, err`, they compose with `?`.

## `try ... end`

Use `try ... end` when you want to capture a propagated error instead of returning it from the current function.

```magnesium
let value, err = try
    dict.require(config, "port")?
end
```

The expression returns two values. On success, `value` is the last expression in the block and `err` is `null`. On failure, `value` is `null` and `err` is the propagated error.

## `try ... catch`

Use `try ... catch` when you want to handle the error locally:

```magnesium
try
    let port = dict.require(config, "port")?
    print(port)
catch err
    print(err.kind)
    print(err.message)
end
```

The catch variable is a normal local name.

## Plain Lookup

Plain lookup still returns `null` for missing keys or indexes. Use plain lookup when `null` is acceptable. Use `?` when missing data should be an error.

| Expression | Plain result on failure | `?` result on failure |
| --- | --- | --- |
| `dict<key>` | `null` | returns `null, Err("KeyError", ...)` |
| `array[index]` | `null` | returns `null, Err("IndexError", ...)` |
| `string[index]` | `null` | returns `null, Err("IndexError", ...)` |
| `fallible_call()` | first return value | propagates second return value if it is an error |

## Designing Good Errors

Prefer stable kinds and specific messages:

```magnesium
return null, Err("ConfigError", "Missing database.host", "Set database.host in config.mg.")
```

Kinds should be short categories such as `KeyError`, `IndexError`, `TypeError`, `ConfigError`, or `ArgumentError`. Messages should say what failed. Hints should tell the caller what to do next.

## Try It

```magnesium
fn second_plus_one(items)
    let value = array.get(items, 1)?
    return value + 1
end

let good, good_err = second_plus_one([10, 20])
print(good)      // 21
print(good_err)  // null

let bad, bad_err = second_plus_one([10])
print(bad)           // null
print(bad_err.kind)  // IndexError
```

Next: [Modules](09-modules.md).
