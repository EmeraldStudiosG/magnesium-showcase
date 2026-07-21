# Making Decisions

## If Statement

```magnesium
let score = 95

if score >= 90 then
    print("A")
end
```

Pattern:
```magnesium
if CONDITION then
    // runs if CONDITION is true
end
```

## Else

```magnesium
if score >= 90 then
    print("A")
else
    print("Not an A")
end
```

## Elseif

```magnesium
if score >= 90 then
    print("A")
elseif score >= 80 then
    print("B")
elseif score >= 70 then
    print("C")
else
    print("F")
end
```

Magnesium checks top to bottom. The first true condition wins.

Order matters. Put the most specific first:

```magnesium,not_desired_behavior
// WRONG: B never prints
if score >= 70 then print("C")
elseif score >= 80 then print("B")
end
```

```magnesium,good_practice
// RIGHT
if score >= 80 then print("B")
elseif score >= 70 then print("C")
end
```

## Nested If

```magnesium
let age = 20
let has_ticket = true

if age >= 18 then
    if has_ticket then
        print("Welcome!")
    else
        print("Need a ticket.")
    end
else
    print("Too young.")
end
```

Combine with `and`/`or` instead when possible:
```magnesium
if age >= 18 and has_ticket then
    print("Welcome!")
end
```

## Guard Clauses

Inside a function, return early when a condition makes the rest of the work impossible:

```magnesium,good_practice
fn withdraw(balance, amount)
    if amount <= 0 then return null, "Amount must be positive" end
    if amount > balance then return null, "Not enough balance" end

    return balance - amount
end
```

This keeps the normal path less nested and makes failure conditions easy to scan.

## Conditions With `null`

Because `null` is falsey, you can use it directly:

```magnesium
let player = null

if player then
    print("loaded")
else
    print("missing")
end
```

## Try It

1. Check if a number is positive, negative, or zero.
2. Check if someone can vote (`age >= 18`).
3. Store a secret number. Check if a guess is too high, too low, or correct.
4. Fix: `if score > 50` then `else if score > 90`. What's wrong?

→ [Doing Things Over and Over](05-doing-things-over-and-over.md)
