!strict

type Named = &<
    name: string
>

struct Person
    name: string
    age: number
end

fn greet(person: Named): string
    return person.name
end

let person = Person { name = "Ada", age = 37 }
print(greet(person))
print(greet(&< name = "Grace", extra = true >))

let local_name: string = "local"
let @local_name: number = 41
let local_copy: string = local_name
let global_copy: number = @local_name
print(local_copy)
print(global_copy + 1)

let double: fn(number): number = fn(value: number): number
    return value * 2
end

fn make_adder(amount: number): fn(number): number
    fn add(value: number): number
        return value + amount
    end
    return add
end

let add_three: fn(number): number = make_adder(3)
print(double(add_three(4)))

let from_and: string = true and "and value"
let from_or: string = false or "or value"
print(from_and)
print(from_or)
