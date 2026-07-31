# FireVAD Console Example

Professional console-based example demonstrating FireVAD model loading and management on ESP32-P4.

## Overview

This example provides a minimal, hardware-agnostic console interface for FireVAD voice activity detection. It focuses exclusively on model management and inspection without requiring any external audio hardware.

## Features

- **Model Loading**: Load `.frvd` models from LittleFS partition into PSRAM
- **Model Inspection**: View model architecture details (D, H, P, N1, N2, odim)
- **Model Discovery**: List all available models in LittleFS
- **System Monitoring**: Display heap and PSRAM usage statistics
- **Interactive Console**: REPL with command history and auto-completion

## Hardware Requirements

- **ESP32-P4** (any revision: v1.0, v1.1, v1.3, v3.x)
- **USB connection** for serial console
- **No external hardware needed** - this example operates without audio peripherals

## Supported Model Types

### Stream-VAD (Real-time)
- **Architecture**: N2 = 0 (causal, no lookahead)
- **Output**: Single dimension (speech probability)
- **Latency**: 10ms per frame
- **Use Case**: Real-time voice activity detection

### Offline VAD (Batch Processing)
- **Architecture**: N2 = 20 (non-causal, bidirectional)
- **Output**: Single dimension (speech probability)
- **Latency**: 1-second chunks
- **Use Case**: High-accuracy post-processing

### AED (Audio Event Detection)
- **Architecture**: N2 = 20 (non-causal)
- **Output**: 3 dimensions (Speech/Music/Singing)
- **Use Case**: Multi-class audio classification

## Quick Start

### 1. Build and Flash

```bash
cd examples/console_vad
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with your serial port (e.g., `COM3` on Windows, `/dev/ttyUSB0` on Linux).

### 2. Upload Models to LittleFS

Before using the example, you need to upload `.frvd` model files to the LittleFS partition:

#### Using parttool.py (ESP-IDF)

```bash
# Create a LittleFS image with your models
mkdir littlefs_image
cp path/to/*.frvd littlefs_image/

# Generate LittleFS image
python $IDF_PATH/components/spiffs/spiffsgen.py 12582912 littlefs_image littlefs.bin

# Flash to device
python $IDF_PATH/components/partition_table/parttool.py -p PORT write_partition --partition-name=spiffs --input littlefs.bin
```

#### Model Files Location

Models should be placed at:
- `/spiffs/firered_stream_vad_int8.frvd` (Stream-VAD)
- `/spiffs/firered_vad_int8.frvd` (Offline VAD)
- `/spiffs/firered_aed_int8.frvd` (AED)

### 3. Console Commands

Once flashed and running, use these commands in the serial console:

```
firevad> help                              # Show all available commands
firevad> model_list                        # List available .frvd models
firevad> model_load firered_stream_vad_int8.frvd  # Load a model
firevad> model_info                        # Show model architecture details
firevad> test_file speech_welcome_constant_volume.wav  # Test with WAV file
firevad> status                            # Display system status
firevad> threshold 0.5                     # Set detection threshold (0.0-1.0)
firevad> gain -s 2.0                       # Set software gain multiplier
firevad> start                             # Start VAD inference (Stream-VAD only)
firevad> stop                              # Stop VAD inference
firevad> calibrate                         # Toggle calibration mode
firevad> reset_stats                       # Reset statistics
```

## Test Audio Samples

The example includes 5 pre-converted WAV files (16kHz, 16-bit, Mono) for testing different detection scenarios:

### 1. speech_welcome_constant_volume.wav (15s)
- **Purpose**: Basic speech detection test
- **Content**: "Welcome..." with natural pauses
- **Volume**: Constant throughout
- **Expected**: Stream-VAD/VAD should detect speech segments with high confidence

### 2. speech_welcome_varied_volume.wav (15s)
- **Purpose**: Robustness test for varying audio levels
- **Content**: Same "Welcome..." phrase at different volumes
- **Volume Profile**:
  - 0-5s: 100% (Normal) - Should easily detect
  - 5-10s: 30% (Quiet) - Should still detect
  - 10-15s: 10% (Very Quiet) - Tests sensitivity limits
- **Expected**: Tests model robustness to volume changes

### 3. music_rock.wav (8.8s)
- **Purpose**: Music detection (AED only)
- **Content**: Guitar riffs + drums (instrumental)
- **Expected**: 
  - Stream-VAD/VAD: Should NOT detect as speech
  - AED: Should classify as "Music"

### 4. singing_vocal.wav (14s)
- **Purpose**: Singing voice detection
- **Content**: Pure singing voice (a cappella, no instruments)
- **Expected**:
  - Stream-VAD/VAD: May detect as speech (voice is present)
  - AED: Should classify as "Singing"

### 5. negative_birds.wav (15s)
- **Purpose**: False-positive test
- **Content**: Bird chirping sounds
- **Expected**: All models should NOT detect as speech/music/singing

### Testing Workflow

```bash
# 1. Load a model
firevad> model_load firered_stream_vad_int8.frvd

# 2. Test with speech (should detect high speech probability)
firevad> test_file speech_welcome_constant_volume.wav

# 3. Test robustness (watch probability decrease with volume)
firevad> test_file speech_welcome_varied_volume.wav

# 4. Negative test (should show low speech probability)
firevad> test_file negative_birds.wav

# 5. Adjust threshold if needed
firevad> threshold 0.4
firevad> test_file speech_welcome_constant_volume.wav

# 6. Test AED model (requires loading AED model)
firevad> model_load firered_aed_int8.frvd
firevad> test_file music_rock.wav        # Should detect "Music"
firevad> test_file singing_vocal.wav     # Should detect "Singing"
```

### Sample Output Format

**Stream-VAD** (frame-by-frame, 10ms frames):
```
Processing with Stream-VAD (frame-by-frame)...
Time(s) | Speech Prob | Status
--------|-------------|--------
   0.00 |       0.892 | SPEECH
   1.00 |       0.745 | SPEECH
   2.00 |       0.234 | silence
   3.00 |       0.876 | SPEECH
...

=== Results ===
Total frames:  1500
Speech frames: 892 (59.5%)
Threshold:     0.60
```

**Offline VAD** (1-second chunks):
```
Processing with Offline VAD (1-second chunks)...
Chunk | Time(s) | Speech Prob | Status
------|---------|-------------|--------
    0 |     1.0 |       0.945 | SPEECH
    1 |     2.0 |       0.823 | SPEECH
    2 |     3.0 |       0.156 | silence
...
```

**AED** (Audio Event Detection):
```
Processing with AED (Audio Event Detection)...
Chunk | Time(s) |  Speech |   Music | Singing | Classification
------|---------|---------|---------|---------|---------------
    0 |     1.0 |   0.123 |   0.834 |   0.043 | Music
    1 |     2.0 |   0.089 |   0.891 |   0.020 | Music
...
```



## Console Output Example

```
╔════════════════════════════════════════════════════════════════╗
║                                                                ║
║            FireVAD Console - ESP32-P4 Edition                  ║
║                                                                ║
║     Professional Voice Activity Detection System               ║
║                                                                ║
╚════════════════════════════════════════════════════════════════╝

Hardware: ESP32-P4 rev 1.3, 2 cores
Features:
  • Stream-VAD    : Real-time detection (10ms latency)
  • Offline VAD   : High-accuracy batch processing
  • AED           : Audio event classification
  • Console REPL  : Interactive command interface

Type 'help' for available commands.

firevad> model_list

Filename                       | Type            | Size
------------------------------------------------------------------------
firered_stream_vad_int8.frvd   | Stream-VAD      |     245 KB
firered_vad_int8.frvd          | Offline         |     245 KB
firered_aed_int8.frvd          | Offline         |     247 KB

firevad> model_load firered_stream_vad_int8.frvd
Loading model (245 KB)...
[OK] Model loaded successfully
     Run 'model_info' to see details

firevad> model_info

=== Model Information ===
Type:         Stream-VAD
Precision:    Int8
Mode:         CAUSAL (Streaming)
Memory Usage: 245 KB

Architecture:
  D (Input):    64
  H (Hidden):   128
  P (Proj):     32
  odim:         1
  N1 (Past):    10 frames (100.0 ms)
  N2 (Future):  0 frames (0.0 ms)

Usage: Use 'start' for real-time inference
```

## Project Structure

```
console_vad/
├── main/
│   ├── main.cpp              # Console application
│   ├── CMakeLists.txt        # Component configuration
│   └── idf_component.yml     # LittleFS dependency
├── CMakeLists.txt            # Project configuration
├── sdkconfig.defaults        # ESP-IDF defaults
├── partitions.csv            # Partition table (12MB for models)
└── README.md                 # This file
```

## Configuration

### Partition Table (partitions.csv)

```csv
# Name,   Type, SubType, Offset,  Size,  Flags
nvs,      data, nvs,     0x9000,  24K,
phy_init, data, phy,     0xf000,  4K,
factory,  app,  factory, 0x10000, 3M,
spiffs,   data, spiffs,  0x310000,12M,
```

The LittleFS partition has 12MB storage capacity for model files.

### SDK Configuration (sdkconfig.defaults)

Key settings:
- **PSRAM**: Enabled (OPI mode, 80MHz, quad line)
- **Console**: USB Serial JTAG
- **LittleFS**: Mounted at `/spiffs`
- **DSP**: ESP-DSP optimizations enabled

## Memory Requirements

- **Heap**: ~50KB for console and system
- **PSRAM**: 245-300KB per loaded model
- **Flash**: ~350KB for firmware, 12MB for models

## Limitations

- This example does **NOT** include audio capture functionality
- Only demonstrates model loading and inspection
- Designed for development and testing purposes

## Next Steps

For a complete audio integration example with hardware support:
1. Reference the `esp32-p4-es1811-mic-spk-vad-gc9a01` project
2. Add I2S/I2C audio codec support
3. Implement audio capture and preprocessing
4. Integrate VAD inference pipeline

## Troubleshooting

### Model not found
Ensure models are uploaded to the LittleFS partition at `/spiffs/*.frvd`

### Out of PSRAM
Models are loaded into PSRAM. Check available PSRAM with `status` command.

### Build errors
Ensure ESP-IDF v6.0 or later is installed and activated:
```bash
cd $IDF_PATH
./install.sh
. ./export.sh
```

## Audio Sample Files Attribution

The example audio files in `example_wave/` are sourced from [Pixabay](https://pixabay.com/) and licensed under the [Pixabay License](https://pixabay.com/service/license-summary/) (free for commercial and non-commercial use, no attribution required). The original files have been edited and shortened for demonstration purposes.

## License

This example is provided under the same license as the parent FireVAD project.

## Support

For issues, questions, or contributions, please refer to the main project repository.
