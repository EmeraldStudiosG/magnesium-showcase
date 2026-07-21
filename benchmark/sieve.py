def sieve(n):
    count = 0
    flags = [True] * (n + 1)
    
    for i in range(2, n + 1):
        if flags[i]:
            count += 1
            j = i * 2
            while j <= n:
                flags[j] = False
                j += i
    return count

print("Sieving up to 1,000,000...")
print(sieve(1000000))
