# Functions

A function is a reusable block of code.

## Define and Call

```magnesium
fn greet()
    print("Hello!")
end

greet()
greet()
```

Output: `Hello!` `Hello!`

## Parameters

```magnesium
fn greet(name)
    print("Hello, " + name + "!")
end

greet("Alice")
greet("Bob")
```

Output: `Hello, Alice!` `Hello, Bob!`

## Multiple Parameters

```magnesium
fn describe_pet(name, species, age)
    print(_"I have a {species} named {name} who is {age} years old.")
end

describe_pet("Fluffy", "cat", 3)
```

## Return Values

```magnesium
fn add(a, b)
    return a + b
end

let result = add(3, 5)
print(result)  // 8
```

`return` stops the function and sends a value back.

## Multiple Returns

```magnesium
fn get_info()
    return "Alice", 30
end

let name, age = get_info()
```

Callers can ask for more return values than a function provides. Missing values become `null`:

```magnesium
fn answer()
    return 42
end

let value, err = answer()
print(value)    // 42
print(err)      // null
```

This is useful for `value, err` APIs. Success paths can return only the value, while failure paths return `null, err`.

```magnesium
fn find_player(players, name)
    return players<name>?
end
```

## Scope

Variables inside a function exist only there:

```magnesium
fn make_message()
    let msg = "Secret"
    print(msg)
end

make_message()
// print(msg)  // ERROR: msg doesn't exist here
```

## Calling Functions Within Functions

```magnesium
fn square(x)
    return x * x
end

fn sum_of_squares(a, b)
    return square(a) + square(b)
end

print(sum_of_squares(3, 4))  // 25
```

## Recursion

A function calling itself:

```magnesium
fn countdown(n)
    if n <= 0 then
        print("Blast off!")
    else
        print(n)
        countdown(n - 1)
    end
end

countdown(5)  // 5 4 3 2 1 Blast off!
```

![Flow is concerned](../assets/images/FlowAngry.png)

Always include a base case (`n <= 0` above). Without it, infinite recursion crashes the program.

## Functions as Values

```magnesium
fn apply_twice(f, x)
    return f(f(x))
end

fn add_one(n)
    return n + 1
end

print(apply_twice(add_one, 5))  // 7
```

Anonymous function:
```magnesium
let doubled = map_array(nums, fn(x) return x * 2 end)
```

## Methods

Methods are functions attached to a struct name:

```magnesium
struct Player
    name
    hp
end

fn Player.damage(self, amount)
    self.hp = self.hp - amount
end

let p = Player { name = "Ada", hp = 100 }
p.damage(25)
print(p.hp)      // 75
```

The method receives `self` automatically when called with dot-call syntax.

## Defer

![Flow is happy](../assets/images/FlowHappy.png)

`defer` schedules a call to run when the current scope exits:

```magnesium
fn use_resource()
    let resource = "file"
    defer print("closed " + resource)

    print("using " + resource)
end
```

Deferred calls are useful for cleanup-style code. Keep deferred work small and predictable.

## Try It

1. `is_even(n)` - return `true` if even.
2. `max(a, b)` - return the larger number.
3. Recursive `factorial(n)` where `factorial(5)` = 120.
4. A `banner(name)` function that prints:
   ```
   ==============
   Hello, Alice!
   ==============
   ```

→ [Collections](07-collections.md)
