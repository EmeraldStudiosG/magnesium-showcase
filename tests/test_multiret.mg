// Multiple return values
fn divide(a, b)
    if b == 0 then return null, "Division by zero" end
    return a / b, null
end

let result, err = divide(10, 3)
print(result)
print(err)

let result2, err2 = divide(10, 0)
print(result2)
print(err2)

// Simple multi-return
fn swap(a, b)
    return b, a
end

let x, y = swap(1, 2)
print(x)
print(y)

// Three return values
fn triple()
    return 10, 20, 30
end

let a, b, c = triple()
print(a)
print(b)
print(c)

// Missing return slots are filled with null for user and native calls
fn one()
    return 42
end

let only, missing = one()
print(only)
print(missing)

let len_value, len_err = len([1, 2, 3])
print(len_value)
print(len_err)
