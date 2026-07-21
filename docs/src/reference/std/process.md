# process

Process metadata. Overlaps with `os` and `fs` for convenience in scripts that prefer a single `process.*` namespace.

## process.clock()

CPU time. Same as `os.clock()`.

## process.time()

Unix timestamp. Same as `os.time()`.

## process.getenv(name)

Environment variable. Same as `os.getenv(name)`.

## process.cwd()

Current working directory. Same as `fs.cwd()`.

## process.platform()

Return a platform identifier string: `"linux"`, `"macos"`, `"windows"`, or `"unknown"`.

```magnesium
print(process.platform())  // linux
```
