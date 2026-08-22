#!/usr/bin/env python3
"""
ESP32-P4 File Transfer Speed Test
Tests actual upload/download speeds to verify optimizations.
"""

import os
import sys
import time
import argparse
from pathlib import Path
from esp32_protocol import ESP32Protocol

# Add parent directory to path
sys.path.insert(0, str(Path(__file__).parent.parent))
from esp32_config import get_config


def format_speed(bytes_per_sec):
    """Format speed in human-readable format."""
    if bytes_per_sec < 1024:
        return f"{bytes_per_sec:.1f} B/s"
    elif bytes_per_sec < 1024 * 1024:
        return f"{bytes_per_sec / 1024:.1f} KB/s"
    else:
        return f"{bytes_per_sec / (1024 * 1024):.1f} MB/s"


def create_test_file(size_bytes):
    """Create a temporary test file with random data."""
    import tempfile
    import random
    
    temp_file = tempfile.NamedTemporaryFile(mode='wb', delete=False, suffix='.bin')
    
    # Write in 1KB chunks for speed
    chunk_size = 1024
    remaining = size_bytes
    
    while remaining > 0:
        chunk = remaining if remaining < chunk_size else chunk_size
        data = bytes([random.randint(0, 255) for _ in range(chunk)])
        temp_file.write(data)
        remaining -= chunk
    
    temp_file.close()
    return temp_file.name


def run_speed_test(port=None, baud=None, test_sizes=[100*1024, 500*1024, 1024*1024]):
    """Run comprehensive speed test."""
    config = get_config()
    port = port or config.file_port
    baud = baud or config.file_baud
    
    print("\n" + "="*70)
    print("ESP32-P4 FILE TRANSFER SPEED TEST")
    print("="*70)
    print(f"Port: {port}")
    print(f"Baud: {baud:,}")
    print("="*70 + "\n")
    
    proto = ESP32Protocol()
    
    # Connect
    print("[INFO] Connecting to ESP32-P4...")
    if not proto.connect(port, baud):
        print("[FAIL] Failed to connect!")
        return False
    
    print("[OK] Connected!\n")
    
    # Get device info
    try:
        info = proto.get_device_info()
        print(f"[INFO] Device: {info.device_name}")
        print(f"[INFO] Max Payload: {info.max_payload_size:,} bytes")
        print(f"[INFO] Optimal Chunk: {info.optimal_chunk_size:,} bytes")
        
        if info.sd_present:
            sd_size_mb = info.sd_size / (1024 * 1024)
            sd_free_mb = info.sd_free / (1024 * 1024)
            print(f"[INFO] SD Card: {sd_size_mb:.0f} MB total, {sd_free_mb:.0f} MB free")
        else:
            print("[WARN]  Warning: No SD card detected!")
            proto.disconnect()
            return False
        
        print()
    except Exception as e:
        print(f"[FAIL] Failed to get device info: {e}")
        proto.disconnect()
        return False
    
    # Run tests for each size
    results = []
    
    for test_size in test_sizes:
        size_kb = test_size / 1024
        print(f"\n{'='*70}")
        print(f"[INFO] Testing with {size_kb:.0f} KB file")
        print(f"{'='*70}\n")
        
        # Create test file
        print(f"[INFO] Creating {size_kb:.0f} KB test file...")
        test_file = create_test_file(test_size)
        local_path = test_file
        remote_path = f"/sd/speedtest_{int(size_kb)}kb.bin"
        
        try:
            # UPLOAD TEST
            print(f"\n[UP]  UPLOAD TEST ({size_kb:.0f} KB)")
            print("-" * 50)
            
            with open(local_path, 'rb') as f:
                file_data = f.read()
            
            start_time = time.time()
            try:
                proto.write_file(remote_path, file_data)
                upload_time = time.time() - start_time
                success = True
            except Exception as e:
                print(f"[FAIL] Upload exception: {e}")
                upload_time = 0
                success = False
            
            if success:
                upload_speed = test_size / upload_time
                print(f"[OK] Upload complete!")
                print(f"   Time: {upload_time:.2f} seconds")
                print(f"   Speed: {format_speed(upload_speed)}")
                print(f"   Efficiency: {(upload_speed / (baud / 10)) * 100:.1f}%")
            else:
                print(f"[FAIL] Upload failed!")
                upload_speed = 0
                upload_time = 0
            
            # DOWNLOAD TEST
            print(f"\n[DOWN]  DOWNLOAD TEST ({size_kb:.0f} KB)")
            print("-" * 50)
            
            start_time = time.time()
            try:
                downloaded_data = proto.read_file(remote_path)
                download_time = time.time() - start_time
                success = True
            except Exception as e:
                print(f"[FAIL] Download exception: {e}")
                download_time = 0
                success = False
                downloaded_data = b''
            
            if success:
                download_speed = test_size / download_time
                print(f"[OK] Download complete!")
                print(f"   Time: {download_time:.2f} seconds")
                print(f"   Speed: {format_speed(download_speed)}")
                print(f"   Efficiency: {(download_speed / (baud / 10)) * 100:.1f}%")
                
                # Verify file integrity
                if file_data == downloaded_data:
                    print(f"   Integrity: [OK] VERIFIED")
                else:
                    print(f"   Integrity: [FAIL] MISMATCH!")
            else:
                print(f"[FAIL] Download failed!")
                download_speed = 0
                download_time = 0
            
            # Clean up remote file
            proto.delete_file(remote_path)
            
            # Store results
            results.append({
                'size': test_size,
                'upload_speed': upload_speed,
                'download_speed': download_speed,
                'upload_time': upload_time,
                'download_time': download_time
            })
            
        except Exception as e:
            print(f"[FAIL] Test failed: {e}")
        finally:
            # Clean up local file
            if os.path.exists(test_file):
                os.unlink(test_file)
    
    # Summary
    print("\n" + "="*70)
    print("[INFO] SUMMARY")
    print("="*70)
    
    theoretical_max = baud / 10  # bits/sec -> bytes/sec
    
    print(f"\n Theoretical Maximum: {format_speed(theoretical_max)} @ {baud:,} baud")
    print()
    
    print("Size       Upload Speed    Efficiency    Download Speed  Efficiency")
    print("-" * 70)
    
    for r in results:
        size_kb = r['size'] / 1024
        up_eff = (r['upload_speed'] / theoretical_max * 100) if r['upload_speed'] > 0 else 0
        dn_eff = (r['download_speed'] / theoretical_max * 100) if r['download_speed'] > 0 else 0
        
        print(f"{size_kb:6.0f} KB   {format_speed(r['upload_speed']):>12}    {up_eff:5.1f}%     "
              f"{format_speed(r['download_speed']):>12}    {dn_eff:5.1f}%")
    
    # Average efficiency
    if results:
        avg_upload = sum(r['upload_speed'] for r in results) / len(results)
        avg_download = sum(r['download_speed'] for r in results) / len(results)
        avg_up_eff = (avg_upload / theoretical_max * 100)
        avg_dn_eff = (avg_download / theoretical_max * 100)
        
        print("-" * 70)
        print(f"AVERAGE    {format_speed(avg_upload):>12}    {avg_up_eff:5.1f}%     "
              f"{format_speed(avg_download):>12}    {avg_dn_eff:5.1f}%")
        
        print("\n" + "="*70)
        print("[INFO] PERFORMANCE VERDICT")
        print("="*70)
        
        if avg_dn_eff >= 95:
            print("[OK] EXCELLENT: Download efficiency 95% - Near theoretical maximum!")
        elif avg_dn_eff >= 85:
            print("[OK] VERY GOOD: Download efficiency 85% - Excellent performance!")
        elif avg_dn_eff >= 70:
            print("[WARN]  GOOD: Download efficiency 70% - Acceptable but could improve")
        else:
            print("[FAIL] POOR: Download efficiency <70% - Optimization needed!")
        
        if avg_up_eff >= 75:
            print("[OK] EXCELLENT: Upload efficiency 75% - Very good for SD writes!")
        elif avg_up_eff >= 65:
            print("[OK] GOOD: Upload efficiency 65% - Good for SD card writes")
        else:
            print("[WARN]  LOW: Upload efficiency <65% - SD card may be slow")
        
        print("="*70)
    
    proto.disconnect()
    print("\n[OK] Speed test complete!\n")
    return True


def main():
    parser = argparse.ArgumentParser(description='ESP32-P4 File Transfer Speed Test')
    parser.add_argument('--port', help='Serial port (default from config.ini)')
    parser.add_argument('--baud', type=int, help='Baud rate (default from config.ini)')
    parser.add_argument('--sizes', type=str, default='100,500,1000', 
                       help='Test sizes in KB, comma-separated (default: 100,500,1000)')
    
    args = parser.parse_args()
    
    # Parse test sizes
    test_sizes = [int(s.strip()) * 1024 for s in args.sizes.split(',')]
    
    try:
        run_speed_test(port=args.port, baud=args.baud, test_sizes=test_sizes)
    except KeyboardInterrupt:
        print("\n\n[WARN]  Test interrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"\n[FAIL] Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == '__main__':
    main()
