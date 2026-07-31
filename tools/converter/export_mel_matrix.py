import numpy as np
import librosa
import os

def generate_povey_window(window_length):
    # Kaldi uses Povey window by default: a = 0.54, but pow is 0.85
    # w(n) = (0.54 - 0.46 * cos(2 pi n / (N - 1))) ^ 0.85
    n = np.arange(window_length)
    window = (0.54 - 0.46 * np.cos(2 * np.pi * n / (window_length - 1))) ** 0.85
    return window

def main():
    sr = 16000
    n_fft = 512
    n_mels = 80
    fmin = 20
    fmax = sr // 2  # Kaldi default usually goes up to nyquist unless specified

    # Kaldi uses 'htk' formula for Mel scale by default
    mel_basis = librosa.filters.mel(sr=sr, n_fft=n_fft, n_mels=n_mels, fmin=fmin, fmax=fmax, htk=True, norm=None)
    
    # In Kaldi, the power spectrum is usually computed without scaling by 1/N.
    # librosa.filters.mel returns weights. We just need to export these as a 1D C array (size 80 * 257)
    
    window = generate_povey_window(400)

    out_file = os.path.join(os.path.dirname(__file__), "..", "..", "components", "esp_firevad", "src", "mel_constants.h")
    with open(out_file, "w") as f:
        f.write("#pragma once\n\n")
        f.write(f"// Generated Kaldi Sparse Mel Filterbank (n_mels={n_mels}, n_fft={n_fft})\n")
        
        # Build CSR representation
        counts = []
        indices = []
        weights = []
        
        for m in range(n_mels):
            row = mel_basis[m]
            nz_idx = np.where(row > 1e-6)[0]
            counts.append(len(nz_idx))
            indices.extend(nz_idx)
            weights.extend(row[nz_idx])
            
        f.write(f"const uint8_t KALDI_MEL_COUNTS[{n_mels}] = {{\n")
        for i in range(0, len(counts), 16):
            f.write("    " + ", ".join([str(c) for c in counts[i:i+16]]) + ",\n")
        f.write("};\n\n")
        
        f.write(f"const uint16_t KALDI_MEL_OFFSETS[{n_mels}] = {{\n")
        offsets = np.cumsum([0] + counts[:-1])
        for i in range(0, len(offsets), 16):
            f.write("    " + ", ".join([str(o) for o in offsets[i:i+16]]) + ",\n")
        f.write("};\n\n")
        
        f.write(f"const uint16_t KALDI_MEL_INDICES[{len(indices)}] = {{\n") # uint16_t just in case FFT > 256
        for i in range(0, len(indices), 16):
            f.write("    " + ", ".join([str(idx) for idx in indices[i:i+16]]) + ",\n")
        f.write("};\n\n")
        
        f.write(f"const float KALDI_MEL_WEIGHTS[{len(weights)}] = {{\n")
        for i in range(0, len(weights), 8):
            f.write("    " + ", ".join([f"{w:.6f}f" for w in weights[i:i+8]]) + ",\n")
        f.write("};\n\n")
        
        f.write(f"// Generated Kaldi Povey Window (size 400)\n")
        f.write(f"const float KALDI_WINDOW[400] = {{\n")
        for i in range(0, len(window), 8):
            f.write("    " + ", ".join([f"{val:.6f}f" for val in window[i:i+8]]) + ",\n")
        f.write("};\n")
    
    print(f"Exported {out_file}")

if __name__ == "__main__":
    main()
