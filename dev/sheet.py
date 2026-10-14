# Contact sheet of dev\shots\<prefix>*.png, in name order, cropped to the middle of the frame.
#   python sheet.py <prefix> [columns]
import sys
from pathlib import Path
from PIL import Image, ImageDraw

prefix = sys.argv[1]
columns = int(sys.argv[2]) if len(sys.argv) > 2 else 4
shots = Path(__file__).parent / 'shots'
files = sorted(shots.glob(prefix + '*.png'))
if not files:
    sys.exit('no shots for ' + prefix)
tiles = []
for f in files:
    im = Image.open(f).convert('RGB')
    w, h = im.size
    im = im.crop((int(w * 0.3), int(h * 0.3), int(w * 0.7), int(h * 0.95))).resize((400, 390))
    ImageDraw.Draw(im).text((6, 6), f.stem, fill=(255, 255, 0))
    tiles.append(im)
rows = (len(tiles) + columns - 1) // columns
sheet = Image.new('RGB', (400 * columns, 390 * rows))
for i, t in enumerate(tiles):
    sheet.paste(t, ((i % columns) * 400, (i // columns) * 390))
out = shots / ('sheet_' + prefix + '.png')
sheet.save(out)
print(out)
