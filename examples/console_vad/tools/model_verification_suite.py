#!/usr/bin/env python3
"""
FireRedVAD ESP32-P4 - Model Verification Suite
===============================================
Full accuracy verification of all 12 FRVD models against
real-world SD-card WAV recordings.

Tests speech detection rate (on speech files) and false positive rate
(on silence files) for every model. Produces a Markdown accuracy report.

Usage (from examples/console_vad/):
    python tools/model_verification_suite.py

Requires: pyserial
"""

import serial
import time
import sys
import os
import datetime
import re
from typing import Optional

import argparse

parser = argparse.ArgumentParser(description="FireRedVAD Model Verification Suite")
parser.add_argument("--port", default="COM4", help="Console Serial Port")
parser.add_argument("--baud", type=int, default=115200, help="Console Baud Rate")
args = parser.parse_args()

PORT = args.port
BAUD = args.baud
# Verdict thresholds
SPEECH_PASS_THRESHOLD = 50.0    # >= 50% speech detection at 0.1m-1m = PASS
SPEECH_WARN_THRESHOLD = 20.0    # >= 20% = WARN, below = FAIL
FPR_PASS_THRESHOLD   = 15.0    # FPR <= 15% on silence = PASS
FPR_WARN_THRESHOLD   = 40.0    # FPR <= 40% = WARN, above = FAIL

# WAV inference timeout. 30s of audio at 10ms/frame = 3000 iterations.
# Each iteration yields every 10 frames (100ms), so worst-case ~30s compute + 20s margin.
WAV_TIMEOUT_SECONDS = 90

# ---------------------------------------------------------------------------
# Model Matrix (12 models - all 3 types x 4 quantizations)
# Paths are relative to the SD card mount point (/sd/), as the firmware
# prepends FS_MOUNT_POINT automatically.
# ---------------------------------------------------------------------------

MODELS = {
    # --- VAD (Offline) ---
    "VAD-FP32":     "models/firered-vad-fp32.frvd",
    "VAD-INT16":    "models/firered-vad-int16.frvd",
    "VAD-INT8":     "models/firered-vad-int8.frvd",
    "VAD-INT8-CH":  "models/firered-vad-int8-ch.frvd",
    # --- Stream-VAD ---
    "SVAD-FP32":    "models/firered-stream-vad-fp32.frvd",
    "SVAD-INT16":   "models/firered-stream-vad-int16.frvd",
    "SVAD-INT8":    "models/firered-stream-vad-int8.frvd",
    "SVAD-INT8-CH": "models/firered-stream-vad-int8-ch.frvd",
    # --- AED (Audio Event Detection) ---
    "AED-FP32":     "models/firered-aed-fp32.frvd",
    "AED-INT16":    "models/firered-aed-int16.frvd",
    "AED-INT8":     "models/firered-aed-int8.frvd",
    "AED-INT8-CH":  "models/firered-aed-int8-ch.frvd",
}

# ---------------------------------------------------------------------------
# WAV file groups - 36 files total, all at SD root (/sd/)
# Each group has a silence file (FPR test) and 5 distance files (speech test).
# The 'label' is used in the report for readability.
# ---------------------------------------------------------------------------

DISTANCES = ["silence", "3m", "2m", "1m", "0.5m", "0.1m"]

WAV_GROUPS = [
    {
        "label": "INMP441 (Baseline)",
        "prefix": "inmp441-h60cm",
        "mic": "INMP441",
        "gain": "Default",
    },
    {
        "label": "INMP441 SW-Gain 10x",
        "prefix": "inmp441-h60cm-sw10x",
        "mic": "INMP441",
        "gain": "SW 10x",
    },
    {
        "label": "INMP441 AGC (sw1.0x)",
        "prefix": "inmp441-h60cm-sw1.0x-agc1",
        "mic": "INMP441",
        "gain": "AGC",
    },
    {
        "label": "ES8311 (Baseline)",
        "prefix": "es8311-h60cm",
        "mic": "ES8311",
        "gain": "Default",
    },
    {
        "label": "ES8311 HW-PGA 42dB",
        "prefix": "es8311-h60cm-42db",
        "mic": "ES8311",
        "gain": "HW 42dB",
    },
    {
        "label": "ES8311 HW-PGA 42dB + AGC",
        "prefix": "es8311-h60cm-42db-agc1",
        "mic": "ES8311",
        "gain": "HW 42dB + AGC",
    },
]


def build_wav_list():
    """Build the full flat WAV list with metadata."""
    wavs = []
    for group in WAV_GROUPS:
        for dist in DISTANCES:
            filename = f"{group['prefix']}-{dist}.wav"
            is_silence = (dist == "silence")
            wavs.append({
                "filename": filename,
                "group_label": group["label"],
                "mic": group["mic"],
                "gain": group["gain"],
                "distance": dist,
                "is_silence": is_silence,
            })
    return wavs

ALL_WAVS = build_wav_list()

# ---------------------------------------------------------------------------
# Serial Communication
# ---------------------------------------------------------------------------

def send_command(ser: serial.Serial, cmd: str) -> None:
    """Send a command to the ESP32 console with the character-drop bypass.

    The USB-JTAG on this board revision drops the first ~30 chars of a
    command. We prepend 40 spaces; linenoise trims them if they arrive,
    so the actual command survives either way.
    """
    print(f"\n[CMD] > {cmd}")
    ser.reset_input_buffer()

    padded_cmd = " " * 40 + cmd
    for c in padded_cmd:
        ser.write(c.encode("utf-8"))
        ser.flush()
        time.sleep(0.01)

    ser.write(b"\r\n")
    time.sleep(0.5)


def wait_for_prompt(
    ser: serial.Serial,
    timeout: float = 30.0,
    capture: bool = False,
    expected_marker: Optional[str] = None,
) -> object:
    """Read serial output until the ESP32 prompt ('firevad>') is seen.

    Args:
        ser: Open serial port.
        timeout: Max seconds to wait.
        capture: If True, return the full accumulated buffer. Else return bool.
        expected_marker: If set, wait until BOTH this string AND the prompt are
                         seen in the buffer before returning.

    Returns:
        str buffer if capture=True, else True/False.
    """
    start = time.time()
    buffer = ""
    marker_seen = (expected_marker is None)  # True immediately if no marker needed

    while time.time() - start < timeout:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting).decode("utf-8", errors="ignore")
            try:
                sys.stdout.write(chunk)
            except UnicodeEncodeError:
                sys.stdout.write(
                    chunk.encode(sys.stdout.encoding, errors="replace").decode(sys.stdout.encoding)
                )
            sys.stdout.flush()
            buffer += chunk

            if not marker_seen and expected_marker and expected_marker in buffer:
                marker_seen = True

            if marker_seen and "firevad>" in buffer:
                # CRITICAL: Wait for linenoise DSR timeout before reading again
                time.sleep(1.5)
                if ser.in_waiting:
                    trailing = ser.read(ser.in_waiting).decode("utf-8", errors="ignore")
                    buffer += trailing
                return buffer if capture else True

        time.sleep(0.05)

    return buffer if capture else False


def load_model(ser: serial.Serial, name: str, path: str) -> bool:
    """Attempt to load a model with up to 3 retries. Returns True on success."""
    for attempt in range(1, 4):
        send_command(ser, f"vad_model_load {path}")
        out = wait_for_prompt(ser, timeout=15.0, capture=True)

        if "Unrecognized command" in out:
            print(f"  [Attempt {attempt}] Character drop detected. Retrying...")
            time.sleep(1.0)
            continue
        elif "Failed" in out or "ERROR" in out:
            print(f"  [ERROR] Model load failed for {name}: {path}")
            return False
        else:
            print(f"  [OK] Model loaded: {name}")
            return True

    print(f"  [FAIL] Could not load {name} after 3 attempts.")
    return False


def infer_wav(ser: serial.Serial, filename: str) -> Optional[float]:
    """Run vad_infer_wav on a single WAV file. Returns speech % or None on error."""
    for attempt in range(1, 4):
        send_command(ser, f"vad_infer_wav {filename}")
        out = wait_for_prompt(
            ser,
            timeout=WAV_TIMEOUT_SECONDS,
            capture=True,
            expected_marker="=== Analysis Results ===",
        )

        # Crash or watchdog reset detection
        if "task_wdt" in out or "Guru Meditation Error" in out or "rst:0x" in out:
            print(f"\n  [CRASH] Watchdog or crash during {filename}! Skipping.")
            time.sleep(3.0)
            return None

        if "Unrecognized command" in out:
            print(f"  [Attempt {attempt}] Character drop for WAV. Retrying...")
            time.sleep(1.0)
            continue

        if "[ERROR] Failed to open" in out:
            print(f"  [ERROR] File not found on SD: {filename}")
            return None

        # Parse: "Speech frames: X / Y (Z.Z%)"
        match = re.search(
            r"Speech frames:\s+(\d+)\s*/\s*(\d+)\s*\((\d+\.\d+)%\)",
            out,
        )
        if match:
            speech_pct = float(match.group(3))
            speech_frames = int(match.group(1))
            total_frames = int(match.group(2))
            print(f"  [OK] {filename}: {speech_frames}/{total_frames} frames = {speech_pct:.1f}%")
            return speech_pct
        else:
            print(f"  [WARN] No results parsed from output for {filename}")
            return None

    print(f"  [FAIL] Could not infer {filename} after 3 attempts.")
    return None


# ---------------------------------------------------------------------------
# Verdict Logic
# ---------------------------------------------------------------------------

def compute_verdict(avg_speech_near: Optional[float], avg_fpr: Optional[float]) -> str:
    """Determine PASS/WARN/FAIL based on near-field speech % and silence FPR.

    Args:
        avg_speech_near: Average speech detection % for 0.1m + 0.5m + 1m files.
        avg_fpr: Average false positive rate on silence files.

    Returns:
        'PASS', 'WARN', or 'FAIL'.
    """
    if avg_speech_near is None or avg_fpr is None:
        return "FAIL"

    speech_ok = avg_speech_near >= SPEECH_PASS_THRESHOLD
    speech_warn = avg_speech_near >= SPEECH_WARN_THRESHOLD
    fpr_ok = avg_fpr <= FPR_PASS_THRESHOLD
    fpr_warn = avg_fpr <= FPR_WARN_THRESHOLD

    if speech_ok and fpr_ok:
        return "PASS"
    elif speech_warn and fpr_warn:
        return "WARN"
    else:
        return "FAIL"


# ---------------------------------------------------------------------------
# Report Generation
# ---------------------------------------------------------------------------

def verdict_badge(verdict: str) -> str:
    """Return a Markdown-friendly badge string for the verdict."""
    badges = {
        "PASS": "**PASS**",
        "WARN": "_WARN_",
        "FAIL": "~~FAIL~~",
    }
    return badges.get(verdict, verdict)


def format_pct(value: Optional[float]) -> str:
    """Format a percentage value or return 'N/A' if None."""
    if value is None:
        return "N/A"
    return f"{value:.1f}%"


def generate_report(
    all_results: dict,
    timestamp: str,
    elapsed_seconds: float,
) -> str:
    """Generate the full Markdown verification report.

    Args:
        all_results: Dict keyed by model_name -> Dict keyed by wav_filename -> speech_pct (float|None)
        timestamp: ISO-format timestamp string for the report header.
        elapsed_seconds: Total test duration.

    Returns:
        Markdown report as a string.
    """
    lines = []

    lines.append(f"# FireRedVAD Model Verification Report")
    lines.append(f"")
    lines.append(f"**Date:** {timestamp}")
    lines.append(f"**Hardware:** ESP32-P4 rev1.3 (Dual-Core RISC-V @ 360MHz, 32MB PSRAM)")
    lines.append(f"**Port:** {PORT} @ {BAUD} baud")
    lines.append(f"**Test Duration:** {elapsed_seconds/60.0:.1f} minutes")
    lines.append(f"**Models Tested:** {len(all_results)}")
    lines.append(f"**WAV Files per Model:** {len(ALL_WAVS)}")
    lines.append(f"")
    lines.append(f"---")
    lines.append(f"")

    # --- Summary Table ---
    lines.append(f"## Summary - All Models")
    lines.append(f"")
    lines.append(f"Verdict criteria:")
    lines.append(f"- **PASS**: Speech detection >= {SPEECH_PASS_THRESHOLD:.0f}% at 0.1m-1m AND FPR <= {FPR_PASS_THRESHOLD:.0f}% on silence")
    lines.append(f"- **WARN**: Speech detection >= {SPEECH_WARN_THRESHOLD:.0f}% at 0.1m-1m AND FPR <= {FPR_WARN_THRESHOLD:.0f}% on silence")
    lines.append(f"- **FAIL**: Below WARN thresholds")
    lines.append(f"")
    lines.append(f"| Model | Avg FPR (silence) | Avg Speech @ 0.1m | Avg Speech @ 0.5m | Avg Speech @ 1m | Near-Field Avg | Verdict |")
    lines.append(f"|---|---|---|---|---|---|---|")

    near_distances = {"0.1m", "0.5m", "1m"}

    for model_name, wav_results in all_results.items():
        # Collect silence FPR across all 6 groups
        silence_pcts = [
            wav_results.get(w["filename"])
            for w in ALL_WAVS
            if w["is_silence"] and wav_results.get(w["filename"]) is not None
        ]
        avg_fpr = (sum(silence_pcts) / len(silence_pcts)) if silence_pcts else None

        # Collect near-field speech per distance
        near_by_dist = {}
        for dist in near_distances:
            vals = [
                wav_results.get(w["filename"])
                for w in ALL_WAVS
                if w["distance"] == dist and not w["is_silence"]
                and wav_results.get(w["filename"]) is not None
            ]
            near_by_dist[dist] = (sum(vals) / len(vals)) if vals else None

        all_near = [v for v in near_by_dist.values() if v is not None]
        avg_near = (sum(all_near) / len(all_near)) if all_near else None

        verdict = compute_verdict(avg_near, avg_fpr)

        lines.append(
            f"| `{model_name}` "
            f"| {format_pct(avg_fpr)} "
            f"| {format_pct(near_by_dist.get('0.1m'))} "
            f"| {format_pct(near_by_dist.get('0.5m'))} "
            f"| {format_pct(near_by_dist.get('1m'))} "
            f"| {format_pct(avg_near)} "
            f"| {verdict_badge(verdict)} |"
        )

    lines.append(f"")
    lines.append(f"---")
    lines.append(f"")

    # --- Per-Group Breakdown ---
    lines.append(f"## Detailed Results - Per Microphone Group")
    lines.append(f"")
    lines.append(f"Each table shows speech detection % per model and distance.")
    lines.append(f"Silence column = False Positive Rate (lower is better).")
    lines.append(f"")

    for group in WAV_GROUPS:
        group_prefix = group["prefix"]
        group_wavs = [w for w in ALL_WAVS if w["group_label"] == group["label"]]

        lines.append(f"### {group['label']}")
        lines.append(f"Mic: `{group['mic']}` | Gain: `{group['gain']}`")
        lines.append(f"")

        # Header: Model | silence | 3m | 2m | 1m | 0.5m | 0.1m
        header_cols = " | ".join(["Model"] + DISTANCES)
        sep_cols    = " | ".join(["---"] * (len(DISTANCES) + 1))
        lines.append(f"| {header_cols} |")
        lines.append(f"| {sep_cols} |")

        for model_name, wav_results in all_results.items():
            row_cells = [f"`{model_name}`"]
            for dist in DISTANCES:
                target_wav = next(
                    (w for w in group_wavs if w["distance"] == dist), None
                )
                if target_wav:
                    val = wav_results.get(target_wav["filename"])
                    cell = format_pct(val)
                    # Annotate silence column with [FPR] for clarity
                    if dist == "silence" and val is not None:
                        cell = f"**{cell}**" if val > FPR_WARN_THRESHOLD else cell
                else:
                    cell = "N/A"
                row_cells.append(cell)
            lines.append(f"| {' | '.join(row_cells)} |")

        lines.append(f"")

    lines.append(f"---")
    lines.append(f"")

    # --- Distance Sensitivity Analysis ---
    lines.append(f"## Distance Sensitivity Analysis")
    lines.append(f"")
    lines.append(f"Average speech detection % across all 6 mic/gain groups, per distance.")
    lines.append(f"")
    lines.append(f"| Model | 3m | 2m | 1m | 0.5m | 0.1m |")
    lines.append(f"|---|---|---|---|---|---|")

    for model_name, wav_results in all_results.items():
        row_cells = [f"`{model_name}`"]
        for dist in ["3m", "2m", "1m", "0.5m", "0.1m"]:
            vals = [
                wav_results.get(w["filename"])
                for w in ALL_WAVS
                if w["distance"] == dist and wav_results.get(w["filename"]) is not None
            ]
            avg = (sum(vals) / len(vals)) if vals else None
            row_cells.append(format_pct(avg))
        lines.append(f"| {' | '.join(row_cells)} |")

    lines.append(f"")
    lines.append(f"---")
    lines.append(f"")

    # --- Raw Data ---
    lines.append(f"## Raw Data - Full Per-File Results")
    lines.append(f"")
    lines.append(f"Complete per-model, per-file speech detection percentages.")
    lines.append(f"")

    for model_name, wav_results in all_results.items():
        lines.append(f"### {model_name}")
        lines.append(f"")
        lines.append(f"| File | Group | Dist | Type | Speech % |")
        lines.append(f"|---|---|---|---|---|")
        for wav in ALL_WAVS:
            val = wav_results.get(wav["filename"])
            wav_type = "silence (FPR)" if wav["is_silence"] else "speech"
            lines.append(
                f"| `{wav['filename']}` "
                f"| {wav['group_label']} "
                f"| {wav['distance']} "
                f"| {wav_type} "
                f"| {format_pct(val)} |"
            )
        lines.append(f"")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main Test Runner
# ---------------------------------------------------------------------------

def run_verification() -> None:
    """Main entry point: connect to ESP32, run all models against all WAVs,
    generate and save the Markdown report."""

    print("=" * 70)
    print("FireRedVAD Model Verification Suite - Session 21")
    print("=" * 70)
    print(f"Port  : {PORT} @ {BAUD} baud")
    print(f"Models: {len(MODELS)}")
    print(f"WAVs  : {len(ALL_WAVS)}")
    print(f"Total : {len(MODELS) * len(ALL_WAVS)} inference runs")
    print("=" * 70)

    print(f"\nConnecting to ESP32 on {PORT}...")
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.1)
    except serial.SerialException as e:
        print(f"\n[FATAL] Cannot open {PORT}: {e}")
        print("Make sure idf_monitor is CLOSED before running this script.")
        sys.exit(1)

    ser.reset_input_buffer()
    # Wake up the console with a newline
    ser.write(b"\r")
    wait_for_prompt(ser, timeout=3.0)

    # Store all results: { model_name: { filename: speech_pct } }
    all_results: dict = {}

    test_start = time.time()
    total_runs = len(MODELS) * len(ALL_WAVS)
    run_count = 0

    for model_idx, (model_name, model_path) in enumerate(MODELS.items(), start=1):
        print(f"\n{'=' * 70}")
        print(f"[{model_idx}/{len(MODELS)}] Model: {model_name}")
        print(f"  Path: {model_path}")
        print(f"{'=' * 70}")

        wav_results: dict = {}

        if not load_model(ser, model_name, model_path):
            # Record all files as None (failed to load)
            for wav in ALL_WAVS:
                wav_results[wav["filename"]] = None
            all_results[model_name] = wav_results
            run_count += len(ALL_WAVS)
            continue

        for wav_idx, wav in enumerate(ALL_WAVS, start=1):
            run_count += 1
            progress_pct = run_count / total_runs * 100.0
            elapsed = time.time() - test_start
            eta_seconds = (elapsed / run_count) * (total_runs - run_count) if run_count > 0 else 0

            print(
                f"\n[{run_count}/{total_runs} | {progress_pct:.0f}% | "
                f"ETA {eta_seconds/60:.1f}min] "
                f"WAV [{wav_idx}/{len(ALL_WAVS)}]: {wav['filename']}"
            )

            speech_pct = infer_wav(ser, wav["filename"])
            wav_results[wav["filename"]] = speech_pct

        all_results[model_name] = wav_results
        print(f"\n[+] {model_name} complete.")

    ser.close()
    elapsed_total = time.time() - test_start

    # --- Generate and save report ---
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    report_filename = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S") + "_verification_report.md"

    # Save alongside the benchmark script in tools/
    report_path = os.path.join(os.path.dirname(__file__), report_filename)

    report_content = generate_report(all_results, timestamp, elapsed_total)

    with open(report_path, "w", encoding="utf-8") as f:
        f.write(report_content)

    print(f"\n{'=' * 70}")
    print(f"[SUCCESS] Verification complete in {elapsed_total/60.0:.1f} minutes.")
    print(f"[REPORT]  {report_path}")
    print(f"{'=' * 70}")


if __name__ == "__main__":
    run_verification()
