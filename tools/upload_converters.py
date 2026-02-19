#!/usr/bin/env python3
"""
Upload Converter DB to ESP32 Gateway via MQTT

Reads JSON converter files from a local directory and uploads them
to the device via MQTT bridge requests.

Usage:
    python3 upload_converters.py --broker 192.168.1.100 --dir data/converters/
    python3 upload_converters.py --broker mqtt.local --dir data/converters/ --topic zigbee2mqtt
"""

import argparse
import base64
import json
import os
import sys
import time
from pathlib import Path

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("Error: paho-mqtt not installed. Run: pip3 install paho-mqtt", file=sys.stderr)
    sys.exit(1)


class ConverterUploader:
    def __init__(self, broker, port, topic_base, username=None, password=None):
        self.broker = broker
        self.port = port
        self.topic_base = topic_base
        self.username = username
        self.password = password
        self.connected = False
        self.upload_results = {}
        self.client = mqtt.Client(client_id="converter_uploader", protocol=mqtt.MQTTv311)

        if username:
            self.client.username_pw_set(username, password)

        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            self.connected = True
            # Subscribe to response topic
            resp_topic = f"{self.topic_base}/bridge/response/converter_db/update"
            client.subscribe(resp_topic)
            print(f"Connected to {self.broker}:{self.port}")
        else:
            print(f"Connection failed with code {rc}", file=sys.stderr)

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode())
            status = payload.get("status", "unknown")
            data = payload.get("data", {})
            filename = data.get("file", "unknown")
            self.upload_results[filename] = status
            if status == "ok":
                print(f"  OK: {filename}")
            else:
                error = data.get("error", "unknown error")
                print(f"  FAIL: {filename} — {error}", file=sys.stderr)
        except Exception as e:
            print(f"  Error parsing response: {e}", file=sys.stderr)

    def connect(self):
        self.client.connect(self.broker, self.port, keepalive=60)
        self.client.loop_start()

        # Wait for connection
        timeout = 10
        while not self.connected and timeout > 0:
            time.sleep(0.1)
            timeout -= 0.1

        if not self.connected:
            print("Failed to connect to MQTT broker", file=sys.stderr)
            return False
        return True

    def upload_file(self, filepath):
        """Upload a single JSON file to the device."""
        filename = os.path.basename(filepath)

        try:
            with open(filepath, 'rb') as f:
                data = f.read()
        except Exception as e:
            print(f"  Error reading {filepath}: {e}", file=sys.stderr)
            return False

        # Encode as base64
        b64_data = base64.b64encode(data).decode('ascii')

        payload = json.dumps({
            "file": filename,
            "data": b64_data,
        })

        topic = f"{self.topic_base}/bridge/request/converter_db/update"
        result = self.client.publish(topic, payload, qos=1)

        if result.rc != mqtt.MQTT_ERR_SUCCESS:
            print(f"  Failed to publish {filename}: rc={result.rc}", file=sys.stderr)
            return False

        return True

    def upload_directory(self, dir_path):
        """Upload all JSON files from a directory."""
        json_files = sorted(Path(dir_path).glob("*.json"))
        if not json_files:
            print(f"No JSON files found in {dir_path}", file=sys.stderr)
            return False

        # Upload index.json last
        index_file = None
        other_files = []
        for f in json_files:
            if f.name == "index.json":
                index_file = f
            else:
                other_files.append(f)

        total = len(other_files) + (1 if index_file else 0)
        print(f"Uploading {total} files from {dir_path}...")

        # Upload manufacturer files first
        for i, filepath in enumerate(other_files, 1):
            size = filepath.stat().st_size
            print(f"  [{i}/{total}] {filepath.name} ({size:,} bytes)")
            self.upload_file(filepath)
            time.sleep(0.1)  # Small delay between files

        # Upload index.json last
        if index_file:
            size = index_file.stat().st_size
            print(f"  [{total}/{total}] index.json ({size:,} bytes)")
            self.upload_file(index_file)

        # Wait for responses
        print("Waiting for device acknowledgments...")
        time.sleep(3)

        # Report results
        ok = sum(1 for s in self.upload_results.values() if s == "ok")
        fail = sum(1 for s in self.upload_results.values() if s != "ok")
        no_resp = total - len(self.upload_results)

        print(f"\nResults: {ok} OK, {fail} failed, {no_resp} no response")
        return fail == 0 and no_resp == 0

    def disconnect(self):
        self.client.loop_stop()
        self.client.disconnect()


def main():
    parser = argparse.ArgumentParser(description="Upload Converter DB via MQTT")
    parser.add_argument("--broker", required=True, help="MQTT broker hostname/IP")
    parser.add_argument("--port", type=int, default=1883, help="MQTT port (default: 1883)")
    parser.add_argument("--topic", default="zigbee2mqtt", help="Base topic (default: zigbee2mqtt)")
    parser.add_argument("--username", help="MQTT username")
    parser.add_argument("--password", help="MQTT password")
    parser.add_argument("--dir", required=True, help="Directory containing converter JSON files")
    parser.add_argument("--file", help="Upload a single file instead of directory")
    args = parser.parse_args()

    uploader = ConverterUploader(
        broker=args.broker,
        port=args.port,
        topic_base=args.topic,
        username=args.username,
        password=args.password,
    )

    if not uploader.connect():
        sys.exit(1)

    try:
        if args.file:
            if not os.path.isfile(args.file):
                print(f"Error: {args.file} not found", file=sys.stderr)
                sys.exit(1)
            success = uploader.upload_file(args.file)
            time.sleep(2)
        else:
            if not os.path.isdir(args.dir):
                print(f"Error: {args.dir} is not a directory", file=sys.stderr)
                sys.exit(1)
            success = uploader.upload_directory(args.dir)
    finally:
        uploader.disconnect()

    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
