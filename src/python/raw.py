import os
import time
import threading
import argparse
from collections import deque
import csv
import shutil
import asyncio
import websockets
import json
import subprocess


import numpy as np
from picamera2 import Picamera2
import tifffile


# Save frames as 8-bit TIFF images (otherwise 16-bit)
save_as_8bit = False
base_dir = "/mnt/ramdisk/CAM/frames"

# Camera settings
RES = (1456, 1088)
FPS = 60
PRE_SECONDS = 2
POST_SECONDS = 2
BUFFER_FRAMES = FPS * (PRE_SECONDS + POST_SECONDS)

# WebSocket trigger
WEBSOCKET_ENABLED = False
WEBSOCKET_SERVER_URL = "ws://mill.lan:8080/trigger"

# Sync to the remote server (rsync)
SYNC_ENABLED = False
SYNC_DEST = "mill.lan:/storage/CAM/"

# Light (intensity) trigger: fires when the frame's mean pixel value rises by more
# than LIGHT_THRESHOLD_RATIO over the moving average of the last
# LIGHT_BASELINE_FRAMES frames.
LIGHT_TRIGGER_ENABLED = True
LIGHT_THRESHOLD_RATIO = 1.3   # 30% increase
LIGHT_BASELINE_FRAMES = FPS // 2  # 0.5 s baseline window

# Directory of the most recent capture (used by the COMPRESS_AND_SYNC command)
OUTPUT_DIR = None


def create_output_directory(base_dir):
    """Create a time-based directory for storing captured data."""
    current_time = time.localtime()
    year = current_time.tm_year
    month = current_time.tm_mon
    day = current_time.tm_mday

    output_dir = os.path.join(base_dir, f"{year}/{month:02}/{day:02}")
    os.makedirs(output_dir, exist_ok=True)

    # Use ISO datetime in UTC instead of a Unix timestamp
    iso_datetime = time.strftime("%Y-%m-%dT%H-%M-%SZ", time.gmtime())
    output_subdir = os.path.join(output_dir, f"rawcap_{iso_datetime}")
    os.makedirs(output_subdir, exist_ok=True)

    print(f"📂 Data will be saved to: {output_subdir}")
    return output_subdir


def save_metadata_csv(output_dir, buffer, timestamps, trigger_time):
    """Save per-frame metadata to a CSV file."""
    csv_path = os.path.join(output_dir, "metadata.csv")
    with open(csv_path, mode="w", newline="") as csv_file:
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow([
            "Timestamp", "Frame Number", "Relative Time (s)",
            "Max Pixel Value", "Min Pixel Value", "Mean Pixel Value", "Median Pixel Value"
        ])  # CSV header

        for i, (frame, timestamp) in enumerate(zip(buffer, timestamps)):
            frame_cleaned = frame
            relative_time = timestamp - trigger_time
            max_pixel = frame_cleaned.max()
            min_pixel = frame_cleaned.min()
            mean_pixel = frame_cleaned.mean()
            median_pixel = np.median(frame_cleaned)

            csv_writer.writerow([
                timestamp, i, relative_time,
                max_pixel, min_pixel, mean_pixel, median_pixel
            ])
    print(f"✅ Metadata saved to: {csv_path}")


def save_frames(output_dir, buffer, timestamps, trigger_time, save_as_8bit):
    """Save frames as TIFF files with metadata in the header."""
    for i, (frame, timestamp) in enumerate(zip(buffer, timestamps)):
        # Compute metadata for each frame
        relative_time = float(timestamp - trigger_time)
        max_pixel = float(frame.max())
        min_pixel = float(frame.min())
        mean_pixel = float(frame.mean())
        median_pixel = float(np.median(frame))

        # Build metadata dictionary
        metadata = {
            'Timestamp': float(timestamp),
            'FrameNumber': int(i),
            'RelativeTime': relative_time,
            'MaxPixelValue': max_pixel,
            'MinPixelValue': min_pixel,
            'MeanPixelValue': mean_pixel,
            'MedianPixelValue': median_pixel
        }

        tiff_path = os.path.join(output_dir, f"frame_{i:04d}.tiff")

        if save_as_8bit:
            frame_8bit = (frame >> 8).astype(np.uint8)
            tifffile.imwrite(tiff_path, frame_8bit, metadata=metadata)
        else:
            tifffile.imwrite(tiff_path, frame.astype(np.uint16), metadata=metadata)

    print(f"✅ Saved {len(buffer)} TIFF frames to: {output_dir}")


def save_snapshot(frame, base_dir, timestamp=None):
    """Save a single frame as TIFF with metadata into a year/month/day structure."""
    if timestamp is None:
        timestamp = time.time()

    # Current time for organizing files
    current_time = time.localtime()
    year = current_time.tm_year
    month = current_time.tm_mon
    day = current_time.tm_mday

    # Build path as year/month/day
    snapshot_base_dir = "/mnt/ramdisk/CAM/snapshots"
    snapshot_dir = os.path.join(snapshot_base_dir, f"{year}/{month:02}/{day:02}")
    os.makedirs(snapshot_dir, exist_ok=True)

    tiff_timestamp = int(timestamp)
    tiff_path = os.path.join(snapshot_dir, f"snapshot_{tiff_timestamp}.tiff")

    # Compute metadata
    max_pixel = float(frame.max())  # Convert to native Python type
    min_pixel = float(frame.min())
    mean_pixel = float(frame.mean())
    median_pixel = float(np.median(frame))

    metadata = {
        'Timestamp': float(timestamp),
        'MaxPixelValue': max_pixel,
        'MinPixelValue': min_pixel,
        'MeanPixelValue': mean_pixel,
        'MedianPixelValue': median_pixel
    }

    if save_as_8bit:
        frame_8bit = (frame >> 8).astype(np.uint8)
        tifffile.imwrite(tiff_path, frame_8bit, metadata=metadata)
    else:
        tifffile.imwrite(tiff_path, frame.astype(np.uint16), metadata=metadata)

    print(f"📸 Snapshot saved: {tiff_path}")

    # Sync snapshot to the remote server
    sync_to_remote(snapshot_base_dir)


def compress_directory(output_dir, remove_source=False):
    """Compress a directory into a .zip archive using the zip command (fast)."""
    archive_path = f"{output_dir}.zip"
    try:
        subprocess.run(["zip", "-r", archive_path, output_dir], check=True)
        print(f"✅ Directory {output_dir} compressed to: {archive_path}")

        if remove_source:
            shutil.rmtree(output_dir)
            print(f"🗑️ Source directory {output_dir} removed.")
    except subprocess.CalledProcessError as e:
        print(f"❌ Error compressing directory: {e}")


def sync_to_remote(source_dir):
    """Move data to the remote server using rsync."""
    if not SYNC_ENABLED:
        return
    try:
        print(f"🔄 Syncing data from {source_dir} to: {SYNC_DEST}")
        subprocess.run([
            "rsync", "-avz", "--remove-source-files",
            source_dir, SYNC_DEST
        ], check=True)
        print(f"✅ Data successfully synced to: {SYNC_DEST}")
    except subprocess.CalledProcessError as e:
        print(f"❌ Error syncing data: {e}")


def wait_for_trigger():
    """Wait (forever) for Enter key presses to fire the trigger."""
    while True:
        input("▶ Press Enter to TRIGGER...\n")
        triggered.set()
        time.sleep(0.1)  # Short delay to avoid repeated triggers


async def websocket_client():
    """WebSocket client receiving triggers and compress/sync commands."""
    async with websockets.connect(WEBSOCKET_SERVER_URL) as websocket:
        print(f"🌐 Connected to WebSocket server at {WEBSOCKET_SERVER_URL}")
        while True:
            try:
                raw = await websocket.recv()
                ts = time.strftime("%H:%M:%S", time.localtime())
                print(f"[{ts}] 📡 WS raw: {raw}")
                try:
                    message = json.loads(raw) if isinstance(raw, str) else raw
                except json.JSONDecodeError:
                    print(f"[{ts}] ⚠️ Failed to parse JSON: {raw}")
                    continue
                print(f"[{ts}] 📡 Received message: {message}")

                if message.get('type') == "TRIGG":
                    print("📡 WebSocket TRIGGER received!")
                    triggered.set()

                elif message.get('type') == "COMPRESS_AND_SYNC":
                    print("📡 WebSocket request to compress and sync data!")
                    if OUTPUT_DIR is None:
                        print("⚠️ No capture yet, nothing to compress.")
                        continue
                    compress_directory(OUTPUT_DIR, remove_source=True)
                    sync_to_remote(OUTPUT_DIR + "/../")
            except websockets.ConnectionClosed:
                print("⚠️ WebSocket connection closed.")
                break


def main():
    global OUTPUT_DIR
    last_snapshot_time = 0
    snapshot_interval = 30  # Interval in seconds

    # Run the WebSocket client in a separate thread (if enabled)
    if WEBSOCKET_ENABLED:
        threading.Thread(target=lambda: asyncio.run(websocket_client()), daemon=True).start()
        print(f"🌐 WebSocket trigger: {WEBSOCKET_SERVER_URL}")
    else:
        print("🌐 WebSocket trigger: disabled")

    if LIGHT_TRIGGER_ENABLED:
        print(f"💡 Light trigger: enabled (ratio {LIGHT_THRESHOLD_RATIO}, "
              f"baseline {LIGHT_BASELINE_FRAMES} frames)")
    else:
        print("💡 Light trigger: disabled")

    while True:
        try:
            print(f"🔁 Buffering the last {PRE_SECONDS + POST_SECONDS}s of RAW data ({RES}, {FPS} FPS)... [press Enter to trigger]")
            buffer.clear()
            timestamps.clear()
            triggered.clear()
            mean_history = deque(maxlen=LIGHT_BASELINE_FRAMES)

            while not triggered.is_set():
                timestamp = time.time()
                frame = (picam2.capture_array("raw").view(np.uint16) << 6)
                buffer.append(frame)
                timestamps.append(timestamp)

                # Light trigger: rapid rise of the mean pixel value
                if LIGHT_TRIGGER_ENABLED:
                    frame_mean = frame.mean()
                    if len(mean_history) == LIGHT_BASELINE_FRAMES:
                        baseline = np.mean(mean_history)
                        if baseline > 0 and frame_mean > baseline * LIGHT_THRESHOLD_RATIO:
                            print(f"💡 Light trigger! mean={frame_mean:.0f} > {LIGHT_THRESHOLD_RATIO}× baseline={baseline:.0f}")
                            triggered.set()
                    mean_history.append(frame_mean)

                # Check whether the snapshot interval has elapsed
                if timestamp - last_snapshot_time >= snapshot_interval:
                    # Save a copy of the current frame
                    save_snapshot(frame.copy(), base_dir, timestamp)
                    last_snapshot_time = timestamp

            # Create a new directory for each trigger
            OUTPUT_DIR = create_output_directory(base_dir)

            print(f"📸 TRIGGER! Recording {POST_SECONDS}s more...")
            trigger_time = time.time()
            for _ in range(FPS * POST_SECONDS):
                timestamp = time.time()
                frame = (picam2.capture_array("raw").view(np.uint16) << 6)
                buffer.append(frame)
                timestamps.append(timestamp)

            save_metadata_csv(OUTPUT_DIR, buffer, timestamps, trigger_time)
            save_frames(OUTPUT_DIR, buffer, timestamps, trigger_time, save_as_8bit)

            if len(timestamps) > 1:
                span_s = timestamps[-1] - timestamps[0]
                measured_fps = (len(timestamps) - 1) / span_s if span_s > 0 else 0.0
                print(f"📈 Measured FPS: {measured_fps:.2f} (target: {FPS} FPS)")

            # compress_directory(OUTPUT_DIR, remove_source=True)
            sync_to_remote(base_dir)
            print(f"\n🎬 Capture finished — {len(buffer)} frames saved to: {OUTPUT_DIR}")
            print("⏳ Ready for the next trigger...\n")

        except KeyboardInterrupt:
            print("⛔ Interrupted by user.")
            break


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="CircularPiCam — circular RAW buffer with triggers")
    parser.add_argument("--websocket", action="store_true", default=WEBSOCKET_ENABLED,
                        help="Enable the WebSocket trigger")
    parser.add_argument("--no-websocket", dest="websocket", action="store_false",
                        help="Disable the WebSocket trigger")
    parser.add_argument("--websocket-url", default=WEBSOCKET_SERVER_URL, metavar="URL",
                        help=f"WebSocket server URL (default: {WEBSOCKET_SERVER_URL})")
    parser.add_argument("--sync", action="store_true", default=SYNC_ENABLED,
                        help="Enable rsync synchronization after each trigger")
    parser.add_argument("--no-sync", dest="sync", action="store_false",
                        help="Disable rsync synchronization")
    parser.add_argument("--sync-dest", default=SYNC_DEST, metavar="DEST",
                        help=f"rsync destination (default: {SYNC_DEST})")
    parser.add_argument("--light-trigger", action="store_true", default=LIGHT_TRIGGER_ENABLED,
                        help="Enable the light-intensity trigger (default)")
    parser.add_argument("--no-light-trigger", dest="light_trigger", action="store_false",
                        help="Disable the light-intensity trigger")
    parser.add_argument("--light-threshold", type=float, default=LIGHT_THRESHOLD_RATIO, metavar="RATIO",
                        help=f"Trigger when frame mean > baseline × RATIO (default: {LIGHT_THRESHOLD_RATIO})")
    parser.add_argument("--light-baseline-frames", type=int, default=LIGHT_BASELINE_FRAMES, metavar="N",
                        help=f"Frames averaged for the baseline (default: {LIGHT_BASELINE_FRAMES})")
    args = parser.parse_args()

    # Override global configuration from arguments
    WEBSOCKET_ENABLED = args.websocket
    WEBSOCKET_SERVER_URL = args.websocket_url
    SYNC_ENABLED = args.sync
    SYNC_DEST = args.sync_dest
    LIGHT_TRIGGER_ENABLED = args.light_trigger
    LIGHT_THRESHOLD_RATIO = args.light_threshold
    LIGHT_BASELINE_FRAMES = args.light_baseline_frames

    # Camera initialization
    picam2 = Picamera2()

    # Detect mono vs. color sensor
    is_mono = "AwbEnable" not in picam2.camera_controls
    print(f"📷 Sensor: {'mono' if is_mono else 'color (AWB available)'}")

    FRAME_US = int(1e6 // FPS)

    cam_controls = {
        "FrameDurationLimits": (FRAME_US, FRAME_US),
        "FrameRate": 60,
        "AeEnable": True,
        "ExposureValue": 0,
        #"AeExposureMode": controls.AeExposureModeEnum.Long,
        #"AnalogueGain": 1.0,
        #"DigitalGain": 2.0,
    }
    if not is_mono:
        cam_controls["AwbEnable"] = False

    config = picam2.create_video_configuration(
        raw={"size": RES, "format": "R10"},
        encode=None,
        controls=cam_controls,
    )
    picam2.configure(config)
    picam2.start()

    # Circular buffer for frames and timestamps
    buffer = deque(maxlen=BUFFER_FRAMES)
    timestamps = deque(maxlen=BUFFER_FRAMES)

    # Keyboard trigger
    triggered = threading.Event()
    threading.Thread(target=wait_for_trigger, daemon=True).start()

    try:
        main()
    finally:
        picam2.stop()
