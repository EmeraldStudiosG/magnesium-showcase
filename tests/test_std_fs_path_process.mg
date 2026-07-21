let file = path.join("tests", "tmp_std_fs_path_process.txt")
let renamed = path.join("tests", "tmp_std_fs_path_process_renamed.txt")

fs.remove(file)
fs.remove(renamed)

let write_ok = fs.write(file, "alpha")?
print(write_ok)
print(fs.exists(file))

fs.append(file, "-beta")?
let text = fs.read(file)?
print(text)

print(path.basename(file))
print(path.dirname(file))
print(path.ext("archive.tar.mg"))

fs.rename(file, renamed)?
print(fs.exists(file))
print(fs.exists(renamed))

let missing, missing_err = try
    fs.read(file)?
end
print(missing)
print(missing_err.kind)

fs.remove(renamed)?
print(type(fs.cwd()))
print(type(process.platform()))
