# string

String manipulation functions. Strings are immutable - all functions return new strings.

`string.sub`, `string.find`, and `string.byte` use Lua-style one-based positions. `string.at` uses Magnesium's normal zero-based indexing and returns an error object when used as `string.at(text, index)?`.

## string.len(s)

Return the length of `s`.

```magnesium
print(string.len("hello"))  // 5
```

The global `len` also works on strings:

```magnesium
print(len("hello"))  // 5
```

## string.lower(s) / string.upper(s)

Convert to lowercase or uppercase.

```magnesium
print(string.lower("HELLO"))  // hello
print(string.upper("hello"))  // HELLO
```

## string.sub(s, i, j?)

Return a substring. Positions are one-based (Lua convention). `i` is the start, `j` is the inclusive end (defaults to end of string). Negative indices count from the end.

```magnesium
print(string.sub("hello", 1, 3))   // hel
print(string.sub("hello", 2))       // ello
print(string.sub("hello", -3))      // llo
```

## string.find(s, pattern)

Find the first occurrence of `pattern` in `s`. Returns the one-based start index, or `-1` if not found.

```magnesium
print(string.find("hello", "ell"))  // 2
print(string.find("hello", "xyz"))  // -1
```

## string.trim(s)

Remove leading and trailing whitespace.

```magnesium
print(string.trim("  hi  "))  // hi
```

## string.byte(s, i?)

Return the byte value of character at one-based position `i` (default 1).

```magnesium
print(string.byte("A"))      // 65
print(string.byte("ABC", 2)) // 66
```

## string.char(...)

Return a string from one or more byte values.

```magnesium
print(string.char(65, 66, 67))  // ABC
```

## string.split(s, delimiter)

Split `s` by `delimiter` and return an array of parts.

```magnesium
let parts = string.split("one,two,three", ",")
print(parts[0])  // one
print(parts[1])  // two
print(parts[2])  // three
```

## string.contains(s, needle)

Return `true` if `s` contains `needle`.

```magnesium
print(string.contains("hello", "ell"))  // true
print(string.contains("hello", "xyz"))  // false
```

## string.starts_with(s, prefix) / string.ends_with(s, suffix)

Check whether `s` starts or ends with the given string.

```magnesium
print(string.starts_with("hello", "hel"))  // true
print(string.ends_with("hello", "llo"))    // true
```

## string.repeat(s, n)

Return `s` repeated `n` times.

```magnesium
print(string.repeat("ha", 3))  // hahaha
```

## string.reverse(s)

Return `s` reversed.

```magnesium
print(string.reverse("abc"))  // cba
```

## string.replace(s, needle, replacement)

Replace all occurrences of `needle` with `replacement`.

```magnesium
print(string.replace("a-b-c", "-", " "))  // a b c
```

## string.at(s, index)

Return the character at zero-based `index`. Unlike `string.sub`, this uses Magnesium's zero-based indexing. Fallible - returns an error on out-of-range index.

```magnesium
print(string.at("hello", 1))  // e

fn safe_at(text, i)
    return string.at(text, i)?
end
```
