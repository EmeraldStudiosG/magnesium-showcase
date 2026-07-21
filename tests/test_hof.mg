fn map_array(arr, f)
    let result = []
    for item in arr
        push(result, f(item))
    end
    return result
end

let nums = [1, 2, 3, 4, 5]
let doubled = map_array(nums, fn(x) return x * 2 end)
print(len(doubled))
for d in doubled
    print(d)
end