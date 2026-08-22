import sys
import time
import argparse
from pathlib import Path
from esp32_protocol import ESP32Protocol, ESP32ProtocolError

# Add parent directory to path to import config
sys.path.insert(0, str(Path(__file__).parent.parent))
from esp32_config import get_config

# Filesystem types mapping
FSTYPE_NONE = 0

FSTYPE_FAT32 = 2
FSTYPE_EXFAT = 3

def main():
    config = get_config()
    
    parser = argparse.ArgumentParser(description="Format ESP32-P4 Storage (Flash or SD Card)")
    parser.add_argument('target', choices=['sd'], help="Target storage to format")
    parser.add_argument('--port', type=str, default=None, help=f"COM port of the ESP32 (default: {config.file_port})")
    parser.add_argument('--baud', type=int, default=None, help=f"Baud rate (default: {config.file_baud})")
    args = parser.parse_args()
    
    # Use config values if not overridden
    port = args.port or config.file_port
    baud = args.baud or config.file_baud

    print(f"Connecting to ESP32 on {port} @ {baud} baud...")
    esp32 = ESP32Protocol()
    
    try:
        if not esp32.connect(port, baud):
            print("ERROR: Could not connect to ESP32 on this port.")
            sys.exit(1)
            
        info = esp32.get_device_info()
        print(f"Connected to: {info.device_name}")
        
        if not info.sd_present:
            print("ERROR: No SD card detected in the ESP32!")
            sys.exit(1)
        fs_type = FSTYPE_FAT32
        print("WARNING: You are about to format the SD Card.")
        print("All data in the /sd directory will be permanently destroyed!")
            
        print("\nType 'YES' (all caps) to confirm formatting:")
        confirmation = input("> ")
        if confirmation != "YES":
            print("Format cancelled.")
            sys.exit(0)
            
        print(f"\nFormatting {args.target}... please wait. This can take a few seconds.")
        start_time = time.time()
        
        esp32.format_fs(fs_type)
        
        duration = time.time() - start_time
        print(f"SUCCESS: {args.target.upper()} formatted successfully in {duration:.1f} seconds!")
        
    except ESP32ProtocolError as e:
        print(f"ERROR: Formatting failed: {e}")
    except KeyboardInterrupt:
        print("\nOperation cancelled by user.")
    finally:
        esp32.disconnect()

if __name__ == '__main__':
    main()
