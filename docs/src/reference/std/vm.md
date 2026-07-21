# vm

Isolated VM operations. `vm.spawn` creates a fresh VM on a native worker thread and runs the `.mg` or `.mgc` file. The child has separate globals, heap, imports, and task queue. It still shares process-level IO and filesystem permissions.

## vm.spawn(path)

Spawn an isolated VM running the given script. Returns a task handle or an `Err`.

```magnesium
let handle = vm.spawn("scripts/worker.mg")
```

## vm.status(handle)

Return the task status: `"running"`, `"ok"`, or `"error"`.

```magnesium
let handle = vm.spawn("scripts/worker.mg")
print(vm.status(handle))  // running
```

## vm.join(handle, timeout_ms?)

Wait for the spawned VM to complete. Returns `true` on success or an `Err`. Optional `timeout_ms` returns `null, Err("TimeoutError", ...)` if the child is still running after the timeout.

```magnesium
let handle = vm.spawn("scripts/worker.mg")
let ok = vm.join(handle)
if ok == true then
    print("worker finished")
else
    print("worker failed")
end
```

With timeout:

```magnesium
let handle = vm.spawn("scripts/worker.mg")
let result = vm.join(handle, 5000)
if result == null then
    print("timed out after 5s")
end
```

## vm.try_join(handle)

Non-blocking check. Returns `true` if the worker finished, `false` if still running. Use in host loops that need to poll without blocking.

```magnesium
let handle = vm.spawn("scripts/worker.mg")
loop
    if vm.try_join(handle) then break end
    // do other work
end
```

## vm.cancel(handle)

Request cooperative cancellation. Tight Magnesium loops observe it, but blocking native IO may not stop until the native operation returns. Returns `true` if the cancel signal was sent.

```magnesium
let handle = vm.spawn("scripts/worker.mg")
vm.cancel(handle)
```
