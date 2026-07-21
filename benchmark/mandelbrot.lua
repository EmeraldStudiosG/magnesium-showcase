function mandelbrot(n)
    local total = 0
    for y = 0, n - 1 do
        local y_pos = y * 2 / n - 1
        for x = 0, n - 1 do
            local x_pos = x * 3.5 / n - 2.5
            local z_re = 0
            local z_im = 0
            local i = 0
            while i < 50 do
                local z_re2 = z_re * z_re
                local z_im2 = z_im * z_im
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
