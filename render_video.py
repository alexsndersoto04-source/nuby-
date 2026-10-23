import os
import sys
import math
import json
import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H = 1280, 720
FPS = 15

# Load fonts
FONT_BOLD_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

def get_font(size, bold=True):
    p = FONT_BOLD_PATH if bold else FONT_PATH
    return ImageFont.truetype(p, size)

font_huge = get_font(90, True)
font_big = get_font(52, True)
font_med = get_font(34, True)
font_card = get_font(28, True)
font_sub = get_font(26, True)
font_small = get_font(22, True)

# Preload character images
sprites = {}
for name in ['sprite_character_a.png', 'sprite_character_e.png', 'sprite_character_i.png', 'sprite_character_o.png', 'sprite_character_u.png']:
    p = os.path.join('images', name)
    if os.path.exists(p):
        sprites[name] = Image.open(p).convert('RGBA')

if os.path.exists('images/characters_all.png'):
    im_all = Image.open('images/characters_all.png').convert('RGBA')
    sprites['characters_all.png'] = im_all

# Pre-render colorful base background (Sky + Sun + Hills)
def make_base_background():
    bg = Image.new('RGB', (W, H), (140, 215, 255))
    draw = ImageDraw.Draw(bg)
    # Sky gradient
    for y in range(H):
        r = int(140 + (220 - 140) * (y / H))
        g = int(215 + (245 - 215) * (y / H))
        b = int(255 + (220 - 255) * (y / H))
        draw.line([(0, y), (W, y)], fill=(r, g, b))
    
    # Sun
    sun_x, sun_y = 1100, 110
    draw.ellipse([sun_x - 80, sun_y - 80, sun_x + 80, sun_y + 80], fill=(255, 240, 80))
    draw.ellipse([sun_x - 65, sun_y - 65, sun_x + 65, sun_y + 65], fill=(255, 220, 50))
    
    # Rolling green hills at bottom
    # Back hill
    draw.chord([ -200, 420, 900, 950 ], 0, 360, fill=(130, 205, 80))
    # Front hill
    draw.chord([ 400, 450, 1500, 1000 ], 0, 360, fill=(95, 185, 60))
    draw.chord([ -100, 480, 800, 1050 ], 0, 360, fill=(80, 175, 50))
    return bg

base_bg = make_base_background()

# Card definitions for each vowel
CARDS_DATA = {
    "A a": [
        {"icon": "✈️", "word": "AVIÓN", "sub": "Vuela alto", "color": (255, 100, 100)},
        {"icon": "🌳", "word": "ÁRBOL", "sub": "Verde y lindo", "color": (80, 190, 80)},
        {"icon": "🐝", "word": "ABEJA", "sub": "Dulce miel", "color": (255, 190, 40)}
    ],
    "E e": [
        {"icon": "⭐", "word": "ESTRELLA", "sub": "Brilla de noche", "color": (255, 210, 50)},
        {"icon": "🪜", "word": "ESCALERA", "sub": "Para subir", "color": (80, 170, 240)},
        {"icon": "🐘", "word": "ELEFANTE", "sub": "Gran trompa", "color": (180, 140, 230)}
    ],
    "I i": [
        {"icon": "🏝️", "word": "ISLA", "sub": "En el mar", "color": (60, 210, 200)},
        {"icon": "🧲", "word": "IMÁN", "sub": "Atrae metal", "color": (255, 110, 130)},
        {"icon": "🦎", "word": "IGUANA", "sub": "Verde saltarina", "color": (110, 200, 70)}
    ],
    "O o": [
        {"icon": "🐻", "word": "OSO", "sub": "Cariñoso", "color": (190, 120, 70)},
        {"icon": "👁️", "word": "OJO", "sub": "Para mirar", "color": (255, 140, 60)},
        {"icon": "🍲", "word": "OLLA", "sub": "Sopa rica", "color": (240, 90, 100)}
    ],
    "U u": [
        {"icon": "🍇", "word": "UVAS", "sub": "Dulces frutas", "color": (180, 90, 220)},
        {"icon": "🦄", "word": "UNICORNIO", "sub": "Mágico", "color": (255, 140, 210)},
        {"icon": "1️⃣", "word": "UNO", "sub": "El número 1", "color": (70, 180, 240)}
    ]
}

def draw_rounded_rect(draw, bbox, radius, fill, outline=None, width=3):
    draw.rounded_rectangle(bbox, radius=radius, fill=fill, outline=outline, width=width)

def draw_text_with_outline(draw, pos, text, font, fill=(255, 255, 255), outline=(0, 0, 0), outline_w=3):
    x, y = pos
    for dx in range(-outline_w, outline_w + 1):
        for dy in range(-outline_w, outline_w + 1):
            if dx != 0 or dy != 0:
                draw.text((x + dx, y + dy), text, font=font, fill=outline)
    draw.text((x, y), text, font=font, fill=fill)

def render_frame(t, scene):
    frame = base_bg.copy()
    draw = ImageDraw.Draw(frame)
    
    # Animated clouds
    cloud_offsets = [
        (int((t * 22) % (W + 300)) - 150, 70, 160, 55),
        (int((t * 14 + 500) % (W + 400)) - 200, 130, 220, 65),
        (int((t * 18 + 900) % (W + 350)) - 180, 45, 180, 60),
    ]
    for cx, cy, cw, ch in cloud_offsets:
        draw.ellipse([cx, cy, cx + cw, cy + ch], fill=(255, 255, 255, 220))
        draw.ellipse([cx + 30, cy - 20, cx + cw - 30, cy + ch], fill=(255, 255, 255, 240))

    vocal = scene.get('vocal')
    char_sprite_name = scene.get('char_sprite')
    char_name = scene.get('char_name', '')
    tip = scene.get('tip', '')
    scene_time = t - scene['start']
    
    # TOP HEADER BANNER
    banner_w, banner_h = 1000, 68
    bx = (W - banner_w) // 2
    by = 18
    # Shadow
    draw_rounded_rect(draw, [bx + 4, by + 4, bx + banner_w + 4, by + banner_h + 4], 25, (0, 0, 0, 60))
    # Main banner
    draw_rounded_rect(draw, [bx, by, bx + banner_w, by + banner_h], 25, (255, 245, 210), (255, 170, 0), width=4)
    
    header_text = f"¡APRENDE LAS VOCALES CON TUS AMIGOS!"
    if vocal:
        header_text = f"★ VOCAL {vocal} - {char_name.upper()} ★"
    bbox_h = draw.textbbox((0, 0), header_text, font=font_med)
    tx = bx + (banner_w - (bbox_h[2] - bbox_h[0])) // 2
    ty = by + (banner_h - (bbox_h[3] - bbox_h[1])) // 2 - 2
    draw_text_with_outline(draw, (tx, ty), header_text, font=font_med, fill=(255, 70, 0), outline=(255, 255, 255), outline_w=2)

    # If it is Intro or Finale: show group image centered!
    if vocal is None or vocal == "A E I O U":
        sp = sprites.get('characters_all.png')
        if sp:
            # Scale to fit nicely
            scale_pulse = 1.0 + 0.015 * math.sin(t * 3.0)
            target_w = int(720 * scale_pulse)
            target_h = int(392 * scale_pulse)
            sp_scaled = sp.resize((target_w, target_h), Image.Resampling.LANCZOS)
            sx = (W - target_w) // 2
            sy = 110 + int(6 * math.sin(t * 2.5))
            
            # Frame shadow & card
            draw_rounded_rect(draw, [sx - 12, sy - 12, sx + target_w + 12, sy + target_h + 12], 24, (255, 255, 255), (255, 190, 30), width=6)
            frame.paste(sp_scaled, (sx, sy))
            
            # Subtitle message banner below
            msg_banner_y = sy + target_h + 24
            draw_rounded_rect(draw, [180, msg_banner_y, W - 180, msg_banner_y + 70], 20, (255, 255, 255), (100, 180, 255), width=4)
            bbox_t = draw.textbbox((0, 0), tip, font=font_med)
            mx = (W - (bbox_t[2] - bbox_t[0])) // 2
            my = msg_banner_y + (70 - (bbox_t[3] - bbox_t[1])) // 2 - 2
            draw_text_with_outline(draw, (mx, my), tip, font=font_med, fill=(0, 110, 220), outline=(255, 255, 255), outline_w=2)
    else:
        # Standard Vowel Scene Layout:
        # Left side: Character cutout + Character Name tag
        sp = sprites.get(char_sprite_name)
        if sp:
            bob = int(10 * math.sin(t * 4.0))
            char_x = 70
            char_y = 100 + bob
            
            # Soft shadow
            shadow_w, shadow_h = int(sp.width * 0.7), 28
            draw.ellipse([char_x + (sp.width - shadow_w)//2, char_y + sp.height - 10, char_x + (sp.width + shadow_w)//2, char_y + sp.height + 18], fill=(0, 0, 0, 50))
            
            # Paste character
            frame.paste(sp, (char_x, char_y), sp)
            
            # Character name badge
            name_y = char_y + sp.height - 35
            name_w = sp.width + 40
            nx = char_x - 20
            draw_rounded_rect(draw, [nx, name_y, nx + name_w, name_y + 48], 18, (255, 255, 255), (255, 140, 0), width=3)
            bbox_n = draw.textbbox((0, 0), char_name, font=font_small)
            tx = nx + (name_w - (bbox_n[2] - bbox_n[0])) // 2
            draw.text((tx, name_y + 11), char_name, font=font_small, fill=(220, 60, 0))

        # Right side:
        # 1. Giant Letter Badge (Top Right)
        letter_x, letter_y = 530, 105
        lw, lh = 220, 130
        pulse = 1.0 + 0.03 * math.sin(t * 5.0)
        draw_rounded_rect(draw, [letter_x, letter_y, letter_x + lw, letter_y + lh], 28, (255, 255, 255), (255, 80, 120), width=5)
        bbox_l = draw.textbbox((0, 0), vocal, font=font_huge)
        lx = letter_x + (lw - (bbox_l[2] - bbox_l[0])) // 2
        ly = letter_y + (lh - (bbox_l[3] - bbox_l[1])) // 2 - 8
        draw_text_with_outline(draw, (lx, ly), vocal, font=font_huge, fill=(255, 40, 100), outline=(255, 240, 180), outline_w=3)

        # 2. Pronunciation Guide Badge (Right of Giant Letter)
        guide_x, guide_y = 770, 105
        gw, gh = 450, 130
        draw_rounded_rect(draw, [guide_x, guide_y, guide_x + gw, guide_y + gh], 24, (255, 248, 220), (80, 180, 240), width=4)
        
        draw.text((guide_x + 20, guide_y + 18), "¡CÓMO SE PRONUNCIA!", font=font_card, fill=(0, 120, 220))
        # Tip text wrapping
        draw.text((guide_x + 20, guide_y + 58), tip, font=font_small, fill=(60, 60, 60))

        # 3. Three Educational Word Cards (Middle-Right area)
        cards = CARDS_DATA.get(vocal, [])
        card_start_y = 255
        card_h = 88
        card_w = 218
        card_gap = 18
        
        # Determine highlighted card based on scene time
        # E.g. cycle highlight across cards
        active_card_idx = int((scene_time / 4.0)) % 3 if len(cards) == 3 else 0

        for idx, c in enumerate(cards):
            cx = 530 + idx * (card_w + card_gap)
            cy = card_start_y
            is_active = (idx == active_card_idx)
            
            border_col = (255, 220, 0) if is_active else (220, 220, 220)
            fill_col = (255, 255, 255) if not is_active else (255, 255, 230)
            border_w = 5 if is_active else 2
            
            if is_active:
                cy -= int(4 * math.sin(t * 6.0))
            
            draw_rounded_rect(draw, [cx, cy, cx + card_w, cy + card_h], 18, fill_col, border_col, width=border_w)
            
            # Card header line
            draw.text((cx + 14, cy + 12), c['word'], font=font_card, fill=c['color'])
            draw.text((cx + 14, cy + 48), c['sub'], font=font_small, fill=(100, 100, 100))
            
            if is_active:
                draw.text((cx + card_w - 40, cy + 12), "★", font=font_card, fill=(255, 180, 0))

        # 4. Animated Educational Interactive Prompt Bar
        prompt_y = 365
        pw = 690
        draw_rounded_rect(draw, [530, prompt_y, 530 + pw, prompt_y + 80], 20, (255, 255, 255), (120, 210, 80), width=4)
        
        p_text = f"¡Escucha, repite y aprende con {char_name.split()[0]}!"
        bbox_p = draw.textbbox((0, 0), p_text, font=font_med)
        px = 530 + (pw - (bbox_p[2] - bbox_p[0])) // 2
        py = prompt_y + (80 - (bbox_p[3] - bbox_p[1])) // 2 - 2
        draw_text_with_outline(draw, (px, py), p_text, font=font_med, fill=(50, 160, 40), outline=(255, 255, 255), outline_w=2)

    # BOTTOM SUBTITLE BANNER (Karaoke/Reader style)
    sub_w, sub_h = 1180, 82
    sub_x = (W - sub_w) // 2
    sub_y = H - sub_h - 18
    # Background
    draw_rounded_rect(draw, [sub_x, sub_y, sub_x + sub_w, sub_y + sub_h], 22, (20, 30, 50, 230), (255, 215, 0), width=3)
    
    # Subtitle text
    sub_text = tip
    if vocal:
        sub_text = f"La vocal {vocal}: ¡{tip}!"
    bbox_s = draw.textbbox((0, 0), sub_text, font=font_sub)
    sx = sub_x + (sub_w - (bbox_s[2] - bbox_s[0])) // 2
    sy = sub_y + (sub_h - (bbox_s[3] - bbox_s[1])) // 2 - 2
    draw_text_with_outline(draw, (sx, sy), sub_text, font=font_sub, fill=(255, 240, 80), outline=(0, 0, 0), outline_w=2)

    return frame

if __name__ == "__main__":
    with open("timeline.json") as f:
        data = json.load(f)
    scenes = data["scenes"]
    
    # Test rendering sample frames
    print("Testing frame render...")
    f0 = render_frame(2.0, scenes[0])
    f0.save("sample_intro.png")
    
    f1 = render_frame(40.0, scenes[1])
    f1.save("sample_vocal_a.png")
    
    f2 = render_frame(120.0, scenes[3])
    f2.save("sample_vocal_e.png")
    
    print("Saved sample frames successfully!")
