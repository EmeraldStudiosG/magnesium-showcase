fn mandelbrot(n)
    let total = 0
    for y in 0..n
        let y_pos = y * 2 / n - 1
        for x in 0..n
            let x_pos = x * 3.5 / n - 2.5
            let z_re = 0
            let z_im = 0
            let i = 0
            loop
                if i >= 50 then break end
                let z_re2 = z_re * z_re
                let z_im2 = z_im * z_im
                if z_re2 + z_im2 > 4 then break end
                z_im = 2 * z_re * z_im + y_pos
                z_re = z_re2 - z_im2 + x_pos
                i = i + 1
            end
            total = total + i
        end
    end
    return total
end

print("Calculating Mandelbrot (400x400)...")
print(mandelbrot(400))
print("Done.")
