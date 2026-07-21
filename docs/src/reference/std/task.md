# task

Same-VM task queue.

## task.run(fn)

Queue `fn` for execution on the same VM's event loop. It is not an isolated VM and not a native thread.

```magnesium
task.run(fn()
    print("deferred work")
end)
print("queued")
```

`task.run` is cooperative: the queued function runs after the current function yields or returns.
