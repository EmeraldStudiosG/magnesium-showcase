class Node:
    def __init__(self, left, right):
        self.left = left
        self.right = right

def make_tree(depth):
    if depth <= 0: return Node(None, None)
    return Node(
        make_tree(depth - 1),
        make_tree(depth - 1)
    )

print("Creating binary tree of depth 18...")
tree = make_tree(18)
print("Done.")
