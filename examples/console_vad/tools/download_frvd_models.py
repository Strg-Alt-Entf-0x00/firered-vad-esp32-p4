#!/usr/bin/env python3
"""
Download pre-converted FireRedVAD models from Hugging Face for ESP32-P4

Usage:
    python download_models.py
    python download_models.py --quantization int16
    python download_models.py --quantization int8-ch

"""

import argparse
import os
import shutil
import sys

try:
    from huggingface_hub import hf_hub_download
except ImportError:
    print("[FATAL] huggingface_hub not installed.")
    print("        Run: pip install huggingface-hub")
    sys.exit(1)

# The official Hugging Face repository for ESP32-P4 models
DEFAULT_REPO_ID = "Strg-Alt-Entf-0x00/FireRedVAD-ESP32-P4"

MODELS = {
    "stream-vad": [
        "stream-vad/int8",
        "stream-vad/int8-ch",
        "stream-vad/int16",
        "stream-vad/fp32",
    ],
    "vad": [
        "vad/int8",
        "vad/int8-ch",
        "vad/int16",
        "vad/fp32",
    ],
    "aed": [
        "aed/int8",
        "aed/int8-ch",
        "aed/int16",
        "aed/fp32",
    ],
}

QUANTIZATIONS = ("int8", "int8-ch", "int16", "fp32")


def download_model_variant(repo_id, model_path, output_dir):
    """Download a specific model variant (e.g., stream-vad/int8)."""
    quant = model_path.split("/")[-1]
    
    # Determine filename pattern
    if "stream-vad" in model_path:
        base_name = "firered-stream-vad"
    elif model_path.startswith("vad/"):
        base_name = "firered-vad"
    elif model_path.startswith("aed/"):
        base_name = "firered-aed"
    else:
        base_name = "firered-model"
    
    files = [
        f"{model_path}/{base_name}-{quant}.frvd",
    ]
    
    local_dir = os.path.join(output_dir, model_path)
    os.makedirs(local_dir, exist_ok=True)
    
    print(f"\n[INFO] Downloading {model_path}...")
    
    for file_path in files:
        filename = os.path.basename(file_path)
        
        try:
            local_path = hf_hub_download(
                repo_id=repo_id,
                filename=file_path,
                cache_dir=None,
                force_download=False,
            )
            
            dest_path = os.path.join(local_dir, filename)
            shutil.copy2(local_path, dest_path)
            
            file_size = os.path.getsize(dest_path)
            print(f"  [OK] {filename}: {file_size / 1024:.1f} KB")
            
        except Exception as e:
            print(f"  [ERR] Failed to download {filename}: {e}")
            return False
    
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Download FireRedVAD models for ESP32-P4 from Hugging Face",
        formatter_class=argparse.RawDescriptionHelpFormatter,
                epilog="""Examples:
    # Download all models (default, ~20 MB)
    python download_models.py


    # Download one quantization
    python download_models.py --quantization int8-ch

    # Use a custom repository
    python download_models.py --repo-id username/FireRedVAD-ESP32-P4
""")
    
    parser.add_argument("--repo-id", default=DEFAULT_REPO_ID,
                        help="Hugging Face repository ID")
    parser.add_argument("--quantization", choices=QUANTIZATIONS,
                        help="Only download one quantization (default: all)")

    parser.add_argument("--output-dir", default="../frvd_models",
                        help="Output directory (default: ../frvd_models)")
    
    args = parser.parse_args()
    
    # Determine which variants to download
    quantizations = list(QUANTIZATIONS) if args.quantization is None else [args.quantization]
    
    print(f"{'='*60}")
    print(f"Repository: {args.repo_id}")
    print(f"Downloading: {', '.join(quantizations)} models")
    print(f"Output: {args.output_dir}")
    print(f"{'='*60}")
    
    success_count = 0
    total_count = 0
    
    for model_type in MODELS:
        for model_path in MODELS[model_type]:
            quant = model_path.split('/')[-1]
            if quant in quantizations:
                total_count += 1
                if download_model_variant(args.repo_id, model_path, args.output_dir):
                    success_count += 1
    
    print(f"\n{'='*60}")
    print(f"Downloaded: {success_count}/{total_count} model variants")
    print(f"{'='*60}")
    
    if success_count == total_count:
        print("\n[OK] All models downloaded successfully!")
        print("\nNext steps:")
        print("  1. idf.py build")
        print("  2. idf.py flash monitor")
        print("  3. Use 'model_list' command in console")
    else:
        print("\n[WARN]  Some downloads failed. Check the errors above.")
        sys.exit(1)


if __name__ == "__main__":
    main()
