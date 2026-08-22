import serial
import serial.tools.list_ports
import struct
import binascii
import time
import logging
import threading
from functools import wraps

logger = logging.getLogger(__name__)

# Protocol Constants
PROTO_MAGIC_0 = 0xF1
PROTO_MAGIC_1 = 0x1E
PROTO_MAX_PAYLOAD = 32768
PROTO_FILE_CHUNK_SIZE = 8192  # Increased for optimal throughput with flow control
PROTO_HEADER_SIZE = 9
PROTO_CRC_SIZE = 4

# Command IDs (must match C++)
CMD_HELLO           = 0x01
CMD_DEVICE_INFO     = 0x02
CMD_REBOOT          = 0x03
CMD_ACK             = 0x10
CMD_NACK            = 0x11
CMD_PROGRESS        = 0x12
CMD_LIST_BEGIN      = 0x20
CMD_LIST_ENTRY      = 0x21
CMD_LIST_END        = 0x22
CMD_STAT            = 0x30
CMD_GET_FILE_BEGIN  = 0x31
CMD_GET_FILE_DATA   = 0x32
CMD_GET_FILE_END    = 0x33
CMD_PUT_FILE_BEGIN  = 0x34
CMD_PUT_FILE_DATA   = 0x35
CMD_PUT_FILE_END    = 0x36
CMD_PUT_FILE_ABORT  = 0x37
CMD_DELETE          = 0x40
CMD_RENAME          = 0x41
CMD_MKDIR           = 0x42
CMD_COPY            = 0x43
CMD_SPACE_INFO      = 0x50
CMD_HASH_FILE       = 0x51
CMD_FORMAT_FS       = 0x60

def with_lock(f):
    @wraps(f)
    def wrapper(self, *args, **kwargs):
        with self._lock:
            return f(self, *args, **kwargs)
    return wrapper


class ESP32ProtocolError(Exception):
    def __init__(self, message, error_code=0):
        super().__init__(message)
        self.error_code = error_code


class FileEntry:
    def __init__(self, data: bytes):
        # struct FileEntry: char name[256], uint64_t size, uint32_t timestamp, uint8_t is_dir, uint8_t attr, uint8_t reserved[2]
        unpacked = struct.unpack('<256sQIBB2s', data)
        self.name = unpacked[0].split(b'\0', 1)[0].decode('utf-8')
        self.size = unpacked[1]
        self.timestamp = unpacked[2]
        self.is_directory = bool(unpacked[3])
        self.attributes = unpacked[4]

class FileStat:
    def __init__(self, data: bytes):
        # struct FileStat: uint64_t size, uint32_t timestamp, uint8_t is_dir, uint8_t attr, uint8_t reserved[2]
        unpacked = struct.unpack('<QIBB2s', data)
        self.size = unpacked[0]
        self.timestamp = unpacked[1]
        self.is_directory = bool(unpacked[2])
        self.attributes = unpacked[3]

class DeviceInfo:
    def __init__(self, data: bytes):
        # char device_name[32], uint8_t fw_maj, min, patch, hw_rev, uint32_t flash_sz, flash_fr, psram_sz, psram_fr,
        # uint8_t sd_present, uint64_t sd_size, sd_free, uint8_t sd_type, char sd_fs_type[16],
        # uint32_t max_payload_size, uint32_t optimal_chunk_size
        
        # Determine format based on payload length to maintain backwards compatibility
        if len(data) >= 94:
            unpacked = struct.unpack('<32sBBBBIIIIBQQB16sII', data[:94])
            self.max_payload_size = unpacked[14]
            self.optimal_chunk_size = unpacked[15]
        else:
            unpacked = struct.unpack('<32sBBBBIIIIBQQB16s', data[:86])
            self.max_payload_size = PROTO_MAX_PAYLOAD
            self.optimal_chunk_size = PROTO_FILE_CHUNK_SIZE
            
        self.device_name = unpacked[0].split(b'\0', 1)[0].decode('utf-8')
        self.fw_version = f"{unpacked[1]}.{unpacked[2]}.{unpacked[3]}"
        self.sd_present = bool(unpacked[9])
        self.sd_size = unpacked[10]
        self.sd_free = unpacked[11]
        self.sd_type = unpacked[12]
        self.sd_fs_type = unpacked[13].split(b'\0', 1)[0].decode('utf-8')


class ESP32Protocol:
    def __init__(self):
        self.ser = None
        self.sequence = 0
        self._lock = threading.RLock()
        self.chunk_size = PROTO_FILE_CHUNK_SIZE
        self.max_payload = PROTO_MAX_PAYLOAD

    @with_lock
    def connect(self, port_name: str, baud_rate: int = 921600) -> bool:
        try:
            self.ser = serial.Serial(
                port_name, 
                baud_rate, 
                timeout=1.0,
                rtscts=True  # HW flow ctrl enabled
            )
            self.ser.reset_input_buffer()
            self._drain_boot_noise()
            
            # Force clear any stuck transfer state on the ESP32
            try:
                self._send_frame(CMD_PUT_FILE_END)
                self._wait_ack(timeout_sec=0.5)
            except Exception:
                pass # Ignore if it NACKs (which means no transfer was active)
                
            return self.send_hello()
        except Exception as e:
            logger.error(f"Failed to connect to {port_name}: {e}")
            if self.ser:
                self.ser.close()
                self.ser = None
            return False

    @with_lock
    def auto_detect(self) -> bool:
        ports = serial.tools.list_ports.comports()
        for port in ports:
            logger.info(f"Probing {port.device}...")
            if self.connect(port.device):
                info = self.get_device_info()
                if info and info.device_name.startswith("ESP32-P4"):
                    logger.info(f"Found {info.device_name} on {port.device}!")
                    self.chunk_size = info.optimal_chunk_size
                    self.max_payload = info.max_payload_size
                    logger.info(f"Negotiated payload size: {self.max_payload} bytes, chunk size: {self.chunk_size} bytes")
                    return True
                self.disconnect()
        return False

    @with_lock
    def disconnect(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
        self.ser = None

    def _drain_boot_noise(self, max_wait_ms=5000):
        """Drain boot messages and noise from the serial buffer."""
        start = time.time()
        self.ser.timeout = 0.1
        consecutive_quiet = 0
        total_drained = 0
        
        logger.debug("Draining boot noise...")
        while (time.time() - start) * 1000 < max_wait_ms:
            data = self.ser.read(64)
            if data:
                total_drained += len(data)
                consecutive_quiet = 0
            else:
                consecutive_quiet += 1
                if consecutive_quiet >= 3:  # Increased from 2 to 3
                    break
        
        logger.debug(f"Drained {total_drained} bytes of boot noise")
        self.ser.timeout = 5.0 # Restore normal timeout

    def _calc_crc32(self, data: bytes) -> int:
        return binascii.crc32(data) & 0xFFFFFFFF

    def _send_frame(self, cmd: int, payload: bytes = b'', flags: int = 0):
        if not self.ser or not self.ser.is_open:
            raise ESP32ProtocolError("Not connected")

        # Flush stale RX data to prevent reading old responses (fixes out of sync bugs)
        self.ser.reset_input_buffer()

        length = len(payload)
        # Note: struct.pack('<H', ...) needs to be used for 16-bit fields, B for 8-bit.
        # Header: magic (2), version (1), cmd (1), flags (1), seq (2), len (2)
        # 16-bit sequence and length
        header = struct.pack('<BBBBBHH', PROTO_MAGIC_0, PROTO_MAGIC_1, 0x10, cmd, flags, self.sequence, length)
        self.sequence = (self.sequence + 1) & 0xFFFF
        
        frame = bytearray(header)
        frame.extend(payload)
        
        # Calculate CRC
        crc = binascii.crc32(frame) & 0xFFFFFFFF
        frame.extend(struct.pack('<I', crc))
        
        # Send to ESP32 (single write for minimal latency)
        self.ser.write(bytes(frame))
        self.ser.flush()

    def _receive_frame(self, expected_cmd=None, timeout_sec=5.0):
        if not self.ser or not self.ser.is_open:
            raise ESP32ProtocolError("Not connected")

        self.ser.timeout = timeout_sec
        
        # Sync to magic
        synced = False
        start_time = time.time()
        while time.time() - start_time < timeout_sec:
            b = self.ser.read(1)
            if not b: continue
            if b[0] == PROTO_MAGIC_0:
                b2 = self.ser.read(1)
                if b2 and b2[0] == PROTO_MAGIC_1:
                    synced = True
                    break
        
        if not synced:
            raise ESP32ProtocolError("Sync timeout")

        header_rest = self.ser.read(PROTO_HEADER_SIZE - 2)
        if len(header_rest) != PROTO_HEADER_SIZE - 2:
            raise ESP32ProtocolError("Header timeout")

        version, cmd, flags, seq, length = struct.unpack('<BBBHH', header_rest)
        
        payload = b''
        if length > 0:
            payload = self.ser.read(length)
            if len(payload) != length:
                raise ESP32ProtocolError("Payload timeout")

        crc_bytes = self.ser.read(4)
        if len(crc_bytes) != 4:
            raise ESP32ProtocolError("CRC timeout")
        
        received_crc = struct.unpack('<I', crc_bytes)[0]
        
        full_frame = bytes([PROTO_MAGIC_0, PROTO_MAGIC_1]) + header_rest + payload
        calc_crc = self._calc_crc32(full_frame)
        if calc_crc != received_crc:
            raise ESP32ProtocolError("CRC mismatch")

        if expected_cmd is not None and cmd != expected_cmd:
            if cmd == CMD_NACK:
                raise ESP32ProtocolError("Device returned NACK")
            raise ESP32ProtocolError(f"Unexpected command: {cmd}")

        return cmd, payload

    def _wait_ack(self, timeout_sec=2.0) -> bool:
        start_time = time.time()
        while time.time() - start_time < timeout_sec:
            cmd, _ = self._receive_frame(timeout_sec=timeout_sec)
            if cmd == CMD_PROGRESS:
                continue
            if cmd == CMD_NACK:
                return False
            return cmd == CMD_ACK
        return False

    def _wait_ack_with_error(self, timeout_sec=2.0) -> tuple[bool, int]:
        start_time = time.time()
        while time.time() - start_time < timeout_sec:
            cmd, payload = self._receive_frame(timeout_sec=timeout_sec)
            if cmd == CMD_PROGRESS:
                continue
            if cmd == CMD_NACK:
                err_code = payload[0] if payload else 0
                return False, err_code
            return cmd == CMD_ACK, 0
        return False, 0

    @with_lock
    def send_hello(self) -> bool:
        self._send_frame(CMD_HELLO)
        return self._wait_ack()

    @with_lock
    def get_device_info(self) -> DeviceInfo:
        self._send_frame(CMD_DEVICE_INFO)
        cmd, payload = self._receive_frame(CMD_DEVICE_INFO)
        return DeviceInfo(payload)

    @with_lock
    def list_directory(self, path: str):
        path_bytes = path.encode('utf-8') + b'\0'
        self._send_frame(CMD_LIST_BEGIN, path_bytes)
        self._wait_ack()
        
        entries = []
        while True:
            cmd, payload = self._receive_frame(timeout_sec=10.0)
            if cmd == CMD_LIST_END:
                break
            elif cmd == CMD_LIST_ENTRY:
                entries.append(FileEntry(payload))
            elif cmd == CMD_NACK:
                raise ESP32ProtocolError("NACK during list")
        return entries

    @with_lock
    def get_stat(self, path: str) -> FileStat:
        path_bytes = path.encode('utf-8') + b'\0'
        self._send_frame(CMD_STAT, path_bytes)
        cmd, payload = self._receive_frame(CMD_STAT)
        return FileStat(payload)

    @with_lock
    def read_file(self, path: str) -> bytes:
        path_bytes = path.encode('utf-8') + b'\0'
        self._send_frame(CMD_GET_FILE_BEGIN, path_bytes)
        
        # Read file size from ACK
        cmd, ack_payload = self._receive_frame(CMD_ACK, timeout_sec=5.0)
        
        data = bytearray()
        while True:
            cmd, payload = self._receive_frame(timeout_sec=10.0)
            if cmd == CMD_GET_FILE_END:
                break
            elif cmd == CMD_GET_FILE_DATA:
                data.extend(payload)
            elif cmd == CMD_NACK:
                raise ESP32ProtocolError("NACK during file read")
        return bytes(data)

    @with_lock
    def write_file(self, path: str, data: bytes):
        file_size = len(data)
        path_bytes = path.encode('utf-8') + b'\0'
        
        payload = struct.pack('<Q', file_size) + path_bytes
        self._send_frame(CMD_PUT_FILE_BEGIN, payload)
        if not self._wait_ack():
            raise ESP32ProtocolError("NACK on PUT_FILE_BEGIN")
        
        offset = 0
        try:
            while offset < file_size:
                chunk = data[offset:offset + self.chunk_size]
                
                # Streaming mode: Just send the frame, no wait_ack() needed!
                # Hardware Flow Control handles pacing.
                self._send_frame(CMD_PUT_FILE_DATA, chunk)
                
                # Check for asynchronous NACKs (e.g. SD error) without blocking
                if self.ser.in_waiting > 0:
                    try:
                        async_cmd, _ = self._receive_frame(timeout_sec=0)
                        if async_cmd in (CMD_NACK, CMD_ERROR):
                            raise ESP32ProtocolError("Received async NACK/ERROR during transfer")
                    except serial.SerialTimeoutException:
                        pass
                    except Exception as e:
                        if isinstance(e, ESP32ProtocolError): raise
                
                offset += len(chunk)
                
            self._send_frame(CMD_PUT_FILE_END)
            if not self._wait_ack():
                raise ESP32ProtocolError("NACK on PUT_FILE_END")
        except Exception as e:
            # Try to abort transfer
            try:
                self._send_frame(CMD_PUT_FILE_END)
            except:
                pass
            raise ESP32ProtocolError(f"Error writing file: {e}")

    @with_lock
    def begin_write_stream(self, path: str, file_size: int = 0):
        path_bytes = path.encode('utf-8') + b'\0'
        payload = struct.pack('<Q', file_size) + path_bytes
        self._send_frame(CMD_PUT_FILE_BEGIN, payload)
        if not self._wait_ack():
            raise ESP32ProtocolError("NACK on PUT_FILE_BEGIN")

    @with_lock
    def write_stream_data(self, chunk: bytes):
        """Write file data chunk in streaming mode.
        Hardware flow control will block ser.write() automatically if ESP32 buffers fill up.
        """
        self._send_frame(CMD_PUT_FILE_DATA, chunk)
        
        # Check for asynchronous NACKs without blocking
        if self.ser.in_waiting > 0:
            try:
                cmd, _ = self._receive_frame(timeout_sec=0)
                if cmd in (CMD_NACK, CMD_ERROR):
                    raise ESP32ProtocolError("Received async NACK/ERROR during streaming")
            except serial.SerialTimeoutException:
                pass
            except Exception as e:
                if isinstance(e, ESP32ProtocolError): raise

    @with_lock
    def end_write_stream(self):
        self._send_frame(CMD_PUT_FILE_END)
        if not self._wait_ack():
            raise ESP32ProtocolError("NACK on PUT_FILE_END")

    @with_lock
    def delete_file(self, path: str):
        path_bytes = path.encode('utf-8') + b'\0'
        self._send_frame(CMD_DELETE, path_bytes)
        success, err_code = self._wait_ack_with_error()
        if not success:
            raise ESP32ProtocolError(f"NACK on DELETE: {err_code}", error_code=err_code)

    @with_lock
    def mkdir(self, path: str):
        path_bytes = path.encode('utf-8') + b'\0'
        self._send_frame(CMD_MKDIR, path_bytes)
        if not self._wait_ack():
            raise ESP32ProtocolError("NACK on MKDIR")

    @with_lock
    def format_fs(self, fs_type: int):
        # fs_type: 2 = FAT32, 3 = exFAT
        payload = struct.pack('<B', fs_type)
        self._send_frame(CMD_FORMAT_FS, payload)
        # Formatting can take a long time, especially for SD cards
        success, err_code = self._wait_ack_with_error(timeout_sec=30.0)
        if not success:
            raise ESP32ProtocolError(f"NACK or Timeout on FORMAT_FS: {err_code}", error_code=err_code)
