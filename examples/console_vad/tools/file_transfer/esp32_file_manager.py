#!/usr/bin/env python3
"""
ESP32-P4 File Manager - Professional tool for managing files on ESP32 SD card.

Usage:
    python esp32_file_manager.py upload              # Upload all files
    python esp32_file_manager.py verify              # Verify files only
    python esp32_file_manager.py upload --verify     # Upload and verify
    python esp32_file_manager.py list                # List SD card contents
    python esp32_file_manager.py info                # Show device info
    python esp32_file_manager.py models              # Check model integrity
    python esp32_file_manager.py delete <path>       # Delete file/directory
    python esp32_file_manager.py download <path>     # Download file from ESP32
"""

import os
import sys
import argparse
import logging
import binascii
from pathlib import Path
from esp32_protocol import ESP32Protocol, ESP32ProtocolError

# Add parent directory to path to import config
sys.path.insert(0, str(Path(__file__).parent.parent))
from esp32_config import get_config

# Setup logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)


class ESP32FileManager:
    def __init__(self, port=None, baud=None):
        """Initialize file manager with config.ini values or override parameters."""
        config = get_config()
        self.port = port or config.file_port
        self.baud = baud or config.file_baud
        self.proto = ESP32Protocol()
        
    def connect(self):
        """Connect to ESP32 with retries."""
        logger.info(f"Connecting to ESP32-P4 on {self.port}...")
        
        max_retries = 3
        for attempt in range(max_retries):
            if self.proto.connect(self.port, self.baud):
                logger.info("[OK] Connected successfully!")
                return True
            if attempt < max_retries - 1:
                logger.warning(f"Connection attempt {attempt+1} failed, retrying...")
                import time
                time.sleep(2)
        
        logger.error("Failed to connect to ESP32 after multiple attempts")
        return False
    
    def disconnect(self):
        """Disconnect from ESP32."""
        self.proto.disconnect()
        logger.info("Disconnected from ESP32")
    
    def get_device_info(self):
        """Get and display device information."""
        info = self.proto.get_device_info()
        
        logger.info("\n" + "="*60)
        logger.info("DEVICE INFORMATION")
        logger.info("="*60)
        logger.info(f"Device: {info.device_name}")
        logger.info(f"Firmware: {info.fw_version}")
        logger.info(f"SD Card: {'Present' if info.sd_present else 'Not present'}")
        logger.info(f"Max Payload: {info.max_payload_size:,} bytes")
        logger.info(f"Optimal Chunk: {info.optimal_chunk_size:,} bytes")
        logger.info("="*60)
        
        return info
    
    def list_directory(self, path='/sd'):
        """List directory contents."""
        logger.info(f"\nListing {path}...")
        
        try:
            entries = self.proto.list_directory(path)
            
            if not entries:
                logger.info("  (empty)")
                return []
            
            # Separate directories and files
            dirs = [e for e in entries if e.is_directory]
            files = [e for e in entries if not e.is_directory]
            
            # Show directories first
            for entry in sorted(dirs, key=lambda e: e.name):
                logger.info(f"  [DIR]  {entry.name}")
            
            # Show files with sizes
            for entry in sorted(files, key=lambda e: e.name):
                logger.info(f"  {entry.size:>12,} bytes  {entry.name}")
            
            logger.info(f"\nTotal: {len(dirs)} directories, {len(files)} files")
            return entries
            
        except ESP32ProtocolError as e:
            logger.error(f"Failed to list {path}: {e}")
            return []
    
    def upload_files(self, verify=False):
        """Upload all files to ESP32 from multiple sources."""
        
        from esp32_config import get_config
        config = get_config()
        
        # Models from frvd_models
        models_dir = config.frvd_models_dir
        
        # Test audio from example_wave
        audio_dir = Path(__file__).parent.parent.parent / 'example_wave'
        
        # Collect files
        model_files = []
        audio_files = []
        
        if models_dir.exists():
            # Find all .frvd files in frvd_models subdirectories
            model_files = sorted(models_dir.rglob('*.frvd'))
        
        if audio_dir.exists():
            # Find all .wav files
            audio_files = sorted(audio_dir.glob('*.wav'))
        
        if not model_files and not audio_files:
            logger.error("No files found to upload!")
            logger.error(f"  Models dir: {models_dir}")
            logger.error(f"  Audio dir:  {audio_dir}")
            return False
        
        logger.info(f"\nFound {len(model_files)} model files + {len(audio_files)} test files")
        if model_files:
            logger.info(f"Models source: {models_dir}")
        if audio_files:
            logger.info(f"Audio source:  {audio_dir}")
        
        # Create test_audio directory
        logger.info("\nCreating /sd/test_audio directory...")
        try:
            self.proto.mkdir('/sd/test_audio')
            logger.info("[OK] Directory created")
        except ESP32ProtocolError as e:
            if "NACK" in str(e):
                logger.info("Directory already exists")
            else:
                raise
        
        success_count = 0
        fail_count = 0
        
        # Upload model files to /sd root
        if model_files:
            logger.info("\n" + "="*60)
            logger.info("UPLOADING MODEL FILES")
            logger.info("="*60)
            
            for local_path in model_files:
                filename = local_path.name
                remote_path = f'/sd/{filename}'
                
                try:
                    with open(local_path, 'rb') as f:
                        data = f.read()
                    
                    logger.info(f"Uploading {filename} ({len(data):,} bytes)...")
                    self.proto.write_file(remote_path, data)
                    logger.info(f"  [OK] Success")
                    success_count += 1
                except Exception as e:
                    logger.error(f"  [FAIL] Failed: {e}")
                    fail_count += 1
        
        # Upload test audio/txt files to /sd/test_audio
        if audio_files:
            logger.info("\n" + "="*60)
            logger.info("UPLOADING TEST FILES")
            logger.info("="*60)
            
            for local_path in audio_files:
                filename = local_path.name
                remote_path = f'/sd/test_audio/{filename}'
                
                try:
                    with open(local_path, 'rb') as f:
                        data = f.read()
                    
                    logger.info(f"Uploading {filename} ({len(data):,} bytes)...")
                    self.proto.write_file(remote_path, data)
                    logger.info(f"  [OK] Success")
                    success_count += 1
                
                except Exception as e:
                    logger.error(f"  [FAIL] Failed: {e}")
                    fail_count += 1
        
        # Upload model files
        logger.info("\n" + "="*60)
        logger.info("UPLOADING MODEL FILES")
        logger.info("="*60)
        
        for local_path in model_files:
            filename = local_path.name
            remote_path = f'/sd/{filename}'
            
            try:
                with open(local_path, 'rb') as f:
                    data = f.read()
                
                logger.info(f"Uploading {filename} ({len(data):,} bytes)...")
                self.proto.write_file(remote_path, data)
                logger.info(f"  [OK] Success")
                success_count += 1
                
            except Exception as e:
                logger.error(f"  [FAIL] Failed: {e}")
                fail_count += 1
        
        # Summary
        logger.info("\n" + "="*60)
        logger.info("UPLOAD SUMMARY")
        logger.info("="*60)
        logger.info(f"[OK] Successful: {success_count}")
        logger.info(f"[FAIL] Failed: {fail_count}")
        logger.info(f"Total: {success_count + fail_count}")
        
        if fail_count == 0:
            logger.info("\n[OK] All files uploaded successfully!")
            
            # Auto-verify if requested
            if verify:
                logger.info("\n" + "="*60)
                logger.info("AUTO-VERIFY ENABLED")
                logger.info("="*60)
                return self.verify_upload()
            
            return True
        else:
            logger.warning(f"\n[WARN]  {fail_count} files failed to upload")
            return False
    
    def verify_upload(self):
        """Verify uploaded files."""
        logger.info("\n" + "="*60)
        logger.info("VERIFYING UPLOAD")
        logger.info("="*60)
        
        # Verify test_audio directory
        logger.info("\nVerifying /sd/test_audio...")
        try:
            entries = self.proto.list_directory('/sd/test_audio')
            
            wav_files = [e for e in entries if e.name.endswith('.wav')]
            txt_files = [e for e in entries if e.name.endswith('.txt')]
            
            logger.info(f"  WAV files: {len(wav_files)}")
            logger.info(f"  TXT files: {len(txt_files)}")
            
            # Expected files
            expected = [
                'test_hello_male_en.wav',
                'test_hello_male_en_transcription.txt',
            ]
            
            logger.info("\nChecking critical files:")
            for exp in expected:
                found = any(e.name == exp for e in entries)
                status = "[OK]" if found else "[FAIL]"
                logger.info(f"  {status} {exp}")
            
        except Exception as e:
            logger.error(f"Failed to verify /sd/test_audio: {e}")
            return False
        
        # Verify model files
        logger.info("\nVerifying model files in /sd...")
        try:
            entries = self.proto.list_directory('/sd')
            
            expected_models = [
                'moonshine-tiny-encoder-int8ch.mshn',
                'moonshine-tiny-decoder-int8ch.mshn',
            ]
            
            for exp in expected_models:
                found_entry = next((e for e in entries if e.name == exp), None)
                if found_entry:
                    logger.info(f"  [OK] {exp} - {found_entry.size:,} bytes")
                else:
                    logger.error(f"  [FAIL] {exp} - NOT FOUND")
                    return False
            
        except Exception as e:
            logger.error(f"Failed to verify /sd: {e}")
            return False
        
        # Test file read
        logger.info("\nTesting file read...")
        test_file = '/sd/test_audio/test_hello_male_en_transcription.txt'
        try:
            content = self.proto.read_file(test_file)
            logger.info(f"  [OK] Read {len(content)} bytes")
            logger.info(f"  Content: {content.decode('utf-8', errors='replace')}")
        except Exception as e:
            logger.error(f"  [FAIL] Failed: {e}")
            return False
        
        logger.info("\n" + "="*60)
        logger.info("[OK] VERIFICATION PASSED")
        logger.info("="*60)
        return True
    
    def check_model_integrity(self):
        """Check model file CRC32 integrity."""
        sd_card_dir = Path(__file__).parent.parent.parent / 'sd-card'
        
        models = [
            ('moonshine-tiny-encoder-int8ch.mshn', '/sd/moonshine-tiny-encoder-int8ch.mshn'),
            ('moonshine-tiny-decoder-int8ch.mshn', '/sd/moonshine-tiny-decoder-int8ch.mshn'),
        ]
        
        logger.info("\n" + "="*60)
        logger.info("MODEL INTEGRITY CHECK (CRC32)")
        logger.info("="*60)
        
        all_valid = True
        
        for local_name, remote_path in models:
            local_path = sd_card_dir / local_name
            
            logger.info(f"\nChecking {local_name}...")
            
            # Calculate local CRC
            logger.info(f"  Calculating local CRC32...")
            local_crc = 0
            with open(local_path, 'rb') as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    local_crc = binascii.crc32(chunk, local_crc)
            local_crc &= 0xFFFFFFFF
            logger.info(f"  Local CRC32: 0x{local_crc:08X}")
            
            # Read remote and calculate CRC
            try:
                logger.info(f"  Reading remote file...")
                remote_data = self.proto.read_file(remote_path)
                remote_crc = binascii.crc32(remote_data) & 0xFFFFFFFF
                logger.info(f"  Remote CRC32: 0x{remote_crc:08X}")
                
                if local_crc == remote_crc:
                    logger.info(f"  [OK] CRC32 MATCH - File intact!")
                else:
                    logger.error(f"  [FAIL] CRC32 MISMATCH!")
                    all_valid = False
                    
            except Exception as e:
                logger.error(f"  [FAIL] Error: {e}")
                all_valid = False
        
        logger.info("\n" + "="*60)
        if all_valid:
            logger.info("[OK] ALL MODELS VERIFIED - Ready for inference!")
        else:
            logger.error("[FAIL] MODEL INTEGRITY CHECK FAILED - Re-upload recommended")
        logger.info("="*60)
        
        return all_valid
    
    def download_file(self, remote_path, local_path=None):
        """Download file from ESP32 to local system."""
        if local_path is None:
            local_path = Path(remote_path).name
        
        logger.info(f"Downloading {remote_path}...")
        try:
            data = self.proto.read_file(remote_path)
            
            with open(local_path, 'wb') as f:
                f.write(data)
            
            logger.info(f"[OK] Downloaded {len(data):,} bytes to {local_path}")
            return True
            
        except Exception as e:
            logger.error(f"[FAIL] Failed: {e}")
            return False
    
    def delete_file(self, remote_path):
        """Delete file from ESP32."""
        logger.info(f"Deleting {remote_path}...")
        try:
            self.proto.delete_file(remote_path)
            logger.info(f"[OK] Deleted")
            return True
        except Exception as e:
            logger.error(f"[FAIL] Failed: {e}")
            return False


def main():
    parser = argparse.ArgumentParser(
        description='ESP32-P4 File Manager - Professional SD card file management',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s upload              Upload all files
  %(prog)s upload --verify     Upload and auto-verify
  %(prog)s verify              Verify files only
  %(prog)s models              Check model CRC integrity
  %(prog)s list                List SD card root
  %(prog)s list /sd/test_audio List specific directory
  %(prog)s info                Show device info
  %(prog)s download /sd/file.txt Download file
  %(prog)s delete /sd/old.wav  Delete file
        """
    )
    
    parser.add_argument('command', 
                       choices=['upload', 'verify', 'models', 'list', 'info', 'download', 'delete'],
                       help='Command to execute')
    parser.add_argument('path', nargs='?', help='Path for list/download/delete commands')
    parser.add_argument('--verify', action='store_true', help='Auto-verify after upload')
    parser.add_argument('--port', default='COM13', help='Serial port (default: COM13)')
    parser.add_argument('--baud', type=int, default=921600, help='Baud rate (default: 921600)')
    
    args = parser.parse_args()
    
    # Create manager
    manager = ESP32FileManager(args.port, args.baud)
    
    try:
        # Connect to ESP32
        if not manager.connect():
            return 1
        
        # Execute command
        if args.command == 'info':
            manager.get_device_info()
            
        elif args.command == 'list':
            path = args.path or '/sd'
            manager.list_directory(path)
            
        elif args.command == 'upload':
            if not manager.upload_files(verify=args.verify):
                return 1
                
        elif args.command == 'verify':
            if not manager.verify_upload():
                return 1
                
        elif args.command == 'models':
            if not manager.check_model_integrity():
                return 1
                
        elif args.command == 'download':
            if not args.path:
                logger.error("Path required for download command")
                return 1
            if not manager.download_file(args.path):
                return 1
                
        elif args.command == 'delete':
            if not args.path:
                logger.error("Path required for delete command")
                return 1
            if not manager.delete_file(args.path):
                return 1
        
        return 0
        
    except KeyboardInterrupt:
        logger.info("\nInterrupted by user")
        return 130
        
    except Exception as e:
        logger.exception(f"Unexpected error: {e}")
        return 1
        
    finally:
        manager.disconnect()


if __name__ == '__main__':
    sys.exit(main())
