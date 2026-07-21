total = 0
for i in range(50000):
    row = []
    for j in range(50):
        row.append(i + j)
    total = total + len(row)
print(total)
