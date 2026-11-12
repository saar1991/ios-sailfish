# Render ASCII 32..126 from DejaVu Sans Mono into an 8x16 bitmap font (font.h)
from PIL import Image, ImageDraw, ImageFont
f=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf',14)
out=['/* 8x16 font, ASCII 32..126, rendered from DejaVu Sans Mono */','static const unsigned char font8x16[95][16]={']
for c in range(32,127):
    im=Image.new('L',(8,16),0); ImageDraw.Draw(im).text((0,0),chr(c),font=f,fill=255)
    rows=[]
    for y in range(16):
        b=0
        for x in range(8):
            if im.getpixel((x,y))>110: b|=0x80>>x
        rows.append('0x%02x'%b)
    out.append(' {'+','.join(rows)+'},')
out.append('};'); open('font.h','w').write('\n'.join(out)+'\n')
