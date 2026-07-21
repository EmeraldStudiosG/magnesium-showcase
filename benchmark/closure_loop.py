def make_accumulator(seed):
    total = seed
    def add(n):
        nonlocal total
        total = total + n
        return total
    return add

add = make_accumulator(0)
check = 0
for i in range(5000000):
    check = add(i)
print(check)
