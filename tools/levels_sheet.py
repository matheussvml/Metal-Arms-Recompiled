"""Folha de contato da varredura de fases: a última captura de cada fase, com o número.

Uso: python tools/levels_sheet.py <primeira> <última> <saída.png>
"""
import os
import sys

from PIL import Image, ImageDraw

first, last, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
W, H, COLS = 320, 240, 4
items = []
for i in range(first, last + 1):
    d = f"build/levels/shots_{i:02d}"
    shots = sorted(os.listdir(d)) if os.path.isdir(d) else []
    items.append((i, os.path.join(d, shots[-1]) if shots else None))
rows = (len(items) + COLS - 1) // COLS
sheet = Image.new("RGB", (W * COLS, H * rows))
draw = ImageDraw.Draw(sheet)
for k, (i, path) in enumerate(items):
    x, y = (k % COLS) * W, (k // COLS) * H
    if path:
        sheet.paste(Image.open(path).convert("RGB").resize((W, H)), (x, y))
    draw.rectangle([x, y, x + 34, y + 20], fill=(0, 0, 0))
    draw.text((x + 6, y + 4), f"{i:02d}", fill=(255, 255, 0))
sheet.save(out)
