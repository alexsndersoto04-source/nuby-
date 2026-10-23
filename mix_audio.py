import os
import subprocess
import json
from generate_music import generate_children_bgm

with open("timeline.json") as f:
    data = json.load(f)

total_dur = data["total_duration"]
scenes = data["scenes"]

# 1. Generate silence audio (0.8s)
subprocess.run([
    'ffmpeg', '-y', '-f', 'lavfi', '-i', 'anullsrc=r=44100:cl=stereo',
    '-t', '0.8', '-q:a', '9', '-acodec', 'libmp3lame', 'audio/silence.mp3'
], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# 2. Concat list for speech
with open("audio/concat_speech.txt", "w") as f:
    for sc in scenes[:-1]: # exclude finale which has no speech file
        f.write(f"file '{sc['file']}'\n")
        f.write("file 'silence.mp3'\n")

# Concat speech into audio/speech_full.wav
subprocess.run([
    'ffmpeg', '-y', '-f', 'concat', '-safe', '0',
    '-i', 'audio/concat_speech.txt',
    '-ar', '44100', '-ac', '2', 'audio/speech_full.wav'
], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# 3. Generate background music for total duration
print(f"Generating BGM for {total_dur:.1f}s...")
generate_children_bgm(total_dur + 1.0, "audio/bgm_music.wav", sr=44100)

# 4. Mix speech and BGM
# Speech at full volume (volume=1.0), BGM at gentle volume=0.18 with a smooth fade in (1s) and fade out (3s)
print("Mixing speech and BGM...")
subprocess.run([
    'ffmpeg', '-y',
    '-i', 'audio/speech_full.wav',
    '-i', 'audio/bgm_music.wav',
    '-filter_complex',
    '[0:a]volume=1.1[speech];[1:a]volume=0.20,afade=t=in:st=0:d=1.0,afade=t=out:st=' + str(total_dur - 3.0) + ':d=3.0[bgm];[speech][bgm]amix=inputs=2:duration=first:dropout_transition=2[out]',
    '-map', '[out]',
    '-t', str(total_dur),
    '-ar', '44100', '-ac', '2',
    'audio/master_audio.aac'
], check=True)

print("Master audio ready: audio/master_audio.aac")
