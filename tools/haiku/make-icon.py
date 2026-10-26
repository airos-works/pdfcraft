#!/usr/bin/env python3
"""Original Haiku PDF pages and annotation. No vendor artwork (MIT OR Apache-2.0)."""
from icon_art import Drawing, INK, plane

d = Drawing()
p = plane(10, 4, 0.3)
d.shadow([(10,47),(43,57),(62,57),(32,47)])
d.shape([(17,5),(48,14),(48,51),(44,56),(17,48)], d.gradient('e7eaf0','9caab9'), INK)
d.shape(p([(0,0),(25,0),(33,8),(33,43),(0,43)]), d.gradient('fffefa','d9dde2'), INK)
d.shape(p([(0,0),(5,0),(5,43),(0,43)]), d.gradient('e99a88','ad5149'))
d.shape(p([(25,0),(25,8),(33,8)]), d.gradient('e2e7ec','9ba9b9'), '71818f', 1)
for v, end in [(16,27),(22,27),(28,21)]:
    d.line(p([(10,v),(end,v)]), '7e8995', 1.8)
# Flat PDF label: original simple vector lettering, no font dependency.
d.shape([(5,37),(37,37),(37,51),(5,51)], d.gradient('db8070','a9443d', (8,37),(37,53)), INK)
for points in [ [(10,48),(10,40),(15,40),(15,44),(10,44)],
                [(20,48),(20,40),(24,40),(26,42),(26,46),(24,48),(20,48)],
                [(30,48),(30,40),(35,40)], [(30,44),(34,44)] ]:
    d.line(points, 'fff7ea', 1.7)
# Annotation pencil at the right edge.
d.shape([(41,47),(51,27),(57,30),(47,50),(40,54)], d.gradient('ffe5a0','cc963e'), INK, 1.7)
d.shape([(51,27),(53,23),(59,26),(57,30)], d.gradient('e8c4b9','b87368'), INK, 1.5)
d.shape([(41,47),(47,50),(40,54)], 'f3ddad', INK, 1.2)
d.line([(40,54),(42,51)], INK, 1.5)
d.save(__file__, 'pdfcraft', 'PdfCraft')
