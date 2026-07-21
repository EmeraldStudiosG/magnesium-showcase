let name = "Magnesium"
let count = 5

for i in 0..count
    print("Hello from " + name + " #" + tostring(i + 1))
end

let sum = 0
for i in 0..=100
    sum += i
end

print("Sum 1..100 = " + tostring(sum))
