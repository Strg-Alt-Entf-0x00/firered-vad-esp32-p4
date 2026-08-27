import serial
import time
import sys
import winsound

PORT = "COM4"
BAUD = 115200

# Test sequence configuration
MICS = ["inmp441", "es8311"]  # Test both microphones

# Distances and their durations in seconds
DISTANCES = [
    ("silence", 15),  # 15 seconds silence
    ("3m", 5),
    ("2m", 5),
    ("1m", 5),
    ("0.5m", 5),
    ("0.1m", 5)
]

def wait_for_prompt(ser, timeout=30):
    start = time.time()
    buffer = ""
    while time.time() - start < timeout:
        if ser.in_waiting:
            chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            buffer += chunk
            sys.stdout.write(chunk)
            sys.stdout.flush()
            if "firevad>" in buffer:
                return True
        time.sleep(0.05)
    return False

def send_command(ser, cmd):
    print(f"\n[Sending command] -> {cmd}")
    ser.reset_input_buffer()
    cmd = cmd + "\n"
    # Send in chunks to prevent USB-Serial buffer overflow (64 bytes)
    for i in range(0, len(cmd), 32):
        ser.write(cmd[i:i+32].encode('utf-8'))
        ser.flush()
        time.sleep(0.1)

def countdown_and_beep(seconds):
    print(f"\nMove to position... Recording starts in {seconds} seconds!")
    for i in range(seconds, 0, -1):
        print(f"{i}...", end=" ", flush=True)
        time.sleep(1)
        winsound.Beep(1000, 100)
    print("\n")

def main():
    print("=" * 60)
    print(" FIRE-RED-VAD - AUTOMATED MICROPHONE TEST SUITE ")
    print("=" * 60)
    print(f"Connecting to ESP32 on {PORT}...")

    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.1)
    except Exception as e:
        print(f"[ERROR] Could not open {PORT}: {e}")
        print("Is idf.py monitor still running? Close it first.")
        sys.exit(1)

    print("Waiting for console (press Reset on ESP32 if needed)...")
    ser.write(b'\n')
    if not wait_for_prompt(ser, timeout=5):
        print("\n[ERROR] Timeout! Please reset the ESP32 or ensure it is ready.")
        ser.close()
        sys.exit(1)

    # Initial setup
    send_command(ser, "agc_enable 1")
    wait_for_prompt(ser)

    for mic in MICS:
        print(f"\n{'#'*60}")
        print(f" SWITCHING TO MICROPHONE: {mic.upper()}")
        print(f"{'#'*60}\n")

        send_command(ser, f"mic_select {mic}")
        wait_for_prompt(ser)

        if mic == "inmp441":
            print("\n[INFO] Setting default software gain (1.0x) for INMP441 to test raw AGC.")
            send_command(ser, "mic_gain -s 1.0")
            wait_for_prompt(ser)
            gain_tag = "sw1.0x-agc1"
        elif mic == "es8311":
            print("\n[INFO] Setting hardware gain (42 dB) for ES8311 to test AGC.")
            send_command(ser, "mic_gain -h 42")
            wait_for_prompt(ser)
            gain_tag = "42db-agc1"

        for dist, duration in DISTANCES:
            filename = f"{mic}-h60cm-mono-{gain_tag}-{dist}.wav"
            print(f"\n{'='*40}")
            print(f" NEXT TEST: {mic.upper()} - Distance: {dist}")
            print(f" Saving as: {filename}")
            print(f" Duration: {duration} seconds")
            print(f"{'='*40}")

            user_input = input("Press [ENTER] to start (or 's' to skip, 'q' to quit): ").strip().lower()
            if user_input == 'q':
                print("Test aborted.")
                ser.close()
                sys.exit(0)
            elif user_input == 's':
                print("Test skipped.")
                continue

            # Countdown
            countdown_and_beep(3)

            # Start Recording
            print(f"RECORDING ({duration} seconds)... PLEASE SPEAK!")
            send_command(ser, f"record_mic {filename} {duration}")

            # Wait for completion
            wait_for_prompt(ser, timeout=duration + 10)

            print("RECORDING COMPLETE!\n")

    print("\nALL TESTS COMPLETE! Download the files using the file_manager tool.")
    ser.close()

if __name__ == "__main__":
    main()
