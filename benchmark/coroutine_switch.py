limit = 200_000

def worker():
    for i in range(limit):
        yield i

total = 0
co = worker()
for _ in range(limit):
    total += next(co)

print(total)
