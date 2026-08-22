import sys
import time
import logging
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))
from esp32_config import get_config
from file_transfer.esp32_protocol import ESP32Protocol, ESP32ProtocolError

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

def upload_large_file(proto, local_path, remote_path, chunk_size=2048):
    file_size = local_path.stat().st_size
    logger.info(f"Uploading {local_path.name}")
    logger.info(f"  Size: {file_size:,} bytes")
    logger.info(f"  Destination: {remote_path}")
    
    try:
        # Check if file exists and has same size
        entries = proto.list_directory(str(Path(remote_path).parent).replace('\\', '/'))
        for e in entries:
            if e.name == local_path.name and e.size == file_size:
                logger.info(f"  Skipping {local_path.name}, already exists with same size.")
                return
    except ESP32ProtocolError:
        pass # Directory might not exist yet
        
    proto.begin_write_stream(remote_path, file_size)
    start_time = time.time()
    bytes_sent = 0
    last_progress = 0
    
    with open(local_path, 'rb') as f:
        while bytes_sent < file_size:
            chunk = f.read(chunk_size)
            if not chunk: break
            proto.write_stream_data(chunk)
            bytes_sent += len(chunk)
            progress = int((bytes_sent / file_size) * 100)
            if progress >= last_progress + 5 or bytes_sent == file_size:
                elapsed = time.time() - start_time
                speed = bytes_sent / elapsed / 1024
                logger.info(f"  Progress: {progress}% ({bytes_sent:,}/{file_size:,} bytes) Speed: {speed:.1f} KB/s")
                last_progress = progress
                
    proto.end_write_stream()
    elapsed = time.time() - start_time
    logger.info(f"  Upload complete in {elapsed:.1f}s")

def ensure_remote_dir(proto, remote_path):
    parts = remote_path.strip('/').split('/')
    current = ""
    for p in parts:
        current += '/' + p
        try:
            proto.mkdir(current)
        except ESP32ProtocolError:
            pass # Probably already exists

def upload_directory(proto, local_dir, remote_base):
    for path in Path(local_dir).rglob('*'):
        if path.is_file():
            # Skip git files and debug JSON files
            if '.git' in path.parts or path.suffix == '.json':
                continue
                
            rel_path = path.relative_to(local_dir)
            remote_path = f"{remote_base}/{rel_path.as_posix()}"
            remote_dir = str(Path(remote_path).parent).replace('\\', '/')
            ensure_remote_dir(proto, remote_dir)
            
            try:
                upload_large_file(proto, path, remote_path)
            except Exception as e:
                logger.error(f"Failed to upload {path}: {e}")

def main():
    config = get_config()
    proto = ESP32Protocol()
    
    logger.info("Connecting to ESP32...")
    if not proto.connect(config.file_port, config.file_baud):
        logger.error("Failed to connect!")
        return 1
        
    logger.info("Connected!")
    
    try:
        # 1. Upload Converted Models
        logger.info(f"\nUploading Converted Models from {config.frvd_models_dir}")
        upload_directory(proto, config.frvd_models_dir, "/sd/models/FireRedVAD-ESP32-P4")
        
        # 2. Upload WAV Files
        wav_dir = Path(__file__).parent.parent.parent / 'example_wave'
        if wav_dir.exists():
            logger.info(f"\nUploading FireRedVAD WAVs from {wav_dir}")
            upload_directory(proto, wav_dir, "/sd/audio/vad")
        else:
            logger.error(f"\nCould not find audio directory: {wav_dir}")
        
        logger.info("\nALL UPLOADS COMPLETE!")
    finally:
        proto.disconnect()

if __name__ == '__main__':
    sys.exit(main())
