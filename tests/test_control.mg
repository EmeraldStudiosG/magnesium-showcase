// Control flow test

// If/else
let x = 10
if x > 5 then
    print("x is greater than 5")
else
    print("x is 5 or less")
end

// If/elseif/else
let score = 85
if score >= 90 then
    print("A")
elseif score >= 80 then
    print("B")
elseif score >= 70 then
    print("C")
else
    print("F")
end

// Logical operators
if true and true then
    print("and works")
end

if false or true then
    print("or works")
end

// Comparison
print(10 == 10)
print(10 != 5)
print("hello" == "hello")

// Loop with break
let count = 0
loop
    count = count + 1
    if count >= 5 then
        break
    end
end
print(count)

// For range (exclusive)
let sum = 0
for i in 0..5
    sum = sum + i
end
print(sum)

// For range (inclusive)
let sum2 = 0
for i in 1..=5
    sum2 = sum2 + i
end
print(sum2)