function make_tree(depth)
    if depth <= 0 then return {left = nil, right = nil} end
    return {
        left = make_tree(depth - 1),
        right = make_tree(depth - 1)
    }
end

print("Creating binary tree of depth 18...")
local tree = make_tree(18)
print("Done.")
