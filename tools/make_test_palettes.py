"""Make two obviously recolored test palettes from the game's own Reimu and Sanae palette 000
(game format .pal: 1 byte bpp + 256 16-bit colors, e.g. from a shady-loader package or TextureDump).

  python make_test_palettes.py <reimu palette000.pal> <sanae palette000.pal> <output folder>

Writes "Blue Reimu.pal" and "Inverted Sanae.pal" there: put them in a library folder
(modules\PaletteShare\palettes\reimu, ...\sanae) or point [Debug] ForcePaletteLeft/Right at them."""
import os, struct, sys

if len(sys.argv) != 4:
    sys.exit(__doc__)
reimu_file, sanae_file, out = sys.argv[1:]

def load(path):
    d = open(path, "rb").read()
    assert d[0] == 16 and len(d) == 513, path + ": not a 16-bit game palette"
    return list(struct.unpack("<256H", d[1:]))

def save(name, colors):
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, name)
    open(path, "wb").write(bytes([16]) + struct.pack("<256H", *colors))
    print(path)

def split(c): return c >> 15, (c >> 10) & 31, (c >> 5) & 31, c & 31
def join(a, r, g, b): return a << 15 | r << 10 | g << 5 | b

# Reimu -> red/blue swapped ("blue Reimu")
reimu = load(reimu_file)
save("Blue Reimu.pal", [c if i == 0 else join(split(c)[0], split(c)[3], split(c)[2], split(c)[1]) for i, c in enumerate(reimu)])
# Sanae -> color negative ("inverted Sanae"), alpha bit and index 0 kept
sanae = load(sanae_file)
save("Inverted Sanae.pal", [c if i == 0 else join(split(c)[0], 31 - split(c)[1], 31 - split(c)[2], 31 - split(c)[3]) for i, c in enumerate(sanae)])
