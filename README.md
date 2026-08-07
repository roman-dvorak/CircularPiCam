# CircularPiCam

Systém kruhového bufferu pro Raspberry Pi kameru (IMX296 Global Shutter) s ukládáním RAW snímků před a po triggeru.

## Přehled komponent

```
src/
├── python/
│   ├── raw.py          # Hlavní capture démon (Python/picamera2) — produkční
│   └── circ.py         # Experiment s H.264 kruhovým bufferem (nevyužito v produkci)
├── SimpleCapture/
│   ├── main.cpp        # C++ přepis capture démona (libcamera) — výkonnější varianta
│   └── config.ini      # Konfigurace pro main.cpp
├── go/
│   └── process_capture/
│       └── process_capture.go  # Post-processing: TIFF → MP4 s overlayem
└── systemd/
    └── circular-picam.service  # Systemd unit pro automatický start
```

Binárka `build/simple_capture` je předkompilovaný C++ démon pro aarch64.

## Jak to funguje

1. Kamera snímá RAW snímky (1456×1088, 60 FPS) do kruhového bufferu v RAM.
2. Po triggeru (Enter, WebSocket, GPIO, nebo detekce jasu) se uloží N sekund před + po triggeru jako TIFF soubory na RAMFS.
3. Data se synchronizují na vzdálený server přes `rsync`.

## Nastavení Python prostředí

### 1. Systémové závislosti (jednou)

```bash
sudo apt install python3-picamera2 python3-venv
```

### 2. Vytvoření virtualenv

```bash
python3 -m venv --system-site-packages src/python/venv
```

> `--system-site-packages` je nutné — picamera2 se instaluje přes apt a není dostupná přes pip.

### 3. Instalace pip závislostí

```bash
src/python/venv/bin/pip install -r requirements-pip.txt
```

### 4. Spuštění

```bash
source src/python/venv/bin/activate
python3 src/python/raw.py
```

Stiskni **Enter** pro manuální trigger. Nebo pošli WebSocket zprávu `{"type": "TRIGG"}` na `ws://mill.lan:8080/trigger`.

## C++ varianta (main.cpp)

Výkonnější varianta s GPIO triggerem, brightness triggerem (meteor/blesk), auto-gain a čtením konfigurace z `config.ini`.

### Závislosti

```bash
sudo apt install build-essential cmake pkg-config libcamera-dev libtiff-dev libgpiod-dev
```

### Build

Z kořene repozitáře:

```bash
cmake -B build .
cmake --build build -j4
```

Výsledná binárka: `build/simple_capture`. Po úpravě `main.cpp` stačí znovu spustit
`cmake --build build -j4` (re-konfigurace přes `cmake -B build .` je potřeba jen po změně
`CMakeLists.txt`).

### Spuštění

```bash
./build/simple_capture src/SimpleCapture/config.ini
```

Konfigurace se načítá z `src/SimpleCapture/config.ini` (nebo z `/etc/circularpicam.conf`).

### CLI argumenty

Stejně jako `raw.py`, i `simple_capture` umožňuje přepsat vybrané volby z `config.ini` argumenty
příkazové řádky (cesta ke config.ini může být buď poziční argument, nebo `--config`):

```bash
./build/simple_capture --config src/SimpleCapture/config.ini \
    --websocket --websocket-url ws://mill.lan:8080/trigger \
    --sync --sync-dest mill.lan:/storage/CAM/
```

| Volba | Popis |
|-------|-------|
| `--config <cesta>` | Cesta ke konfiguračnímu souboru |
| `--keyboard` / `--no-keyboard` | Trigger přes Enter |
| `--gpio` / `--no-gpio` | GPIO trigger |
| `--brightness` / `--no-brightness` | Brightness trigger |
| `--websocket` / `--no-websocket` | WebSocket trigger |
| `--websocket-url <URL>` | `ws://host:port/path` — rozloží se na host/port/path |
| `--sync` / `--no-sync` | Rsync synchronizace po triggeru |
| `--sync-dest <cíl>` | Cíl rsync synchronizace |
| `--save-8bit` / `--no-save-8bit` | Ukládat TIFF jako 8bit místo 16bit |
| `-h`, `--help` | Nápověda |

Argumenty mají přednost před hodnotami v `config.ini`.

## Systemd service

Upravte `src/systemd/circular-picam.service` — změňte cesty k repozitáři a uživatele dle svého prostředí:

```ini
[Service]
User=<your_user>
WorkingDirectory=/path/to/CircularPiCam/src/python
ExecStart=/bin/bash -c 'source /path/to/CircularPiCam/src/python/venv/bin/activate && exec python3 raw.py'
```

Instalace:

```bash
sudo cp src/systemd/circular-picam.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now circular-picam
sudo systemctl status circular-picam
```

## Post-processing: TIFF → MP4

`src/go/process_capture/process_capture.go` vezme složku s TIFF snímky a sestaví z nich MP4 video s overlayem (číslo snímku, relativní čas).

### Závislosti

```bash
sudo apt install golang libopencv-dev
go get gocv.io/x/gocv github.com/fogleman/gg
```

### Spuštění

```bash
cd src/go/process_capture
go mod init process_capture
go mod tidy
go run process_capture.go
```

Výstup: `output.mp4` v aktuální složce.

## RAMFS

Data se ukládají na `/mnt/ramdisk`. Vytvoření:

```bash
sudo mkdir -p /mnt/ramdisk
echo 'tmpfs /mnt/ramdisk tmpfs rw,size=4G 0 0' | sudo tee -a /etc/fstab
sudo mount -a
```

## Konfigurace (C++ varianta)

Hlavní parametry v `src/SimpleCapture/config.ini`:

| Klíč | Výchozí | Popis |
|------|---------|-------|
| `fps` | 60 | Počet snímků za sekundu |
| `pre_seconds` | 2 | Sekund bufferu před triggerem |
| `post_seconds` | 2 | Sekund záznamu po triggeru |
| `resolution_width/height` | 1456×1088 | Rozlišení |
| `analogue_gain` | 22.0 | Analogový zisk |
| `save_as_8bit` | false | Ukládat TIFF jako 8bit (menší soubory) místo 16bit |
| `trigger_keyboard` | true | Trigger přes Enter |
| `trigger_gpio` | false | Trigger přes GPIO (pin 17) |
| `trigger_brightness` | true | Trigger při náhlém nárůstu jasu |
| `trigger_websocket` | false | Trigger přes WebSocket |
| `sync_enabled` | true | Synchronizovat data přes rsync po triggeru |
