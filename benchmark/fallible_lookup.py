config = {"value": 7}
total = 0

for _ in range(3_000_000):
    total += config["value"]

print(total)
