dict = {}
print("Filling dict with 250,000 keys...")
for i in range(250000):
    dict[str(i)] = i

sum_val = 0
print("Accessing 250,000 keys...")
for i in range(250000):
    sum_val += dict[str(i)]
print("Sum:", sum_val)
