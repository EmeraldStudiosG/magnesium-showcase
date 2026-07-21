# Doing Things Over and Over

## Loop

Repeat until `break`:

```magnesium
let count = 0

loop
    print(count)
    count = count + 1
    if count >= 5 then
        break
    end
end
```

Output: `0` `1` `2` `3` `4`

![Flow is concerned](../assets/images/FlowAngry.png)

Always ensure a loop can exit. Forgetting `break` creates an infinite loop.

## For Loop

Count from A to B:

```magnesium
for i in 1..5
    print(i)
end
```

Output: `1` `2` `3` `4`

![Flow is happy](../assets/images/FlowHappy.png)

`..` is exclusive (does not include the upper bound). `..=` is inclusive:

```magnesium
for i in 1..=5
    print(i)
end
```

Output: `1` `2` `3` `4` `5`

| Operator | Meaning |
|----------|---------|
| `..` | Up to but not including |
| `..=` | Up to and including |

## Iterate an Array

```magnesium
let fruits = ["apple", "banana", "cherry"]

for fruit in fruits
    print(fruit)
end
```

Output: `apple` `banana` `cherry`

## Iterate a Dict

Use two loop variables for key and value:

```magnesium
let scores = &< Ada = 10, Grace = 12 >

for name, score in scores
    print(name + ": " + tostring(score))
end
```

Dict iteration is useful for dynamic data. If your data has a fixed shape, prefer a struct and direct fields.

## Sum Example

```magnesium
let numbers = [10, 20, 30, 40]
let total = 0

for n in numbers
    total = total + n
end

print(total)  // 100
```

## Nested Loops

```magnesium
for row in 1..3
    let line = ""
    for col in 1..4
        line = line + "* "
    end
    print(line)
end
```

Output:
```
* * * *
* * * *
* * * *
```

## When to Use What

| Situation | Use |
|-----------|-----|
| Repeat until condition | `loop` with `break` |
| Count A to B (exclusive) | `for i in A..B` |
| Count A to B (inclusive) | `for i in A..=B` |
| Go through a list | `for item in array` |
| Go through a dict | `for key, value in dict` |

## Try It

1. Print 1 to 20.
2. Print even numbers 2 to 100 (hint: `if i % 2 == 0`).
3. Array of names. Print "Hello, [name]!" for each.
4. Count down from 10, then print "Liftoff!".
5. Multiplication table 1-5 (nested loops).

→ [Functions](../language/06-functions.md)
