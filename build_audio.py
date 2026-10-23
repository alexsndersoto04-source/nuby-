import os
import subprocess
import json

scenes = [
    {
        "id": "intro",
        "file": "01_intro.mp3",
        "vocal": None,
        "char_name": "¡Bienvenidos!",
        "char_sprite": "characters_all.png",
        "tip": "¡Hoy aprenderemos las 5 vocales!",
        "words": []
    },
    {
        "id": "vocal_a_part1",
        "file": "02_vocal_a_part1.mp3",
        "vocal": "A a",
        "char_name": "Anita la Abejita",
        "char_sprite": "sprite_character_a.png",
        "tip": "¡Abre grande la boca: AAAAA!",
        "words": ["Avión", "Árbol", "Abeja"]
    },
    {
        "id": "vocal_a_part2",
        "file": "03_vocal_a_part2.mp3",
        "vocal": "A a",
        "char_name": "Anita la Abejita",
        "char_sprite": "sprite_character_a.png",
        "tip": "¿Con qué letra empieza Avión? ¡Con la A!",
        "words": ["Avión", "Árbol", "Abeja"]
    },
    {
        "id": "vocal_e_part1",
        "file": "04_vocal_e_part1.mp3",
        "vocal": "E e",
        "char_name": "Elí el Elefantito",
        "char_sprite": "sprite_character_e.png",
        "tip": "¡Sonríe mostrando los dientes: EEEEE!",
        "words": ["Estrella", "Escalera", "Elefante"]
    },
    {
        "id": "vocal_e_part2",
        "file": "05_vocal_e_part2.mp3",
        "vocal": "E e",
        "char_name": "Elí el Elefantito",
        "char_sprite": "sprite_character_e.png",
        "tip": "¡E de Estrella, Escalera y Elefante!",
        "words": ["Estrella", "Escalera", "Elefante"]
    },
    {
        "id": "vocal_i_part1",
        "file": "06_vocal_i_part1.mp3",
        "vocal": "I i",
        "char_name": "Ignacio la Iguana",
        "char_sprite": "sprite_character_i.png",
        "tip": "¡Dientecitos juntos y sonriendo: IIIII!",
        "words": ["Isla", "Imán", "Iguana"]
    },
    {
        "id": "vocal_i_part2",
        "file": "07_vocal_i_part2.mp3",
        "vocal": "I i",
        "char_name": "Ignacio la Iguana",
        "char_sprite": "sprite_character_i.png",
        "tip": "¿Cuál tiene un puntito arriba? ¡La I!",
        "words": ["Isla", "Imán", "Iguana"]
    },
    {
        "id": "vocal_o",
        "file": "08_vocal_o.mp3",
        "vocal": "O o",
        "char_name": "Osvaldo el Osito",
        "char_sprite": "sprite_character_o.png",
        "tip": "¡Boquita redonda en círculo: OOOOO!",
        "words": ["Oso", "Ojo", "Olla"]
    },
    {
        "id": "vocal_u",
        "file": "09_vocal_u.mp3",
        "vocal": "U u",
        "char_name": "Ulises el Unicornio",
        "char_sprite": "sprite_character_u.png",
        "tip": "¡Boquita de trompeta o de beso: UUUUU!",
        "words": ["Uvas", "Unicornio", "Uno"]
    }
]

def get_duration(fpath):
    res = subprocess.run(['ffmpeg', '-i', fpath], stderr=subprocess.PIPE, text=True)
    for line in res.stderr.split('\n'):
        if 'Duration:' in line:
            parts = line.split('Duration:')[1].split(',')[0].strip().split(':')
            return float(parts[0])*3600 + float(parts[1])*60 + float(parts[2])
    return 0.0

current_time = 0.0
pause_between = 0.8 # small friendly pause between sections

timeline = []
concat_list = []

for sc in scenes:
    p = os.path.join('audio', sc['file'])
    dur = get_duration(p)
    sc['start'] = current_time
    sc['duration'] = dur
    sc['end'] = current_time + dur
    timeline.append(sc)
    concat_list.append(p)
    current_time += dur + pause_between

# Finale celebration outro (12 seconds)
finale_duration = 12.0
timeline.append({
    "id": "finale",
    "file": None,
    "vocal": "A E I O U",
    "char_name": "¡Felicidades Campeones!",
    "char_sprite": "characters_all.png",
    "tip": "¡Ya te sabes las 5 vocales mágicas!",
    "words": ["¡Excelente!", "¡Bravo!", "¡Lo lograste!"],
    "start": current_time,
    "duration": finale_duration,
    "end": current_time + finale_duration
})

total_video_duration = current_time + finale_duration

print(f"Total video duration: {total_video_duration:.2f} seconds ({total_video_duration/60:.2f} minutes)")

with open("timeline.json", "w") as f:
    json.dump({"total_duration": total_video_duration, "scenes": timeline}, f, indent=2)

print("Saved timeline.json")
