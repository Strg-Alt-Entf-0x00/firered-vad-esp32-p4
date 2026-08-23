import os
import sys
import subprocess
import pathlib
import torch
import numpy as np
import struct
import soundfile as sf

# Auto-clone FireRedVAD reference repo if not present
REPO_DIR = pathlib.Path(__file__).parent / "FireRedVAD_Repo"
REPO_URL = "https://github.com/FireRedTeam/FireRedVAD"

if not REPO_DIR.exists():
    print(f"[INFO] FireRedVAD_Repo not found, cloning from {REPO_URL} ...")
    subprocess.check_call(["git", "clone", REPO_URL, str(REPO_DIR)])
    print("[INFO] Clone complete.")

sys.path.append(str(REPO_DIR))

from fireredvad.core.audio_feat import AudioFeat
from fireredvad.core.detect_model import DetectModel

def save_bin(filepath, np_array):
    # Flatten the array and save as 32-bit floats
    with open(filepath, 'wb') as f:
        # Write dimensions first for validation
        shape = np_array.shape
        f.write(struct.pack('I', len(shape)))
        for s in shape:
            f.write(struct.pack('I', s))
        # Write data
        f.write(np_array.astype(np.float32).tobytes())
    print(f"[+] Saved {filepath} (shape: {shape})")

def main():
    wav_path = "../../examples/console_vad/example_wave/speech-welcome-constant-volume.wav"
    model_dir = "../../pth_models/Stream-VAD"
    output_dir = "golden_dumps"
    
    os.makedirs(output_dir, exist_ok=True)
    
    # Emulate ESP32's dumb 44-byte skip
    with open(wav_path, "rb") as f:
        f.seek(44)
        raw_data = f.read()
    
    # Calculate how many int16 samples we have, but truncate to a multiple of 160 (frame size)
    # The ESP32 read 1500 frames of 160 samples = 240,000 samples
    wav_np = np.frombuffer(raw_data, dtype=np.int16)
    max_samples = 1500 * 160
    if len(wav_np) > max_samples:
        wav_np = wav_np[:max_samples]
    
    sample_rate = 16000
    save_bin(os.path.join(output_dir, "golden_pcm.bin"), wav_np)
    
    # The ESP32 is a streaming system, so its first 10ms frame (160 samples)
    # is extracted using a 400-sample window where the first 240 samples are zero!
    # Python offline kaldi.fbank uses the first 400 actual samples.
    # To prove bit-perfect parity, we pad the python data with 240 zeros at the start.
    streaming_padded_wav = np.concatenate((np.zeros(240, dtype=np.int16), wav_np))
    
    # 2. Setup Feature Extractor
    cmvn_path = os.path.join(model_dir, "cmvn.ark")
    feat_extractor = AudioFeat(cmvn_path)
    
    # INTERCEPT FBank (pre-CMVN)
    fbank_raw = feat_extractor.fbank((sample_rate, streaming_padded_wav))
    save_bin(os.path.join(output_dir, "golden_fbank_pre_cmvn.bin"), fbank_raw)
    
    # INTERCEPT CMVN (post-CMVN)
    fbank_cmvn = feat_extractor.cmvn(fbank_raw)
    save_bin(os.path.join(output_dir, "golden_fbank_post_cmvn.bin"), fbank_cmvn)
    
    # 3. Setup Model
    vad_model = DetectModel.from_pretrained(model_dir)
    vad_model.eval()
    
    with torch.no_grad():
        feat = torch.from_numpy(fbank_cmvn).unsqueeze(0).float() # (1, T, 80)
        
        # Proper streaming inference frame by frame
        T = feat.size(1)
        probs_list = []
        states = None
        
        for t in range(T):
            frame = feat[:, t:t+1, :]
            outputs, states = vad_model.forward(frame, states)
            prob = outputs.squeeze().item()
            probs_list.append(prob)
            
        probs = np.array(probs_list, dtype=np.float32)
        
    save_bin(os.path.join(output_dir, "golden_probs.bin"), probs.squeeze())
    print("Golden Dumps generated successfully!")

if __name__ == "__main__":
    main()
