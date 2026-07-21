let dict = &< >
print("Filling dict with 250,000 keys...")
for i in 0..250000
    dict<tostring(i)> = i
end

let sum = 0
print("Accessing 250,000 keys...")
for i in 0..250000
    sum = sum + dict<tostring(i)>
end
print("Sum:", sum)
