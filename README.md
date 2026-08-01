# FireVAD ESP32-P4

A highly optimized, bare-bones native C++ implementation of the **FireRedVAD** Voice Activity Detection (VAD) model, specifically ported and optimized for the **ESP32-P4** microcontroller series.

This repository strips away external deep-learning dependencies (no TFLite, no ONNX runtime) and executes standard DFSMN inference directly via bare-metal matrix-vector operations. It is designed to be as lightweight and fast as possible while fully running in local PSRAM and Flash.

## Core Features & Architecture

- **Pure C++ Inference**: Zero heavy dependencies. Uses only `esp-dsp` for rapid audio feature extraction (FFT and Mel-Filterbanks).
- **Offline ML Execution**: 100% offline inference ensuring strict privacy. Models are stored dynamically on flash or compiled into the binary.
- **Dynamic Feature Extraction**: Real-time extraction of 80-dimensional log-mel filterbank features from raw 16kHz PCM audio buffers.
- **DFSMN Architecture**: Implements Deep Feedforward Sequential Memory Networks (DFSMN) using a highly efficient memory ring-buffer to store historical states.

## Technical Specifications & Performance

Running advanced speech models on microcontrollers requires navigating severe hardware constraints. The following data represents the real-world performance on the ESP32-P4 (Dual-Core RISC-V @ 400MHz, 32MB PSRAM).

### The PSRAM Bandwidth Bottleneck

Our empirical benchmarks revealed a critical hardware limitation: The ESP32-P4's Octal-SPI PSRAM bandwidth (~200-400 MB/s) is insufficient for large, unoptimized models. FP32 variants demand too much memory bandwidth, causing the CPU to stall while waiting for weights.

**Benchmark Results (10ms Audio Frame):**
| Model Type                 | Avg Latency | RT-Load  | Viable for Real-Time? |
|----------------------------|-------------|----------|-----------------------|
| `stream-fp32`              | 35.2 ms     | 352%     | ❌ No (Audio Drops)    |
| `firered-vad-int8` (dense) |  4.7 ms     |  47%     | ❌ No (Context lost)   |
| **`stream-int8-ch`**       | **4.5 ms**  | **45%**  | ✅ **Yes (Recommended)** |

*RT-Load > 100% means inference takes longer than the 10ms audio chunk.*

**Conclusion**: You **must** use the `stream-int8` or `stream-int8-ch` models. They reduce memory bandwidth by 4x (via 8-bit quantization) and use a FSMN ring-buffer to fit entirely in the high-speed L2 cache, enabling inference in ~4.5ms (well under the 10ms real-time budget).

### Quantization: The `int8_ch` Necessity

When quantizing the DFSMN architecture from FP32 down to INT8, we encountered significant accuracy degradation using standard per-tensor quantization (`int8`). The variance in weight distribution across neurons in DFSMN layers is too wide for a single scaling factor.

**Solution: Per-Channel Quantization (`int8_ch`)**
We implemented per-channel quantization, assigning individual scale factors to each output neuron. 
- **`int8_ch` (Recommended)**: Preserves near-FP32 accuracy by accurately capturing the dynamic range of each channel, with virtually zero performance penalty (4.54ms vs 4.47ms).
- **ESP32-P4 PIE Alignment**: The ESP32-P4 Vector unit (PIE) instructions (e.g., `esp.vmulas.s8.xacc`) enforce strict 16-byte memory alignment. Our runtime dynamically aligns the quantized weight matrices to ensure maximum vector-instruction throughput without CPU emulation traps.

## Known Limitations & Accuracies

We prioritize scientific honesty over marketing claims. Please consider the following limitations before production deployment:

1. **Background Noise Susceptibility**: FireRedVAD performs exceptionally well in quiet or moderately noisy environments. However, in low Signal-to-Noise Ratio (SNR) environments (e.g., loud machinery, wind), false positive rates increase. 
2. **Microphone Dependency**: The model expects clean 16kHz PCM audio. A high-quality I2S microphone (e.g., INMP441) with hardware gain control is highly recommended. The ESP-IDF software does not include noise suppression out-of-the-box.
3. **Hardware DAC Constraints**: Relying on internal 8-bit DACs or raw ADC input for voice processing is actively discouraged due to poor signal quality. Use digital I2S peripherals.

### Roadmap
The architecture is actively being evolved for professional deployment. Current development priorities include:
- **Sparse Mel-Filterbanks**: The 80-bin Mel-Filterbank matrix is ~95% sparse. Moving from dense to Compressed Sparse Row (CSR) logic will significantly reduce feature extraction latency.
- **Professional Power Architecture**: Implementing a multi-stage wake pipeline (LP-Core energy detection → HP-Core VAD) with adaptive noise-floor calibration to reach <5mW idle power.

## Using the Component in Your Project

The core component is isolated in `components/esp_firevad`.

Simply copy the `components/esp_firevad` directory into your project's `components` folder, or add it via your `idf_component.yml`.

In your `main.cpp`:
```cpp
#include "esp_firevad.h"
#include "esp_firevad_dsp.h"

// 1. Load the model from memory or SPIFFS
EspFirevadModel model;
esp_firevad_load(frvd_binary_data, data_len, &model);

// 2. Initialize DSP (FFT)
esp_firevad_dsp_init();
esp_firevad_reset(&model);

// 3. Process Audio (160 samples = 10ms at 16kHz)
float features[80];
esp_firevad_dsp_extract_features(pcm_160_buffer, features, NULL);
float prob = esp_firevad_infer_frame(&model, features, true);

if (prob > 0.6f) {
    printf("Speech Detected!\n");
}
```

> [!TIP]
> **Example Application**
> For a full, interactive implementation including model downloading from HuggingFace and SPIFFS flashing, see the [Console Example](examples/console_vad/README.md).

## License
This project is licensed under the **Apache License 2.0** - see the `LICENSE` file for details. Based on the architecture of FireRedVAD.
