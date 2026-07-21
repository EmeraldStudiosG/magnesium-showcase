# Standard Library

Magnesium exposes its standard library as global functions and named namespaces. A namespace is a normal global dict containing native functions; it is not a separate language type.

There is no public `table` namespace. User data is split deliberately:

- arrays use `[]` and `array.*`
- dicts use `&< >`, `dict<key>`, and `dict.*`
- structs use `struct ... end` and dot fields

## Always Available Globals

- [Global Functions](./std/global-functions.md) - `print`, `len`, `push`, `type`, `tostring`, `tonumber`, `Err`, `Error`, `assert`, `error`, `input`

These names are installed by the VM before user code runs. They are available without imports.

## Namespaces

- [math](./std/math.md) - `abs`, `floor`, `ceil`, `sqrt`, `sin`, `cos`, `tan`, `pow`, `min`, `max`, `random`, `randomseed`, `round`, `clamp`, `pi`, `huge`
- [string](./std/string.md) - `len`, `lower`, `upper`, `sub`, `find`, `trim`, `byte`, `char`, `split`, `contains`, `starts_with`, `ends_with`, `repeat`, `reverse`, `replace`, `at`
- [array](./std/array.md) - `new`, `len`, `push`, `pop`, `insert`, `remove`, `get`, `at`, `clear`, `contains`, `index_of`, `first`, `last`, `extend`, `slice`
- [dict](./std/dict.md) - `new`, `has`, `get`, `set`, `delete`, `keys`, `values`, `require`, `len`, `clear`, `clone`, `merge`
- [io](./std/io.md) - `write`, `read_line`, `read_file`, `write_file`, `append_file`
- [fs](./std/fs.md) - `read`, `write`, `append`, `exists`, `remove`, `rename`, `cwd`
- [path](./std/path.md) - `join`, `basename`, `dirname`, `ext`
- [os](./std/os.md) - `clock`, `time`, `getenv`
- [process](./std/process.md) - `clock`, `time`, `getenv`, `cwd`, `platform`
- [task](./std/task.md) - `run`
- [coroutine](./std/coroutine.md) - `create`, `resume`, `yield`, `status`
- [vm](./std/vm.md) - `spawn`, `status`, `join`, `try_join`, `cancel`

## Design Rule

Prefer the namespace that matches the data:

```mg
array.push(items, value)
dict.set(settings, "width", 1280)
string.trim(name)
```

Do not write `table.*`. Magnesium does not have Lua-style tables as a public data model.
