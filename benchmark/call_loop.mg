fn mix(a, b, c)
    return a + b * 2 - c
end

let total = 0
for i in 0..10000000
    total = total + mix(i, 3, 1)
end
print(total)
