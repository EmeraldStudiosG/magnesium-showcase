let secret = "hidden"

fn helper(x)
    return x + 1
end

export const value = helper(40)

export fn reveal()
    return secret
end
