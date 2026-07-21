# Modules

Split code into files and import them.

Each `.mg` file is compiled as its own module runtime boundary. Top-level code in that file runs when the module is first imported. The importer receives the module's exported values through a namespace.

## Export

By default, everything in a file is private. Use `export` to expose it.

**`math_utils.mg`**:
```magnesium
fn add(a, b)
    return a + b
end

export fn multiply(a, b)
    return a * b
end
```

`add` is private. `multiply` is public.

## Import

**`main.mg`**:
```magnesium
import "math_utils"

let result = math_utils.multiply(3, 4)
print(result)  // 12
```

What happens:
1. Magnesium finds `math_utils.mg`
2. Runs it once
3. Puts exports into the `math_utils` namespace
4. Caches it - importing again is instant

The cache is keyed by the resolved module path, so repeated imports of the same file share the same module exports.

## Paths

```magnesium
import "lib/network"
import "utils/helpers"
```

Looks for `lib/network.mg` and `utils/helpers.mg`.

Prefer relative project paths without the `.mg` suffix:

```magnesium
import "game/player"
import "game/items"
```

That keeps module names stable if tooling later adds alternate source forms.

## Aliases

Use `as` when the file name is long or when you want a clearer local name:

```magnesium
import "game/player_controller" as players

let p = players.new("Ada")
```

## What Stays Private

Anything not exported is private to the module:

```magnesium
let secret = 10

fn helper()
    return secret * 2
end

export fn public_value()
    return helper()
end
```

Other files can call `public_value`, but cannot access `secret` or `helper`.

## Host APIs and FFI

Production hosts should register APIs explicitly and document them with `extern` declarations:

```magnesium
!strict

extern fn read_score(player: string): number
extern fn save_score(player: string, score: number): null

let score = read_score("Ada")
save_score("Ada", score)
```

`extern` declarations are for the checker and LSP. They do not create runtime values. The host still has to provide the function or constant through the C API, Rust wrapper, or another embedding layer.

Script-level C header/library imports are experimental. Prefer explicit host registration for production embedding.

## Module-Level Setup

Top-level code runs once on first import:

```magnesium
// config.mg
let @settings = &< debug = true, version = "1.0" >

export fn get_setting(key)
    return @settings<key>?
end
```

Module-level setup should be cheap and deterministic. Expensive work belongs in an exported function that callers choose to run.

## Module Design Tips

Good modules have a small public surface:

```magnesium,good_practice
// inventory.mg
fn normalize_name(name)
    return string.lower(string.trim(name))
end

export fn has_item(items, name)
    return dict.has(items, normalize_name(name))
end

export fn add_item(items, name, count)
    dict.set(items, normalize_name(name), count)
end
```

The helper is private. The exported functions are the API.

## Try It

1. `shapes.mg` with exported `area_of_square(side)` and `area_of_circle(radius)`.
2. `main.mg` that imports `shapes` and prints areas.
3. `utils.mg` with `repeat_string(text, count)`. Use it from another file.

→ [Syntax Reference](../reference/syntax.md)
