# fs

Filesystem operations for whole files. Failed operations return `Err("IOError", ...)`.

## fs.read(path)

Read entire file contents. Fallible.

```magnesium
let text = fs.read("data.txt")?
```

## fs.write(path, text)

Write entire file contents. Fallible.

```magnesium
fs.write("output.txt", "hello")?
```

## fs.append(path, text)

Append to a file. Fallible.

```magnesium
fs.append("log.txt", "new entry\n")?
```

## fs.exists(path)

Return `true` if the file exists. Fallible - returns an `Err` when the check itself fails.

```magnesium
if fs.exists("config.txt") then
    let text = fs.read("config.txt")?
    print(text)
end
```

## fs.remove(path)

Delete a file. Fallible.

```magnesium
fs.remove("temp.txt")?
```

## fs.rename(from, to)

Rename or move a file. Fallible.

```magnesium
fs.rename("old.txt", "new.txt")?
```

## fs.cwd()

Return the current working directory. Fallible.

```magnesium
print(fs.cwd()?)  // /home/user/project
```
