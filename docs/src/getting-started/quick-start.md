# Quick Start

This is the whole language in one fast pass. Do not worry about memorizing it; the tutorial chapters explain each part slowly.

## Hello World

```magnesium
print("Hello, World!")
```

Run it with:

```bash
magnesium hello.mg
```

## Variables

Use `let` for normal variables:

```magnesium
let name = "Ada"
let score = 100
let alive = true
```

Normal variables are local to their scope. Global declarations use `@`:

```magnesium
let @difficulty = "hard"
print(@difficulty)
```

## Values

Magnesium has numbers, strings, booleans, `null`, arrays, dicts, structs, functions, and native host values.

```magnesium
let n = 42
let title = "Engineer"
let ready = false
let missing = null
let items = ["sword", "shield"]
let config = &< fullscreen = true, width = 1280 >
```

## Operators

```magnesium
let total = 10 + 5 * 2
let wrapped = 17 % 5
let same = total == 20
let ok = ready or score > 50
```

String concatenation uses `+`:

```magnesium
let message = "Score: " + tostring(score)
```

Interpolated strings use `_""`:

```magnesium
let message = _"{name} has {score} points"
```

## Conditions

```magnesium
if score >= 90 then
    print("A")
elseif score >= 80 then
    print("B")
else
    print("Keep going")
end
```

`false` and `null` are falsey. Other values are truthy.

## Loops

Use `loop` when you control the exit manually:

```magnesium
let count = 0
loop
    count = count + 1
    if count == 3 then break end
end
```

Use ranges for numeric loops:

```magnesium
for i in 1..5
    print(i)       // 1, 2, 3, 4
end

for i in 1..=5
    print(i)       // 1, 2, 3, 4, 5
end
```

Use `for item in array` or `for key, value in dict` for collections:

```magnesium
for item in items
    print(item)
end

for key, value in config
    print(key + " = " + tostring(value))
end
```

## Functions

```magnesium
fn add(a, b)
    return a + b
end

let result = add(3, 5)
print(result)
```

Functions can return multiple values:

```magnesium
fn split_name(full)
    return "Ada", "Lovelace"
end

let first, last = split_name("Ada Lovelace")
```

## Errors

Magnesium does not use exceptions for normal recoverable failure. Use returned values:

```magnesium
fn divide(a, b)
    if b == 0 then return null, "Division by zero" end
    return a / b
end

let value, err = divide(10, 0)
if err then
    print("Error: " + err)
end
```

For fallible lookups, postfix `?` removes the repeated error plumbing:

```magnesium
fn get_score(scores, name)
    return scores<name>?
end

let scores = &< Ada = 10 >
let score, err = get_score(scores, "Grace")
if err then print(err) end
```

On success, the value is returned. On failure, Magnesium returns `null, err` from the current function.

## Collections

Arrays are ordered and zero-indexed:

```magnesium
let items = ["a", "b", "c"]
push(items, "d")
print(items[0])
```

Dicts map string keys to values:

```magnesium
let player = &< name = "Ada", hp = 100 >
print(player<"name">)
player<"hp"> = 90
```

Structs give named fields a fixed shape:

```magnesium
struct Point
    x
    y
end

let p = Point { x = 1, y = 2 }
print(p.x)
```

## Modules

`math_utils.mg`:

```magnesium
export fn square(x)
    return x * x
end
```

`main.mg`:

```magnesium
import "math_utils"
print(math_utils.square(5))
```

Next: [What Is Programming?](../basics/01-what-is-programming.md).
