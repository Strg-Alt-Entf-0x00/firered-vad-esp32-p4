# FireRedVAD for ESP32-P4 (Industrial Grade)

A professional, hardware-accelerated Voice Activity Detection (VAD) pipeline specifically optimized for the **ESP32-P4 (Chip Revision v1.3)**. This repository provides a fully multithreaded, real-time AI audio pipeline running on the **Waveshare ESP32-P4-WIFI6** board.

## 🚀 Key Features

* **Multithreaded Audio Pipeline:** Lock-free Producer/Consumer ringbuffers safely separate the I2S Audio-Rx (Core 0), DSP processing (Core 0), and Neural Network inference (Core 1). This ensures zero audio drops even if the AI or SD card blocks momentarily.
* **Dual-Core Math (Model Dependent):** Matrix multiplications for large models (FP32/INT16) are split across both CPU cores. To maximize efficiency, heavily quantized **INT8** models skip the dual-core overhead and use an ultra-fast **single-core fast path** (~5.5ms per 10ms frame).
* **ESP-DSP Hardware Acceleration:** Implements a highly efficient Biquad IIR High-Pass Filter (80Hz cutoff) to remove DC-offset and low-frequency mechanical rumble *before* AI inference.
* **Smart Speaker Audio Pipeline:** Features adaptive Automatic Gain Control (AGC) with noise-gating and attack/decay smoothing.
* **Auto-Calibration:** Boot-time environmental noise calibration (1-second absolute silence check) for dynamic Pre-VAD thresholding.

## 🎤 Audio Frontend & AGC Results
We achieved a highly robust audio frontend using the INMP441 I2S microphone combined with a custom Automatic Gain Control (AGC) and DC-Offset High-Pass filter:
- **Dynamic Range Compression**: Without AGC, voice volume drops steeply from -27 dB (at 10cm) to -47 dB (at 2m). With AGC enabled, the volume stays rock-solid between -30 dB and -37 dB!
- **Distance Boost**: The AGC automatically boosts weak signals at a 2-meter distance by +10 dB, while seamlessly throttling loud, up-close speech.
- **SNR**: Crystal clear 23 dB SNR.
- **Golden Test**: Bit-parity and VAD-quality mathematically verified.

👉 *For deep technical details on our hardware tests, the Golden Test mathematics, FFT analyses, and AGC performance, read our [Engineering & Hardware Insights](docs/ENGINEERING_INSIGHTS.md).*

## 🎛️ Hardware Setup (Waveshare ESP32-P4-WIFI6)

This project is optimized for the **Waveshare ESP32-P4-WIFI6** development board.

### Primary Microphone (Recommended)
For the best AI inference results, an external digital I2S microphone (e.g., **INMP441**) is heavily recommended over the onboard analog mic.
* **Mic Type:** INMP441 (Mono I2S)
* **Pins:**
  * `BCLK` = GPIO 20
  * `WS` = GPIO 21
  * `DIN` = GPIO 22

### Fallback/Onboard Microphone (ES8311)
The board includes an onboard analog SMD microphone routed through the ES8311 codec.
* **Pins:** I2C (`SDA`=GPIO7, `SCL`=GPIO8), I2S0 (`MCLK`=GPIO13, `BCLK`=GPIO12, `WS`=GPIO10, `DIN`=GPIO9).
* *Note:* Due to analog interference from the LCD and 2.4GHz WiFi traces, this microphone is suitable as a secondary environment sensor but not recommended as the primary VAD input.

## 📦 Using it as an ESP-IDF Component

You can integrate this VAD directly into your own firmware:
1. Copy the `components/esp_firevad` folder into your project's `components/` directory.
2. Link it in your `CMakeLists.txt` via `REQUIRES esp_firevad`.
3. Use the C-API (`esp_firevad.h` and `esp_firevad_dsp.h`) to extract features and run inferences.

## 🧠 Neural Network Models & Tools

This project uses the official FireRedVAD weights, which must be downloaded and optionally converted.

1. **Original Models:** If you want the original raw PyTorch/ONNX models (for research or manual conversion), run:
   ```bash
   python tools/download_pth_models.py
   ```
2. **Optimized ESP32 Models (.frvd):** If you just want to run the code, use our pre-quantized `INT8` and `FP32` models specifically built for the ESP32:
   ```bash
   python examples/console_vad/tools/download_frvd_models.py
   ```

Models are heavily quantized (`INT8`) to fit into the ESP32's L2 Cache and execute in under **6 milliseconds** per 10ms audio frame on a single 360MHz RISC-V core.

## 🛠️ Getting Started

To compile the firmware, explore the hardware, and test the microphone pipeline, navigate to the fully featured CLI example:
👉 **[examples/console_vad](examples/console_vad)**

## 📊 Performance Benchmarks & Quantization (ESP32-P4)

*(Last Benchmark Run: 2026-08-21 17:26:16)*

All numbers below represent the **raw Neural Network (NN) inference latency** measured sequentially on a single core (ESP32-P4 Dual-Core RISC-V @ 360MHz, 32MB PSRAM).

> [!NOTE]
> **Real-World C++ Throughput:** In production, our C++ firmware (`console_vad`) utilizes an asynchronous **Dual-Core Pipeline** (Audio DSP on Core 0, NN on Core 1). Because these tasks run in parallel, the total system throughput is bound only by the slower core. 
> As a result, even the massive `Stream-VAD FP32` model achieves an end-to-end pipeline throughput of **~8.4 ms** (RTF = 0.84) on real hardware, comfortably meeting the 10ms real-time budget without any quantization!

### 1. Raw NN Inference (Python / Single-Core)
*(10ms audio frame = 10,000 µs real-time budget)*

| Model | Avg Latency | CPU Cycles | Real-Time Load | Verdict |
|-------|-------------|------------|----------------|---------|
| `Stream-VAD FP32` | 27.31 ms | 9,832,326 | 273.1% | No - Over budget |
| `Stream-VAD INT16`| 11.77 ms | 4,235,911 | 117.7% | No - Over budget |
| `Stream-VAD INT8` | 6.07 ms | 2,186,134 | 60.7% | **Yes - Real-time capable** |
| `Stream-VAD INT8-CH`| **6.12 ms** | 2,203,320 | **61.2%** | **Yes - Recommended** |
| `VAD FP32`        | 38.91 ms | 12,609,993| 389.1% | No - Offline only |
| `VAD INT16`       | 48.45 ms | 15,701,420| 484.5% | No - Offline only |
| `VAD INT8`        | 5.22 ms  | 1,694,745 | 52.2%  | Yes - Offline only |
| `VAD INT8-CH`     | **5.25 ms** | 1,702,164 | **52.5%** | **Yes - Offline Recommended** |
| `AED-VAD FP32`    | 28.79 ms | 10,364,610| 287.9% | No - Offline only |
| `AED-VAD INT16`   | 12.83 ms | 4,619,155 | 128.3% | No - Offline only |
| `AED-VAD INT8`    | 7.01 ms  | 2,522,769 | 70.1%  | Yes - Offline only |
| `AED-VAD INT8-CH` | **7.05 ms** | 2,530,100 | **70.5%** | **Yes - Offline Recommended** |

### 2. End-to-End Pipeline Throughput (C++ Console / Hardware)
*(Includes I2S Capture + DSP + RTOS Overhead. Measured via `cmd_benchmark`)*

| Model | C++ Execution Mode | Pipeline Throughput | RTF | Real-Time Capable? |
|-------|--------------------|---------------------|-----|--------------------|
| **FP32 Models** | *(2.3 MB)* | | | |
| `Stream-VAD FP32` | Dual-Core (DSP+NN parallel) | **8.42 ms** | 0.84 | **Yes** |
| `AED-VAD FP32`    | Dual-Core (DSP+NN parallel) | **8.67 ms** | 0.87 | **Yes** |
| `VAD FP32`        | Dual-Core (DSP+NN parallel) | **8.73 ms** | 0.87 | **Yes** |
| **INT16 Models** | *(1.1 MB)* | | | |
| `Stream-VAD INT16`| Dual-Core (DSP+NN parallel) | **6.01 ms** | 0.60 | **Yes (Fastest!)** |
| `AED-VAD INT16`   | Dual-Core (DSP+NN parallel) | **6.04 ms** | 0.60 | **Yes** |
| `VAD INT16`       | Dual-Core (DSP+NN parallel) | **6.03 ms** | 0.60 | **Yes** |
| **INT8 / INT8-CH Models** | *(575 KB)* | | | |
| `Stream-VAD INT8` | Single-Core (Inline seq.)   | **12.55 ms**| 1.25 | **No** (Buffering req.) |
| `AED-VAD INT8`    | Single-Core (Inline seq.)   | **13.52 ms**| 1.35 | **No** (Buffering req.) |
| `VAD INT8`        | Single-Core (Inline seq.)   | **13.49 ms**| 1.34 | **No** (Buffering req.) |
| `Stream-VAD INT8-CH` | Single-Core (Inline seq.)| **12.58 ms**| 1.25 | **No** (Buffering req.) |
| `AED-VAD INT8-CH` | Single-Core (Inline seq.)   | **13.55 ms**| 1.35 | **No** (Buffering req.) |
| `VAD INT8-CH`     | Single-Core (Inline seq.)   | **13.53 ms**| 1.35 | **No** (Buffering req.) |

### Quantization: Why INT8-CH is Required for Production

Standard per-tensor INT8 quantization (`int8`) assigns **one** global scale factor per weight matrix. DFSMN architectures have wide variance in weight distribution across output channels — a single scale factor cannot capture this range accurately, causing silent accuracy degradation.
**Per-Channel INT8 (`int8-ch`)** assigns **one scale factor per output channel**. This preserves near-FP32 accuracy at INT8 speed and memory cost.

## ⚠️ Known Limitations (Scientific Honesty)

We prioritize scientific honesty over marketing claims:
1. **Background Noise Susceptibility**: FireRedVAD performs exceptionally well in quiet or moderately noisy environments. In low SNR conditions (loud machinery, wind), false positive rates increase (e.g. babble noise is often detected as speech-like).
2. **APLL Sharing**: When I2S0 (ES8311 codec, TX) and I2S1 (INMP441, RX) are both active, the shared APLL runs at 8,191,999 Hz instead of 8,192,000 Hz. This 1Hz deviation is expected behavior.

## 📜 License

This project is licensed under the **Apache License 2.0**.
Based on the architecture of [FireRedVAD](https://github.com/FireRedTeam/FireRedVAD) by Xiaohongshu (FireRedTeam).
