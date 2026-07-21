# coroutine

Coroutines are cooperative and run inside the same VM. They are not native threads and do not isolate globals or heap objects. `yield` and final `return` can pass multiple values back to `resume`.

## coroutine.create(fn)

Create a coroutine from a function. Returns a coroutine handle or an `Err`.

```magnesium
let co = coroutine.create(fn()
    coroutine.yield("step1", 10)
    return "done", 20
end)
```

## coroutine.resume(co)

Resume a suspended coroutine. Returns all values passed to `yield` or `return` inside the coroutine.

```magnesium
let label, amount = coroutine.resume(co)
print(label)   // step1
print(amount)  // 10

let done, code = coroutine.resume(co)
print(done)    // done
print(code)    // 20
```

## coroutine.yield(values...)

Yield back to the caller of `resume`. Can pass multiple values.

```magnesium
fn producer()
    for i in 1..=3
        coroutine.yield(i)
    end
end

let co = coroutine.create(producer)
print(coroutine.resume(co))  // 1
print(coroutine.resume(co))  // 2
print(coroutine.resume(co))  // 3
```

## coroutine.status(co)

Return the coroutine state: `"suspended"`, `"running"`, `"normal"`, or `"dead"`.

```magnesium
let co = coroutine.create(fn()
    coroutine.yield()
end)
print(coroutine.status(co))  // suspended
coroutine.resume(co)
print(coroutine.status(co))  // suspended (yielded) or dead (if returned)
```
