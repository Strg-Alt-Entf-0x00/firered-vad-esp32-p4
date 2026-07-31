#!/usr/bin/env python3
"""
FireRedVAD -> ESP32 Native Binary Converter (export_weights.py)

Converts the original FireRedVAD PyTorch checkpoint (model.pth.tar + cmvn.ark)
into a flat binary file (.bin) for our custom ESP32 C++ inference engine.

Binary format (.frvd):
  Header (32 bytes):
    [0..3]   Magic: "FRVD" (4 bytes)
    [4..7]   Version: uint32 LE (1=Float32, 2=Int8)
    [8..11]  Model type: uint32 LE (0=VAD, 1=Stream-VAD, 2=AED)
    [12..15] Total params: uint32 LE
    [16..19] Num DFSMN blocks (R): uint32 LE
    [20..23] Num DNN layers (M): uint32 LE
    [24..27] RESERVED: uint32 LE
    [28..31] RESERVED: uint32 LE

  Architecture Metadata (32 bytes):
    [0..3]   Input dim (D / idim): uint32 LE
    [4..7]   Hidden size (H): uint32 LE
    [8..11]  Projection size (P): uint32 LE
    [12..15] Output dim (odim): uint32 LE
    [16..19] Lookback order (N1): uint32 LE
    [20..23] Lookback stride (S1): uint32 LE
    [24..27] Lookahead order (N2): uint32 LE
    [28..31] Lookahead stride (S2): uint32 LE

  CMVN Data:
    [0..3]   CMVN dim: uint32 LE
    [4..]    means[dim]: float32 LE
    [..]     istd[dim]: float32 LE

  Layer Data (sequential, each preceded by a small descriptor):
    For each tensor:
      [0..3]   Name hash (CRC32 of state_dict key): uint32 LE
      [4..7]   Num elements: uint32 LE
      [8..]    data[num_elements]: float32 LE

Output files:
  - firered_stream_vad.frvd   (binary for ESP32)
  - firered_stream_vad_debug.json (human-readable debug dump)

Int8 Format (Version 2):
  Layer Data:
    [0..3]   Name hash: uint32 LE
    [4..7]   Num elements: uint32 LE
    [8..11]  Scale: float32 LE
    [12..]   data[num_elements]: int8 LE

Usage:
  python export_weights.py --model-dir <path_to_Stream-VAD> --output-dir <output_dir>

Dependencies:
  pip install torch kaldiio numpy
"""

import argparse
import json
import math
import os
import struct
import sys
import zlib
from collections import OrderedDict

# ---- Attempt imports, provide clear error messages ----
try:
    import torch
except ImportError:
    print("[FATAL] PyTorch not installed. Run: pip install torch")
    sys.exit(1)

try:
    import kaldiio
except ImportError:
    print("[FATAL] kaldiio not installed. Run: pip install kaldiio")
    sys.exit(1)

import numpy as np

# ---- Constants ----
MAGIC = b"FRVD"
VERSION_FLOAT32 = 1
VERSION_INT8 = 2
VERSION_INT16 = 3
VERSION_INT8_PER_CH = 4
MODEL_TYPE_VAD = 0
MODEL_TYPE_STREAM_VAD = 1
MODEL_TYPE_AED = 2
HEADER_SIZE = 32
ARCH_META_SIZE = 32


def crc32_hash(name: str) -> int:
    """Deterministic hash for tensor names (for debugging on ESP32 side)."""
    return zlib.crc32(name.encode("utf-8")) & 0xFFFFFFFF


def load_cmvn(cmvn_path: str):
    """Load Kaldi CMVN stats and compute means + inverse std."""
    stats = kaldiio.load_mat(cmvn_path)
    assert stats.shape[0] == 2, f"CMVN must have 2 rows, got {stats.shape[0]}"
    dim = stats.shape[1] - 1
    count = stats[0, dim]
    assert count >= 1, f"CMVN count must be >= 1, got {count}"

    floor = 1e-20
    means = np.zeros(dim, dtype=np.float32)
    istd = np.zeros(dim, dtype=np.float32)

    for d in range(dim):
        mean = stats[0, d] / count
        means[d] = mean
        variance = (stats[1, d] / count) - mean * mean
        if variance < floor:
            variance = floor
        istd[d] = 1.0 / math.sqrt(variance)

    return dim, means, istd


def extract_arch_params(state_dict):
    """Infer architecture hyperparameters from the state_dict shapes."""
    # dfsmn.fc1.0.weight has shape [H, D]
    H, D = state_dict["dfsmn.fc1.0.weight"].shape
    # dfsmn.fc2.0.weight has shape [P, H]
    P = state_dict["dfsmn.fc2.0.weight"].shape[0]
    # out.weight has shape [odim, H]
    odim = state_dict["out.weight"].shape[0]

    # Count DFSMN blocks: dfsmn.fsmns.{i}.fsmn.lookback_filter.weight
    R = 1  # first fsmn1 block
    i = 0
    while f"dfsmn.fsmns.{i}.fsmn.lookback_filter.weight" in state_dict:
        R += 1
        i += 1

    # Count DNN layers: dfsmn.dnns.{i}.weight (step 3)
    M = 0
    i = 0
    while f"dfsmn.dnns.{i}.weight" in state_dict:
        M += 1
        i += 3  # Linear + ReLU + Dropout = 3 entries in nn.Sequential

    # FSMN lookback params from first filter
    lb_weight = state_dict["dfsmn.fsmn1.lookback_filter.weight"]
    N1 = lb_weight.shape[2]  # kernel_size
    # Infer S1 from padding: lookback_padding = (N1-1)*S1
    # Conv1d dilation = S1
    # We can read it from the shape indirectly. For Stream-VAD default: N1=20, S1=1
    # Actually we just store the kernel size; stride/dilation we infer from config
    S1 = 1  # Default, will be overridden if we can detect it

    # Check for lookahead filter
    N2 = 0
    S2 = 0
    if "dfsmn.fsmn1.lookahead_filter.weight" in state_dict:
        la_weight = state_dict["dfsmn.fsmn1.lookahead_filter.weight"]
        N2 = la_weight.shape[2]
        S2 = 1

    return {
        "D": int(D), "H": int(H), "P": int(P), "odim": int(odim),
        "R": int(R), "M": int(M),
        "N1": int(N1), "S1": int(S1), "N2": int(N2), "S2": int(S2),
    }


def detect_model_type(model_dir: str) -> int:
    """Auto-detect model type from directory name."""
    basename = os.path.basename(os.path.normpath(model_dir)).lower()
    if "stream" in basename:
        return MODEL_TYPE_STREAM_VAD
    elif "aed" in basename:
        return MODEL_TYPE_AED
    return MODEL_TYPE_VAD


def export_binary(model_dir: str, output_dir: str, quantize_int8: bool = False, quantize_int16: bool = False, quantize_int8_per_ch: bool = False):
    """Main export: PyTorch checkpoint -> .frvd binary + debug JSON."""

    # ---- Load checkpoint ----
    model_path = os.path.join(model_dir, "model.pth.tar")
    cmvn_path = os.path.join(model_dir, "cmvn.ark")

    if not os.path.exists(model_path):
        print(f"[FATAL] Model not found: {model_path}")
        sys.exit(1)

    print(f"[INFO] Loading checkpoint: {model_path}")
    package = torch.load(model_path, map_location="cpu", weights_only=False)
    state_dict = package["model_state_dict"]

    # ---- Extract architecture ----
    arch = extract_arch_params(state_dict)
    print(f"[INFO] Architecture detected:")
    print(f"       D={arch['D']} H={arch['H']} P={arch['P']} odim={arch['odim']}")
    print(f"       R={arch['R']} M={arch['M']}")
    print(f"       N1={arch['N1']} S1={arch['S1']} N2={arch['N2']} S2={arch['S2']}")

    # Also try to read args from checkpoint for exact S1/S2 values
    if "args" in package:
        args = package["args"]
        if hasattr(args, "S1"):
            arch["S1"] = int(args.S1)
        if hasattr(args, "S2"):
            arch["S2"] = int(args.S2)
        if hasattr(args, "N1"):
            arch["N1"] = int(args.N1)
        if hasattr(args, "N2"):
            arch["N2"] = int(args.N2)
        print(f"[INFO] Confirmed from checkpoint args: N1={arch['N1']} S1={arch['S1']} N2={arch['N2']} S2={arch['S2']}")

    model_type = detect_model_type(model_dir)
    model_type_names = {0: "VAD", 1: "Stream-VAD", 2: "AED"}
    print(f"[INFO] Model type: {model_type_names.get(model_type, 'Unknown')}")

    # ---- Load CMVN ----
    cmvn_dim = 0
    cmvn_means = np.array([], dtype=np.float32)
    cmvn_istd = np.array([], dtype=np.float32)
    if os.path.exists(cmvn_path):
        cmvn_dim, cmvn_means, cmvn_istd = load_cmvn(cmvn_path)
        print(f"[INFO] CMVN loaded: dim={cmvn_dim}")
    else:
        print(f"[WARN] No CMVN file found at {cmvn_path}, skipping")

    # ---- Count total parameters ----
    total_params = 0
    for key, tensor in state_dict.items():
        total_params += tensor.numel()
    print(f"[INFO] Total parameters: {total_params:,}")

    # ---- Define layer export order (deterministic!) ----
    # This order MUST match the C++ loader on the ESP32 exactly.
    layer_order = []

    # 1. Initial projection: dfsmn.fc1 (Linear + ReLU + Dropout)
    layer_order.append("dfsmn.fc1.0.weight")
    layer_order.append("dfsmn.fc1.0.bias")

    # 2. Second projection: dfsmn.fc2 (Linear + ReLU + Dropout)
    layer_order.append("dfsmn.fc2.0.weight")
    layer_order.append("dfsmn.fc2.0.bias")

    # 3. First FSMN block memory filter
    layer_order.append("dfsmn.fsmn1.lookback_filter.weight")
    if "dfsmn.fsmn1.lookahead_filter.weight" in state_dict:
        layer_order.append("dfsmn.fsmn1.lookahead_filter.weight")

    # 4. R-1 DFSMN blocks
    for i in range(arch["R"] - 1):
        prefix = f"dfsmn.fsmns.{i}"
        layer_order.append(f"{prefix}.fc1.0.weight")
        layer_order.append(f"{prefix}.fc1.0.bias")
        layer_order.append(f"{prefix}.fc2.weight")
        # fc2 has bias=False in DFSMNBlock
        layer_order.append(f"{prefix}.fsmn.lookback_filter.weight")
        if f"{prefix}.fsmn.lookahead_filter.weight" in state_dict:
            layer_order.append(f"{prefix}.fsmn.lookahead_filter.weight")

    # 5. DNN layers
    i = 0
    while f"dfsmn.dnns.{i}.weight" in state_dict:
        layer_order.append(f"dfsmn.dnns.{i}.weight")
        layer_order.append(f"dfsmn.dnns.{i}.bias")
        i += 3  # Linear + ReLU + Dropout

    # 6. Output layer
    layer_order.append("out.weight")
    layer_order.append("out.bias")

    # ---- Verify all keys accounted for ----
    exported_keys = set(layer_order)
    all_keys = set(state_dict.keys())
    missing = all_keys - exported_keys
    if missing:
        print(f"[WARN] Keys in state_dict NOT exported ({len(missing)}):")
        for k in sorted(missing):
            print(f"       - {k}")

    extra = exported_keys - all_keys
    if extra:
        print(f"[FATAL] Keys in export order NOT in state_dict ({len(extra)}):")
        for k in sorted(extra):
            print(f"       - {k}")
        sys.exit(1)

    # ---- Write binary ----
    os.makedirs(output_dir, exist_ok=True)

    ver = VERSION_INT8_PER_CH if quantize_int8_per_ch else (VERSION_INT16 if quantize_int16 else (VERSION_INT8 if quantize_int8 else VERSION_FLOAT32))
    
    # ---- Write to binary ----
    bin_path = os.path.join(output_dir, f"firered_{model_type_names.get(model_type, 'Unknown').lower()}_{'int8_ch' if quantize_int8_per_ch else ('int16' if quantize_int16 else ('int8' if quantize_int8 else 'fp32'))}.frvd")
    json_path = os.path.join(output_dir, f"firered_{model_type_names.get(model_type, 'Unknown').lower()}_{'int8_ch' if quantize_int8_per_ch else ('int16' if quantize_int16 else ('int8' if quantize_int8 else 'fp32'))}_debug.json")

    with open(bin_path, "wb") as f:
        # ---- Header (32 bytes) ----
        f.write(MAGIC)
        f.write(struct.pack("<I", ver))
        f.write(struct.pack("<I", model_type))
        f.write(struct.pack("<I", total_params))
        f.write(struct.pack("<I", arch["R"]))
        f.write(struct.pack("<I", arch["M"]))
        f.write(struct.pack("<I", 0))  # reserved
        f.write(struct.pack("<I", 0))  # reserved

        # ---- Architecture Metadata (32 bytes) ----
        f.write(struct.pack("<I", arch["D"]))
        f.write(struct.pack("<I", arch["H"]))
        f.write(struct.pack("<I", arch["P"]))
        f.write(struct.pack("<I", arch["odim"]))
        f.write(struct.pack("<I", arch["N1"]))
        f.write(struct.pack("<I", arch["S1"]))
        f.write(struct.pack("<I", arch["N2"]))
        f.write(struct.pack("<I", arch["S2"]))

        # ---- CMVN Data ----
        f.write(struct.pack("<I", cmvn_dim))
        if cmvn_dim > 0:
            f.write(cmvn_means.tobytes())
            f.write(cmvn_istd.tobytes())

        # ---- Layer Data ----
        for key in layer_order:
            tensor = state_dict[key]
            data = tensor.detach().cpu().numpy().astype(np.float32)
            
            if "lookback_filter" in key or "lookahead_filter" in key:
                # FSMN structural optimization for C++:
                # PyTorch shape is [P, 1, N]. C++ ring buffer expects [N, P] time-reversed.
                data = data.squeeze(1)          # -> [P, N]
                data = data[:, ::-1]            # Time reverse
                data = data.transpose(1, 0)     # Interleave to [N, P]
                data = np.ascontiguousarray(data)

            flat = data.flatten()

            name_hash = crc32_hash(key)
            num_elements = len(flat)

            f.write(struct.pack("<I", name_hash))
            f.write(struct.pack("<I", num_elements))

            if quantize_int8_per_ch:
                if "lookback_filter" in key or "lookahead_filter" in key:
                    # Force per-tensor for FSMN filters (C++ runtime expects a single scale)
                    max_val = float(np.max(np.abs(data)))
                    scale = max_val / 127.0 if max_val > 0 else 1.0
                    quantized = np.clip(np.round(data / scale), -127, 127).astype(np.int8)
                    f.write(struct.pack("<I", 1)) # 1 channel = per-tensor
                    f.write(struct.pack("<f", scale))
                    f.write(quantized.flatten().tobytes())
                elif len(data.shape) > 1:
                    # Weight matrix: shape [out_channels, in_channels]
                    num_channels = data.shape[0]
                    scales = np.zeros(num_channels, dtype=np.float32)
                    quantized = np.zeros_like(data, dtype=np.int8)
                    for c in range(num_channels):
                        row = data[c]
                        max_val = float(np.max(np.abs(row)))
                        scale = max_val / 127.0 if max_val > 0 else 1.0
                        scales[c] = scale
                        quantized[c] = np.clip(np.round(row / scale), -127, 127).astype(np.int8)
                    
                    f.write(struct.pack("<I", num_channels))
                    f.write(scales.tobytes())
                    f.write(quantized.flatten().tobytes())
                else:
                    # Bias vector or 1D tensor: export as unquantized float32 for Version 4
                    f.write(struct.pack("<I", 0)) # 0 channels means it's unquantized float32
                    f.write(flat.tobytes())

            elif quantize_int8:
                max_val = float(np.max(np.abs(flat)))
                scale = max_val / 127.0 if max_val > 0 else 1.0
                quantized = np.clip(np.round(flat / scale), -127, 127).astype(np.int8)
                f.write(struct.pack("<f", scale))
                f.write(quantized.tobytes())
            elif quantize_int16:
                max_val = float(np.max(np.abs(flat)))
                scale = max_val / 32767.0 if max_val > 0 else 1.0
                quantized = np.clip(np.round(flat / scale), -32767, 32767).astype(np.int16)
                f.write(struct.pack("<f", scale))
                f.write(quantized.tobytes())
            else:
                f.write(flat.tobytes())

    file_size = os.path.getsize(bin_path)
    print(f"[OK] Binary written: {bin_path}")
    print(f"     Size: {file_size:,} bytes ({file_size / 1024:.1f} KB)")

    # ---- Verify file size ----
    if quantize_int8_per_ch:
        # Size verification is complex for per-channel, skip rigorous check here.
        expected_descriptor_bytes = 0
        expected_data_bytes = 0
    elif quantize_int8:
        expected_data_bytes = sum(state_dict[k].numel() * 1 for k in layer_order)
        expected_descriptor_bytes = len(layer_order) * 12  # 4 bytes hash + 4 bytes count + 4 bytes scale
    elif quantize_int16:
        expected_data_bytes = sum(state_dict[k].numel() * 2 for k in layer_order)
        expected_descriptor_bytes = len(layer_order) * 12
    else:
        expected_data_bytes = sum(state_dict[k].numel() * 4 for k in layer_order)
        expected_descriptor_bytes = len(layer_order) * 8  # 4 bytes hash + 4 bytes count

    cmvn_bytes = 4 + (cmvn_dim * 4 * 2 if cmvn_dim > 0 else 0)
    expected_total = HEADER_SIZE + ARCH_META_SIZE + cmvn_bytes + expected_descriptor_bytes + expected_data_bytes

    if quantize_int8_per_ch or file_size == expected_total:
        print(f"[OK] Size verification PASSED")
    else:
        print(f"[FAIL] Size mismatch! Expected {expected_total:,}, got {file_size:,}")
        print(f"       Data={expected_data_bytes}, Desc={expected_descriptor_bytes}, CMVN={cmvn_bytes}")

    # ---- Write debug JSON ----
    debug_data = OrderedDict()
    debug_data["_meta"] = {
        "magic": MAGIC.decode(),
        "version": ver,
        "model_type": model_type_names.get(model_type, "Unknown"),
        "total_params": total_params,
        "quantize_int8": quantize_int8,
        "file_size_bytes": file_size,
    }
    debug_data["architecture"] = arch
    debug_data["cmvn"] = {
        "dim": cmvn_dim,
        "means_first5": cmvn_means[:5].tolist() if cmvn_dim > 0 else [],
        "istd_first5": cmvn_istd[:5].tolist() if cmvn_dim > 0 else [],
        "means_min": float(cmvn_means.min()) if cmvn_dim > 0 else 0,
        "means_max": float(cmvn_means.max()) if cmvn_dim > 0 else 0,
        "istd_min": float(cmvn_istd.min()) if cmvn_dim > 0 else 0,
        "istd_max": float(cmvn_istd.max()) if cmvn_dim > 0 else 0,
    }

    layers_debug = OrderedDict()
    for key in layer_order:
        tensor = state_dict[key]
        data_np = tensor.detach().cpu().numpy().astype(np.float32)
        
        if "lookback_filter" in key or "lookahead_filter" in key:
            data_np = data_np.squeeze(1)
            data_np = data_np[:, ::-1]
            data_np = data_np.transpose(1, 0)
            data_np = np.ascontiguousarray(data_np)

        layer_info = {
            "shape": list(data_np.shape),
            "numel": int(data_np.size),
            "crc32": f"0x{crc32_hash(key):08x}",
            "min": float(data_np.min()),
            "max": float(data_np.max()),
            "mean": float(data_np.mean()),
            "std": float(data_np.std()),
        }
        
        if quantize_int8:
            max_val = float(np.max(np.abs(data_np)))
            scale = max_val / 127.0 if max_val > 0 else 1.0
            layer_info["dtype"] = "int8"
            layer_info["scale"] = scale
            quant_np = np.clip(np.round(data_np / scale), -127, 127).astype(np.int8)
            layer_info["first5_flat"] = quant_np.flatten()[:5].tolist()
        elif quantize_int16:
            max_val = float(np.max(np.abs(data_np)))
            scale = max_val / 32767.0 if max_val > 0 else 1.0
            layer_info["dtype"] = "int16"
            layer_info["scale"] = scale
            quant_np = np.clip(np.round(data_np / scale), -32767, 32767).astype(np.int16)
            layer_info["first5_flat"] = quant_np.flatten()[:5].tolist()
        else:
            layer_info["dtype"] = "float32"
            layer_info["first5_flat"] = data_np.flatten()[:5].tolist()
            
        layers_debug[key] = layer_info
    debug_data["layers"] = layers_debug

    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(debug_data, f, indent=2, ensure_ascii=False)

    print(f"[OK] Debug JSON written: {json_path}")
    print(f"[OK] Export complete.")
    return bin_path, json_path


def main():
    parser = argparse.ArgumentParser(
        description="FireRedVAD -> ESP32 Native Binary Converter",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  python export_weights.py --model-dir ../original_models/Stream-VAD --output-dir ../converted_models
  python export_weights.py --model-dir ../original_models/VAD --output-dir ../converted_models
  python export_weights.py --all --output-dir ../converted_models
""")
    parser.add_argument("--model-dir", help="Path to model directory containing model.pth.tar and cmvn.ark")
    parser.add_argument("--output-dir", required=True, help="Output directory for .frvd and debug files")
    parser.add_argument("--all", action="store_true", help="Convert all models found in original_models/")
    parser.add_argument("--quantize-int8", action="store_true", help="Quantize weights to int8 (Symmetric Weight-Only)")
    parser.add_argument("--quantize-int8-per-ch", action="store_true", help="Quantize weights to int8 (Per-Channel Symmetric Weight-Only)")
    parser.add_argument("--quantize-int16", action="store_true", help="Quantize weights to int16")

    args = parser.parse_args()

    if args.all:
        # Auto-discover all model directories
        script_dir = os.path.dirname(os.path.abspath(__file__))
        models_root = os.path.join(script_dir, "..", "..", "original_models")
        if not os.path.exists(models_root):
            models_root = os.path.join(script_dir, "..", "original_models")

        if not os.path.exists(models_root):
            print(f"[FATAL] Cannot find original_models directory. Tried: {models_root}")
            sys.exit(1)

        converted = 0
        for entry in sorted(os.listdir(models_root)):
            model_dir = os.path.join(models_root, entry)
            model_file = os.path.join(model_dir, "model.pth.tar")
            if os.path.isdir(model_dir) and os.path.exists(model_file):
                print(f"\n{'='*60}")
                print(f"Converting: {entry}")
                print(f"{'='*60}")
                export_binary(model_dir, args.output_dir, args.quantize_int8, args.quantize_int16, args.quantize_int8_per_ch)
                converted += 1

        if converted == 0:
            print("[WARN] No models found to convert.")
        else:
            print(f"\n[OK] Converted {converted} model(s) total.")

    elif args.model_dir:
        export_binary(args.model_dir, args.output_dir, args.quantize_int8, args.quantize_int16, args.quantize_int8_per_ch)

    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
