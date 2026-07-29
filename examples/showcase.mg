// ============================================
// Magnesium v1.1: Feature Showcase
// ============================================

// --- Variables ---
let x = 42
const PI = 3.14159
let @global_counter = 0

print("--- Variables ---")
print(x)
print(PI)

// --- Data Types ---
print("--- Types ---")
print(type(42))
print(type("hello"))
print(type(true))
print(type(null))

// --- Arithmetic ---
print("--- Arithmetic ---")
print(10 + 5)
print(10 - 3)
print(6 * 7)
print(100 / 4)
print(17 % 5)
print(-42)

// --- Comparison & Logic ---
print("--- Comparison ---")
print(10 > 5)
print(10 <= 10)
print(10 == 10)
print(10 != 5)
print(true and true)
print(false or true)
print(!false)

// --- Strings ---
print("--- Strings ---")
let greeting = "Hello" + ", " + "World!"
print(greeting)
print(len(greeting))

// --- Control Flow ---
print("--- Control Flow ---")
let score = 92
if score >= 90 then
    print("Grade: A")
elseif score >= 80 then
    print("Grade: B")
else
    print("Grade: C or below")
end

// --- Loops ---
print("--- Loops ---")

// For range (exclusive)
let sum = 0
for i in 1..6
    sum = sum + i
end
print(sum)

// For range (inclusive)
let product = 1
for i in 1..=5
    product = product * i
end
print(product)

// Loop with break
let n = 0
loop
    n = n + 1
    if n > 3 then
        break
    end
end
print(n)

// --- Arrays ---
print("--- Arrays ---")
let colors = ["red", "green", "blue"]
print(colors[0])
print(len(colors))
push(colors, "yellow")
for c in colors
    print(c)
end

// --- Dictionaries ---
print("--- Dictionaries ---")
let config = &< host = "localhost", port = 8080 >
print(config<"host">)
config<"debug"> = true
print(config<"debug">)

// --- Functions ---
print("--- Functions ---")
fn fibonacci(n)
    if n <= 1 then
        return n
    end
    return fibonacci(n - 1) + fibonacci(n - 2)
end

for i in 0..10
    print(fibonacci(i))
end

// --- Closures ---
print("--- Closures ---")
fn make_multiplier(factor)
    fn multiply(x)
        return x * factor
    end
    return multiply
end

let triple = make_multiplier(3)
print(triple(7))
print(triple(10))

// --- Higher-order Functions ---
fn map_array(arr, f)
    let result = []
    for item in arr
        push(result, f(item))
    end
    return result
end

let nums = [1, 2, 3, 4, 5]
let doubled = map_array(nums, fn(x) return x * 2 end)
for d in doubled
    print(d)
end

// --- Global State ---
print("--- Globals ---")
fn increment_global()
    @global_counter = @global_counter + 1
end

increment_global()
increment_global()
increment_global()
print(@global_counter)

print("--- Done! ---")
