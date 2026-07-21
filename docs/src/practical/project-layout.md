# Project Layout

Magnesium projects can stay simple. A small script may be one file. Larger projects should group code by feature and keep modules focused.

## A Small Project

```text
project/
  main.mg
  player.mg
  inventory.mg
```

`main.mg`:

```magnesium
import "player"
import "inventory"

let p = player.new("Ada")
inventory.add(p.items, "key", 1)
```

## A Game-Style Project

```text
game/
  main.mg
  systems/
    input.mg
    physics.mg
    combat.mg
  data/
    items.mg
    enemies.mg
  ui/
    hud.mg
```

Imports use paths without the `.mg` suffix:

```magnesium
import "systems/combat"
import "data/items"
import "ui/hud"
```

## Module Boundaries

Keep module APIs small. Export commands and queries; keep helpers private.

```magnesium
// combat.mg
fn clamp_damage(amount)
    if amount < 0 then return 0 end
    return amount
end

export fn apply_damage(target, amount)
    target.hp = target.hp - clamp_damage(amount)
end
```

## Naming

Use names that describe the script's job:

| Good | Weak |
| --- | --- |
| `inventory.mg` | `stuff.mg` |
| `player_controller.mg` | `misc_player.mg` |
| `combat_rules.mg` | `helpers.mg` |

Small names are fine when the module is obvious. Long names are fine when they prevent confusion.

## Generated Files

Bytecode files use `.mgc`. They are build artifacts and should usually be ignored by git:

```text
*.mgc
```

Benchmark scripts may create and delete `.mgc` files as part of performance measurement.

## Tests and Benchmarks

Keep tests close to language behavior:

```text
tests/
  test_arrays.mg
  test_errors_runtime.mg
  expected/
    test_arrays.expected
```

Keep benchmarks in a separate folder so performance scripts can run all cases consistently:

```text
benchmark/
  arith_loop.mg
  mandelbrot.mg
```

Tests should include scripts that are supposed to fail. A language is not hardened until its error paths are tested too.
