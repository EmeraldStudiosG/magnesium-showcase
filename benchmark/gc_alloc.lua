local total = 0
for i = 0, 49999 do
    local row = {}
    for j = 0, 49 do
        row[j + 1] = i + j
    end
    total = total + #row
end
print(total)
