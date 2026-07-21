# The Magnesium Book

![Magnesium Header](assets/images/MagnesiumMainHeader.png)

Magnesium is an embeddable scripting language built around a small syntax, predictable runtime behavior, and a fast register VM. Lua is an embedding reference point, but Magnesium keeps arrays, dicts, and structs as separate data types instead of exposing one public table type.

This book teaches Magnesium from the ground up. If you are new to programming, start with the tutorial. If you already know Lua, Python, JavaScript, or Rust, read the quick start and then use the reference chapters as needed.

## Meet Flow

![Flow is happy](assets/images/FlowHappy.png)

Flow is the Magnesium mascot. In the tutorial chapters, Flow callouts mark beginner notes, common mistakes, and places where the language makes a deliberate design choice.

## A First Program

Create `hello.mg`:

```magnesium
let name = input("Name: ")
print("Hello, " + name)
```

Run it:

```bash
magnesium hello.mg
```

Magnesium executes the file from top to bottom. `let` creates a variable, `input` reads a line from stdin, and `print` writes a line to stdout.

## What Makes Magnesium Different

Magnesium is designed around a few direct ideas:

| Idea | What it means in practice |
| --- | --- |
| Data-oriented scripts | Small files, simple control flow, functions, arrays, dicts, structs, modules, and embeddability. |
| Explicit globals | Normal variables are local; global names use the `@` prefix when declared. |
| Values over exceptions | Functions can return `value, err`; `?` removes repeated error plumbing for fallible lookups. |
| Fast interpreter | Bytecode runs on a register VM with compact values and targeted bytecode optimizations. |
| Engine-friendly | The language should be easy to embed and reason about from a host engine. |

## Reading Order

New programmers should read the tutorial in order:

| Chapter | You will learn |
| --- | --- |
| [Installation](getting-started/installation.md) | How to build and run Magnesium. |
| [Quick Start](getting-started/quick-start.md) | The whole language in one pass. |
| [What Is Programming?](basics/01-what-is-programming.md) | Files, statements, and running code. |
| [Variables and Math](basics/02-variables-and-math.md) | Numbers, locals, globals, and operators. |
| [Text and Booleans](basics/03-text-and-booleans.md) | Strings, interpolation, `true`, `false`, and `null`. |
| [Making Decisions](basics/04-making-decisions.md) | `if`, `elseif`, `else`, and conditions. |
| [Loops](basics/05-doing-things-over-and-over.md) | `loop`, `for`, `break`, and `continue`. |
| [Functions](language/06-functions.md) | Parameters, returns, closures, methods, and `defer`. |
| [Collections](language/07-collections.md) | Arrays, dicts, structs, enums, and iteration. |
| [Error Handling](language/08-error-handling.md) | Multi-return errors and the `?` operator. |
| [Modules](language/09-modules.md) | Importing files and exporting public APIs. |

After that, use the [Practical Guide](practical/input-output.md) for common patterns and the [Reference](reference/syntax.md) for exact syntax.

## Current Status

Magnesium is still moving quickly. The docs describe the current implementation unless a section clearly says it is a design note. For hardening work, examples prefer code that is easy to test, easy to benchmark, and explicit about failure.
