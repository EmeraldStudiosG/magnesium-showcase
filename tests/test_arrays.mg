// Array test
let items = ["Engine", "Compiler", "VM"]
print(items[0])
print(items[1])
print(items[2])
print(len(items))

// Push
push(items, "GC")
print(len(items))
print(items[3])

// For-in iteration
for item in items
    print(item)
end

// Array of numbers
let nums = [10, 20, 30, 40]
let total = 0
for n in nums
    total = total + n
end
print(total)