# CircularPiCam

Circular-buffer RAW capture for the Raspberry Pi camera (developed for the IMX296
global-shutter sensor, mono or color). The camera continuously records into an
in-memory ring buffer; when a trigger fires, the frames from **before and after**
the event are saved as TIFF images with per-frame metadata and optionally synced
to a remote server.

Everything lives in a single script: [`src/python/raw.py`](src/python/raw.py).

> Older experiments (C++ libcamera daemon, H.264 buffer `circ.py`, Go TIFF→MP4
> post-processing) are kept in the [`archive`](../../tree/archive) branch.

## How it works

1. The camera runs at **1456×1088, 60 FPS**, 10-bit RAW (`R10`), auto-exposure
   enabled. Mono vs. color sensor is detected automatically (AWB is disabled on
   color sensors).
2. Frames are kept in a ring buffer covering `PRE_SECONDS + POST_SECONDS`
   (default 2 s + 2 s = 240 frames).
3. When a trigger fires, another `POST_SECONDS` of frames are captured and the
   whole buffer is written to a new directory:

   ```
   /mnt/ramdisk/CAM/frames/YYYY/MM/DD/rawcap_YYYY-MM-DDTHH-MM-SSZ/
       frame_0000.tiff
       frame_0001.tiff
       ...
       metadata.csv
   ```

   Each TIFF (16-bit by default) carries metadata in its header: timestamp,
   frame number, time relative to the trigger, min/max/mean/median pixel value.
   The same values are written to `metadata.csv`. The measured FPS is printed
   after each capture.
4. If sync is enabled, data are moved to the remote server with
   `rsync --remove-source-files`.
5. Independently of triggers, a **snapshot** frame is saved every 30 s to
   `/mnt/ramdisk/CAM/snapshots/YYYY/MM/DD/snapshot_<unix_ts>.tiff`.

## Triggers

| Trigger | Default | Description |
|---|---|---|
| Keyboard | on | Press **Enter** in the terminal. |
| Light (intensity) | on | Fires when the mean pixel value of a frame exceeds the moving-average baseline of the previous frames by a given ratio (meteor, flash, other sudden brightening). |
| WebSocket | off | Message `{"type": "TRIGG"}` from the WebSocket server. |

The WebSocket server can also send `{"type": "COMPRESS_AND_SYNC"}`, which zips the
last capture directory, removes the source and syncs it.

## Command-line options

| Option | Default | Description |
|---|---|---|
| `--light-trigger` / `--no-light-trigger` | enabled | Enable/disable the light-intensity trigger. |
| `--light-threshold RATIO` | `1.3` | Fire when frame mean > baseline × `RATIO` (1.3 = 30 % increase). |
| `--light-baseline-frames N` | `30` | Number of previous frames averaged for the baseline (30 = 0.5 s at 60 FPS). |
| `--websocket` / `--no-websocket` | disabled | Enable/disable the WebSocket trigger. |
| `--websocket-url URL` | `ws://mill.lan:8080/trigger` | WebSocket server URL. |
| `--sync` / `--no-sync` | disabled | rsync data to the remote server after each trigger. |
| `--sync-dest DEST` | `mill.lan:/storage/CAM/` | rsync destination. |

Examples:

```bash
python3 raw.py                          # keyboard + light trigger, no sync
python3 raw.py --no-light-trigger       # keyboard only
python3 raw.py --light-threshold 1.5 --light-baseline-frames 60
python3 raw.py --websocket --sync --sync-dest user@server:/storage/CAM/
```

Other settings (`RES`, `FPS`, `PRE_SECONDS`, `POST_SECONDS`, `save_as_8bit`,
`base_dir`, camera controls) are constants at the top of `raw.py`.

## Installation

### 1. System dependencies

```bash
sudo apt install python3-picamera2 python3-venv rsync zip
```

### 2. Virtualenv

```bash
python3 -m venv --system-site-packages src/python/venv
src/python/venv/bin/pip install -r requirements-pip.txt
```

> `--system-site-packages` is required — picamera2 is installed via apt and is
> not available from pip.

### 3. Ramdisk

Data are written to `/mnt/ramdisk`:

```bash
sudo mkdir -p /mnt/ramdisk
echo 'tmpfs /mnt/ramdisk tmpfs rw,size=4G 0 0' | sudo tee -a /etc/fstab
sudo mount -a
```

### 4. Run

```bash
source src/python/venv/bin/activate
python3 src/python/raw.py
```

For `--sync`, the user needs SSH key access to the remote server.

## Systemd service

Edit [`src/systemd/circular-picam.service`](src/systemd/circular-picam.service)
(user, repository path, options on the `ExecStart` line), then:

```bash
sudo cp src/systemd/circular-picam.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now circular-picam
sudo systemctl status circular-picam
```
