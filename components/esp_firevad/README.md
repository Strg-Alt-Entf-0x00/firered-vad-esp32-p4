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

The inference engine processes audio frames every 10ms. Logging every frame would generate ~100 logs/second, causing UART TX buffer overflow and watchdog timeout.

**Three logging modes available via menuconfig:**

#### 1. **DISABLED** (Production Default) ✅
```
idf.py menuconfig
→ Component config
→ ESP FireVAD Configuration
→ Performance logging mode
→ (*) Disabled (production)
```
**Use when:** Production deployment, maximum stability  
**Log rate:** 0 logs/second  
**Safety:** ✅ Safe

#### 2. **RATE_LIMITED** (Debugging) 🔧
```
idf.py menuconfig
→ Component config
→ ESP FireVAD Configuration  
→ Performance logging mode
→ (*) Rate-limited (safe debugging)
→ Log rate divider: 100 (= 1 log/second)
```
**Use when:** Performance analysis, debugging  
**Log rate:** Configurable (default: 1 log/second)  
**Safety:** ✅ Safe for extended use  

**Rate divider examples:**
- `10` = 10 logs/second (very verbose, increase baudrate!)
- `50` = 2 logs/second (detailed analysis)
- `100` = 1 log/second (recommended)
- `200` = 0.5 logs/second (conservative)

#### 3. **FULL** (Testing Only) ⚠️
```
idf.py menuconfig
→ Component config
→ ESP FireVAD Configuration
→ Performance logging mode
→ (*) Full per-frame (DANGEROUS - testing only!)
```
**Use when:** Short precision tests (<5 seconds)  
**Log rate:** 100 logs/second  
**Safety:** ⚠️ **WILL CRASH** after ~7 seconds at 115200 baud!  

**Requirements for FULL mode:**
- High baudrate (921600+)
- UART flow control enabled
- Test duration <5 seconds
- Only for precise performance measurements

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
