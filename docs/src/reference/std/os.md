# os

Operating system interface.

## os.clock()

Return the approximate CPU time used by the process, in seconds.

```magnesium
let start = os.clock()
// ... do work ...
let elapsed = os.clock() - start
print("took " + tostring(elapsed) + "s")
```

## os.time()

Return the current Unix timestamp (seconds since epoch).

```magnesium
print(os.time())  // 1700000000
```

## os.getenv(name)

Return the value of environment variable `name`, or `null` if not set.

```magnesium
let home = os.getenv("HOME")
if home != null then
    print("Home: " + home)
end
```
