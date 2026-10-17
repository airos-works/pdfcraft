#!/usr/bin/env python3
"""Original folded-page icon for the air/OS fork (MIT OR Apache-2.0)."""
from pathlib import Path
import hvif
icon = {'styles': [{'color': hvif.hex_color(c)} for c in ['#244B72','#F7F3E8','#C7D7E5','#D59A32']], 'paths': [], 'shapes': []}
def shape(style, points):
    n = len(icon['paths'])
    icon['paths'].append({'closed': True, 'points': points})
    icon['shapes'].append({'style': style, 'paths': [n]})
shape(0, [(10,3),(40,3),(55,18),(55,61),(10,61)])
shape(1, [(14,7),(37,7),(51,21),(51,57),(14,57)])
shape(2, [(37,7),(37,21),(51,21)])
for y,w in [(28,28),(35,28),(42,20)]:
    shape(0, [(19,y),(19+w,y),(19+w,y+3),(19,y+3)])
shape(3, [(6,48),(33,48),(33,57),(6,57)])
out = Path(__file__).resolve().parents[2] / 'apps/pdfcraft-haiku/resources/PdfCraft.hvif'
out.write_bytes(hvif.encode(icon))
