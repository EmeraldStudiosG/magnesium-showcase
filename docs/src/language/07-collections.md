# Collections

## Array

Ordered list. Zero-indexed.

```magnesium
let fruits = ["apple", "banana", "cherry"]
print(fruits[0])     // apple
print(fruits[1])     // banana
print(len(fruits))   // 3

push(fruits, "date")
print(len(fruits))   // 4
```

Iterate:
```magnesium
for fruit in fruits
    print(fruit)
end
```

Start empty:
```magnesium
let scores = []
push(scores, 10)
push(scores, 20)
```

### Array Lookup

Plain array lookup returns `null` if the index is out of range:

```magnesium
let items = ["a", "b"]
print(items[10])       // null
```

Use `?` when the current function should return an error instead:

```magnesium
fn second(items)
    return items[1]?
end
```

The standard library also has fallible helpers that compose with `?`:

```magnesium
fn second(items)
    return array.get(items, 1)?
end
```

## Dict

Dynamic key-value lookup. Keys are strings.

```magnesium
let config = &< host = "localhost", port = 8080, debug = true >
print(config<"host">)     // localhost
print(config<"port">)     // 8080
```

Set a value:
```magnesium
config<"port"> = 3000
```

### Dict Lookup

Plain dict lookup returns `null` when the key is missing:

```magnesium
let player = &< name = "Ada" >
print(player<"level">)     // null
```

Use `?` when missing keys are errors:

```magnesium
fn require_name(player)
    return player<"name">?
end
```

If the key exists, the value is returned. If it does not, the current function returns `null, err`.

For dictionary-heavy code, `dict.require` is often clearer:

```magnesium
fn require_name(player)
    return dict.require(player, "name")?
end
```

### Why `&<>`?

![Flow is concerned](../assets/images/FlowAngry.png)

Using `&<` avoids ambiguity. `<` and `>` are already comparison operators. The `&` makes dict syntax unambiguous, which makes the compiler faster.

`[]` is reserved for arrays and string indexing. Dict lookup is always `dict<key>`.

Magnesium does not expose a public table type. Arrays, dicts, and structs are different data containers because that keeps scripts data-oriented and keeps tooling precise.

## Struct

Blueprint for a custom data type.

```magnesium
struct Player
    x
    y
    name
end

let p = Player { x = 10, y = 20, name = "Alice" }
print(p.x)       // 10
print(p.name)    // Alice
```

### Methods

```magnesium
fn Player.move(self, nx, ny)
    self.x = nx
    self.y = ny
end

fn Player.say_name(self)
    print(_"I am {self.name}")
end

p.say_name()
p.move(50, 50)
```

`self` is passed automatically when calling `p.move(...)`.

## Dict vs Struct

![Flow is happy](../assets/images/FlowHappy.png)

| | Dict `&<>` | Struct |
|---|---|---|
| Keys | String | Field name |
| Known at compile | No | Yes |
| Performance | Hash lookup | Direct offset |
| Use for | Dynamic data | Fixed shapes |

Optional data shape aliases can describe dict-shaped data in `!strict` files:

```magnesium
!strict

type PlayerData = &<
    name: string,
    score: number
>

let data: PlayerData = &<
    name = "Ada",
    score = 10,
    level = 2
>
```

This is still a dict at runtime. The alias is static metadata for diagnostics and editor support.

## Enums

Enums are named sets of variants:

```magnesium
enum Direction { Up, Down, Left, Right }

let dir = Direction<"Left">
print(dir)
```

Use enums when a value should come from a small known set.

## Choosing a Collection

Use an array when order matters and indexes are natural:

```magnesium
let inventory = ["key", "map", "coin"]
```

Use a dict when keys come from data:

```magnesium
let settings = &< width = 1280, height = 720 >
```

Use a struct when the shape is known by the program:

```magnesium
struct Vec2
    x
    y
end
```

## Try It

1. Array of 5 numbers. Print the sum.
2. Dict with your name, age, favorite color. Print each.
3. `Book` struct with `title`, `author`, `pages`. Make one and print it.
4. Add a `Book.describe` method.

→ [Error Handling](08-error-handling.md)
