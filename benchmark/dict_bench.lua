local dict = {}
print("Filling dict with 250,000 keys...")
for i = 0, 249999 do
    dict[tostring(i)] = i
end

local sum = 0
print("Accessing 250,000 keys...")
for i = 0, 249999 do
    sum = sum + dict[tostring(i)]
end
print("Sum: " .. sum)
