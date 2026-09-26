from PIL import Image
import sys

def convert(image_path):
    img = Image.open(image_path).convert('RGB')
    print(f"const uint16_t sprite[] = {{")
    for y in range(img.height):
        for x in range(img.width):
            r, g, b = img.getpixel((x, y))
            # Convert to RGB565
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            print(f"0x{rgb565:04X}, ", end="")
        print("")
    print("};")

if __name__ == "__main__":
    convert(sys.argv[1])
