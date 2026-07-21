let items = []
for i in 0..3000000
    push(items, i)
end

let total = 0
for i in 0..3000000
    total = total + items[i]
end
print(total)
