items = []
for i in range(3000000):
    items.append(i)

total = 0
for i in range(3000000):
    total = total + items[i]
print(total)
