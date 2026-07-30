# ESP32-P4 FireVAD

A highly optimized, bare-bones native C++ implementation of the **FireRedVAD** Voice Activity Detection (VAD) model, specifically ported for the **ESP32-P4** microcontroller series. 

This repository strips away external deep-learning dependencies (no TFLite, no ONNX runtime) and executes standard DFSMN inference directly via optimized matrix-vector operations. It is designed to be as lightweight and fast as possible while fully running in local PSRAM and Flash.

## Core Features

- **Pure C++ Inference**: Zero heavy dependencies. Uses only `esp-dsp` for rapid audio feature extraction (FFT and Mel-Filterbanks).
- **Offline ML Execution**: 100% offline inference ensuring strict privacy. Models are stored dynamically on flash or compiled into the binary.
- **Dynamic Feature Extraction**: Real-time extraction of 80-dimensional log-mel filterbank features from raw 16kHz PCM audio buffers.
- **Modular ESP-IDF Component**: Designed to be integrated effortlessly into any existing ESP-IDF project via `idf_component.yml`.
- **Python Toolchain**: Includes `.onnx` to custom `.frvd` format converters, along with int8/int16 quantizers to radically reduce memory footprint.

## Repository Architecture

```text
├── components/
│   └── esp_firevad/           # Core ESP-IDF Component
│       ├── include/           # Public APIs (esp_firevad.h, esp_firevad_dsp.h)
│       └── src/               # Native inference and DSP feature extraction 
├── examples/
│   └── console_vad/           # Interactive Console Example (REPL)
│       ├── main/              # Console commands for model loading and testing
│       └── converted_models/  # Pre-converted .frvd models (Stream-VAD, VAD, AED)
├── tools/
│   ├── download_models.py     # Download original models from Hugging Face
│   └── converter/             # Python scripts to convert PyTorch → .frvd
│       ├── export_weights.py  # Main converter (FP32, INT8, INT16)
│       └── verify_conversion.py # Bit-exact verification against PyTorch
└── LICENSE                    # Apache License 2.0
```

## Getting Started

### 1. Clone Repository

```bash
git clone https://github.com/Strg-Alt-Entf-0x00/firered-vad-esp32-p4
cd firered-vad-esp32-p4
```

### 2. Requirements
*   ESP-IDF v5.2+ (v6.0+ recommended)
*   ESP32-P4 Development Board (e.g., Waveshare ESP32-P4-WiFi6)
*   Python 3.8+ with pip

### 2. Using the Component in your Project
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

### 3. Quick Start with Console Example

The easiest way to test FireVAD is with the **Console Example**:

```bash
cd examples/console_vad

# 1. Download pre-converted models from HuggingFace
python download_models.py              # INT8 only (~2 MB)
# OR
python download_models.py --all-quantizations  # All variants (~12 MB)

# 2. Build & Flash (SPIFFS image created automatically!)
idf.py build flash monitor

# 3. In the console, try:
firevad> model_list
firevad> model_load firered-stream-vad-int8.frvd
firevad> start
```

**What happens during build:**
- ✅ CMake checks if `converted_models/` exists
- ✅ Automatically creates SPIFFS partition image
- ✅ `idf.py flash` flashes both firmware + models in one command!

```bash
cd examples/console_vad

# Download pre-converted models from HuggingFace
pip install huggingface-hub
python download_models.py  # Downloads INT8 models (~2 MB)

# Build and flash
idf.py set-target esp32p4
idf.py build flash monitor
```

Once running, use the interactive console:
```
firevad> model_list                           # List available models
firevad> model_load stream-vad/int8/firered-stream-vad-int8.frvd
firevad> model_info                           # Show model details
firevad> start                                # Start inference
```

**Models available:**
- **Stream-VAD INT8** (556 KB) - Real-time, 10ms latency ⭐ Recommended
- **VAD INT8** (576 KB) - Offline, high accuracy
- **AED INT8** (576 KB) - Audio Event Detection (Speech/Music/Singing)

📦 **Models Repository:** https://huggingface.co/Strg-Alt-Entf-0x00/FireRedVAD-ESP32-P4

### 4. Using in Your Project

Add `esp_firevad` to your project's components:

```bash
cp -r components/esp_firevad your_project/components/
```

Or via `idf_component.yml`:
```yaml
dependencies:
  esp_firevad:
    git: https://github.com/Strg-Alt-Entf-0x00/firered-vad-esp32-p4
    path: components/esp_firevad
```

### 5. Converting Your Own Models (Advanced)

To convert original FireRedVAD models from PyTorch to `.frvd` format:

```bash
# 1. Install dependencies
cd tools
pip install -r converter/requirements.txt
pip install huggingface-hub

# 2. Download original models from Hugging Face
python download_models.py --all --output-dir ./original_models

# 3. Convert to .frvd format with INT8 quantization
cd converter
python export_weights.py \
    --model-dir ../original_models/Stream-VAD \
    --output-dir ../converted_models/stream-vad/int8 \
    --quantize-int8
```

**Available quantization options:**
- `--quantize-int8`: ~556 KB, minimal accuracy loss (recommended for ESP32-P4)
- `--quantize-int16`: ~1.1 MB, better accuracy
- No flag: FP32, ~2.2 MB, highest accuracy

## License
This project is licensed under the **Apache License 2.0** - see the `LICENSE` file for details. Based on the architecture of FireRedVAD.
