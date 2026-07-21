fn sieve(n)
    let count = 0
    let flags = []
    for i in 0..=n push(flags, true) end
    
    for i in 2..=n
        if flags[i] then
            count = count + 1
            let j = i * 2
            loop
                if j > n then break end
                flags[j] = false
                j = j + i
            end
        end
    end
    return count
end

print("Sieving up to 1,000,000...")
print(sieve(1000000))
