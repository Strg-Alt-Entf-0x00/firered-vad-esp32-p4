# ESP FireVAD Component

FireRedVAD Native C++ Inference Engine for ESP32-P4 with Int8/Int16/FP32 quantization support.

## Features

- DFSMN (Deep Feedforward Sequential Memory Network) inference
- Streaming and offline VAD modes
- Hardware-accelerated Int8 operations using ESP32-P4 PIE
- Optimized for low latency (~7ms per 10ms frame)
- PSRAM-friendly weight storage

## Configuration

### Performance Logging

By default, per-frame performance logging is **disabled** to prevent system instability.

The inference engine processes audio frames every 10ms, which would generate ~100 logs/second if enabled. This can cause:
- UART TX buffer overflow
- Task watchdog timeout (system crash after ~7 seconds)
- Significant performance degradation

**To enable performance logging for debugging:**

```
idf.py menuconfig
→ Component config
→ ESP FireVAD Configuration
→ [*] Enable per-frame performance logging
```

**Warning:** Only enable this for active performance debugging. Always disable for production builds.

## Usage

```c
#include "esp_firevad.h"

// Load model
EspFirevadModel model;
uint8_t* model_buf = load_model_file();
esp_firevad_load(model_buf, file_size, &model);

// Process audio frame (160 samples, 10ms @ 16kHz)
float features[80];
float prob = 0.0f;
esp_firevad_dsp_extract_features(audio_frame, features, &energy);
esp_firevad_infer_frame(&model, features, true, &prob);

// Check VAD result
if (prob > 0.6f) {
    // Speech detected
}
```

## Performance

- **Dense layer:** ~2.1ms
- **FSMN layer:** ~5.0ms  
- **Total:** ~7.1ms per frame (70% CPU load @ 360MHz)

## License

See LICENSE file in repository root.
