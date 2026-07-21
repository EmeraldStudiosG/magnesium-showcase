# array

Array manipulation functions. Arrays are zero-indexed.

`array.get`, `array.at`, and direct `arr[index]` access use Magnesium's zero-based indexing. `array.insert` and `array.remove` take the insertion/removal position used by the current standard-library implementation.

## array.new()

Create an empty array.

```magnesium
let items = array.new()
push(items, 1)
print(len(items))  // 1
```

## array.len(arr)

Return the number of elements. Same as the global `len`.

```magnesium
print(array.len([1, 2, 3]))  // 3
```

## array.push(arr, val) / array.pop(arr)

Append to the end, or remove and return the last element.

```magnesium
let items = [1, 2]
array.push(items, 3)          // [1, 2, 3]
let last = array.pop(items)   // 3, items = [1, 2]
```

## array.insert(arr, i, val)

Insert `val` at one-based position `i`, shifting later elements right.

```magnesium
let items = ["a", "c"]
array.insert(items, 2, "b")  // ["a", "b", "c"]
```

## array.remove(arr, i)

Remove and return the element at one-based position `i`, shifting later elements left.

```magnesium
let items = ["a", "b", "c"]
let removed = array.remove(items, 2)  // "b", items = ["a", "c"]
```

## array.get(arr, index) / array.at(arr, index)

Get the element at zero-based `index`. Fallible - returns an error on out of range.

```magnesium
let items = [10, 20, 30]
let val = array.get(items, 1)?
print(val)  // 20
```

## array.clear(arr)

Remove all elements.

```magnesium
let items = [1, 2, 3]
array.clear(items)
print(len(items))  // 0
```

## array.contains(arr, value)

Return `true` if `arr` contains `value`.

```magnesium
print(array.contains([1, 2, 3], 2))  // true
print(array.contains([1, 2, 3], 5))  // false
```

## array.index_of(arr, value)

Return the zero-based index of the first occurrence, or `null` if not found.

```magnesium
print(array.index_of(["a", "b", "c"], "b"))  // 1
print(array.index_of(["a", "b", "c"], "z"))  // null
```

## array.first(arr) / array.last(arr)

Return the first or last element, or `null` if the array is empty.

```magnesium
let items = [10, 20, 30]
print(array.first(items))  // 10
print(array.last(items))   // 30
```

## array.extend(arr, other)

Append all elements from `other` to `arr`. Returns the modified `arr`.

```magnesium
let a = [1, 2]
let b = [3, 4]
array.extend(a, b)
print(a)  // [1, 2, 3, 4]
```

## array.slice(arr, start, end?)

Return a new array with elements from zero-based `start` to `end` (exclusive). Defaults to the end of the array.

```magnesium
let items = [10, 20, 30, 40, 50]
print(array.slice(items, 1, 3))   // [20, 30]
print(array.slice(items, 2))      // [30, 40, 50]
```
