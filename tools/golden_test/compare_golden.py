import os
import sys
import numpy as np
import struct

def load_bin(filepath, raw_dtype=None, raw_shape=None):
    if not os.path.exists(filepath):
        print(f"[ERROR] File not found: {filepath}")
        return None
        
    with open(filepath, 'rb') as f:
        if raw_dtype is not None:
            # It's a raw dump from ESP32 without headers
            data = np.frombuffer(f.read(), dtype=raw_dtype)
            if raw_shape is not None and len(raw_shape) > 1:
                # Calculate number of frames based on file size and feature dimension
                feat_dim = raw_shape[1]
                frames = len(data) // feat_dim
                data = data[:frames * feat_dim].reshape((frames, feat_dim))
            return data
            
        # Otherwise it's a Python dump with headers
        ndim_bytes = f.read(4)
        if not ndim_bytes:
            return None
        ndim = struct.unpack('I', ndim_bytes)[0]
        
        # Read shape
        shape = []
        for _ in range(ndim):
            s = struct.unpack('I', f.read(4))[0]
            shape.append(s)
            
        # Read data
        data = np.frombuffer(f.read(), dtype=np.float32).reshape(shape)
    return data

def calc_mse(arr1, arr2, name):
    if arr1 is None or arr2 is None:
        return
        
    # Ensure shapes match up to the min length
    min_len = min(arr1.shape[0], arr2.shape[0])
    
    # If 1D
    if len(arr1.shape) == 1:
        a1 = arr1[:min_len]
        a2 = arr2[:min_len]
    # If 2D
    elif len(arr1.shape) == 2:
        a1 = arr1[:min_len, :]
        a2 = arr2[:min_len, :]
    else:
        print(f"Unsupported shape for {name}")
        return
        
    mse = np.mean((a1 - a2)**2)
    max_diff = np.max(np.abs(a1 - a2))
    
    print(f"[{name}] Frames compared: {min_len}")
    print(f"[{name}] MSE:      {mse:.6e}")
    print(f"[{name}] Max Diff: {max_diff:.6e}")
    
    # Also print the first 5 values for debugging
    if len(arr1.shape) == 1:
        print(f"  Python: {a1[:5]}")
        print(f"  ESP32:  {a2[:5]}")
    elif len(arr1.shape) == 2:
        print(f"  Python: {a1[0, :5]}")
        print(f"  ESP32:  {a2[0, :5]}")
    print("-" * 50)

def main():
    py_dir = "golden_dumps"
    esp_dir = "esp32_dumps"
    
    print("=" * 50)
    print(" FireRedVAD Golden Test Comparator")
    print("=" * 50)
    
    esp_pcm_file = os.path.join(esp_dir, "golden_esp32_pcm.bin")
    if os.path.exists(esp_pcm_file):
        esp_pcm = np.fromfile(esp_pcm_file, dtype=np.int16).astype(np.float32)
        py_pcm = load_bin(os.path.join(py_dir, "golden_pcm.bin"))
        
        if py_pcm is not None:
            calc_mse(py_pcm.flatten(), esp_pcm.flatten(), "PCM")
    
    # 2. Compare FBank (Pre-CMVN)
    py_fbank_pre = load_bin(os.path.join(py_dir, "golden_fbank_pre_cmvn.bin"))
    esp_fbank_pre_path = os.path.join(esp_dir, "golden_esp32_fbank_pre.bin")
    if os.path.exists(esp_fbank_pre_path):
        esp_fbank_pre = np.fromfile(esp_fbank_pre_path, dtype=np.float32)
        if py_fbank_pre is not None and esp_fbank_pre is not None:
            esp_fbank_pre = esp_fbank_pre.reshape(-1, 80)
            calc_mse(py_fbank_pre, esp_fbank_pre, "FBank (Pre-CMVN)")
        
    # 3. Compare FBank (Post-CMVN)
    py_fbank_post = load_bin(os.path.join(py_dir, "golden_fbank_post_cmvn.bin"))
    esp_fbank_post_path = os.path.join(esp_dir, "golden_esp32_fbank_post.bin")
    if os.path.exists(esp_fbank_post_path):
        esp_fbank_post = np.fromfile(esp_fbank_post_path, dtype=np.float32)
        if py_fbank_post is not None and esp_fbank_post is not None:
            esp_fbank_post = esp_fbank_post.reshape(-1, 80)
            calc_mse(py_fbank_post, esp_fbank_post, "FBank (Post-CMVN)")
        
    # 4. Compare Probs
    py_probs = load_bin(os.path.join(py_dir, "golden_probs.bin"))
    esp_probs_path = os.path.join(esp_dir, "golden_esp32_probs.bin")
    if os.path.exists(esp_probs_path):
        esp_probs = np.fromfile(esp_probs_path, dtype=np.float32)
        if py_probs is not None and esp_probs is not None:
            calc_mse(py_probs, esp_probs, "Probabilities")

if __name__ == "__main__":
    main()
