# FireVAD Console Example

This example demonstrates how to integrate and run the FireVAD component on an ESP32-P4. It provides an interactive serial console (REPL) to load models, analyze test WAV files, and measure real-time performance.

## Prerequisites
- ESP-IDF v5.2+ (v6.0+ recommended)
- ESP32-P4 Development Board
- Python 3.8+ for downloading models

## 1. Download the Models

Before compiling, you must download the pre-converted `.frvd` models from HuggingFace. A Python script is provided to automate this:

```bash
# Install HuggingFace Hub client (if not already installed)
pip install huggingface-hub

# Download ONLY the recommended INT8 per-channel models (~2MB)
python download_models.py

# OR: Download all quantizations (FP32, INT16, INT8) (~12MB)
python download_models.py --all-quantizations
```

The models will be saved to `converted_models/`. 

## 2. Build and Flash

The build system is configured to automatically package the `converted_models/` directory into a SPIFFS image and flash it alongside the firmware.

```bash
idf.py set-target esp32p4
idf.py build flash monitor
```

### Advanced: Disabling Automatic SPIFFS Flashing
When iterating on the C++ code, flashing the 16MB SPIFFS model partition every time is extremely slow and unnecessary. You can disable the automatic SPIFFS flashing by modifying `CMakeLists.txt`.

Open `CMakeLists.txt` and change the `FLASH_IN_PROJECT` parameter to `FALSE`:
```cmake
# Change this:
spiffs_create_partition_image(models converted_models FLASH_IN_PROJECT)

# To this:
spiffs_create_partition_image(models converted_models FALSE)
```
Now, `idf.py flash` will *only* flash the fast application firmware. If you ever add new models, you must temporarily set it back to `FLASH_IN_PROJECT` or flash the SPIFFS partition manually.

## 3. Using the Interactive Console

Once the firmware is running, type commands into the serial monitor.

### Available Commands

| Command | Description |
|---------|-------------|
| `model_list` | Lists all `.frvd` models found in the SPIFFS partition. |
| `model_load <path>` | Loads a model into PSRAM. Example: `model_load stream-vad/int8/firered_stream-vad_int8_ch.frvd` |
| `model_info` | Displays architecture details (dims, version) of the loaded model. |
| `play_wav <path>` | Runs VAD inference on a test WAV file. Example: `play_wav example_wave/speech-welcome-varied-volume.wav` |
| `start` | Starts the live microphone inference task (if I2S is configured). |
| `stop` | Stops the live inference task. |
| `heap` | Shows current FreeRTOS memory usage (SRAM and PSRAM). |

## 4. Test WAV Files Included

The `example_wave/` directory contains short 16kHz PCM WAV files to test the system offline:
- `speech-welcome-constant-volume.wav` (Clear speech)
- `negative-birds.wav` (Background noise - should not trigger VAD)
- `music-rock.wav` (Music - AED models can classify this)
