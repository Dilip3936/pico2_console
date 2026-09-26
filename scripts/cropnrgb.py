from PIL import Image

def extract_sprites(image_path, output_file):
    img = Image.open(image_path).convert('RGBA')
    
    sprite_names = [
        "pipe_middle", "pipe_bottom", "pipe_top", 
        "bird1", "bird2", "bird3"
    ]
    
    with open(output_file, "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n\n")
        
        for i in range(6):
            name = sprite_names[i]
            x = i * 16  # Moves right by 16 pixels for each sprite
            y = 0
            w, h = 16, 16
            
            f.write(f"const uint16_t {name}[{w * h}] = {{\n")
            
            cropped = img.crop((x, y, x + w, y + h))
            for cy in range(h):
                for cx in range(w):
                    r, g, b, a = cropped.getpixel((cx, cy))
                    
                    # If pixel is mostly transparent OR is pure white (background), force it to Magenta (0xF81F)
                    if a < 128 or (r == 255 and g == 255 and b == 255):
                        rgb565 = 0xF81F
                    else:
                        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                        
                    f.write(f"0x{rgb565:04X}, ")
                f.write("\n")
            
            f.write("};\n\n")

if __name__ == "__main__":
    extract_sprites("assets/flappy.png", "include/sprites.h")
    print("Successfully generated include/sprites.h")
