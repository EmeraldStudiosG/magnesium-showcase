# dict

Dictionary functions. Dicts are dynamic key-value maps where keys are strings. They are not Lua-style tables; arrays and structs are separate runtime types.

`dict.require` is the fallible version of `dict.get`. It returns an error object for missing keys and is designed for `?`.

## dict.new()

Create an empty dict.

```magnesium
let d = dict.new()
dict.set(d, "name", "Ada")
print(d<"name">)  // Ada
```

## dict.has(d, key)

Return `true` if `key` exists.

```magnesium
let d = &<name = "Ada", hp = 100>
print(dict.has(d, "name"))  // true
print(dict.has(d, "age"))   // false
```

## dict.get(d, key, default?)

Return the value for `key`, or `default` (or `null`) if missing.

```magnesium
let d = &<name = "Ada">
print(dict.get(d, "name"))       // Ada
print(dict.get(d, "age", 0))     // 0
```

## dict.set(d, key, val)

Set `key` to `val`.

```magnesium
let d = dict.new()
dict.set(d, "x", 10)
print(d<"x">)  // 10
```

## dict.delete(d, key)

Remove `key` from the dict.

```magnesium
let d = &<a = 1, b = 2>
dict.delete(d, "a")
print(dict.has(d, "a"))  // false
```

## dict.keys(d) / dict.values(d)

Return an array of all keys or all values.

```magnesium
let d = &<x = 1, y = 2>
print(dict.keys(d))    // ["x", "y"]
print(dict.values(d))  // [1, 2]
```

## dict.require(d, key)

Return the value for `key`, or an `Err` if missing. Designed for `?`.

```magnesium
fn get_name(config)
    return dict.require(config, "name")?
end
```

## dict.len(d)

Return the number of entries.

```magnesium
print(dict.len(&<a = 1, b = 2>))  // 2
```

## dict.clear(d)

Remove all entries.

```magnesium
let d = &<a = 1>
dict.clear(d)
print(dict.len(d))  // 0
```

## dict.clone(d)

Return a shallow copy.

```magnesium
let original = &<x = 1>
let copy = dict.clone(original)
dict.set(copy, "x", 99)
print(original<"x">)  // 1 (unaffected)
```

## dict.merge(target, source)

Merge all entries from `source` into `target`. Existing keys in `target` are overwritten.

```magnesium
let base = &<x = 1, y = 2>
let extra = &<y = 20, z = 30>
dict.merge(base, extra)
print(base<"y">)  // 20
print(base<"z">)  // 30
```
