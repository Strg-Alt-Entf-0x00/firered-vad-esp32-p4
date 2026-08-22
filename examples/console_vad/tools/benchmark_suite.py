import serial
import time
import sys
import os
import datetime
import re

from esp32_config import get_config

config = get_config()
PORT = config.monitor_port
BAUD = config.monitor_baud

MODELS = {
    "Stream-VAD FP32": "models/FireRedVAD-ESP32-P4/stream-vad/fp32/firered-stream-vad-fp32.frvd",
    "Stream-VAD INT16": "models/FireRedVAD-ESP32-P4/stream-vad/int16/firered-stream-vad-int16.frvd",
    "Stream-VAD INT8": "models/FireRedVAD-ESP32-P4/stream-vad/int8/firered-stream-vad-int8.frvd",
    "Stream-VAD INT8-CH": "models/FireRedVAD-ESP32-P4/stream-vad/int8-ch/firered-stream-vad-int8-ch.frvd",
    "AED-VAD FP32": "models/FireRedVAD-ESP32-P4/aed/fp32/firered-aed-fp32.frvd",
    "AED-VAD INT16": "models/FireRedVAD-ESP32-P4/aed/int16/firered-aed-int16.frvd",
    "AED-VAD INT8": "models/FireRedVAD-ESP32-P4/aed/int8/firered-aed-int8.frvd",
    "AED-VAD INT8-CH": "models/FireRedVAD-ESP32-P4/aed/int8-ch/firered-aed-int8-ch.frvd",
}

# Automatically gather all diverse audio sources from the local example_wave directory,
# but assume they are stored under 'audio/vad/' on the ESP32 SD card.
WAVS = []
if os.path.exists('example_wave'):
    for f in os.listdir('example_wave'):
        if f.endswith('.wav'):
            WAVS.append(f"audio/vad/{f}")
else:
    print("[WARN] example_wave directory not found. Please run this from examples/console_vad")
    
# Fallback if empty
if not WAVS:
    WAVS = [
        "audio/vad/music-rock.wav"
    ]

def send_command(ser, cmd):
    print(f"\n[ESP32] < {cmd}")
    ser.reset_input_buffer()
    
    # BLACK HOLE BYPASS: The ESP32/USB-Serial-JTAG on this specific board revision
    # deterministically drops the first ~30 characters of the first command.
    # We pad the command with 40 spaces. If they are dropped, the command survives.
    # If they are NOT dropped, esp_console (linenoise) simply trims leading spaces!
    padded_cmd = "                                        " + cmd
    
    for c in padded_cmd:
        ser.write(c.encode('utf-8'))
        ser.flush()
        time.sleep(0.01)
        
    ser.write(b'\r\n')
    time.sleep(0.5)

def wait_for_prompt(ser, timeout=30, capture=False, expected_marker="firevad>"):
    start = time.time()
    buffer = ""
    while time.time() - start < timeout:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            try:
                sys.stdout.write(chunk)
            except UnicodeEncodeError:
                sys.stdout.write(chunk.encode(sys.stdout.encoding, errors='replace').decode(sys.stdout.encoding))
            sys.stdout.flush()
            buffer += chunk
            
            # If we are waiting for a specific marker (like the end of metrics),
            # we must check for BOTH the marker AND the prompt to be safe.
            if expected_marker in buffer and ("firevad>" in chunk or "firevad>" in buffer.split('\n')[-1]):
                # CRITICAL: Wait 1.5 seconds for linenoise DSR timeout to expire!
                time.sleep(1.5) 
                if ser.in_waiting:
                    ser.read(ser.in_waiting) # Clear out anything remaining
                return buffer if capture else True
        time.sleep(0.05)
    return buffer if capture else False

def run_benchmark():
    print(f"Connecting to ESP32 on {PORT} at {BAUD} baud...")
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.1)
    except Exception as e:
        print(f"Failed to open {PORT}. Make sure idf_monitor is CLOSED!")
        sys.exit(1)
        
    ser.reset_input_buffer()
    ser.write(b'\r')
    wait_for_prompt(ser, 3)

    results = {}

    for name, path in MODELS.items():
        print(f"\n=======================================================")
        print(f"Benchmarking Model: {name}")
        print(f"=======================================================")
        
        # Retry loop for model loading (bypasses any esp_console/linenoise/USB drops)
        loaded = False
        for attempt in range(3):
            send_command(ser, f"vad_model_load {path}")
            out = wait_for_prompt(ser, 15, capture=True)
            
            if "Unrecognized command" in out:
                print(f"  [Attempt {attempt+1}] ESP32 dropped characters. Retrying...")
                time.sleep(1)
                continue
            elif "Failed" in out or "ERROR" in out:
                print(f"[!] Warning: Could not load {name}.")
                break
            else:
                loaded = True
                break
                
        if not loaded:
            print(f"[!] Skipping {name} due to load failure.")
            continue
            
        model_latencies = []
        model_cycles = []
        
        # Run against WAVs with retry loop
        for wav in WAVS:
            for attempt in range(3):
                send_command(ser, f"vad_infer_wav {wav}")
                
                # We MUST wait for the '=== Analysis Results ===' block to ensure WAV inference is done.
                out = wait_for_prompt(ser, 90, capture=True, expected_marker="=== Analysis Results ===")
                
                # If we watchdog rebooted, the ESP32 prints a boot screen and a prompt, but NO results!
                if "task_wdt" in out or "Guru Meditation Error" in out:
                    print(f"\n[!] WARNING: Watchdog triggered or crash during {wav}! This model is too heavy.")
                    time.sleep(3)
                    break
                
                if "Unrecognized command" in out:
                    print(f"  [Attempt {attempt+1}] ESP32 dropped characters for WAV. Retrying...")
                    time.sleep(1)
                    continue
                    
                # Inference completed successfully, now fetch the metrics!
                send_command(ser, "vad_metrics")
                out_metrics = wait_for_prompt(ser, 10, capture=True, expected_marker="Performance Metrics")
                out += out_metrics
                
                break # Success or normal error
            
            match_time = re.search(r'Avg Time\s*:\s*(\d+)\s*us', out)
            match_cycles = re.search(r'Avg Cycles\s*:\s*(\d+)', out)
            
            if match_time and match_cycles:
                model_latencies.append(int(match_time.group(1)))
                model_cycles.append(int(match_cycles.group(1)))
                
        if len(model_latencies) > 0:
            avg_lat = sum(model_latencies) / len(model_latencies)
            avg_cyc = sum(model_cycles) / len(model_cycles)
            rt_load = (avg_lat / 10000.0) * 100.0
            
            results[name] = {
                'latency': avg_lat,
                'cycles': avg_cyc,
                'rt_load': rt_load
            }
            
            print(f"\n[+] {name} Results: {avg_lat:.0f} us | {avg_cyc:.0f} cycles | {rt_load:.1f}% RT Load")
        else:
            print(f"[-] No valid metrics found for {name}.")
            
    ser.close()
    
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    report_file = f"{timestamp}_benchmark_results.md"
    
    with open(report_file, "w", encoding="utf-8") as f:
        f.write(f"# ESP32-P4 FireRedVAD Benchmark Results\n\n")
        f.write(f"**Date:** {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"**Hardware:** ESP32-P4 (Dual-Core RISC-V @ 360MHz, 32MB PSRAM)\n\n")
        
        f.write("## Automated Inference Metrics (10ms frame budget)\n\n")
        f.write("| Model | Avg Latency | CPU Cycles | RT Load | Verdict |\n")
        f.write("|---|---|---|---|---|\n")
        
        for name, metrics in results.items():
            lat = metrics['latency']
            cyc = metrics['cycles']
            rt = metrics['rt_load']
            
            if rt <= 100.0:
                verdict = "**Yes - Recommended**" if "INT8-CH" in name else "Yes"
                row = f"| **`{name}`** | **{lat:,.0f} us** | {cyc:,.0f} | **{rt:.1f}%** | {verdict} |\n"
            else:
                verdict = f"No - {rt/100.0:.1f}x over budget"
                row = f"| `{name}` | {lat:,.0f} us | {cyc:,.0f} | {rt:.1f}% | {verdict} |\n"
                
            f.write(row)
            
    print(f"\n[SUCCESS] Benchmark complete! Report saved to {report_file}")

if __name__ == "__main__":
    run_benchmark()
