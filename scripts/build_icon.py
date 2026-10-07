"""Original geometric Studio mark; reproducible PNG/Windows ICO, no external artwork."""
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1] / 'assets'
root.mkdir(exist_ok=True)
svg = '''<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
<rect x="8" y="8" width="240" height="240" rx="52" fill="#242235"/>
<path d="M56 58H128V88M164 181V211H210" fill="none" stroke="#9085ff" stroke-width="12" stroke-linejoin="round"/>
<circle cx="56" cy="58" r="13" fill="#60eadb"/><circle cx="210" cy="211" r="13" fill="#60eadb"/>
<rect x="41" y="88" width="174" height="101" rx="29" fill="#a79eff"/>
<rect x="58" y="105" width="140" height="52" rx="18" fill="#242235"/>
<rect x="77" y="119" width="24" height="24" rx="8" fill="#60eadb"/>
<rect x="155" y="119" width="24" height="24" rx="8" fill="#60eadb"/>
<path d="M104 174H152" stroke="#242235" stroke-width="10" stroke-linecap="round"/>
</svg>'''
(root / 'studio.svg').write_text(svg + '\n', encoding='utf8')
# Draw the same source geometry at 4x for crisp antialiased Windows icon sizes.
s = 4
image = Image.new('RGBA', (256*s, 256*s))
d = ImageDraw.Draw(image)
def rect(box, radius, color): d.rounded_rectangle(tuple(v*s for v in box), radius=radius*s, fill=color)
def line(points, color, width): d.line([(x*s,y*s) for x,y in points], fill=color, width=width*s, joint='curve')
rect((8,8,248,248),52,'#242235')
line([(56,58),(128,58),(128,88)],'#9085ff',12)
line([(164,181),(164,211),(210,211)],'#9085ff',12)
for x,y in [(56,58),(210,211)]: d.ellipse(((x-13)*s,(y-13)*s,(x+13)*s,(y+13)*s),fill='#60eadb')
rect((41,88,215,189),29,'#a79eff')
rect((58,105,198,157),18,'#242235')
rect((77,119,101,143),8,'#60eadb');rect((155,119,179,143),8,'#60eadb')
rect((99,169,157,179),5,'#242235')
image = image.resize((256,256),Image.Resampling.LANCZOS)
image.save(root/'studio.png')
image.save(root/'studio.ico',sizes=[(v,v) for v in (16,20,24,32,40,48,64,128,256)])
print('Created original SVG, PNG and 9-size Windows ICO')
