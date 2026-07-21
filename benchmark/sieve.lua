function sieve(n)
    local count = 0
    local flags = {}
    for i = 0, n do flags[i] = true end
    
    for i = 2, n do
        if flags[i] then
            count = count + 1
            local j = i * 2
            while j <= n do
                flags[j] = false
                j = j + i
            end
        end
    end
    return count
end

print("Sieving up to 1,000,000...")
print(sieve(1000000))
