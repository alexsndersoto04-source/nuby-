import os
import sys
import math
import json
import subprocess
import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H = 1280, 720
FPS = 15

FONT_BOLD_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

def get_font(size, bold=True):
    p = FONT_BOLD_PATH if bold else FONT_PATH
    return ImageFont.truetype(p, size)

font_huge = get_font(84, True)
font_big = get_font(48, True)
font_med = get_font(32, True)
font_card_word = get_font(26, True)
font_card_sub = get_font(20, False)
font_badge = get_font(28, True)
font_sub = get_font(26, True)
font_small = get_font(22, True)

# Preload sprites
sprites = {}
for name in ['sprite_character_a.png', 'sprite_character_e.png', 'sprite_character_i.png', 'sprite_character_o.png', 'sprite_character_u.png']:
    p = os.path.join('images', name)
    if os.path.exists(p):
        sprites[name] = Image.open(p).convert('RGBA')

if os.path.exists('images/characters_all.png'):
    im_all = Image.open('images/characters_all.png').convert('RGBA')
    sprites['characters_all.png'] = im_all

# Pre-render colorful base background
def make_base_background():
    bg = Image.new('RGB', (W, H), (140, 215, 255))
    draw = ImageDraw.Draw(bg)
    # Sky gradient
    for y in range(H):
        r = int(140 + (220 - 140) * (y / H))
        g = int(215 + (245 - 215) * (y / H))
        b = int(255 + (220 - 255) * (y / H))
        draw.line([(0, y), (W, y)], fill=(r, g, b))
    
    # Sun with rays
    sun_x, sun_y = 1120, 100
    draw.ellipse([sun_x - 75, sun_y - 75, sun_x + 75, sun_y + 75], fill=(255, 240, 80))
    draw.ellipse([sun_x - 60, sun_y - 60, sun_x + 60, sun_y + 60], fill=(255, 220, 40))
    
    # Rolling green hills at bottom
    draw.chord([ -200, 420, 900, 950 ], 0, 360, fill=(130, 205, 80))
    draw.chord([ 400, 450, 1500, 1000 ], 0, 360, fill=(95, 185, 60))
    draw.chord([ -100, 480, 800, 1050 ], 0, 360, fill=(80, 175, 50))
    return bg

base_bg = make_base_background()

CARDS_DATA = {
    "A a": [
        {"badge": "✈", "word": "AVIÓN", "sub": "Vuela alto en el cielo", "color": (230, 50, 50)},
        {"badge": "🌳", "word": "ÁRBOL", "sub": "Grande, fuerte y verde", "color": (40, 160, 40)},
        {"badge": "🐝", "word": "ABEJA", "sub": "Produce rica miel", "color": (230, 150, 0)}
    ],
    "E e": [
        {"badge": "★", "word": "ESTRELLA", "sub": "Brilla en la noche", "color": (220, 160, 0)},
        {"badge": "🪜", "word": "ESCALERA", "sub": "Para subir muy alto", "color": (20, 140, 220)},
        {"badge": "🐘", "word": "ELEFANTE", "sub": "Tierna trompa larga", "color": (150, 90, 210)}
    ],
    "I i": [
        {"badge": "🏝", "word": "ISLA", "sub": "Tierra rodeada de mar", "color": (0, 170, 160)},
        {"badge": "🧲", "word": "IMÁN", "sub": "Atrae cosas de metal", "color": (230, 40, 100)},
        {"badge": "🦎", "word": "IGUANA", "sub": "Amiga verde y curiosa", "color": (80, 170, 40)}
    ],
    "O o": [
        {"badge": "🐻", "word": "OSO", "sub": "Peludito y cariñoso", "color": (170, 90, 40)},
        {"badge": "👁", "word": "OJO", "sub": "Para mirar y descubrir", "color": (230, 110, 20)},
        {"badge": "🍲", "word": "OLLA", "sub": "Para cocinar comida rica", "color": (210, 60, 60)}
    ],
    "U u": [
        {"badge": "🍇", "word": "UVAS", "sub": "Frutitas ricas y dulces", "color": (150, 50, 200)},
        {"badge": "🦄", "word": "UNICORNIO", "sub": "Mágico con cuerno de oro", "color": (220, 80, 170)},
        {"badge": "1", "word": "UNO", "sub": "El primer número", "color": (20, 150, 220)}
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

def render_frame_fast(t, scene):
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
    banner_w, banner_h = 1000, 64
    bx = (W - banner_w) // 2
    by = 16
    draw_rounded_rect(draw, [bx + 4, by + 4, bx + banner_w + 4, by + banner_h + 4], 22, (0, 0, 0, 50))
    draw_rounded_rect(draw, [bx, by, bx + banner_w, by + banner_h], 22, (255, 248, 220), (255, 160, 0), width=4)
    
    header_text = "¡APRENDE LAS VOCALES CON TUS AMIGOS!"
    if vocal and vocal != "A E I O U":
        header_text = f"★ VOCAL {vocal}  -  {char_name.upper()} ★"
    elif vocal == "A E I O U":
        header_text = "★ ¡RONDA DE LAS VOCALES: A, E, I, O, U! ★"

    bbox_h = draw.textbbox((0, 0), header_text, font=font_med)
    tx = bx + (banner_w - (bbox_h[2] - bbox_h[0])) // 2
    ty = by + (banner_h - (bbox_h[3] - bbox_h[1])) // 2 - 2
    draw_text_with_outline(draw, (tx, ty), header_text, font=font_med, fill=(255, 60, 0), outline=(255, 255, 255), outline_w=2)

    # Intro or Finale Layout
    if vocal is None or vocal == "A E I O U":
        sp = sprites.get('characters_all.png')
        if sp:
            scale_pulse = 1.0 + 0.015 * math.sin(t * 3.0)
            target_w = int(720 * scale_pulse)
            target_h = int(392 * scale_pulse)
            sp_scaled = sp.resize((target_w, target_h), Image.Resampling.BILINEAR)
            sx = (W - target_w) // 2
            sy = 100 + int(5 * math.sin(t * 2.5))
            
            draw_rounded_rect(draw, [sx - 10, sy - 10, sx + target_w + 10, sy + target_h + 10], 22, (255, 255, 255), (255, 190, 30), width=6)
            frame.paste(sp_scaled, (sx, sy))
            
            msg_banner_y = sy + target_h + 20
            draw_rounded_rect(draw, [180, msg_banner_y, W - 180, msg_banner_y + 68], 20, (255, 255, 255), (80, 180, 255), width=4)
            bbox_t = draw.textbbox((0, 0), tip, font=font_med)
            mx = (W - (bbox_t[2] - bbox_t[0])) // 2
            my = msg_banner_y + (68 - (bbox_t[3] - bbox_t[1])) // 2 - 2
            draw_text_with_outline(draw, (mx, my), tip, font=font_med, fill=(0, 110, 220), outline=(255, 255, 255), outline_w=2)
    else:
        # Standard Vowel Scene
        sp = sprites.get(char_sprite_name)
        if sp:
            bob = int(9 * math.sin(t * 4.0))
            char_x = 65
            char_y = 100 + bob
            
            shadow_w, shadow_h = int(sp.width * 0.7), 26
            draw.ellipse([char_x + (sp.width - shadow_w)//2, char_y + sp.height - 8, char_x + (sp.width + shadow_w)//2, char_y + sp.height + 18], fill=(0, 0, 0, 45))
            frame.paste(sp, (char_x, char_y), sp)
            
            # Character Name badge
            name_y = char_y + sp.height - 35
            name_w = sp.width + 40
            nx = char_x - 20
            draw_rounded_rect(draw, [nx, name_y, nx + name_w, name_y + 48], 18, (255, 255, 255), (255, 140, 0), width=3)
            bbox_n = draw.textbbox((0, 0), char_name, font=font_small)
            tx = nx + (name_w - (bbox_n[2] - bbox_n[0])) // 2
            draw.text((tx, name_y + 11), char_name, font=font_small, fill=(220, 60, 0))

        # Giant Letter Badge
        letter_x, letter_y = 525, 102
        lw, lh = 225, 128
        draw_rounded_rect(draw, [letter_x, letter_y, letter_x + lw, letter_y + lh], 26, (255, 255, 255), (255, 70, 110), width=5)
        bbox_l = draw.textbbox((0, 0), vocal, font=font_huge)
        lx = letter_x + (lw - (bbox_l[2] - bbox_l[0])) // 2
        ly = letter_y + (lh - (bbox_l[3] - bbox_l[1])) // 2 - 8
        draw_text_with_outline(draw, (lx, ly), vocal, font=font_huge, fill=(255, 30, 90), outline=(255, 240, 180), outline_w=3)

        # Pronunciation Guide Box
        guide_x, guide_y = 770, 102
        gw, gh = 450, 128
        draw_rounded_rect(draw, [guide_x, guide_y, guide_x + gw, guide_y + gh], 24, (255, 248, 220), (60, 170, 240), width=4)
        draw.text((guide_x + 22, guide_y + 16), "¡CÓMO SE PRONUNCIA!", font=font_card_word, fill=(0, 120, 220))
        draw.text((guide_x + 22, guide_y + 56), tip, font=font_small, fill=(50, 50, 50))

        # 3 Educational Word Cards
        cards = CARDS_DATA.get(vocal, [])
        card_start_y = 250
        card_h = 92
        card_w = 218
        card_gap = 20
        
        active_card_idx = int((scene_time / 4.0)) % 3 if len(cards) == 3 else 0

        for idx, c in enumerate(cards):
            cx = 525 + idx * (card_w + card_gap)
            cy = card_start_y
            is_active = (idx == active_card_idx)
            
            border_col = (255, 200, 0) if is_active else (220, 220, 220)
            fill_col = (255, 255, 225) if is_active else (255, 255, 255)
            border_w = 5 if is_active else 2
            
            if is_active:
                cy -= int(4 * math.sin(t * 6.0))
            
            draw_rounded_rect(draw, [cx, cy, cx + card_w, cy + card_h], 18, fill_col, border_col, width=border_w)
            
            # Badge Circle
            draw.ellipse([cx + 12, cy + 12, cx + 46, cy + 46], fill=c['color'])
            draw.text((cx + 18, cy + 12), c['badge'][:1], font=font_badge, fill=(255, 255, 255))
            
            # Word & Sub
            draw.text((cx + 56, cy + 12), c['word'], font=font_card_word, fill=c['color'])
            draw.text((cx + 14, cy + 54), c['sub'], font=font_card_sub, fill=(90, 90, 90))
            
            if is_active:
                draw.text((cx + card_w - 32, cy + 12), "★", font=font_card_word, fill=(255, 170, 0))

        # Educational Interactive Prompt Bar
        prompt_y = 362
        pw = 695
        draw_rounded_rect(draw, [525, prompt_y, 525 + pw, prompt_y + 78], 20, (255, 255, 255), (110, 200, 70), width=4)
        p_text = f"¡Escucha, repite y aprende con {char_name.split()[0]}!"
        bbox_p = draw.textbbox((0, 0), p_text, font=font_med)
        px = 525 + (pw - (bbox_p[2] - bbox_p[0])) // 2
        py = prompt_y + (78 - (bbox_p[3] - bbox_p[1])) // 2 - 2
        draw_text_with_outline(draw, (px, py), p_text, font=font_med, fill=(40, 150, 30), outline=(255, 255, 255), outline_w=2)

    # BOTTOM SUBTITLE BANNER
    sub_w, sub_h = 1180, 80
    sub_x = (W - sub_w) // 2
    sub_y = H - sub_h - 18
    draw_rounded_rect(draw, [sub_x, sub_y, sub_x + sub_w, sub_y + sub_h], 22, (20, 30, 50, 235), (255, 215, 0), width=3)
    
    clean_tip = tip.strip('¡! ')
    if vocal and vocal != "A E I O U":
        sub_text = f"Vocal {vocal}: ¡{clean_tip}!"
    else:
        sub_text = f"¡{clean_tip}!"
    bbox_s = draw.textbbox((0, 0), sub_text, font=font_sub)
    sx = sub_x + (sub_w - (bbox_s[2] - bbox_s[0])) // 2
    sy = sub_y + (sub_h - (bbox_s[3] - bbox_s[1])) // 2 - 2
    draw_text_with_outline(draw, (sx, sy), sub_text, font=font_sub, fill=(255, 240, 80), outline=(0, 0, 0), outline_w=2)

    return frame

def export_full_video(output_mp4="video_vocales_educativo.mp4", max_seconds=None):
    with open("timeline.json") as f:
        data = json.load(f)
    scenes = data["scenes"]
    total_dur = data["total_duration"]
    if max_seconds:
        total_dur = min(total_dur, max_seconds)

    total_frames = int(total_dur * FPS)
    print(f"Starting video export: {total_dur:.2f}s ({total_frames} frames at {FPS} fps)...")

    # FFmpeg command streaming rawvideo RGB24 via stdin
    cmd = [
        'ffmpeg', '-y',
        '-f', 'rawvideo',
        '-vcodec', 'rawvideo',
        '-s', f'{W}x{H}',
        '-pix_fmt', 'rgb24',
        '-r', str(FPS),
        '-i', '-', # read video from stdin
        '-i', 'audio/master_audio.aac',
        '-c:v', 'libx264',
        '-preset', 'ultrafast',
        '-tune', 'animation',
        '-crf', '22',
        '-pix_fmt', 'yuv420p',
        '-c:a', 'aac',
        '-b:a', '128k',
        '-t', str(total_dur),
        output_mp4
    ]

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    # Determine which scene corresponds to each time
    scene_idx = 0
    for frame_i in range(total_frames):
        t = frame_i / FPS
        while scene_idx < len(scenes) - 1 and t >= scenes[scene_idx]['end']:
            scene_idx += 1
        
        cur_scene = scenes[scene_idx]
        frame = render_frame_fast(t, cur_scene)
        raw_bytes = frame.tobytes()
        proc.stdin.write(raw_bytes)

        if frame_i % (FPS * 15) == 0:
            print(f"Progress: {t:.1f}s / {total_dur:.1f}s ({frame_i}/{total_frames} frames)")

    proc.stdin.close()
    stderr = proc.stderr.read().decode('utf-8', errors='ignore')
    proc.wait()

    if proc.returncode != 0:
        print("FFmpeg error:", stderr)
        sys.exit(1)

    print(f"Successfully generated {output_mp4}!")

if __name__ == "__main__":
    max_s = float(sys.argv[1]) if len(sys.argv) > 1 else None
    export_full_video("video_vocales_educativo.mp4", max_s)
