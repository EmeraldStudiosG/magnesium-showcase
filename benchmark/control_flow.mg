let total = 0
for i in 0..3000000
    if i % 3 == 0 then
        total = total + 1
    elseif i % 3 == 1 then
        total = total + 2
    else
        total = total + 3
    end
end
print(total)
