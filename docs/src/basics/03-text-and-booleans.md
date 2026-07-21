# Text and Booleans

## Strings

A string is text in quotes.

```magnesium
let greeting = "Hello"
print(greeting)
```

### Joining Strings

Use `+`:

```magnesium
let first = "Magnesium"
let last = "Language"
print(first + " " + last)  // Magnesium Language
```

### Interpolated Strings

Prefix with `_` to embed variables:

```magnesium
let name = "Alice"
let msg = _"Hello, {name}!"
print(msg)  // Hello, Alice!
```

```magnesium
let x = 10
let y = 20
print(_"The sum of {x} and {y} is {x + y}")  // The sum of 10 and 20 is 30
```

## Booleans

Two values: `true` and `false`.

```magnesium
let is_raining = true
let is_sunny = false
```

### Comparisons

| Symbol | Meaning |
|--------|---------|
| `==` | Equal to |
| `!=` | Not equal to |
| `<` | Less than |
| `>` | Greater than |
| `<=` | Less than or equal |
| `>=` | Greater than or equal |

```magnesium
let a = 5
let b = 10
print(a == b)  // false
print(a < b)   // true
print(a >= b)  // false
```

### Logical Operators

```magnesium
let age = 25
let has_license = true

print(age >= 16 and has_license)  // true
print(age >= 16 or false)         // true
print(not has_license)            // false
```

## Null

`null` means "no value".

```magnesium
let enemy = null
print(enemy)  // null
```

`null` is also the default result for plain missing lookups:

```magnesium
let player = &< name = "Ada" >
print(player<"level">)     // null
```

Later, the error-handling chapter shows how to use `?` when a missing lookup should become an error.

![Flow is happy](../assets/images/FlowHappy.png)

## Common Mistakes

![Flow is concerned](../assets/images/FlowAngry.png)

| Wrong | Right |
|-------|-------|
| `=` for comparison | `==` for comparison |
| `x = 5` (assignment) | `x == 5` (ask if equal) |

## Truthiness

Conditions use truthiness:

```magnesium
if null then
    print("does not run")
end

if false then
    print("does not run")
end

if "hello" then
    print("strings are truthy")
end
```

`false` and `null` are falsey. Other values are truthy.

## Try It

1. Join two strings with a space.
2. Use `_"..."` with variables.
3. What is `5 == 5`? `5 == 6`?
4. Create `temperature = 75`. Print whether it's hot (`> 80`), cold (`< 60`), or neither.

→ [Making Decisions](04-making-decisions.md)
