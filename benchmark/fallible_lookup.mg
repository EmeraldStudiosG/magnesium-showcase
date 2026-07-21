let config = &< value = 7 >
let total = 0

for i in 0..3000000
    total = total + config<"value">?
end

print(total)
