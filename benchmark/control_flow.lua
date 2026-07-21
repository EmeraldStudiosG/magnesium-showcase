local total = 0
for i = 0, 2999999 do
    if i % 3 == 0 then
        total = total + 1
    elseif i % 3 == 1 then
        total = total + 2
    else
        total = total + 3
    end
end
print(total)
