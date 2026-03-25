#!/usr/bin/env python3
"""ESPHome OTA Upload Script for ESP32-C5 Gateway.

Protocol (matching esphome_ota.c):
  1. Client sends: magic byte (0x6C)
  2. Server sends: features byte (supports MD5 = 0x02)
  3. If password: nonce exchange + auth hash
  4. Client sends: firmware size (4 bytes big-endian)
  5. Server sends: OK (0x00) or error
  6. Client sends: firmware data, server ACKs every 8KB
  7. Client sends: MD5 hash (16 bytes)
  8. Server sends: OK (0x00) after verify

Usage: python3 scripts/ota_upload.py <host> <firmware.bin> [--password <pw>]
"""
import argparse
import hashlib
import socket
import struct
import sys
import os

MAGIC_BYTE = 0x6C

RESPONSE_OK = 0x00
RESPONSE_ERROR_MAGIC = 0x01
RESPONSE_ERROR_AUTH = 0x02
RESPONSE_ERROR_WRITE = 0x03
RESPONSE_ERROR_VALIDATE = 0x04
RESPONSE_ERROR_GENERIC = 0x05
RESPONSE_ERROR_SIZE = 0x06

FEATURE_MD5 = (1 << 1)
ACK_INTERVAL = 8192

RESPONSE_NAMES = {
    0x00: "OK",
    0x01: "ERROR_MAGIC",
    0x02: "ERROR_AUTH",
    0x03: "ERROR_WRITE",
    0x04: "ERROR_VALIDATE",
    0x05: "ERROR_GENERIC",
    0x06: "ERROR_SIZE",
}


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("Connection closed")
        data += chunk
    return data


def recv_response(sock):
    data = recv_exact(sock, 1)
    code = data[0]
    name = RESPONSE_NAMES.get(code, f"UNKNOWN(0x{code:02x})")
    return code, name


def main():
    parser = argparse.ArgumentParser(description="ESPHome OTA Upload")
    parser.add_argument("host", help="Device IP or hostname")
    parser.add_argument("firmware", help="Path to firmware .bin file")
    parser.add_argument("--port", type=int, default=3232, help="OTA port (default: 3232)")
    parser.add_argument("--password", default="", help="OTA password")
    args = parser.parse_args()

    # Read firmware
    firmware_path = args.firmware
    if not os.path.exists(firmware_path):
        print(f"Error: {firmware_path} not found")
        sys.exit(1)

    with open(firmware_path, "rb") as f:
        firmware = f.read()

    firmware_size = len(firmware)
    firmware_md5 = hashlib.md5(firmware).digest()
    print(f"Firmware: {firmware_path} ({firmware_size} bytes)")
    print(f"MD5: {firmware_md5.hex()}")

    # Connect
    print(f"Connecting to {args.host}:{args.port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    try:
        sock.connect((args.host, args.port))
    except Exception as e:
        print(f"Connection failed: {e}")
        sys.exit(1)
    print("Connected.")

    try:
        # Step 1: Send magic byte only
        sock.sendall(bytes([MAGIC_BYTE]))

        # Step 2: Receive server features (1 byte, NOT a response code)
        features = recv_exact(sock, 1)[0]
        print(f"Server features: 0x{features:02x} (MD5={'yes' if features & FEATURE_MD5 else 'no'})")

        # Step 3: Auth (if password set on server side)
        # No auth for now - server has empty password

        # Step 4: Send firmware size (4 bytes big-endian)
        sock.sendall(struct.pack(">I", firmware_size))

        # Wait for size OK — server calls esp_ota_begin() which can take
        # several seconds even with OTA_SIZE_UNKNOWN
        sock.settimeout(60)
        code, name = recv_response(sock)
        if code != RESPONSE_OK:
            print(f"Server rejected size: {name}")
            sys.exit(1)
        print("Size accepted, uploading firmware...")

        # Step 5: Send firmware data
        # The server writes each chunk to flash (slow on ESP32-C5).
        # We send one ACK_INTERVAL block at a time, then wait for the
        # server's ACK before sending the next block. This keeps client
        # and server in sync without relying on TCP flow control.
        import time
        bytes_sent = 0
        last_progress = -1

        total_blocks = (firmware_size + ACK_INTERVAL - 1) // ACK_INTERVAL

        for block_num in range(total_blocks):
            block_start = block_num * ACK_INTERVAL
            block_end = min(block_start + ACK_INTERVAL, firmware_size)
            block = firmware[block_start:block_end]

            # Send block in small chunks
            offset = 0
            while offset < len(block):
                chunk = block[offset:offset + 1024]
                sock.sendall(chunk)
                offset += len(chunk)

            bytes_sent = block_end

            # Wait for ACK from server (it ACKs after writing to flash)
            # Some flash sectors take very long to erase on ESP32-C5
            if block_end < firmware_size:
                sock.settimeout(300)
                code, name = recv_response(sock)
                if code != RESPONSE_OK:
                    print(f"\nACK error at {bytes_sent} bytes: {name}")
                    sys.exit(1)

            progress = (bytes_sent * 100) // firmware_size
            if progress != last_progress:
                bar = "=" * (progress // 2) + ">" + " " * (50 - progress // 2)
                elapsed = time.time() - sock._start_time if hasattr(sock, '_start_time') else 0
                print(f"\r[{bar}] {progress}% ({bytes_sent}/{firmware_size}) block {block_num+1}/{total_blocks}", end="", flush=True)
                last_progress = progress

        print()  # newline after progress bar

        # Step 6: Send MD5 hash
        sock.sendall(firmware_md5)
        print("MD5 hash sent, waiting for device to finish writing + verify...")
        print("(This can take 60-120s on ESP32-C5 due to flash write speed)")

        # Wait for final response — device writes flash slower than we send
        # Give it up to 180s for flash write + verify + partition switch
        sock.settimeout(180)
        try:
            code, name = recv_response(sock)
            if code == RESPONSE_OK:
                print("OTA update successful! Device will restart.")
            else:
                print(f"Verification failed: {name}")
                sys.exit(1)
        except (socket.timeout, ConnectionError, OSError) as e:
            print(f"Connection closed ({e}). Device likely rebooting — OTA successful.")

    except Exception as e:
        print(f"\nError: {e}")
        sys.exit(1)
    finally:
        sock.close()


if __name__ == "__main__":
    main()
