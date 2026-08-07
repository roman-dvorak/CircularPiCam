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


# Nastavení pro ukládání jako 8bitové TIFF obrázky
save_as_8bit = False
base_dir = "/mnt/ramdisk/CAM/frames"

# Nastavení kamery
RES = (1456, 1088)
FPS = 60
PRE_SECONDS = 2
POST_SECONDS = 2
BUFFER_FRAMES = FPS * (PRE_SECONDS + POST_SECONDS)

# WebSocket trigger
WEBSOCKET_ENABLED = False
WEBSOCKET_SERVER_URL = "ws://mill.lan:8080/trigger"

# Synchronizace na vzdálený server (rsync)
SYNC_ENABLED = False
SYNC_DEST = "mill.lan:/storage/CAM/"


def create_output_directory(base_dir):
    """Vytvoří dynamickou složku pro ukládání dat podle aktuálního času."""
    current_time = time.localtime()
    year = current_time.tm_year
    month = current_time.tm_mon
    day = current_time.tm_mday

    output_dir = os.path.join(base_dir, f"{year}/{month:02}/{day:02}")
    os.makedirs(output_dir, exist_ok=True)

    # Použití ISO datetime v UTC místo Unix timestamp
    iso_datetime = time.strftime("%Y-%m-%dT%H-%M-%SZ", time.gmtime())
    output_subdir = os.path.join(output_dir, f"rawcap_{iso_datetime}")
    os.makedirs(output_subdir, exist_ok=True)

    print(f"📂 Data budou ukládána do: {output_subdir}")
    return output_subdir


def save_metadata_csv(output_dir, buffer, timestamps, trigger_time):
    """Uloží metadata snímků do CSV souboru."""
    csv_path = os.path.join(output_dir, "metadata.csv")
    with open(csv_path, mode="w", newline="") as csv_file:
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow([
            "Timestamp", "Frame Number", "Relative Time (s)", 
            "Max Pixel Value", "Min Pixel Value", "Mean Pixel Value", "Median Pixel Value"
        ])  # Hlavička CSV

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
    print(f"✅ Metadata uložena do: {csv_path}")


def save_frames(output_dir, buffer, timestamps, trigger_time, save_as_8bit):
    """Uloží snímky jako TIFF soubory s metadaty v hlavičce."""
    for i, (frame, timestamp) in enumerate(zip(buffer, timestamps)):
        # Výpočet metadat pro každý snímek
        relative_time = float(timestamp - trigger_time)
        max_pixel = float(frame.max())
        min_pixel = float(frame.min())
        mean_pixel = float(frame.mean())
        median_pixel = float(np.median(frame))
        
        # Vytvoření slovníku metadat
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
    
    print(f"✅ Uloženo {len(buffer)} TIFF snímků do: {output_dir}")


def save_snapshot(frame, base_dir, timestamp=None):
    """Uloží samostatný snímek jako TIFF s metadaty do hierarchické struktury."""
    if timestamp is None:
        timestamp = time.time()
    
    # Získání aktuálního času pro organizaci souborů
    current_time = time.localtime()
    year = current_time.tm_year
    month = current_time.tm_mon
    day = current_time.tm_mday
    
    # Vytvoření cesty podle struktury rok/měsíc/den
    snapshot_base_dir = "/mnt/ramdisk/CAM/snapshots"
    snapshot_dir = os.path.join(snapshot_base_dir, f"{year}/{month:02}/{day:02}")
    os.makedirs(snapshot_dir, exist_ok=True)
    
    tiff_timestamp = int(timestamp)
    tiff_path = os.path.join(snapshot_dir, f"snapshot_{tiff_timestamp}.tiff")
    
    # Výpočet metadat
    max_pixel = float(frame.max())  # Převod na standardní Python typ
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
    
    print(f"📸 Snapshot uložen: {tiff_path}")
    
    # Synchronizace snapshotu na vzdálený server
    sync_to_remote(snapshot_base_dir)


def compress_directory(output_dir, remove_source=False):
    """Komprimuje složku do .zip archivu rychleji pomocí zip příkazu."""
    archive_path = f"{output_dir}.zip"
    try:
        subprocess.run(["zip", "-r", archive_path, output_dir], check=True)
        print(f"✅ Složka {output_dir} byla úspěšně komprimována do: {archive_path}")

        if remove_source:
            shutil.rmtree(output_dir)
            print(f"🗑️ Zdrojová složka {output_dir} byla odstraněna.")
    except subprocess.CalledProcessError as e:
        print(f"❌ Chyba při komprimaci složky: {e}")


def sync_to_remote(source_dir):
    """Přesune data na vzdálený server pomocí rsync."""
    if not SYNC_ENABLED:
        return
    try:
        print(f"🔄 Synchronizuji data z {source_dir} do: {SYNC_DEST}")
        subprocess.run([
            "rsync", "-avz", "--remove-source-files",
            source_dir, SYNC_DEST
        ], check=True)
        print(f"✅ Data byla úspěšně synchronizována do: {SYNC_DEST}")
    except subprocess.CalledProcessError as e:
        print(f"❌ Chyba při synchronizaci dat: {e}")


def wait_for_trigger():
    """Čeká na nekonečné stisky klávesy Enter pro aktivaci triggeru."""
    while True:
        input("▶ Stiskni Enter pro TRIGGER...\n")
        triggered.set()
        time.sleep(0.1)  # Malá prodleva pro zamezení opakovaného triggeru


async def websocket_client():
    """WebSocket klient pro příjem triggerů s podporou komprimace dat."""
    async with websockets.connect(WEBSOCKET_SERVER_URL) as websocket:
        print(f"🌐 Připojeno k WebSocket serveru na {WEBSOCKET_SERVER_URL}")
        while True:
            try:
                raw = await websocket.recv()
                ts = time.strftime("%H:%M:%S", time.localtime())
                print(f"[{ts}] 📡 WS raw: {raw}")
                try:
                    message = json.loads(raw) if isinstance(raw, str) else raw
                except json.JSONDecodeError:
                    print(f"[{ts}] ⚠️ Nepodařilo se parsovat JSON: {raw}")
                    continue
                print(f"[{ts}] 📡 Přijatá zpráva: {message}")

                if message.get('type') == "TRIGG":
                    print("📡 WebSocket TRIGGER received!")
                    triggered.set()

                elif message.get('type') == "COMPRESS_AND_SYNC":
                    print("📡 WebSocket požadavek na komprimaci a synchronizaci dat!")
                    compress_directory(OUTPUT_DIR, remove_source=True)
                    sync_to_remote(OUTPUT_DIR + "/../")
            except websockets.ConnectionClosed:
                print("⚠️ WebSocket spojení bylo uzavřeno.")
                break


def main():
    last_snapshot_time = 0
    snapshot_interval = 30  # Interval v sekundách

    # Spuštění WebSocket klienta v samostatném vlákně (pokud je povoleno)
    if WEBSOCKET_ENABLED:
        threading.Thread(target=lambda: asyncio.run(websocket_client()), daemon=True).start()
        print(f"🌐 WebSocket trigger: {WEBSOCKET_SERVER_URL}")
    else:
        print("🌐 WebSocket trigger: vypnutý")

    while True:
        try:
            print(f"🔁 Bufferuje posledních {PRE_SECONDS + POST_SECONDS}s RAW dat ({RES}, {FPS} FPS)... [stiskni Enter pro trigger]")
            buffer.clear()
            timestamps.clear()
            triggered.clear()

            while not triggered.is_set():
                timestamp = time.time()
                frame = (picam2.capture_array("raw").view(np.uint16) << 6)
                buffer.append(frame)
                timestamps.append(timestamp)
                
                # Kontrola, zda uplynul interval pro snapshot
                if timestamp - last_snapshot_time >= snapshot_interval:
                    # Uložíme kopii aktuálního snímku
                    save_snapshot(frame.copy(), base_dir, timestamp)
                    last_snapshot_time = timestamp

            # Vytvoření nové složky pro každý trigger
            OUTPUT_DIR = create_output_directory(base_dir)
            
            print(f"📸 TRIGGER! Nahrávám ještě {POST_SECONDS}s...")
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
                print(f"📈 Naměřený FPS: {measured_fps:.2f} (cíl: {FPS} FPS)")

            # compress_directory(OUTPUT_DIR, remove_source=True)
            sync_to_remote(base_dir)
            print(f"\n🎬 Záznam dokončen — {len(buffer)} snímků uloženo do: {OUTPUT_DIR}")
            print("⏳ Připraveno na další trigger...\n")

        except KeyboardInterrupt:
            print("⛔ Přerušeno uživatelem.")
            break


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="CircularPiCam — kruhový RAW buffer s triggerem")
    parser.add_argument("--websocket", action="store_true", default=WEBSOCKET_ENABLED,
                        help="Zapnout WebSocket trigger")
    parser.add_argument("--no-websocket", dest="websocket", action="store_false",
                        help="Vypnout WebSocket trigger")
    parser.add_argument("--websocket-url", default=WEBSOCKET_SERVER_URL, metavar="URL",
                        help=f"URL WebSocket serveru (výchozí: {WEBSOCKET_SERVER_URL})")
    parser.add_argument("--sync", action="store_true", default=SYNC_ENABLED,
                        help="Zapnout rsync synchronizaci po triggeru")
    parser.add_argument("--no-sync", dest="sync", action="store_false",
                        help="Vypnout rsync synchronizaci")
    parser.add_argument("--sync-dest", default=SYNC_DEST, metavar="DEST",
                        help=f"Cíl rsync synchronizace (výchozí: {SYNC_DEST})")
    args = parser.parse_args()

    # Přepis globální konfigurace podle argumentů
    WEBSOCKET_ENABLED = args.websocket
    WEBSOCKET_SERVER_URL = args.websocket_url
    SYNC_ENABLED = args.sync
    SYNC_DEST = args.sync_dest

    # Inicializace kamery
    picam2 = Picamera2()

    # Detekce mono vs. barevný senzor
    is_mono = "AwbEnable" not in picam2.camera_controls
    print(f"📷 Senzor: {'mono' if is_mono else 'barevný (AWB dostupné)'}")

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

    # Kruhový buffer pro snímky a timestampy
    buffer = deque(maxlen=BUFFER_FRAMES)
    timestamps = deque(maxlen=BUFFER_FRAMES)

    # Trigger z klávesnice
    triggered = threading.Event()
    threading.Thread(target=wait_for_trigger, daemon=True).start()

    try:
        main()
    finally:
        picam2.stop()
