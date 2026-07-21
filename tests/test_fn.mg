// Function test
fn add(a, b)
    return a + b
end

print(add(3, 7))

fn greet(name)
    print("Hello, " + name + "!")
end

greet("Magnesium")

// Recursion
fn factorial(n)
    if n <= 1 then
        return 1
    end
    return n * factorial(n - 1)
end

print(factorial(5))
print(factorial(10))