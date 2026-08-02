# FireVAD ESP32-P4

A highly optimized, bare-bones native C++ implementation of the **FireRedVAD** Voice Activity Detection (VAD) model, specifically ported and optimized for the **ESP32-P4** microcontroller series.

This repository strips away external deep-learning dependencies (no TFLite, no ONNX runtime) and executes standard DFSMN inference directly via bare-metal matrix-vector operations. It is designed to be as lightweight and fast as possible while fully running in local PSRAM and Flash.

**Models:** https://huggingface.co/Strg-Alt-Entf-0x00/FireRedVAD-ESP32-P4

## Core Features & Architecture

- **Pure C++ Inference**: Zero heavy dependencies. Uses only `esp-dsp` for rapid audio feature extraction (FFT and Mel-Filterbanks).
- **Offline ML Execution**: 100% offline inference ensuring strict privacy. Models are stored on LittleFS flash.
- **Dynamic Feature Extraction**: Real-time extraction of 80-dimensional log-mel filterbank features from raw 16kHz PCM audio buffers.
- **DFSMN Architecture**: Implements Deep Feedforward Sequential Memory Networks (DFSMN) using a highly efficient memory ring-buffer to store historical states.
- **Asynchronous Audio I/O**: FreeRTOS Ringbuffer on Core 1 for lock-free, stall-free I2S TX. LittleFS for model storage.

## Technical Specifications & Performance

All numbers below are **real measurements** from the ESP32-P4 (Dual-Core RISC-V @ 360MHz, 32MB PSRAM),
benchmarked against 21 diverse audio sources.
Source: [`.docs/2026-08-01_09-42-45_benchmark_results.md`](.docs/2026-08-01_09-42-45_benchmark_results.md)

### The PSRAM Bandwidth Bottleneck

Our empirical benchmarks revealed a critical hardware limitation: The ESP32-P4's Octal-SPI PSRAM
bandwidth is insufficient for large, unoptimized models. FP32 variants demand too much memory
bandwidth, causing the CPU to stall while waiting for weights. INT16 variants are paradoxically
**even slower than FP32** due to conversion overhead on the RISC-V pipeline.

### Benchmark Results (10ms audio frame = 10,000 µs real-time budget)

| Model | Avg Latency | CPU Cycles | RT Load | Verdict |
|---|---|---|---|---|
| `stream-fp32` | 35,216 µs | 12,678,649 | 352% | No — 35x over budget |
| `stream-int16` | 43,499 µs | 15,660,528 | 435% | No — **slower than FP32** |
| `stream-int8` | **4,470 µs** | 1,610,007 | **44.7%** | Yes — 7.87x faster than FP32 |
| **`stream-int8-ch`** | **4,538 µs** | 1,634,483 | **45.4%** | **Yes — Recommended** |
| `vad-fp32` | 35,025 µs | 12,609,993 | 350% | No — batch only |
| `vad-int16` | 43,613 µs | 15,701,420 | 436% | No — batch only |
| `vad-int8` | 4,705 µs | 1,694,745 | 47.0% | Yes — batch only |
| **`vad-int8-ch`** | **4,726 µs** | 1,702,164 | **47.3%** | **Yes — batch Recommended** |

*RT Load = percentage of the 10ms real-time frame budget consumed.*

**Critical note on INT16:** Do not use INT16 for real-time applications. It is 23% **slower** than FP32
on the ESP32-P4 RISC-V pipeline due to the lack of native INT16 vector instructions.
Use INT8 or INT8-CH exclusively for real-time workloads.

### Quantization: Why INT8-CH is Required for Production

Standard per-tensor INT8 quantization (`int8`) assigns **one** global scale factor per weight matrix.
DFSMN architectures have wide variance in weight distribution across output channels — a single
scale factor cannot capture this range accurately, causing silent accuracy degradation.

**Per-Channel INT8 (`int8-ch`, Version 4 in the `.frvd` format)** assigns **one scale factor
per output channel**. This preserves near-FP32 accuracy at INT8 speed and memory cost.

| | int8 | int8-ch | int16 | fp32 |
|---|---|---|---|---|
| `.frvd` version | 2 | 4 | 3 | 1 |
| Avg latency (P4 @ 360MHz) | 4.47ms | 4.54ms | 43.5ms | 35.2ms |
| RT load | 44.7% | 45.4% | 435% | 352% |
| Accuracy vs FP32 | Degraded | Near-identical | High | Reference |
| Usable for real-time | Yes | Yes | No | No |

## Known Limitations (Scientific Honesty)

We prioritize scientific honesty over marketing claims. Please consider the following limitations
before production deployment:

1. **Background Noise Susceptibility**: FireRedVAD performs exceptionally well in quiet or moderately
   noisy environments. In low SNR conditions (loud machinery, wind), false positive rates increase.
   Measured noise floor: `noise-clock-tick` → 0.4% speech, `noise-cafeteria` → ~60% speech
   (the model correctly detects babble noise as speech-like).

2. **INT16 Anomaly**: INT16 models show paradoxically higher latency than FP32 (435% vs 352% RT load)
   on the ESP32-P4. This is a confirmed RISC-V pipeline characteristic, not a bug. Do not use INT16
   for any real-time path.

3. **Microphone Dependency**: The model expects clean 16kHz PCM audio. A high-quality I2S microphone
   (e.g., INMP441) with hardware gain control is required. No built-in noise suppression.

4. **APLL Sharing**: When I2S0 (ES8311 codec, TX) and I2S1 (INMP441, RX) are both active, the
   shared APLL runs at 8,191,999 Hz instead of 8,192,000 Hz (1 Hz deviation). This is expected
   behavior and is actually ideal for AEC — both ports are locked to the same clock.

### Roadmap

- **Sparse Mel-Filterbanks**: The 80-bin Mel-Filterbank matrix is ~95% sparse. Moving to
  Compressed Sparse Row (CSR) logic will significantly reduce feature extraction latency.
- **Professional Power Architecture**: Multi-stage wake pipeline (LP-Core energy detection →
  HP-Core VAD) with adaptive noise-floor calibration targeting <5mW idle power.

## Using the Component in Your Project

The core component is isolated in `components/esp_firevad`.

Copy `components/esp_firevad` into your project's `components` folder, or add it via `idf_component.yml`.

In your `main.cpp`:
```cpp
#include "esp_firevad.h"
#include "esp_firevad_dsp.h"

// 1. Load the model from LittleFS
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
> For a full, interactive implementation including model downloading from HuggingFace and LittleFS flashing, see the [Console Example](examples/console_vad/README.md).

## License

This project is licensed under the **Apache License 2.0** — see the `LICENSE` file for details.
Based on the architecture of [FireRedVAD](https://github.com/FireRedTeam/FireRedVAD) by Xiaohongshu (FireRedTeam).
