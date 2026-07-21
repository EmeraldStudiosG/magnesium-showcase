# What Is Programming?

A program is a list of instructions. The computer reads from top to bottom.

## Your First Program

Save this as `hello.mg`:

```magnesium
print("Hello, World!")
```

Run it:

```bash
magnesium hello.mg
```

Output:
```
Hello, World!
```

## Parts of a Program

- **`print`** - a function. A pre-made instruction.
- **`"Hello, World!"`** - a string. Text wrapped in quotes.
- **`()`** - parentheses hold the inputs to the function.

## Programs Run Top to Bottom

```magnesium
print("First")
print("Second")
print("Third")
```

Output:
```
First
Second
Third
```

## Comments

Comments are notes for humans. The computer ignores them.

```magnesium
// This is a line comment.
print("Hello") // This is also a comment.
```

Block comments:
```magnesium
//[[
    This is a block comment.
    It spans multiple lines.
]]
```

## Common Mistakes

![Flow is concerned](../assets/images/FlowAngry.png)

| Wrong | Right | Why |
|-------|-------|-----|
| `print(Hello)` | `print("Hello")` | Strings need quotes |
| `print "Hello"` | `print("Hello")` | Functions need parentheses |
| `prnt("Hello")` | `print("Hello")` | Typos matter |

## How to Read Errors

When Magnesium reports an error, start with the line number:

```text
[line 3] Error at 'name': Undefined variable.
```

Read it as:

- The problem is near line 3.
- The token `name` is where the compiler got confused.
- The message tells you what rule was broken.

Fix the first error first. Later errors are sometimes caused by the first one.

## Try It

1. Print your name.
2. Print three lines.
3. Misspell `print` on purpose and read the error.

→ [Variables and Math](02-variables-and-math.md)
