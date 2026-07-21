fn require_name(config)
    return dict.require(config, "name")?
end

fn second(items)
    return array.get(items, 1)?
end

fn third_char(text)
    return string.at(text, 2)?
end

let name, name_err = require_name(&< name = "Magnesium" >)
print(name)
print(name_err)

let missing, missing_err = require_name(&< port = 8080 >)
print(missing)
print(missing_err.kind)
print(missing_err.message)

let item, item_err = second(["a", "b", "c"])
print(item)
print(item_err)

try
    let value = second(["only"])? 
    print(value)
catch err
    print(err.kind)
    print(err.hint)
end

let letter, letter_err = try
    third_char("abc")?
end
print(letter)
print(letter_err)

let missed_letter, missed_letter_err = try
    third_char("a")?
end
print(missed_letter)
print(missed_letter_err.kind)
print(missed_letter_err.hint)
