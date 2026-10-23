import numpy as np
from scipy.io import wavfile

def generate_children_bgm(total_duration_sec, output_path="audio/bgm_music.wav", sr=44100):
    bpm = 116
    beat_dur = 60.0 / bpm
    
    # Frequencies (Hz)
    N = {
        'C3': 130.81, 'E3': 164.81, 'F3': 174.61, 'G3': 196.00, 'A3': 220.00, 'B3': 246.94,
        'C4': 261.63, 'D4': 293.66, 'E4': 329.63, 'F4': 349.23, 'G4': 392.00, 'A4': 440.00, 'B4': 493.88,
        'C5': 523.25, 'D5': 587.33, 'E5': 659.25, 'F5': 698.46, 'G5': 783.99, 'A5': 880.00, 'B5': 987.77,
        'C6': 1046.50, 'D6': 1174.66, 'E6': 1318.51, 'G6': 1567.98
    }

    def glockenspiel_bell(freq, duration, amp=0.25):
        t = np.linspace(0, duration, int(sr * duration), False)
        # Fundamental + gentle harmonics with chime decay
        env = np.exp(-t * 6.0)
        env2 = np.exp(-t * 12.0)
        signal = np.sin(2 * np.pi * freq * t) * env
        signal += 0.35 * np.sin(2 * np.pi * freq * 2.0 * t) * env2
        signal += 0.15 * np.sin(2 * np.pi * freq * 3.01 * t) * env2
        return signal * amp

    def pizz_bass(freq, duration, amp=0.22):
        t = np.linspace(0, duration, int(sr * duration), False)
        env = np.exp(-t * 4.5)
        signal = np.sin(2 * np.pi * freq * t) * env
        signal += 0.3 * np.sin(2 * np.pi * freq * 2 * t) * (env ** 1.5)
        return signal * amp

    def soft_perc(duration, amp=0.08):
        t = np.linspace(0, duration, int(sr * duration), False)
        noise = np.random.uniform(-1, 1, len(t))
        env = np.exp(-t * 28.0)
        return noise * env * amp

    # 16-bar looping pattern (64 beats = ~33.1 seconds)
    # Chord progression:
    # Bars 1-4: C - G - Am - F
    # Bars 5-8: C - G - F - C
    # Bars 9-12: C - G - Am - F
    # Bars 13-16: C - F - G - C
    chords = [
        ('C3', ['C4', 'E4', 'G4', 'C5']),
        ('G3', ['B3', 'D4', 'G4', 'B4']),
        ('A3', ['C4', 'E4', 'A4', 'C5']),
        ('F3', ['A3', 'C4', 'F4', 'A4']),
        ('C3', ['C4', 'E4', 'G4', 'C5']),
        ('G3', ['B3', 'D4', 'G4', 'B4']),
        ('F3', ['A3', 'C4', 'F4', 'A4']),
        ('C3', ['C4', 'E4', 'G4', 'C5']),
        ('C3', ['C4', 'E4', 'G4', 'C5']),
        ('G3', ['B3', 'D4', 'G4', 'B4']),
        ('A3', ['C4', 'E4', 'A4', 'C5']),
        ('F3', ['A3', 'C4', 'F4', 'A4']),
        ('C3', ['C4', 'E4', 'G4', 'C5']),
        ('F3', ['A3', 'C4', 'F4', 'A4']),
        ('G3', ['B3', 'D4', 'G4', 'B4']),
        ('C3', ['C4', 'E4', 'G4', 'C5']),
    ]

    melody_notes = [
        # Bar 1
        ('E5', 0.0), ('G5', 0.5), ('E5', 1.0), ('C5', 1.5), ('D5', 2.0), ('E5', 2.5), ('F5', 3.0), ('G5', 3.5),
        # Bar 2
        ('G5', 4.0), ('D5', 4.5), ('B4', 5.0), ('D5', 5.5), ('E5', 6.0), ('D5', 6.5), ('C5', 7.0),
        # Bar 3
        ('A5', 8.0), ('C6', 8.5), ('A5', 9.0), ('E5', 9.5), ('F5', 10.0), ('G5', 10.5), ('A5', 11.0),
        # Bar 4
        ('A5', 12.0), ('F5', 12.5), ('C5', 13.0), ('F5', 13.5), ('G5', 14.0), ('F5', 14.5), ('E5', 15.0), ('D5', 15.5),
        # Bar 5
        ('C5', 16.0), ('E5', 16.5), ('G5', 17.0), ('C6', 17.5), ('B5', 18.0), ('A5', 18.5), ('G5', 19.0),
        # Bar 6
        ('F5', 20.0), ('A5', 20.5), ('G5', 21.0), ('F5', 21.5), ('E5', 22.0), ('D5', 22.5), ('C5', 23.0),
        # Bar 7
        ('D5', 24.0), ('F5', 24.5), ('A5', 25.0), ('G5', 25.5), ('F5', 26.0), ('E5', 26.5), ('D5', 27.0),
        # Bar 8
        ('C5', 28.0), ('E5', 28.5), ('G5', 29.0), ('C6', 29.5), ('C5', 30.0),
        # Repeat with variation for bars 9-16
        ('E5', 32.0), ('G5', 32.5), ('E5', 33.0), ('C5', 33.5), ('D5', 34.0), ('E5', 34.5), ('G5', 35.0),
        ('G5', 36.0), ('D5', 36.5), ('B4', 37.0), ('D5', 37.5), ('F5', 38.0), ('E5', 38.5), ('D5', 39.0),
        ('A5', 40.0), ('C6', 40.5), ('A5', 41.0), ('E5', 41.5), ('F5', 42.0), ('A5', 42.5), ('C6', 43.0),
        ('C6', 44.0), ('A5', 44.5), ('F5', 45.0), ('A5', 45.5), ('G5', 46.0), ('F5', 46.5), ('E5', 47.0), ('D5', 47.5),
        ('C5', 48.0), ('G5', 48.5), ('E5', 49.0), ('C5', 49.5), ('F5', 50.0), ('A5', 50.5), ('C6', 51.0),
        ('A5', 52.0), ('F5', 52.5), ('D5', 53.0), ('F5', 53.5), ('G5', 54.0), ('B5', 54.5), ('D6', 55.0),
        ('G5', 56.0), ('E5', 56.5), ('C5', 57.0), ('E5', 57.5), ('D5', 58.0), ('B4', 58.5), ('G4', 59.0),
        ('C5', 60.0), ('E5', 60.5), ('G5', 61.0), ('C6', 61.5), ('C5', 62.0),
    ]

    pattern_beats = 64
    pattern_dur = pattern_beats * beat_dur
    num_loops = int(np.ceil(total_duration_sec / pattern_dur)) + 1
    
    total_samples = int(sr * total_duration_sec)
    music = np.zeros(total_samples, dtype=np.float32)

    for loop_i in range(num_loops):
        base_time = loop_i * pattern_dur
        if base_time >= total_duration_sec:
            break

        # Chords & bass
        for bar_idx, (bass_note, chord_notes) in enumerate(chords):
            bar_start = base_time + bar_idx * 4 * beat_dur
            if bar_start >= total_duration_sec:
                break
            
            # Bass on beat 1 and 3
            for b_offset in [0, 2]:
                t_note = bar_start + b_offset * beat_dur
                if t_note < total_duration_sec:
                    snd = pizz_bass(N[bass_note], beat_dur * 1.8, amp=0.18)
                    s_idx = int(t_note * sr)
                    e_idx = min(s_idx + len(snd), total_samples)
                    music[s_idx:e_idx] += snd[:e_idx - s_idx]

            # Accompaniment arpeggio
            for step in range(4):
                t_note = bar_start + step * beat_dur
                if t_note < total_duration_sec:
                    c_note = chord_notes[step % len(chord_notes)]
                    snd = glockenspiel_bell(N[c_note], beat_dur * 0.9, amp=0.08)
                    s_idx = int(t_note * sr)
                    e_idx = min(s_idx + len(snd), total_samples)
                    music[s_idx:e_idx] += snd[:e_idx - s_idx]

                # Soft shaker on offbeat
                t_shaker = bar_start + (step + 0.5) * beat_dur
                if t_shaker < total_duration_sec:
                    snd = soft_perc(beat_dur * 0.4, amp=0.035)
                    s_idx = int(t_shaker * sr)
                    e_idx = min(s_idx + len(snd), total_samples)
                    music[s_idx:e_idx] += snd[:e_idx - s_idx]

        # Melody
        for note_name, beat_offset in melody_notes:
            t_note = base_time + beat_offset * beat_dur
            if t_note < total_duration_sec:
                snd = glockenspiel_bell(N[note_name], beat_dur * 1.2, amp=0.16)
                s_idx = int(t_note * sr)
                e_idx = min(s_idx + len(snd), total_samples)
                music[s_idx:e_idx] += snd[:e_idx - s_idx]

    # Normalize gently and soft fade out at the end
    max_val = np.max(np.abs(music))
    if max_val > 0:
        music = music / max_val * 0.35 # gentle background level

    fade_len = int(sr * 3.0)
    if len(music) > fade_len:
        fade = np.linspace(1.0, 0.0, fade_len)
        music[-fade_len:] *= fade

    # Convert to 16-bit PCM
    audio_int16 = (music * 32767).astype(np.int16)
    wavfile.write(output_path, sr, audio_int16)
    print(f"Generated BGM: {output_path}, duration={total_duration_sec:.1f}s")

if __name__ == "__main__":
    import sys
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 350.0
    generate_children_bgm(dur)
