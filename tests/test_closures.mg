// Closure test
fn make_counter()
    let count = 0
    fn increment()
        count = count + 1
        return count
    end
    return increment
end

let counter = make_counter()
print(counter())
print(counter())
print(counter())

// Higher-order functions
fn apply(f, x)
    return f(x)
end

fn double(n)
    return n * 2
end

print(apply(double, 21))