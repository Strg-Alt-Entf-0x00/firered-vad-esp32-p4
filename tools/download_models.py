#!/usr/bin/env python3
"""
FireRedVAD Model Downloader

Downloads original FireRedVAD models from Hugging Face for conversion to ESP32-P4 format.

Usage:
    python download_models.py --output-dir ../original_models
    python download_models.py --model stream-vad --output-dir ../original_models
"""

import argparse
import os
import sys

try:
    from huggingface_hub import hf_hub_download
except ImportError:
    print("[FATAL] huggingface_hub not installed.")
    print("        Run: pip install huggingface-hub")
    sys.exit(1)

# Model configurations
MODELS = {
    "stream-vad": {
        "repo_id": "FireRedTeam/FireRedVAD",
        "files": [
            "Stream-VAD/model.pth.tar",
            "Stream-VAD/cmvn.ark",
        ],
        "output_dir": "Stream-VAD",
    },
    "vad": {
        "repo_id": "FireRedTeam/FireRedVAD",
        "files": [
            "VAD/model.pth.tar",
            "VAD/cmvn.ark",
        ],
        "output_dir": "VAD",
    },
    "aed": {
        "repo_id": "FireRedTeam/FireRedVAD",
        "files": [
            "AED/model.pth.tar",
            "AED/cmvn.ark",
        ],
        "output_dir": "AED",
    },
}


def download_model(model_name, output_root):
    """Download a specific model from Hugging Face."""
    if model_name not in MODELS:
        print(f"[FATAL] Unknown model: {model_name}")
        print(f"        Available: {', '.join(MODELS.keys())}")
        sys.exit(1)

    config = MODELS[model_name]
    output_dir = os.path.join(output_root, config["output_dir"])
    os.makedirs(output_dir, exist_ok=True)

    print(f"\n{'='*60}")
    print(f"Downloading: {model_name}")
    print(f"Repository: {config['repo_id']}")
    print(f"Output: {output_dir}")
    print(f"{'='*60}\n")

    for file_path in config["files"]:
        filename = os.path.basename(file_path)
        print(f"[INFO] Downloading {file_path}...")
        
        try:
            local_path = hf_hub_download(
                repo_id=config["repo_id"],
                filename=file_path,
                cache_dir=None,  # Use default HF cache
                force_download=False,
            )
            
            # Copy to output directory
            import shutil
            dest_path = os.path.join(output_dir, filename)
            shutil.copy2(local_path, dest_path)
            
            file_size = os.path.getsize(dest_path)
            print(f"[OK] {filename}: {file_size:,} bytes ({file_size / (1024*1024):.1f} MB)")
            
        except Exception as e:
            print(f"[ERROR] Failed to download {file_path}: {e}")
            return False

    print(f"\n[OK] Model downloaded to: {output_dir}\n")
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Download FireRedVAD models from Hugging Face",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  # Download all models
  python download_models.py --all --output-dir ../original_models
  
  # Download only Stream-VAD
  python download_models.py --model stream-vad --output-dir ../original_models
  
  # Download Stream-VAD and VAD
  python download_models.py --model stream-vad --model vad --output-dir ../original_models
""")
    
    parser.add_argument("--model", action="append", choices=list(MODELS.keys()),
                        help="Model to download (can be specified multiple times)")
    parser.add_argument("--all", action="store_true",
                        help="Download all available models")
    parser.add_argument("--output-dir", default="./original_models",
                        help="Output directory for downloaded models")
    
    args = parser.parse_args()

    if not args.model and not args.all:
        print("[INFO] No model specified. Use --model <name> or --all")
        print(f"[INFO] Available models: {', '.join(MODELS.keys())}")
        parser.print_help()
        sys.exit(1)

    # Determine which models to download
    models_to_download = list(MODELS.keys()) if args.all else args.model

    # Download
    success_count = 0
    for model_name in models_to_download:
        if download_model(model_name, args.output_dir):
            success_count += 1

    print(f"\n{'='*60}")
    print(f"Summary: {success_count}/{len(models_to_download)} models downloaded successfully")
    print(f"{'='*60}")
    print(f"\nNext steps:")
    print(f"1. cd converter")
    print(f"2. python export_weights.py --all --output-dir ../converted_models")
    print(f"   or")
    print(f"   python export_weights.py --model-dir {args.output_dir}/Stream-VAD --output-dir ../converted_models --quantize-int8")
    print()


if __name__ == "__main__":
    main()
