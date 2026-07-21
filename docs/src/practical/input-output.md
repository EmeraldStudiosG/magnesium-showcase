# Input and Output

Most scripts need to talk to the outside world. Magnesium keeps I/O small and direct: print text, write text, read a line from stdin, and work with whole files through fallible helpers.

## Printing Lines

`print(...)` writes its arguments and then adds a newline:

```magnesium
print("hello")
print("score", 10)
```

Multiple arguments are separated by tabs.

Use `tostring` when building a string yourself:

```magnesium
let score = 10
print("score: " + tostring(score))
```

## Writing Without a Newline

`io.write(...)` writes values without adding a newline:

```magnesium
io.write("Loading")
io.write(".")
io.write(".")
io.write(".")
print("")
```

Use this for prompts, progress text, and compact command-line tools.

## Reading Input

`input(prompt?)` reads one line from stdin:

```magnesium
let name = input("Name: ")
print("Hello, " + name)
```

`io.read_line()` is the namespace form:

```magnesium
let line = io.read_line()
print(line)
```

Both forms return strings. If stdin is exhausted, the implementation returns an empty or null-like value depending on host behavior; check input in scripts that process piped data.

## Reading And Writing Files

Use `fs.read`, `fs.write`, and `fs.append` for whole-file operations:

```magnesium,good_practice
fs.write("notes.txt", "hello")?
fs.append("notes.txt", "\nagain")?

let text = fs.read("notes.txt")?
print(text)
```

The `io` namespace exposes aliases:

```magnesium
let text = io.read_file("notes.txt")?
io.write_file("copy.txt", text)?
```

These functions return structured `Err` values on failure, so `?` is the normal way to propagate file errors.

## Paths

Use the `path` namespace to build and inspect paths without hand-assembling strings:

```magnesium
let file = path.join("assets", "player.png")
print(path.basename(file)) // player.png
print(path.dirname(file))  // assets
print(path.ext(file))      // .png
```

`fs.cwd()` and `process.cwd()` return the current working directory.

## A Small CLI Program

```magnesium
fn ask_number(label)
    let text = input(label + ": ")
    return tonumber(text)
end

let a = ask_number("a")
let b = ask_number("b")
print("sum: " + tostring(a + b))
```

## Current Limits

The current standard library does not yet expose streaming file handles, binary I/O, sockets, subprocess spawning, or terminal control. For now, use whole-file helpers and stdin/stdout unless the host application provides more native functions.

Those larger APIs should be designed as part of the standard library plan, not added ad hoc. The goal is a small, predictable, embeddable standard layer.
