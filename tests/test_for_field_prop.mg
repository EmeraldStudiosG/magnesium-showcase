let config = &< value = 7 >
let total = 0

for i in 0..5
    total = total + config<"value">?
end

print(total)

fn local_sum(items)
    let sum = 0
    for i in 0..5
        sum = sum + items<"value">?
    end
    return sum
end

print(local_sum(config))

fn missing_sum(items)
    let sum = 0
    for i in 0..3
        sum = sum + items<"missing">?
    end
    return sum
end

let missing, missing_err = missing_sum(config)
print(missing)
print(missing_err)

let zero_total = 0
for i in 5..5
    zero_total = zero_total + config<"missing">?
end
print(zero_total)
