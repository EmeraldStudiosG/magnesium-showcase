total = 0
for i in range(5000000):
    total = total + i * 3 - i / 2 + i % 7
print(total)
