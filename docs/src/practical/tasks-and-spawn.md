# Tasks and Spawn

Magnesium separates same-VM scheduling from isolated VM execution.

`task.run(fn)` queues a function inside the current VM. `vm.spawn(path)` starts a script in a fresh VM and returns a handle.

```magnesium
fn job()
    print("from job")
end

task.run(job)
print("main")
```

The queued task runs on the same VM. It is not an isolated VM and it is not a native thread.

## Current Behavior

Use `task.run` for lightweight deferred work:

```magnesium
fn after_load()
    print("load finished")
end

task.run(after_load)
```

The function passed to `task.run` must be a function value:

```magnesium
task.run(fn()
    print("anonymous task")
end)
```

## What Spawn Is Not

`task.run` is not for isolation:

```magnesium,not_desired_behavior
// This is not a separate VM.
task.run(fn()
    @shared_state = @shared_state + 1
end)
```

Because it runs in the same VM, it shares the same runtime state. That is useful for simple task queues, but it is not a sandbox boundary.

## Isolated VMs

Use `vm.spawn(path)` when the child script needs a separate runtime:

```magnesium
let handle = vm.spawn("scripts/worker.mg")
let ok, join_err = vm.join(handle, 1000)
if join_err then
    print(join_err.report)
end
```

`vm.spawn(path)` creates a new VM on a native worker thread, installs a fresh standard library, and compiles or loads the child file. `vm.join(handle, timeout_ms?)` waits for completion and returns `true` or an error. A timeout returns `TimeoutError` without destroying the task. `vm.cancel(handle)` requests cooperative cancellation, and a later join returns `CancelledError` once the child observes it. `vm.try_join(handle)` checks without blocking and returns `false` while the worker is still running. `vm.status(handle)` returns `"running"`, `"ok"`, or `"error"`.

Isolation means the child has separate:

- Globals
- Heap
- Imports/module cache
- Task queue

It still shares process-level things like stdout, stdin, environment variables, and the host filesystem permissions.

Current isolated VM execution is asynchronous through native worker threads. Host embeddings can still choose to wrap or replace this with an engine-owned scheduler.

## Concepts

![Flow is happy](../assets/images/FlowHappy.png)

| Concept | Purpose |
| --- | --- |
| Coroutine | Pause and resume work inside the same VM with `coroutine.create`, `resume`, and `yield`. |
| Task queue | Schedule work cooperatively inside the same VM with `task.run`. |
| Isolated VM | Run a script with separate globals, heap, imports, and host permissions. |
| Native thread | Host-level parallelism, managed carefully by the embedding API. |

Keeping these concepts separate prevents `task.run(fn)` from quietly becoming a sandbox or thread boundary.

## Recommended Use Today

Use `task.run` when:

- The task can run later.
- The task does not need isolation.
- The task is short and cooperative.
- The task is part of the same script runtime.

Use `vm.spawn` when globals and heap state must be decoupled from the caller.

Use `coroutine` when one script needs explicit pause/resume points:

```magnesium
let co = coroutine.create(fn()
    coroutine.yield("first", 1)
    return "second", 2
end)

let a, n = coroutine.resume(co)
print(a) // first
print(n) // 1

let b, m = coroutine.resume(co)
print(b) // second
print(m) // 2
```

Coroutines share the current VM and heap. They are for cooperative control flow, not parallelism. They can yield or return multiple values.
