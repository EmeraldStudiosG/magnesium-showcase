def mix(a, b, c):
    return a + b * 2 - c

total = 0
for i in range(10000000):
    total = total + mix(i, 3, 1)
print(total)
