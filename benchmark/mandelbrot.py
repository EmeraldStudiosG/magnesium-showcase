def mandelbrot(n):
    total = 0
    for y in range(n):
        y_pos = y * 2 / n - 1
        for x in range(n):
            x_pos = x * 3.5 / n - 2.5
            z_re = 0
            z_im = 0
            i = 0
            while i < 50:
                z_re2 = z_re * z_re
                z_im2 = z_im * z_im
                if z_re2 + z_im2 > 4: break
                z_im = 2 * z_re * z_im + y_pos
                z_re = z_re2 - z_im2 + x_pos
                i += 1
            total += i
    return total

print("Calculating Mandelbrot (400x400)...")
print(mandelbrot(400))
print("Done.")
