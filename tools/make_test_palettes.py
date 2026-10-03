"""Make two obviously recolored test palettes from cached vanilla ones and put them in the
PaletteShare caches of the given game installs (named by their PaletteShare hash).

  python make_test_palettes.py <Soku folder> [<another Soku folder> ...]

The first folder's cache must already hold Reimu's and Sanae's palette 000 (play them once)."""
import hashlib, os, shutil, struct, sys

INSTALLS = sys.argv[1:]
if not INSTALLS:
    sys.exit(__doc__)
SRC_CACHE = os.path.join(INSTALLS[0], "modules", "PaletteShare", "cache")

def load(h):
    d = open(os.path.join(SRC_CACHE, h + ".pal"), "rb").read()
    assert d[0] == 16 and len(d) == 513
    return list(struct.unpack("<256H", d[1:]))

def save(colors):
    data = bytes([16]) + struct.pack("<256H", *colors)
    h = hashlib.sha256(data).digest()[:16].hex()
    for inst in INSTALLS:
        d = os.path.join(inst, r"modules\PaletteShare\cache")
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, h + ".pal"), "wb").write(data)
    return h

def split(c): return c >> 15, (c >> 10) & 31, (c >> 5) & 31, c & 31
def join(a, r, g, b): return a << 15 | r << 10 | g << 5 | b

# Reimu palette000 -> red/blue swapped ("blue Reimu")
reimu = load("a494bdaff4fe1fb76679057eb8fd060e")
blue = [c if i == 0 else join(split(c)[0], split(c)[3], split(c)[2], split(c)[1]) for i, c in enumerate(reimu)]
# Sanae palette000 -> color negative ("inverted Sanae"), alpha bit and index 0 kept
sanae = load("438ea549291e2dc0c8ecdcbc9b59f669")
inv = [c if i == 0 else join(split(c)[0], 31 - split(c)[1], 31 - split(c)[2], 31 - split(c)[3]) for i, c in enumerate(sanae)]

print("blue_reimu", save(blue))
print("inverted_sanae", save(inv))
