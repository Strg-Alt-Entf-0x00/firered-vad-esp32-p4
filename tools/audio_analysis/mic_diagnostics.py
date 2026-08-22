import os
import sys
import numpy as np
import soundfile as sf
import matplotlib.pyplot as plt
from glob import glob

def calculate_dbfs(audio_data):
    """Calculate RMS level in dB Full Scale."""
    rms = np.sqrt(np.mean(audio_data**2))
    if rms == 0:
        return -100.0
    # For a normalized float array (-1.0 to 1.0), full scale sine wave RMS is 0.707
    # 20 * log10(rms) gives dBFS relative to digital clipping (0 dBFS max)
    return 20 * np.log10(rms + 1e-10)

def compute_fft(audio_data, sample_rate):
    """Compute the frequency spectrum using Welch's method for smoother visualization."""
    from scipy.signal import welch
    # Use Welch's method to get a smooth Power Spectral Density (PSD)
    freqs, psd = welch(audio_data, fs=sample_rate, nperseg=2048)
    psd_db = 10 * np.log10(psd + 1e-10)
    return freqs, psd_db

def analyze_directory(wav_dir):
    wav_files = glob(os.path.join(wav_dir, "*.wav"))
    if not wav_files:
        print(f"[ERROR] No WAV files found in {wav_dir}.")
        return
    
    print("=" * 60)
    print(f"AUDIO DIAGNOSTICS REPORT")
    print("=" * 60)
    
    plt.figure(figsize=(12, 8))
    
    for f in sorted(wav_files):
        filename = os.path.basename(f)
        try:
            data, sr = sf.read(f)
        except Exception as e:
            print(f"[ERROR] Could not read {filename}: {e}")
            continue
            
        # Ensure mono
        if len(data.shape) > 1:
            data = data.mean(axis=1)
            
        # [CRITICAL FIX] Skip the first 0.5 seconds of the recording.
        # Analog ADCs (ES8311) often produce a massive DC-offset "pop" when turning on the PGA.
        # This single spike would completely ruin the RMS (dBFS) calculation.
        skip_samples = int(0.5 * sr)
        if len(data) > skip_samples:
            data = data[skip_samples:]
            
        dbfs = calculate_dbfs(data)
        # Add 0.5s back to the display length so the printout looks correct
        print(f"File: {filename:<30} | Level: {dbfs:6.2f} dBFS | Length: {(len(data)+skip_samples)/sr:.1f}s")
        
        freqs, psd_db = compute_fft(data, sr)
        
        # Plot styling based on mic type if discernible from filename
        linestyle = '-'
        if 'silence' in filename.lower() or 'stille' in filename.lower():
            linestyle = '--'
        
        plt.plot(freqs, psd_db, label=f"{filename} ({dbfs:.1f} dB)", linestyle=linestyle, alpha=0.8)
    
    print("-" * 60)
    print("Saving frequency analysis plot (FFT)...")

    plt.title("Microphone Frequency Response (Power Spectral Density)", fontsize=14)
    plt.xlabel("Frequency (Hz)", fontsize=12)
    plt.ylabel("Power/Frequency (dB/Hz)", fontsize=12)
    plt.xscale('log')  # Logarithmic scale is standard for audio frequency visualization
    plt.xlim(20, 8000)  # ESP32 records at 16kHz, so Nyquist is 8kHz
    plt.grid(True, which="both", ls="-", alpha=0.2)
    plt.legend(bbox_to_anchor=(1.05, 1), loc='upper left')
    plt.tight_layout()

    output_png = os.path.join(wav_dir, "mic_analysis_fft.png")
    plt.savefig(output_png, dpi=150)
    print(f"[OK] Plot saved to: {output_png}")
    print("Done!")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python mic_diagnostics.py <wav_directory>")
        sys.exit(1)
    analyze_directory(sys.argv[1])
