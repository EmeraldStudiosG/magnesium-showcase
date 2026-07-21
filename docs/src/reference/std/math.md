# math

Mathematical functions and constants. All trigonometric functions use radians.

## Constants

- `math.pi` - 3.14159265358979
- `math.huge` - largest representable number

## math.abs(x)

Absolute value.

```magnesium
print(math.abs(-7))   // 7
print(math.abs(3))    // 3
```

## math.floor(x)

Round down to the nearest integer.

```magnesium
print(math.floor(3.7))   // 3
print(math.floor(-1.2))  // -2
```

## math.ceil(x)

Round up to the nearest integer.

```magnesium
print(math.ceil(3.2))   // 4
print(math.ceil(-1.8))  // -1
```

## math.sqrt(x)

Square root.

```magnesium
print(math.sqrt(16))   // 4
print(math.sqrt(2))     // 1.41421...
```

## math.sin(x), math.cos(x), math.tan(x)

Trigonometric functions (radians).

```magnesium
print(math.sin(math.pi / 2))  // 1
print(math.cos(0))             // 1
print(math.tan(math.pi / 4))  // ~1
```

## math.pow(x, y)

Raise `x` to the power `y`.

```magnesium
print(math.pow(2, 10))  // 1024
print(math.pow(9, 0.5)) // 3
```

## math.min(...) / math.max(...)

Return the smallest or largest of the arguments. Accepts any number of arguments.

```magnesium
print(math.min(3, 1, 4, 1, 5))  // 1
print(math.max(3, 1, 4, 1, 5))  // 5
```

## math.random(low?, high?)

Return a random number. With no arguments, returns a float in `[0, 1)`. With one argument, returns an integer in `[1, low]`. With two arguments, returns an integer in `[low, high]`.

```magnesium
print(math.random())          // 0.0 .. 1.0
print(math.random(6))         // 1 .. 6
print(math.random(10, 20))    // 10 .. 20
```

## math.randomseed(x)

Seed the random number generator. Use once at program start for reproducible runs.

```magnesium
math.randomseed(42)
```

## math.round(x)

Round to the nearest integer.

```magnesium
print(math.round(3.5))   // 4
print(math.round(3.4))   // 3
print(math.round(-2.7))  // -3
```

## math.clamp(x, min, max)

Clamp `x` to the range `[min, max]`. Panics if `max < min`.

```magnesium
print(math.clamp(15, 0, 10))   // 10
print(math.clamp(-3, 0, 10))    // 0
print(math.clamp(5, 0, 10))     // 5
```
