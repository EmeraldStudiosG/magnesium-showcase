# path

Path manipulation without touching the filesystem.

## path.join(...)

Join path segments with the OS separator.

```magnesium
let file = path.join("assets", "sprites", "player.png")
print(file)  // assets/sprites/player.png (or assets\sprites\player.png on Windows)
```

## path.basename(p)

Return the file name from a path.

```magnesium
print(path.basename("assets/player.png"))  // player.png
```

## path.dirname(p)

Return the directory portion of a path.

```magnesium
print(path.dirname("assets/player.png"))  // assets
```

## path.ext(p)

Return the file extension, including the dot.

```magnesium
print(path.ext("player.png"))      // .png
print(path.ext("archive.tar.gz"))  // .gz
```
