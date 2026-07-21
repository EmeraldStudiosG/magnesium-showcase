# io

Console and file I/O. The file helpers are aliases for the `fs` namespace and are designed for `?`.

## io.write(...)

Write values to stdout without a trailing newline.

```magnesium
io.write("Loading")
io.write(".")
io.write(".")
print("")  // force newline
```

## io.read_line()

Read one line from stdin. Same as the global `input()`.

```magnesium
let line = io.read_line()
```

## io.read_file(path)

Read the entire contents of a file. Fallible - returns an `Err` on failure.

```magnesium
let text = io.read_file("config.txt")?
```

## io.write_file(path, text)

Write `text` to a file, replacing existing contents. Fallible.

```magnesium
io.write_file("output.txt", "hello world")?
```

## io.append_file(path, text)

Append `text` to the end of a file. Fallible.

```magnesium
io.append_file("log.txt", "entry\n")?
```
