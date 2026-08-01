#!/usr/bin/env python3
"""
FireVAD ESP32-P4 Automated Benchmark
=====================================
Connects to the device via serial, runs every model on every test WAV,
collects vad_metrics output, and prints a performance comparison table.

Usage:
    python run_benchmark.py [--port COM4] [--baud 115200] [--out results.md]
"""

import serial
import time
import re
import argparse
import sys
from datetime import datetime

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

MODELS = [
    "firered-stream-vad-fp32.frvd",
    "firered-stream-vad-int16.frvd",
    "firered-stream-vad-int8.frvd",
    "firered-stream-vad-int8-ch.frvd",
    "firered-vad-fp32.frvd",
    "firered-vad-int16.frvd",
    "firered-vad-int8.frvd",
    "firered-vad-int8-ch.frvd",
    "firered-aed-fp32.frvd",
    "firered-aed-int16.frvd",
    "firered-aed-int8.frvd",
    "firered-aed-int8-ch.frvd"
]

WAV_FILES = [
    "music-rock.wav",
    "negative-birds.wav",
    "noise-babble-english.wav",
    "noise-babble-small-room.wav",
    "noise-cafeteria.wav",
    "noise-clock-tick.wav",
    "noise-constant-white.wav",
    "noise-electrical-buzz.wav",
    "noise-gym-ambience.wav",
    "noise-laundry-machine.wav",
    "noise-light-fan.wav",
    "noise-machine-room.wav",
    "noise-mic-bumps.wav",
    "noise-mic-static.wav",
    "noise-office-ambience.wav",
    "noise-pizzeria.wav",
    "noise-server-fans.wav",
    "singing-vocal.wav",
    "speech-mic-test.wav",
    "speech-welcome-constant-volume.wav",
    "speech-welcome-varied-volume.wav"
]

# Timeout in seconds per command
TIMEOUT_LOAD   = 20   # model loading (fp32 = ~2.2MB over SPIFFS)
TIMEOUT_INFER  = 60   # WAV inference (longest WAV ~15s of audio)
TIMEOUT_SHORT  = 5    # quick commands like vad_metrics

PROMPT = b"firevad>"

# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------

def ansi_strip(text: str) -> str:
    return re.sub(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])', '', text)


def wait_prompt(ser: serial.Serial, timeout: float) -> str:
    """Read from serial until the firevad> prompt appears or timeout."""
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
            # Linenoise redraws the prompt during typing. We only want the final prompt.
            clean = ansi_strip(buf.decode("utf-8", errors="ignore"))
            if clean.endswith("firevad> ") or clean.endswith("firevad>"):
                break
        else:
            time.sleep(0.005)
    return ansi_strip(buf.decode("utf-8", errors="ignore"))


def cmd(ser: serial.Serial, command: str, timeout: float) -> str:
    """Send a command and wait for the prompt."""
    time.sleep(0.1)
    ser.reset_input_buffer()
    for char in command + "\r\n":
        ser.write(char.encode("utf-8"))
        ser.flush()
        time.sleep(0.005)
    return wait_prompt(ser, timeout)


# ---------------------------------------------------------------------------
# Result parsers
# ---------------------------------------------------------------------------

def parse_metrics(text: str) -> dict:
    """Parse vad_metrics output into a dict."""
    result = {}
    m = re.search(r"Min:\s*([\d]+)\s*us", text)
    if m:
        result["lat_min_us"] = int(m.group(1))
    m = re.search(r"Max:\s*([\d]+)\s*us", text)
    if m:
        result["lat_max_us"] = int(m.group(1))
    m = re.search(r"Avg:\s*([\d]+)\s*us", text)
    if m:
        result["lat_avg_us"] = int(m.group(1))
    m = re.search(r"Avg:\s*([\d]+)\s*\n===", text)
    if not m:
        # fallback: second Avg line (cycles)
        avgs = re.findall(r"Avg:\s*([\d]+)", text)
        if len(avgs) >= 2:
            result["cycles_avg"] = int(avgs[1])
    else:
        result["cycles_avg"] = int(m.group(1))
    m = re.search(r"Total Inferences:\s*([\d]+)", text)
    if m:
        result["total_inferences"] = int(m.group(1))
    return result


def parse_infer(text: str) -> dict:
    """Parse vad_infer_wav output."""
    result = {}
    m = re.search(r"Speech frames:\s*([\d]+)\s*/\s*([\d]+)\s*\(([\d.]+)%\)", text)
    if m:
        result["speech_frames"]  = int(m.group(1))
        result["total_frames"]   = int(m.group(2))
        result["speech_pct"]     = float(m.group(3))
    return result


# ---------------------------------------------------------------------------
# Benchmark runner
# ---------------------------------------------------------------------------

def run_benchmark(port: str, baud: int) -> list:
    print(f"\n[BENCH] Connecting to {port} @ {baud} baud...")
    try:
        ser = serial.Serial(port, baud, timeout=0.1)
    except serial.SerialException as e:
        print(f"[ERROR] Cannot open {port}: {e}")
        print("        Make sure the IDF monitor is closed first.")
        sys.exit(1)

    print("[BENCH] Waiting for ESP32 to boot...")
    # Read until we see the first prompt (or timeout after 5s)
    wait_prompt(ser, 5.0)

    # Ping the device to ensure prompt is clean
    cmd(ser, "", TIMEOUT_SHORT)

    # Disable Pre-VAD so ALL frames go through the neural network
    print("[BENCH] Disabling Pre-VAD for consistent measurement...")
    cmd(ser, "vad_pre_vad 0", TIMEOUT_SHORT)

    results = []

    for model in MODELS:
        print(f"\n[BENCH] ===== {model} =====")

        # Load model
        load_out = cmd(ser, f"vad_model_load {model}", TIMEOUT_LOAD)
        if "ERROR" in load_out or "Failed" in load_out:
            print(f"[WARN]  Could not load {model}, skipping.")
            continue

        for wav in WAV_FILES:
            print(f"[BENCH]   Inferring on {wav}...")

            # Run inference
            infer_out = cmd(ser, f"vad_infer_wav {wav}", TIMEOUT_INFER)
            # print(infer_out) # Commented out to prevent console spam
            
            # Read metrics
            met_out = cmd(ser, "vad_metrics", TIMEOUT_SHORT)

            infer = parse_infer(infer_out)
            met   = parse_metrics(met_out)

            row = {
                "model":    model,
                "wav":      wav,
                **infer,
                **met,
            }
            results.append(row)

            # Print live summary
            lat  = met.get("lat_avg_us", 0)
            cyc  = met.get("cycles_avg", 0)
            spct = infer.get("speech_pct", -1.0)
            rt   = (lat / 10_000) * 100   # % of 10ms real-time budget
            print(f"           Avg latency: {lat:>7} us  |  Cycles: {cyc:>10,}  |"
                  f"  Speech: {spct:5.1f}%  |  RT-load: {rt:5.1f}%")

            # Reload model to reset the metrics counter
            cmd(ser, f"vad_model_load {model}", TIMEOUT_LOAD)

    ser.close()
    return results


# ---------------------------------------------------------------------------
# Table formatting
# ---------------------------------------------------------------------------

def model_short(name: str) -> str:
    name = name.replace("firered-stream-vad-", "stream-")
    name = name.replace("firered_stream-vad_", "stream-")
    name = name.replace(".frvd", "")
    return name


def wav_short(name: str) -> str:
    name = name.replace("speech-welcome-constant-volume", "speech-const")
    name = name.replace("speech-welcome-varied-volume",   "speech-varied")
    name = name.replace("negative-birds", "birds")
    name = name.replace("music-rock", "music")
    name = name.replace(".wav", "")
    return name


def build_table(results: list) -> str:
    if not results:
        return "No results collected.\n"

    lines = []
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    lines.append(f"# FireVAD Benchmark Results")
    lines.append(f"Generated: {ts}  |  Target: ESP32-P4 @ 360 MHz\n")

    # --- Per-WAV breakdown ---
    lines.append("## Per-WAV Performance\n")
    hdr = (f"{'Model':<26} | {'WAV':<14} | {'Avg µs':>8} | "
           f"{'Cycles':>10} | {'RT %':>6} | {'Speech %':>9}")
    lines.append(hdr)
    lines.append("-" * len(hdr))

    for r in results:
        lat  = r.get("lat_avg_us", 0)
        cyc  = r.get("cycles_avg", 0)
        spct = r.get("speech_pct", -1.0)
        rt   = (lat / 10_000) * 100
        lines.append(
            f"{model_short(r['model']):<26} | {wav_short(r['wav']):<14} | "
            f"{lat:>8,} | {cyc:>10,} | {rt:>5.1f}% | {spct:>8.1f}%"
        )

    # --- Model average summary ---
    lines.append("\n## Model Average (across all WAVs)\n")
    hdr2 = (f"{'Model':<26} | {'Avg µs':>8} | {'Avg Cycles':>12} | "
            f"{'vs. fp32':>9} | {'RT %':>6}")
    lines.append(hdr2)
    lines.append("-" * len(hdr2))

    from collections import defaultdict
    per_model = defaultdict(list)
    for r in results:
        if r.get("lat_avg_us") and r.get("cycles_avg"):
            per_model[r["model"]].append(r)

    fp32_cycles = None
    summary_rows = []
    for model in MODELS:
        rows = per_model.get(model, [])
        if not rows:
            continue
        avg_lat = int(sum(r["lat_avg_us"] for r in rows) / len(rows))
        avg_cyc = int(sum(r["cycles_avg"] for r in rows) / len(rows))
        rt = (avg_lat / 10_000) * 100
        if fp32_cycles is None and "fp32" in model:
            fp32_cycles = avg_cyc
        speedup = f"{fp32_cycles / avg_cyc:.2f}x" if avg_cyc and fp32_cycles else "N/A"
        summary_rows.append((model, avg_lat, avg_cyc, speedup, rt))

    for model, avg_lat, avg_cyc, speedup, rt in summary_rows:
        lines.append(
            f"{model_short(model):<26} | {avg_lat:>8,} | {avg_cyc:>12,} | "
            f"{speedup:>9} | {rt:>5.1f}%"
        )

    lines.append("\nRT% = percentage of the 10ms real-time frame budget consumed.")
    lines.append("vs. fp32 = cycle speedup relative to the fp32 baseline.")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="FireVAD ESP32-P4 Benchmark")
    parser.add_argument("--port", default="COM4",    help="Serial port (default: COM4)")
    parser.add_argument("--baud", default=115200,    type=int)
    parser.add_argument("--out",  default=None,
                        help="Output markdown file (default: auto-generated in .docs with timestamp)")
    args = parser.parse_args()

    results = run_benchmark(args.port, args.baud)

    table = build_table(results)
    print("\n\n" + "=" * 70)
    print(table)
    print("=" * 70)

    out_path = args.out
    if out_path is None:
        import os
        timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        script_dir = os.path.dirname(os.path.abspath(__file__))
        out_path = os.path.join(script_dir, "..", f"{timestamp}_benchmark_results.md")

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(table)
    print(f"\n[BENCH] Results written to: {out_path}")


if __name__ == "__main__":
    main()
