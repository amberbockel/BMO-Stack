#!/usr/bin/env python3
"""Generate a tiny placeholder animated GIF so we can verify the firmware
playback works before real GIFs land. Cycles BMO-green frames with a moving
white block that pretends to be a kick."""
from PIL import Image, ImageDraw
import sys, os

out_path = sys.argv[1] if len(sys.argv) > 1 else "placeholder.gif"
W, H, N = 120, 90, 8
frames = []
for i in range(N):
    img = Image.new("RGB", (W, H), (40, 140, 110))  # BMO-ish teal
    d = ImageDraw.Draw(img)
    # Body
    d.rectangle([35, 20, 85, 75], fill=(60, 200, 160), outline=(30, 80, 60))
    # Eyes
    d.rectangle([45, 35, 55, 50], fill=(20, 20, 30))
    d.rectangle([65, 35, 75, 50], fill=(20, 20, 30))
    # Moving kick block
    x = 10 + (i * 12) % 100
    d.rectangle([x, 75, x + 12, 88], fill=(255, 255, 255))
    frames.append(img)

frames[0].save(out_path, save_all=True, append_images=frames[1:],
               duration=120, loop=0)
print(f"wrote {out_path}")
