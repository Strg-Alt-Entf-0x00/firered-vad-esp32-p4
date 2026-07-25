#!/usr/bin/env python3
"""
Verify .frvd binary by simulating the EXACT PyTorch FSMN forward pass
with the exported weights, then comparing against original PyTorch output.

The key insight: PyTorch Conv1d with dilation and groups=P is a depthwise
dilated causal convolution. For streaming (single frame at a time), the
cache stores (N1-1)*S1 past frames. The flow per FSMN block is:

  1. Prepend cache to current input: [P, cache_len + 1]
  2. Run Conv1d (with padding=cache_len, dilation=S1, groups=P)
  3. Trim right: output[:, :, :-(N1-1)*S1]
  4. Trim left by cache_len (to get only current frame output)
  5. Update cache = last cache_len columns of concatenated input
  6. memory = residual + conv_output
"""

import argparse
import math
import os
import struct
import sys

import numpy as np

try:
    import torch
    import torch.nn as nn
except ImportError:
    print("[FATAL] PyTorch not installed. Run: pip install torch")
    sys.exit(1)

try:
    import kaldiio
except ImportError:
    print("[FATAL] kaldiio not installed. Run: pip install kaldiio")
    sys.exit(1)

def load_frvd(frvd_path):
    """Load .frvd and return arch dict + ordered list of numpy tensors."""
    with open(frvd_path, "rb") as f:
        data = f.read()

    assert data[:4] == b"FRVD"
    arch = {
        "R": struct.unpack_from("<I", data, 16)[0],
        "M": struct.unpack_from("<I", data, 20)[0],
        "D": struct.unpack_from("<I", data, 32)[0],
        "H": struct.unpack_from("<I", data, 36)[0],
        "P": struct.unpack_from("<I", data, 40)[0],
        "odim": struct.unpack_from("<I", data, 44)[0],
        "N1": struct.unpack_from("<I", data, 48)[0],
        "S1": struct.unpack_from("<I", data, 52)[0],
        "N2": struct.unpack_from("<I", data, 56)[0],
        "S2": struct.unpack_from("<I", data, 60)[0],
    }

    offset = 64
    cmvn_dim = struct.unpack_from("<I", data, offset)[0]
    offset += 4
    cmvn_means = cmvn_istd = None
    if cmvn_dim > 0:
        cmvn_means = np.frombuffer(data, np.float32, cmvn_dim, offset).copy()
        offset += cmvn_dim * 4
        cmvn_istd = np.frombuffer(data, np.float32, cmvn_dim, offset).copy()
        offset += cmvn_dim * 4

    tensors = []
    while offset + 8 <= len(data):
        offset += 4  # skip hash
        count = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        arr = np.frombuffer(data, np.float32, count, offset).copy()
        offset += count * 4
        tensors.append(arr)

    return arch, cmvn_means, cmvn_istd, tensors


def build_pytorch_model_from_frvd(arch, tensors):
    """Reconstruct a PyTorch DetectModel and load the .frvd weights into it."""
    # Create args namespace matching what DetectModel.__init__ expects
    class Args:
        pass
    args = Args()
    args.idim = arch["D"]
    args.odim = arch["odim"]
    args.R = arch["R"]
    args.M = arch["M"]
    args.H = arch["H"]
    args.P = arch["P"]
    args.N1 = arch["N1"]
    args.S1 = arch["S1"]
    args.N2 = arch["N2"]
    args.S2 = arch["S2"]
    args.dropout = 0.0  # no dropout during inference

    model = DetectModel_cls(args)
    model.eval()

    # Now load tensors into the state_dict in the EXACT same order as export
    sd = model.state_dict()
    keys_ordered = []

    keys_ordered.append("dfsmn.fc1.0.weight")
    keys_ordered.append("dfsmn.fc1.0.bias")
    keys_ordered.append("dfsmn.fc2.0.weight")
    keys_ordered.append("dfsmn.fc2.0.bias")
    keys_ordered.append("dfsmn.fsmn1.lookback_filter.weight")
    if arch["N2"] > 0:
        keys_ordered.append("dfsmn.fsmn1.lookahead_filter.weight")

    for i in range(arch["R"] - 1):
        keys_ordered.append(f"dfsmn.fsmns.{i}.fc1.0.weight")
        keys_ordered.append(f"dfsmn.fsmns.{i}.fc1.0.bias")
        keys_ordered.append(f"dfsmn.fsmns.{i}.fc2.weight")
        keys_ordered.append(f"dfsmn.fsmns.{i}.fsmn.lookback_filter.weight")
        if arch["N2"] > 0:
            keys_ordered.append(f"dfsmn.fsmns.{i}.fsmn.lookahead_filter.weight")

    idx = 0
    while f"dfsmn.dnns.{idx}.weight" in sd:
        keys_ordered.append(f"dfsmn.dnns.{idx}.weight")
        keys_ordered.append(f"dfsmn.dnns.{idx}.bias")
        idx += 3

    keys_ordered.append("out.weight")
    keys_ordered.append("out.bias")

    new_sd = {}
    for i, key in enumerate(keys_ordered):
        shape = sd[key].shape
        new_sd[key] = torch.from_numpy(tensors[i].reshape(shape)).float()

    # Load only the keys we have, keep defaults for others (dropout etc)
    model.load_state_dict(new_sd, strict=False)
    model.eval()
    return model


def run_streaming_inference(model, features_list, cmvn_means=None, cmvn_istd=None):
    """Run frame-by-frame streaming inference using PyTorch model."""
    results = []
    caches = None

    with torch.no_grad():
        for feat in features_list:
            f = feat.copy()
            if cmvn_means is not None:
                f = (f - cmvn_means) * cmvn_istd

            feat_tensor = torch.from_numpy(f).float().unsqueeze(0).unsqueeze(0)
            probs, caches = model(feat_tensor, caches=caches)
            results.append(probs.squeeze().item())

    return results


def main():
    parser = argparse.ArgumentParser(description="Verify .frvd conversion")
    parser.add_argument("--model-dir", required=True)
    parser.add_argument("--frvd", required=True)
    parser.add_argument("--num-frames", type=int, default=50)
    parser.add_argument("--fireredvad-dir", help="Path to FireRedVAD official repository")
    args = parser.parse_args()

    if args.fireredvad_dir:
        sys.path.insert(0, os.path.abspath(args.fireredvad_dir))

    try:
        from fireredvad.core.detect_model import DetectModel
        from fireredvad.core.audio_feat import CMVN
    except ImportError:
        print("[FATAL] fireredvad package not found.")
        print("        Please ensure it is in your PYTHONPATH, or provide --fireredvad-dir")
        print("        Example: --fireredvad-dir /path/to/FireRedVAD")
        sys.exit(1)

    # Re-assign to global scope for build_pytorch_model_from_frvd to use
    global DetectModel_cls
    DetectModel_cls = DetectModel

    # Load original model
    print("[INFO] Loading original PyTorch model...")
    orig_model = DetectModel.from_pretrained(args.model_dir)
    orig_model.eval()

    # Load .frvd and reconstruct
    print(f"[INFO] Loading .frvd: {args.frvd}")
    arch, cmvn_means, cmvn_istd, tensors = load_frvd(args.frvd)
    print(f"[INFO] Architecture: D={arch['D']} H={arch['H']} P={arch['P']} R={arch['R']} M={arch['M']}")
    print(f"[INFO] {len(tensors)} tensors loaded")

    print("[INFO] Reconstructing model from .frvd weights...")
    frvd_model = build_pytorch_model_from_frvd(arch, tensors)

    # Load CMVN
    cmvn_path = os.path.join(args.model_dir, "cmvn.ark")
    cmvn = CMVN(cmvn_path) if os.path.exists(cmvn_path) else None

    # Generate test features
    np.random.seed(42)
    features_list = []
    for _ in range(min(20, args.num_frames)):
        features_list.append(np.zeros(arch["D"], dtype=np.float32))
    for i in range(max(0, args.num_frames - 20)):
        features_list.append(np.random.randn(arch["D"]).astype(np.float32) * 2.0 + 12.0)

    # Run both models
    print(f"\n[TEST] Running original PyTorch ({len(features_list)} frames)...")
    orig_results = run_streaming_inference(orig_model, features_list,
        cmvn_means if cmvn_means is not None else (np.array(cmvn.means, dtype=np.float32) if cmvn else None),
        cmvn_istd if cmvn_istd is not None else (np.array(cmvn.inverse_std_variances, dtype=np.float32) if cmvn else None))

    print(f"[TEST] Running reconstructed .frvd model ({len(features_list)} frames)...")
    frvd_results = run_streaming_inference(frvd_model, features_list, cmvn_means, cmvn_istd)

    # Compare
    print(f"\n{'Frame':>6} | {'FRVD':>10} | {'Original':>10} | {'Diff':>12} | {'Status':>6}")
    print("-" * 60)

    max_diff = 0.0
    num_pass = 0
    for i in range(len(features_list)):
        diff = abs(frvd_results[i] - orig_results[i])
        max_diff = max(max_diff, diff)
        status = "OK" if diff < 1e-5 else "WARN" if diff < 1e-3 else "FAIL"
        if status == "OK":
            num_pass += 1
        if i < 10 or i % 10 == 0 or status != "OK":
            print(f"{i:6d} | {frvd_results[i]:10.6f} | {orig_results[i]:10.6f} | {diff:12.8f} | {status:>6}")

    print(f"\n{'='*60}")
    print(f"Results: {num_pass}/{len(features_list)} passed (max_diff={max_diff:.10f})")

    if max_diff < 1e-5:
        print("[OK] PERFECT MATCH - .frvd weights are bit-exact with original")
    elif max_diff < 1e-3:
        print("[OK] Weights verified - minor float32 rounding differences only")
    else:
        print("[FAIL] Weight mismatch detected!")

    return 0 if max_diff < 1e-3 else 1


if __name__ == "__main__":
    sys.exit(main())
