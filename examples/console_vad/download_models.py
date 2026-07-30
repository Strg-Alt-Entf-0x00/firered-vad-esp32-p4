#!/usr/bin/env python3
"""
Download pre-converted FireRedVAD models from Hugging Face for ESP32-P4

Usage:
    python download_models.py
    python download_models.py --quantization int16
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

# TODO: Update with your Hugging Face username after uploading
DEFAULT_REPO_ID = "Strg-Alt-Entf-0x00/FireRedVAD-ESP32-P4"

MODELS = {
    "stream-vad": ["stream-vad/int8", "stream-vad/int16", "stream-vad/fp32"],
    "vad": ["vad/int8", "vad/int16", "vad/fp32"],
    "aed": ["aed/int8", "aed/int16", "aed/fp32"],
}


def download_model_variant(repo_id, model_path, output_dir):
    """Download a specific model variant (e.g., stream-vad/int8)."""
    model_name = model_path.split('/')[-2]  # e.g., "stream-vad"
    quant = model_path.split('/')[-1]        # e.g., "int8"
    
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
            
            # Copy to output directory
            import shutil
            dest_path = os.path.join(local_dir, filename)
            shutil.copy2(local_path, dest_path)
            
            file_size = os.path.getsize(dest_path)
            print(f"  ✓ {filename}: {file_size / 1024:.1f} KB")
            
        except Exception as e:
            print(f"  ✗ Failed to download {filename}: {e}")
            return False
    
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Download FireRedVAD models for ESP32-P4 from Hugging Face",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  # Download all INT8 models (recommended, ~2 MB)
  python download_models.py
  
  # Download all models (all quantizations, ~20 MB)
  python download_models.py --all-quantizations
  
  # Download specific quantization
  python download_models.py --quantization fp32
  
  # Use custom repository
  python download_models.py --repo-id username/FireRedVAD-ESP32-P4
""")
    
    parser.add_argument("--repo-id", default=DEFAULT_REPO_ID,
                        help="Hugging Face repository ID")
    parser.add_argument("--quantization", choices=["int8", "int16", "fp32"],
                        default="int8",
                        help="Quantization to download (default: int8)")
    parser.add_argument("--all-quantizations", action="store_true",
                        help="Download all quantization variants")
    parser.add_argument("--output-dir", default="./converted_models",
                        help="Output directory")
    
    args = parser.parse_args()
    
    if args.repo_id == DEFAULT_REPO_ID:
        print("[WARN] Using default repo ID. Update download_models.py with your HuggingFace username!")
        print(f"[WARN] Current: {DEFAULT_REPO_ID}")
    
    # Determine which variants to download
    quantizations = ["int8", "int16", "fp32"] if args.all_quantizations else [args.quantization]
    
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
        print("\n✅ All models downloaded successfully!")
        print("\nNext steps:")
        print("  1. idf.py build")
        print("  2. idf.py flash monitor")
        print("  3. Use 'model_list' command in console")
    else:
        print("\n⚠️  Some downloads failed. Check the errors above.")
        sys.exit(1)


if __name__ == "__main__":
    main()
