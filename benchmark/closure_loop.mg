fn make_accumulator(seed)
    let total = seed
    fn add(n)
        total = total + n
        return total
    end
    return add
end

let add = make_accumulator(0)
let check = 0
for i in 0..5000000
    check = add(i)
end
print(check)
