total = 0
for i in range(3000000):
    if i % 3 == 0:
        total = total + 1
    elif i % 3 == 1:
        total = total + 2
    else:
        total = total + 3
print(total)
