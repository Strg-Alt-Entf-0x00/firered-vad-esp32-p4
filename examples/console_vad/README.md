# FireVAD Console Example (ESP32-P4)

This is the primary interactive testing environment for the FireRedVAD pipeline on the **Waveshare ESP32-P4-WIFI6** (Chip Rev v1.3).

This console (REPL) allows you to interactively load AI models, stream audio from the digital `INMP441` or analog `ES8311` microphones, record test `.wav` files, and tune the Automatic Gain Control (AGC) and Pre-VAD thresholds in real-time.

## ⚙️ Prerequisites
- ESP-IDF **v6.0+** (Required for proper ESP32-P4 multi-core routing)
- Waveshare ESP32-P4-WIFI6 Development Board
- SD Card formatted as FAT32
- Python 3.8+ (for pushing models via the custom UART protocol)
- Optional but recommended: **INMP441** digital I2S microphone (Pins: `BCLK=20`, `WS=21`, `DIN=22`).

## 1️⃣ Download and Transfer the AI Models

Instead of slowly flashing models via USB into a SPIFFS partition, this project uses a high-speed custom **UART File Protocol** to transfer models directly to your ESP32's SD card!

1. Download the `.frvd` models from HuggingFace to your PC:
```bash
pip install huggingface-hub pyserial
cd tools
python download_frvd_models.py
```
2. Build and flash the firmware (see step 2) and leave the ESP32 running.
3. Run the esp-file-bridge tool to push the models over Serial to the SD Card:
```bash
pip install -e D:\github-repositorys\esp-uart-filebridge\python
esp-file-bridge upload_dir ../models_frvd /sd/models/ --port COM4
```

## 2️⃣ Build and Flash

```bash
idf.py set-target esp32p4
idf.py build flash monitor
```

> [!IMPORTANT]
> **Boot Calibration:** When the ESP32 boots, it performs a 1-second background noise calibration to configure the internal noise-gates. **You must remain completely silent when the message `PLEASE BE SILENT - CALIBRATING MICROPHONE...` appears on the screen.** After the calibration completes, the system will play a "System Ready" chime.

## 3️⃣ Using the Interactive Console

Once booted, you have access to a powerful CLI. Type `help` to see all commands.

### Quick Start: Live Microphone VAD

1. **Select the digital microphone:**
   ```bash
   mic_select inmp441
   ```
2. **Enable the Automatic Gain Control (AGC):**
   ```bash
   agc_enable 1
   ```
3. **Load the streaming (causal) INT8 model:**
   ```bash
   vad_model_load models/firered-stream-vad-int8.frvd
   ```
4. **Start the live VAD test for 30 seconds:**
   ```bash
   vad_infer_mic 30
   ```

### Full Command Reference

| Command | Description |
|---------|-------------|
| `mic_select <inmp441/es8311>` | Switches the active microphone source. |
| `mic_level` | Measures peak and RMS of the room for 1 second. |
| `agc_enable <0/1>` | Enables/Disables dynamic software gain (AGC). |
| `agc_info` | Shows current AGC RMS targets and gains. |
| `ls [path]` | Lists files and directories on the SD Card. |
| `vad_model_load <filename.frvd>` | Loads an INT8 or FP32 model from `/sd/<filename.frvd>` into PSRAM. |
| `vad_cascade_load <stream> <aed>` | Loads the Dual-Core Gatekeeper Cascade (Stream VAD + Offline AED). |
| `vad_infer_mic [sec]` | Starts live microphone inference for the specified duration. |
| `vad_infer_wav <path>` | Runs offline VAD inference on a test WAV file. |
| `vad_metrics` | Displays precise CPU cycle and microsecond latency timings. |
| `fs_ls [path]` | Lists files and directories on the SD card (`/sd/`). |
| `record_mic <file> <sec>` | Records live audio to a `.wav` file on the SD card for debugging. |
| `play_wav <file>` | Plays a `.wav` file through the ES8311 speaker output. |
