// Structured Lua-style errors with Rust-style propagation ergonomics.

fn require_port(config)
    return config<"port">?
end

fn plus_one_port(config)
    let port = require_port(config)?
    return port + 1
end

let good, good_err = plus_one_port(&< port = 8080 >)
print(good)
print(good_err)

let bad, bad_err = plus_one_port(&< host = "localhost" >)
print(bad)
print(type(bad_err))
print(bad_err.kind)
print(bad_err.message)
print(bad_err.hint)

let tried, tried_err = try
    plus_one_port(&< host = "localhost" >)?
end
print(tried)
print(tried_err.kind)
print(tried_err.message)

try
    let port = plus_one_port(&< host = "localhost" >)?
    print(port)
catch caught
    print("caught")
    print(caught.kind)
    print(caught.message)
end

fn custom_failure()
    return null, Err("ConfigError", "Broken config", "Write a valid config file.")
end

let custom, custom_err = custom_failure()
print(custom)
print(custom_err.kind)
print(custom_err.message)
print(custom_err.hint)

try
    let value = custom_failure()?
    print(value)
catch e
    print(e.kind)
    print(e.message)
end
