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
│   └── esp_firevad/        # The core ESP-IDF Component
│       ├── include/        # Public APIs (esp_firevad.h, esp_firevad_dsp.h)
│       └── src/            # Native inference and DSP feature extraction 
├── examples/
│   └── waveshare_p4_wifi6/ # Minimal demonstration firmware
│       ├── main/           # Clean main.cpp using I2S and ES8311 I2C codec
│       └── components/     # Hardware abstraction layers for audio
├── tools/
│   └── converter/          # Python scripts to convert models (export_weights.py)
└── LICENSE                 # Apache License 2.0
```

## Getting Started

### 1. Requirements
*   ESP-IDF v5.2+
*   ESP32-P4 Development Board (e.g., Waveshare ESP32-P4-WiFi6 with an external microphone/codec)

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

### 3. Downloading Original Models & Converting
To convert the models to the custom `.frvd` format, you first need the original PyTorch models from Hugging Face.

1. Install the Hugging Face CLI:
```bash
pip install -U "huggingface_hub[cli]"
```

2. Download the models to an `original_models` directory:
```bash
huggingface-cli download FireRedTeam/FireRedVAD --local-dir ./original_models
```

3. Navigate to `tools/converter/` and install Python requirements:
```bash
pip install -r requirements.txt
```

4. Use `export_weights.py` to parse the PyTorch checkpoint and export the tightly packed `.frvd` binary. For example:
```bash
python export_weights.py --model-dir ../../original_models/Stream-VAD --output-dir ../../converted_models --quantize-int8
```

## License
This project is licensed under the **Apache License 2.0** - see the `LICENSE` file for details. Based on the architecture of FireRedVAD.
