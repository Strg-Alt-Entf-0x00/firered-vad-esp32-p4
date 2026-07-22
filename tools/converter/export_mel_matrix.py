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
        f.write(f"// Generated Kaldi Mel Filterbank (n_mels={n_mels}, n_fft={n_fft})\n")
        f.write(f"const float KALDI_MEL_BASIS[{n_mels * (n_fft // 2 + 1)}] = {{\n")
        
        flat_basis = mel_basis.flatten()
        for i in range(0, len(flat_basis), 8):
            f.write("    " + ", ".join([f"{val:.6f}f" for val in flat_basis[i:i+8]]) + ",\n")
        f.write("};\n\n")
        
        f.write(f"// Generated Kaldi Povey Window (size 400)\n")
        f.write(f"const float KALDI_WINDOW[400] = {{\n")
        for i in range(0, len(window), 8):
            f.write("    " + ", ".join([f"{val:.6f}f" for val in window[i:i+8]]) + ",\n")
        f.write("};\n")
    
    print(f"Exported {out_file}")

if __name__ == "__main__":
    main()
