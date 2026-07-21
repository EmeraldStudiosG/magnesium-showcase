struct Node
    left
    right
end

fn make_tree(depth)
    if depth <= 0 then return Node { left = null, right = null } end
    return Node {
        left = make_tree(depth - 1),
        right = make_tree(depth - 1)
    }
end

print("Creating binary tree of depth 18...")
let tree = make_tree(18)
print("Done.")
