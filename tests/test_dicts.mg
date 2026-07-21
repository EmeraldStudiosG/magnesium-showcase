// Dictionary test
let config = &< host = "localhost", port = 8080, debug = true >
print(config<"host">)
print(config<"port">)
print(config<"debug">)

// Dict set
config<"port"> = 3000
print(config<"port">)
