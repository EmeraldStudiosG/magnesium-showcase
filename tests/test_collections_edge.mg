let empty = []
print(len(empty))
print(empty[0])

empty[2] = "third"
print(len(empty))
print(empty[0])
print(empty[2])

let dict = &<>
print(dict<"missing">)
dict<"answer"> = 42
print(dict<"answer">)

let text = "abc"
print(text[0])
print(text[3])

let built = "an" + "swer"
print(built == "answer")
print(dict<built>)
