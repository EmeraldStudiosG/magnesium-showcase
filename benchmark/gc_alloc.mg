let total = 0
for i in 0..50000
    let row = []
    for j in 0..50
        push(row, i + j)
    end
    total = total + len(row)
end
print(total)
