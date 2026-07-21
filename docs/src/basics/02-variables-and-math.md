# Variables and Math

A variable stores a value under a name.

## Creating a Variable

```magnesium
let age = 25
print(age)
```

Output: `25`

- **`let`** - create a new variable
- **`age`** - the name
- **`=`** - assign the value
- **`25`** - the value

## Naming Rules

- Start with a letter or underscore
- Use letters, numbers, and underscores
- No spaces

Good names describe what they hold:

```magnesium,good_practice
let x = 3.14159          // Bad
let pi = 3.14159         // Good
```

Good names describe what they hold. Short names like `x` are fine for quick math, but `pi` is clearer.

| Symbol | Operation | Example | Result |
|--------|-----------|---------|--------|
| `+` | Add | `5 + 3` | `8` |
| `-` | Subtract | `5 - 3` | `2` |
| `*` | Multiply | `5 * 3` | `15` |
| `/` | Divide | `15 / 3` | `5` |
| `%` | Remainder | `17 % 5` | `2` |

```magnesium
let apples = 5
let oranges = 3
let total = apples + oranges
print(total)  // 8
```

## Changing a Variable

```magnesium
let score = 0
score = score + 10
score = score + 5
print(score)  // 15
```

`=` means "assign", not "equals".

Compound assignment is available for common math updates:

```magnesium
score += 10
score -= 2
score *= 3
score /= 2
score %= 5
```

These are shorter forms of `score = score + 10` and similar updates.

## Global Variables

Variables with `@` are visible everywhere:

```magnesium,not_desired_behavior
let @player_health = 100

fn take_damage(amount)
    @player_health = @player_health - amount
end

take_damage(20)
print(@player_health)  // 80
```

Prefer `let` over `@` globals when possible. Globals make bugs harder to trace.

## Local First

![Flow is happy](../assets/images/FlowHappy.png)

Use local variables for temporary values:

```magnesium
fn area(width, height)
    let result = width * height
    return result
end
```

Use globals for shared runtime settings that truly need to be visible across functions or modules:

```magnesium
let @debug = true
```

## Try It

1. Create `length` and `width`. Calculate and print the area.
2. Create a `score`. Add and subtract points.
3. What is `7 / 2`?
4. What is `10 % 3`?

→ [Text and Booleans](03-text-and-booleans.md)
