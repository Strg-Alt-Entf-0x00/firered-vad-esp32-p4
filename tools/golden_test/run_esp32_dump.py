import serial
import time
import sys

PORT = "COM4"
BAUD = 115200

def send_command(ser, cmd):
    print(f"\n[ESP32] < {cmd}")
    ser.reset_input_buffer()
    cmd = cmd + "\n"
    # Send in chunks of 32 bytes with delay to avoid USB-Serial/JTAG buffer overflow
    for i in range(0, len(cmd), 32):
        ser.write(cmd[i:i+32].encode('utf-8'))
        ser.flush()
        time.sleep(0.1)
    time.sleep(0.5)

def wait_for_prompt(ser, timeout=30, expected_marker="firevad>"):
    start = time.time()
    buffer = ""
    while time.time() - start < timeout:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            sys.stdout.write(chunk)
            sys.stdout.flush()
            buffer += chunk
            if expected_marker in buffer and ("firevad>" in chunk or "firevad>" in buffer.split('\n')[-1]):
                time.sleep(1.5) 
                if ser.in_waiting:
                    ser.read(ser.in_waiting)
                return True
        time.sleep(0.05)
    return False

def main():
    print(f"Connecting to ESP32 on {PORT} at {BAUD} baud...")
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.1)
    except Exception as e:
        print(f"Failed to open {PORT}.")
        sys.exit(1)
        
    ser.reset_input_buffer()
    print("Waiting for boot and calibration...")
    ser.write(b'\n')
    
    if not wait_for_prompt(ser, timeout=10):
        print("Timeout waiting for initial prompt.")
        ser.close()
        sys.exit(1)
    
    # 1. Load FP32 model
    send_command(ser, "vad_model_load /sd/models/FireRedVAD-ESP32-P4/stream-vad/fp32/firered-stream-vad-fp32.frvd")
    print("\nWaiting for model load to complete...")
    
    if not wait_for_prompt(ser, timeout=20):
        print("Timeout waiting for model load.")
        ser.close()
        sys.exit(1)
        
    # 2. Run Dump
    send_command(ser, "vad_dump_golden /sd/audio/vad/speech-welcome-constant-volume.wav")
    print("\nWaiting for dump to complete...")
    
    if not wait_for_prompt(ser, timeout=30):
        print("Timeout waiting for dump.")
        ser.close()
        sys.exit(1)
        
    print("\nESP32 Dump generation successful!")
    ser.close()

if __name__ == "__main__":
    main()
