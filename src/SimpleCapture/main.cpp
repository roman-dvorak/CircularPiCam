// CircularPiCam - C++ capture daemon
// Kruhový buffer RAW snímků s vícezdrojovým triggerem a ukládáním na RAMFS

#include <libcamera/libcamera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/request.h>
#include <libcamera/control_ids.h>

#include <tiffio.h>
#include <gpiod.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <atomic>
#include <map>
#include <string>
#include <vector>
#include <ctime>
#include <cmath>
#include <csignal>
#include <algorithm>
#include <sys/mman.h>
#include <sys/socket.h>
#include <queue>
#include <netdb.h>
#include <unistd.h>
#include <optional>
#include <cstdlib>

namespace fs = std::filesystem;
using namespace libcamera;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Konfigurace
// ---------------------------------------------------------------------------
struct Config {
    int fps                        = 60;
    int pre_seconds                = 2;
    int post_seconds               = 2;
    int resolution_width           = 1088;
    int resolution_height          = 1088;
    std::string ramdisk_base       = "/mnt/ramdisk/CAM/frames";
    bool sync_enabled              = false;
    std::string rsync_dest         = "mill.lan:/storage/CAM/";
    bool upload_service_enabled    = false;
    std::string upload_trigger_file = "/run/musalacam/upload-trigger";
    int snapshot_interval_sec      = 30;
    int exposure_time_us           = 5000;  // mikrosekundy
    float analogue_gain            = 4.0f;
    bool save_as_8bit              = false;
    bool save_as_packed10          = false;  // sekvenčně balený 10bit TIFF (~37.5% menší než 16bit, beze ztráty)

    bool auto_gain                 = true;
    float auto_gain_target         = 40.0f;   // cílový jas (0-255, MSB rozsah)
    float auto_gain_speed          = 0.02f;   // rychlost adaptace (0-1)
    float auto_gain_min            = 1.0f;
    float auto_gain_max            = 16.0f;

    bool trigger_keyboard          = true;

    bool trigger_gpio              = false;
    std::string trigger_gpio_chip  = "gpiochip0";
    int trigger_gpio_line          = 17;  // TODO: nastavit správný pin

    bool trigger_brightness        = true;
    float trigger_brightness_threshold    = 1.5f;
    int trigger_brightness_debounce_sec   = 5;

    bool trigger_websocket         = false;
    std::string websocket_host     = "mill.lan";
    int websocket_port             = 8080;
    std::string websocket_path     = "/trigger";
};

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static bool parse_bool(const std::string &v) {
    return v == "true" || v == "1" || v == "yes";
}

Config load_config(const std::string &path) {
    Config cfg;
    std::ifstream f(path);
    if (!f.is_open()) return cfg;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key == "fps")                               cfg.fps = std::stoi(val);
        else if (key == "pre_seconds")                  cfg.pre_seconds = std::stoi(val);
        else if (key == "post_seconds")                 cfg.post_seconds = std::stoi(val);
        else if (key == "resolution_width")             cfg.resolution_width = std::stoi(val);
        else if (key == "resolution_height")            cfg.resolution_height = std::stoi(val);
        else if (key == "ramdisk_base")                 cfg.ramdisk_base = val;
        else if (key == "sync_enabled")                  cfg.sync_enabled = parse_bool(val);
        else if (key == "rsync_dest")                   cfg.rsync_dest = val;
        else if (key == "upload_service_enabled")       cfg.upload_service_enabled = parse_bool(val);
        else if (key == "upload_trigger_file")          cfg.upload_trigger_file = val;
        else if (key == "snapshot_interval_sec")        cfg.snapshot_interval_sec = std::stoi(val);
        else if (key == "exposure_time_us")              cfg.exposure_time_us = std::stoi(val);
        else if (key == "analogue_gain")                 cfg.analogue_gain = std::stof(val);
        else if (key == "save_as_8bit")                  cfg.save_as_8bit = parse_bool(val);
        else if (key == "save_as_packed10")              cfg.save_as_packed10 = parse_bool(val);
        else if (key == "trigger_keyboard")             cfg.trigger_keyboard = parse_bool(val);
        else if (key == "trigger_gpio")                 cfg.trigger_gpio = parse_bool(val);
        else if (key == "trigger_gpio_chip")            cfg.trigger_gpio_chip = val;
        else if (key == "trigger_gpio_line")            cfg.trigger_gpio_line = std::stoi(val);
        else if (key == "trigger_brightness")           cfg.trigger_brightness = parse_bool(val);
        else if (key == "trigger_brightness_threshold") cfg.trigger_brightness_threshold = std::stof(val);
        else if (key == "trigger_brightness_debounce_sec") cfg.trigger_brightness_debounce_sec = std::stoi(val);
        else if (key == "trigger_websocket")            cfg.trigger_websocket = parse_bool(val);
        else if (key == "websocket_host")               cfg.websocket_host = val;
        else if (key == "websocket_port")               cfg.websocket_port = std::stoi(val);
        else if (key == "websocket_path")               cfg.websocket_path = val;
        else if (key == "auto_gain")                    cfg.auto_gain = parse_bool(val);
        else if (key == "auto_gain_target")             cfg.auto_gain_target = std::stof(val);
        else if (key == "auto_gain_speed")              cfg.auto_gain_speed = std::stof(val);
        else if (key == "auto_gain_min")                cfg.auto_gain_min = std::stof(val);
        else if (key == "auto_gain_max")                cfg.auto_gain_max = std::stof(val);
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Globální stav
// ---------------------------------------------------------------------------
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_triggered{false};
static std::string g_trigger_source;
static std::mutex g_trigger_mutex;
static std::atomic<float> g_current_gain{4.0f};

void fire_trigger(const std::string &source) {
    std::lock_guard<std::mutex> lock(g_trigger_mutex);
    if (!g_triggered.load()) {
        g_triggered = true;
        g_trigger_source = source;
        std::cout << "[TRIGGER] Zdroj: " << source << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Frame data + kruhový buffer
// ---------------------------------------------------------------------------
struct FrameData {
    std::vector<uint8_t> data;
    int64_t timestamp_us = 0;
    int width = 0;
    int height = 0;
    int stride = 0;  // skutečný stride z libcamera (může být větší než width*10/8)
};

struct CircularBuffer {
    std::deque<FrameData> frames;
    std::mutex mtx;
    size_t max_size;

    explicit CircularBuffer(size_t n) : max_size(n) {}

    void push(FrameData fd) {
        std::lock_guard<std::mutex> lock(mtx);
        if (frames.size() >= max_size)
            frames.pop_front();
        frames.push_back(std::move(fd));
    }

    std::deque<FrameData> snapshot() {
        std::lock_guard<std::mutex> lock(mtx);
        return frames;
    }
};

// ---------------------------------------------------------------------------
// RAW10 unpack + statistiky
// ---------------------------------------------------------------------------
std::vector<uint16_t> unpack_raw10(const uint8_t *data, int width, int height, int stride = 0) {
    int packed_stride = stride > 0 ? stride : (width * 10 + 7) / 8;
    std::vector<uint16_t> out(width * height);
    for (int row = 0; row < height; ++row) {
        const uint8_t *src = data + row * packed_stride;
        uint16_t *dst = &out[row * width];
        int col = 0;
        for (; col + 3 < width; col += 4, src += 5) {
            dst[col+0] = ((src[0] << 2) | ((src[4] >> 0) & 0x3)) << 6;
            dst[col+1] = ((src[1] << 2) | ((src[4] >> 2) & 0x3)) << 6;
            dst[col+2] = ((src[2] << 2) | ((src[4] >> 4) & 0x3)) << 6;
            dst[col+3] = ((src[3] << 2) | ((src[4] >> 6) & 0x3)) << 6;
        }
        int rem = width - col;
        if (rem > 0) {
            uint8_t b[5] = {0,0,0,0,0};
            for (int i = 0; i < std::min(rem + 1, 5); ++i) b[i] = src[i];
            for (int i = 0; i < rem; ++i)
                dst[col+i] = ((b[i] << 2) | ((b[4] >> (2*i)) & 0x3)) << 6;
        }
    }
    return out;
}

struct FrameStats {
    double mean, min_val, max_val;
};

FrameStats compute_stats(const std::vector<uint16_t> &pixels) {
    double sum = 0;
    uint16_t mn = 1023, mx = 0;
    for (auto v : pixels) {
        sum += v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
    }
    return { sum / pixels.size(), (double)mn, (double)mx };
}

// Rychlý odhad průměrného jasu bez plného unpacku (vzorkuje MSB každé skupiny 4px)
float quick_brightness(const uint8_t *data, int width, int height, int stride = 0) {
    int packed_stride = stride > 0 ? stride : (width * 10 + 7) / 8;
    int total_groups  = (width / 4) * height;
    if (total_groups == 0) return 0.f;
    long long sum = 0;
    int count = 0;
    for (int row = 0; row < height; ++row) {
        const uint8_t *src = data + row * packed_stride;
        for (int col = 0; col + 3 < width; col += 4, src += 5) {
            sum += src[0] + src[1] + src[2] + src[3];
            count += 4;
        }
    }
    return count > 0 ? (float)sum / count : 0.f;
}

// Sekvenční bit-balení 10bitových vzorků (MSB-first, řádek = celý počet bajtů) —
// odlišné od CSI2P balení kamery (4px/5B s prokládanými spodními bity), proto se
// musí přebalit z rozbalených pixelů, ne jen přeposlat syrová data kamery.
std::vector<uint8_t> pack10_sequential(const std::vector<uint16_t> &pixels16, int width, int height) {
    int row_bytes = (width * 10 + 7) / 8;
    std::vector<uint8_t> out(row_bytes * height, 0);
    for (int row = 0; row < height; ++row) {
        uint8_t *dst = &out[row * row_bytes];
        uint32_t acc = 0;
        int acc_bits = 0;
        int byte_idx = 0;
        for (int col = 0; col < width; ++col) {
            uint32_t v10 = pixels16[row * width + col] >> 6;  // zpět na skutečných 10 bitů
            acc = (acc << 10) | v10;
            acc_bits += 10;
            while (acc_bits >= 8) {
                acc_bits -= 8;
                dst[byte_idx++] = (uint8_t)((acc >> acc_bits) & 0xFF);
            }
        }
        if (acc_bits > 0)
            dst[byte_idx++] = (uint8_t)((acc << (8 - acc_bits)) & 0xFF);
    }
    return out;
}

// ---------------------------------------------------------------------------
// TIFF uložení
// ---------------------------------------------------------------------------
enum class SaveDepth { Bits16, Bits8, Bits10Packed };

SaveDepth pick_save_depth(const Config &cfg) {
    if (cfg.save_as_8bit) return SaveDepth::Bits8;
    if (cfg.save_as_packed10) return SaveDepth::Bits10Packed;
    return SaveDepth::Bits16;
}

bool save_raw10_to_tiff(const std::string &filename, const uint8_t *data, int width, int height,
                         int stride = 0, SaveDepth depth = SaveDepth::Bits16) {
    auto pixels = unpack_raw10(data, width, height, stride);
    TIFF *tif = TIFFOpen(filename.c_str(), "w");
    if (!tif) return false;
    int bits = depth == SaveDepth::Bits8 ? 8 : (depth == SaveDepth::Bits10Packed ? 10 : 16);
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH,      width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH,     height);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,   bits);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP,    height);
    TIFFSetField(tif, TIFFTAG_COMPRESSION,     COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
    if (depth == SaveDepth::Bits8) {
        std::vector<uint8_t> pixels8(pixels.size());
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels8[i] = (uint8_t)(pixels[i] >> 8);
        for (int row = 0; row < height; ++row)
            TIFFWriteScanline(tif, (void *)(&pixels8[row * width]), row);
    } else if (depth == SaveDepth::Bits10Packed) {
        int row_bytes = (width * 10 + 7) / 8;
        auto packed = pack10_sequential(pixels, width, height);
        for (int row = 0; row < height; ++row)
            TIFFWriteScanline(tif, (void *)(&packed[row * row_bytes]), row);
    } else {
        for (int row = 0; row < height; ++row)
            TIFFWriteScanline(tif, (void *)(&pixels[row * width]), row);
    }
    TIFFClose(tif);
    return true;
}

// ---------------------------------------------------------------------------
// Adresářová struktura + CSV
// ---------------------------------------------------------------------------
std::string iso_utc_now() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H-%M-%SZ");
    return oss.str();
}

std::string date_path() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::localtime(&t), "%Y/%m/%d");
    return oss.str();
}

std::string create_capture_dir(const std::string &base) {
    std::string dir = base + "/" + date_path() + "/rawcap_" + iso_utc_now();
    fs::create_directories(dir);
    return dir;
}

void save_metadata_csv(const std::string &dir,
                       const std::deque<FrameData> &frames,
                       int64_t trigger_ts_us)
{
    std::ofstream csv(dir + "/metadata.csv");
    csv << "Timestamp,FrameNumber,RelativeTime_s,Min,Max,Mean\n";
    int i = 0;
    for (const auto &f : frames) {
        auto pixels = unpack_raw10(f.data.data(), f.width, f.height, f.stride);
        auto stats  = compute_stats(pixels);
        double rel  = (f.timestamp_us - trigger_ts_us) / 1e6;
        csv << std::fixed << std::setprecision(6)
            << (f.timestamp_us / 1e6) << ","
            << i++ << ","
            << rel << ","
            << stats.min_val << ","
            << stats.max_val << ","
            << stats.mean << "\n";
    }
}

void rsync_data(const std::string &src_dir, const std::string &dest) {
    std::string base = fs::path(src_dir).parent_path().string();
    std::string cmd  = "rsync -avz --remove-source-files " + base + "/ " + dest + " &";
    std::system(cmd.c_str());
}

// "Dotkne" se sentinel souboru, který hlídá systemd .path unit
// (musalacam-s3sync.path) a po změně spustí upload službu. Žádné sudo ani
// systemctl tu nejsou potřeba - o spuštění (a ochranu před souběhem) se
// stará systemd + flock uvnitř upload skriptu.
void touch_upload_trigger(const std::string &trigger_file) {
    fs::create_directories(fs::path(trigger_file).parent_path());
    std::ofstream f(trigger_file, std::ios::trunc);
    f << std::chrono::system_clock::now().time_since_epoch().count();
}

// ---------------------------------------------------------------------------
// Save thread
// ---------------------------------------------------------------------------
void save_thread_fn(const Config &cfg,
                    CircularBuffer &circ_buf,
                    std::shared_ptr<Camera> camera)
{
    while (g_running) {
        if (!g_triggered.load()) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        int64_t trigger_ts = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::cout << "[SAVE] Čekám " << cfg.post_seconds << "s po triggeru..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(cfg.post_seconds));

        auto frames = circ_buf.snapshot();
        if (frames.empty()) {
            std::cerr << "[SAVE] Buffer je prázdný, nic k uložení." << std::endl;
            g_triggered = false;
            continue;
        }

        std::string dir = create_capture_dir(cfg.ramdisk_base);
        std::cout << "[SAVE] Ukládám " << frames.size() << " snímků do: " << dir << std::endl;

        int idx = 0;
        for (const auto &f : frames) {
            std::ostringstream name;
            name << dir << "/frame_" << std::setw(4) << std::setfill('0') << idx++ << ".tiff";
            save_raw10_to_tiff(name.str(), f.data.data(), f.width, f.height, f.stride, pick_save_depth(cfg));
        }

        save_metadata_csv(dir, frames, trigger_ts);

        if (frames.size() > 1) {
            double span_s = (frames.back().timestamp_us - frames.front().timestamp_us) / 1e6;
            double measured_fps = span_s > 0 ? (frames.size() - 1) / span_s : 0.0;
            std::cout << "[SAVE] Naměřený FPS: " << std::fixed << std::setprecision(2)
                      << measured_fps << " (cíl: " << cfg.fps << " FPS)" << std::endl;
        }

        std::cout << "[SAVE] Hotovo." << std::endl;
        if (cfg.sync_enabled) {
            std::cout << "[SAVE] Spouštím rsync..." << std::endl;
            rsync_data(dir, cfg.rsync_dest);
        }
        if (cfg.upload_service_enabled) {
            std::cout << "[SAVE] Spouštím upload trigger: " << cfg.upload_trigger_file << std::endl;
            touch_upload_trigger(cfg.upload_trigger_file);
        }

        g_triggered = false;
    }
}

// ---------------------------------------------------------------------------
// GPIO trigger thread
// ---------------------------------------------------------------------------
void gpio_trigger_thread(const Config &cfg) {
    struct gpiod_chip *chip = gpiod_chip_open_by_name(cfg.trigger_gpio_chip.c_str());
    if (!chip) {
        std::cerr << "[GPIO] Nelze otevřít chip: " << cfg.trigger_gpio_chip << std::endl;
        return;
    }
    struct gpiod_line *line = gpiod_chip_get_line(chip, cfg.trigger_gpio_line);
    if (!line) {
        std::cerr << "[GPIO] Nelze získat linii: " << cfg.trigger_gpio_line << std::endl;
        gpiod_chip_close(chip);
        return;
    }
    struct gpiod_line_request_config req_cfg = {};
    req_cfg.consumer     = "circularpicam";
    req_cfg.request_type = GPIOD_LINE_REQUEST_EVENT_RISING_EDGE;
    req_cfg.flags        = 0;
    if (gpiod_line_request(line, &req_cfg, 0) < 0) {
        std::cerr << "[GPIO] Chyba při registraci edge eventu na lince " << cfg.trigger_gpio_line << std::endl;
        gpiod_chip_close(chip);
        return;
    }
    std::cout << "[GPIO] Čekám na náběžnou hranu na " << cfg.trigger_gpio_chip
              << " linie " << cfg.trigger_gpio_line << std::endl;
    while (g_running) {
        struct timespec timeout = { 1, 0 };  // 1s timeout pro kontrolu g_running
        int ret = gpiod_line_event_wait(line, &timeout);
        if (ret == 1) {
            struct gpiod_line_event event;
            gpiod_line_event_read(line, &event);
            fire_trigger("GPIO");
        }
    }
    gpiod_line_release(line);
    gpiod_chip_close(chip);
}

// ---------------------------------------------------------------------------
// Keyboard trigger thread
// ---------------------------------------------------------------------------
void keyboard_trigger_thread() {
    std::cout << "[KEYBOARD] Stiskni Enter pro trigger..." << std::endl;
    while (g_running) {
        std::string line;
        if (std::getline(std::cin, line))
            fire_trigger("KEYBOARD");
    }
}

// ---------------------------------------------------------------------------
// Minimální WebSocket klient
// ---------------------------------------------------------------------------
static int ws_connect(const std::string &host, int port) {
    struct addrinfo hints{}, *res;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (auto *r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static bool ws_handshake(int fd, const std::string &host, const std::string &path) {
    std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (send(fd, req.c_str(), req.size(), 0) < 0) return false;
    char buf[1024] = {};
    recv(fd, buf, sizeof(buf) - 1, 0);
    return std::string(buf).find("101") != std::string::npos;
}

// Přečte jeden WebSocket text frame (pouze jednoduchý, nekomprimovaný, max 65535 B)
static std::string ws_read_frame(int fd) {
    uint8_t header[2];
    if (recv(fd, header, 2, MSG_WAITALL) != 2) return "";
    // bool fin  = header[0] & 0x80;  // zatím ignorujeme fragmentaci
    uint8_t opcode = header[0] & 0x0F;
    if (opcode == 0x8) return "";  // close frame
    bool masked    = header[1] & 0x80;
    uint64_t len   = header[1] & 0x7F;
    if (len == 126) {
        uint8_t ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) != 2) return "";
        len = ((uint64_t)ext[0] << 8) | ext[1];
    }
    uint8_t mask[4] = {};
    if (masked) recv(fd, mask, 4, MSG_WAITALL);
    if (len == 0 || len > 65535) return "";
    std::vector<uint8_t> payload(len);
    ssize_t got = recv(fd, payload.data(), len, MSG_WAITALL);
    if (got <= 0) return "";
    if (masked)
        for (size_t i = 0; i < (size_t)got; ++i)
            payload[i] ^= mask[i % 4];
    return std::string(payload.begin(), payload.begin() + got);
}

void websocket_trigger_thread(const Config &cfg) {
    while (g_running) {
        std::cout << "[WS] Připojuji se na " << cfg.websocket_host << ":" << cfg.websocket_port << std::endl;
        int fd = ws_connect(cfg.websocket_host, cfg.websocket_port);
        if (fd < 0) {
            std::cerr << "[WS] Připojení selhalo, zkusím za 5s..." << std::endl;
            std::this_thread::sleep_for(5s);
            continue;
        }
        if (!ws_handshake(fd, cfg.websocket_host, cfg.websocket_path)) {
            std::cerr << "[WS] Handshake selhal." << std::endl;
            close(fd);
            std::this_thread::sleep_for(5s);
            continue;
        }
        std::cout << "[WS] Připojeno." << std::endl;
        while (g_running) {
            std::string msg = ws_read_frame(fd);
            if (msg.empty()) break;  // disconnect
            if (msg.find("\"TRIGG\"") != std::string::npos ||
                msg.find("TRIGG") != std::string::npos)
                fire_trigger("WEBSOCKET");
        }
        close(fd);
        std::cerr << "[WS] Odpojeno, reconnect za 5s..." << std::endl;
        std::this_thread::sleep_for(5s);
    }
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------
void save_snapshot(const FrameData &f, const std::string &base, SaveDepth depth) {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::ostringstream dir_oss;
    dir_oss << base << "/../snapshots/" << std::put_time(std::localtime(&t), "%Y/%m/%d");
    std::string dir = dir_oss.str();
    fs::create_directories(dir);
    std::string path = dir + "/snapshot_" + std::to_string(f.timestamp_us) + ".tiff";
    save_raw10_to_tiff(path, f.data.data(), f.width, f.height, f.stride, depth);
    std::cout << "[SNAPSHOT] " << path << std::endl;
}



// ---------------------------------------------------------------------------
// RequestHandler – BEZ libcamera::Object → přímé (synchronní) volání v camera threadu
// ---------------------------------------------------------------------------
class RequestHandler {
public:
    RequestHandler(std::queue<Request*> &q, std::mutex &m, std::condition_variable &c,
                   CircularBuffer &buf, const Config &cfg, std::atomic<int64_t> &last_snap)
        : queue(q), mutex(m), cv(c), circ_buf(buf), cfg(cfg), last_snapshot_us(last_snap) {}

    void handleRequest(Request *request) {
        auto t_start = std::chrono::steady_clock::now();

        // Interval mezi po sobě jdoucími voláními callbacku = skutečný frame-to-frame čas
        if (last_call_tp.time_since_epoch().count() != 0) {
            double interval_ms = std::chrono::duration<double, std::milli>(t_start - last_call_tp).count();
            interval_sum_ms += interval_ms;
            interval_max_ms = std::max(interval_max_ms, interval_ms);
        }
        last_call_tp = t_start;

        for (auto &[s, fb] : request->buffers()) {
            const auto &planes = fb->planes();
            if (planes.empty()) continue;
            void *mapped = mmap(nullptr, planes[0].length, PROT_READ, MAP_SHARED,
                                planes[0].fd.get(), 0);
            if (mapped == MAP_FAILED) {
                std::cerr << "[CAM] mmap selhalo" << std::endl;
                continue;
            }
            int W = s->configuration().size.width;
            int H = s->configuration().size.height;
            int ST = s->configuration().stride;

            FrameData fd;
            fd.width = W; fd.height = H; fd.stride = ST;
            fd.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            auto t_copy0 = std::chrono::steady_clock::now();
            fd.data.assign((uint8_t*)mapped, (uint8_t*)mapped + planes[0].length);
            munmap(mapped, planes[0].length);
            double copy_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_copy0).count();
            copy_sum_ms += copy_ms;
            copy_max_ms = std::max(copy_max_ms, copy_ms);

            // Výpočet jasu (pro brightness trigger i auto-gain)
            auto t_bri0 = std::chrono::steady_clock::now();
            if (cfg.trigger_brightness || cfg.auto_gain) {
                float bri = quick_brightness(fd.data.data(), W, H, ST);
                if (!ema_init) {
                    ema = bri; ema_init = true;
                } else {
                    constexpr float alpha = 0.05f;
                    // Brightness trigger
                    // Práh počítáme z baseline s podlahou 1.0 — na tmavé noční obloze
                    // bývá ema téměř/přesně 0, takže "ema * threshold" by bylo 0 a
                    // jakýkoli skutečný záblesk (meteor) by se s podmínkou ema > 0
                    // nikdy nespustil.
                    float baseline = std::max(ema, 1.0f);
                    if (cfg.trigger_brightness &&
                        !g_triggered.load() &&
                        fd.timestamp_us > debounce_until &&
                        bri > baseline * cfg.trigger_brightness_threshold) {
                        fire_trigger("BRIGHTNESS");
                        debounce_until = fd.timestamp_us +
                            (int64_t)cfg.trigger_brightness_debounce_sec * 1000000LL;
                    }
                    ema = alpha * bri + (1.f - alpha) * ema;
                }

                // Auto-gain (jen mimo trigger – gain se nemění během záznamu)
                if (cfg.auto_gain && ema_init && !g_triggered.load() && bri > 0.f) {
                    float current = g_current_gain.load();
                    float ratio   = cfg.auto_gain_target / bri;
                    float desired  = std::clamp(current * ratio, cfg.auto_gain_min, cfg.auto_gain_max);
                    float next     = current + cfg.auto_gain_speed * (desired - current);
                    next = std::clamp(next, cfg.auto_gain_min, cfg.auto_gain_max);
                    g_current_gain.store(next);
                    if (++agc_log_counter % 300 == 0) {
                        std::cout << "[AGC] gain=" << std::fixed << std::setprecision(2)
                                  << next << " jas=" << (int)bri << " ema=" << (int)ema << std::endl;
                        auto exp = request->metadata().get(libcamera::controls::ExposureTime);
                        auto fdur = request->metadata().get(libcamera::controls::FrameDuration);
                        if (exp && fdur)
                            std::cout << "[EXP] skutečná expozice=" << *exp << "us, skutečné trvání snímku="
                                      << *fdur << "us" << std::endl;
                    }
                }
            }
            double bri_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_bri0).count();
            bri_sum_ms += bri_ms;
            bri_max_ms = std::max(bri_max_ms, bri_ms);

            // Snapshot
            int64_t now_us = fd.timestamp_us;
            int64_t last   = last_snapshot_us.load();
            if (now_us - last >= (int64_t)cfg.snapshot_interval_sec * 1000000LL) {
                if (last_snapshot_us.compare_exchange_strong(last, now_us))
                    save_snapshot(fd, cfg.ramdisk_base, pick_save_depth(cfg));
            }

            circ_buf.push(std::move(fd));
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push(request);
        }
        cv.notify_one();

        double total_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_start).count();
        total_sum_ms += total_ms;
        total_max_ms = std::max(total_max_ms, total_ms);

        if (++perf_counter % 120 == 0) {
            double n = (double)perf_counter_window;
            std::cout << std::fixed << std::setprecision(3)
                      << "[PERF] interval avg=" << (interval_sum_ms / n) << "ms max=" << interval_max_ms << "ms"
                      << " | copy avg=" << (copy_sum_ms / n) << "ms max=" << copy_max_ms << "ms"
                      << " | brightness avg=" << (bri_sum_ms / n) << "ms max=" << bri_max_ms << "ms"
                      << " | total avg=" << (total_sum_ms / n) << "ms max=" << total_max_ms << "ms"
                      << std::endl;
            interval_sum_ms = copy_sum_ms = bri_sum_ms = total_sum_ms = 0.0;
            interval_max_ms = copy_max_ms = bri_max_ms = total_max_ms = 0.0;
        }
    }

private:
    std::queue<Request*>     &queue;
    std::mutex               &mutex;
    std::condition_variable  &cv;
    CircularBuffer           &circ_buf;
    const Config             &cfg;
    std::atomic<int64_t>     &last_snapshot_us;
    float    ema = 0.f;
    bool     ema_init = false;
    int64_t  debounce_until = 0;
    int      agc_log_counter = 0;

    std::chrono::steady_clock::time_point last_call_tp{};
    double   interval_sum_ms = 0.0, interval_max_ms = 0.0;
    double   copy_sum_ms = 0.0, copy_max_ms = 0.0;
    double   bri_sum_ms = 0.0, bri_max_ms = 0.0;
    double   total_sum_ms = 0.0, total_max_ms = 0.0;
    long     perf_counter = 0;
    static constexpr long perf_counter_window = 120;
};

// ---------------------------------------------------------------------------
// CLI argumenty (analogie argparse z src/python/raw.py) — přepisují hodnoty z config.ini
// ---------------------------------------------------------------------------
struct CliOverrides {
    std::optional<std::string> cfg_path;
    std::optional<bool>        keyboard;
    std::optional<bool>        gpio;
    std::optional<bool>        brightness;
    std::optional<bool>        websocket;
    std::optional<std::string> websocket_host;
    std::optional<int>         websocket_port;
    std::optional<std::string> websocket_path;
    std::optional<bool>        sync;
    std::optional<std::string> sync_dest;
    std::optional<bool>        upload_service;
    std::optional<std::string> upload_trigger_file;
    std::optional<float>       brightness_threshold;
    std::optional<int>         brightness_debounce;
    std::optional<bool>        save_8bit;
    std::optional<bool>        save_packed10;
};

// Rozloží "ws://host:port/path" na jednotlivé složky pro websocket_host/port/path.
bool parse_websocket_url(const std::string &url, std::string &host, int &port, std::string &path) {
    std::string rest = url;
    auto scheme_pos = rest.find("://");
    if (scheme_pos != std::string::npos) rest = rest.substr(scheme_pos + 3);
    auto slash_pos = rest.find('/');
    path = (slash_pos == std::string::npos) ? "/" : rest.substr(slash_pos);
    std::string hostport = (slash_pos == std::string::npos) ? rest : rest.substr(0, slash_pos);
    auto colon_pos = hostport.find(':');
    if (colon_pos == std::string::npos) return false;
    host = hostport.substr(0, colon_pos);
    try {
        port = std::stoi(hostport.substr(colon_pos + 1));
    } catch (...) {
        return false;
    }
    return true;
}

void print_usage(const char *prog) {
    std::cout <<
        "CircularPiCam — kruhový RAW buffer s triggerem\n\n"
        "Použití: " << prog << " [config.ini] [volby]\n\n"
        "Volby:\n"
        "  --config <cesta>          Cesta ke konfiguračnímu souboru (výchozí: config.ini)\n"
        "  --keyboard / --no-keyboard       Zapnout/vypnout trigger přes Enter\n"
        "  --gpio / --no-gpio               Zapnout/vypnout GPIO trigger\n"
        "  --brightness / --no-brightness   Zapnout/vypnout brightness trigger\n"
        "  --websocket / --no-websocket     Zapnout/vypnout WebSocket trigger\n"
        "  --websocket-url <URL>            ws://host:port/path (rozloží na host/port/path)\n"
        "  --sync / --no-sync               Zapnout/vypnout rsync synchronizaci po triggeru\n"
        "  --sync-dest <cíl>                Cíl rsync synchronizace (např. host:/cesta/)\n"
        "  --upload-service / --no-upload-service  Zapnout/vypnout upload trigger po triggeru\n"
        "  --upload-trigger-file <cesta>    Sentinel soubor pro systemd .path unit\n"
        "  --brightness-threshold <číslo>   Citlivost brightness triggeru (násobitel nad baseline, výchozí 1.5)\n"
        "  --brightness-debounce <sekundy>  Debounce brightness triggeru po spuštění\n"
        "  --save-8bit / --no-save-8bit     Ukládat TIFF jako 8bit místo 16bit\n"
        "  --save-packed10 / --no-save-packed10  Ukládat TIFF jako baleny 10bit (~37.5% mensi, beze ztraty)\n"
        "  -h, --help                       Zobrazit tuto nápovědu\n";
}

CliOverrides parse_cli(int argc, char *argv[]) {
    CliOverrides o;
    auto need_value = [&](int &i, const char *flag) -> std::string {
        if (i + 1 >= argc) {
            std::cerr << "Chybí hodnota pro " << flag << std::endl;
            std::exit(1);
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--config") {
            o.cfg_path = need_value(i, "--config");
        } else if (arg == "--keyboard") {
            o.keyboard = true;
        } else if (arg == "--no-keyboard") {
            o.keyboard = false;
        } else if (arg == "--gpio") {
            o.gpio = true;
        } else if (arg == "--no-gpio") {
            o.gpio = false;
        } else if (arg == "--brightness") {
            o.brightness = true;
        } else if (arg == "--no-brightness") {
            o.brightness = false;
        } else if (arg == "--websocket") {
            o.websocket = true;
        } else if (arg == "--no-websocket") {
            o.websocket = false;
        } else if (arg == "--websocket-url") {
            std::string url = need_value(i, "--websocket-url");
            std::string host, path; int port;
            if (!parse_websocket_url(url, host, port, path)) {
                std::cerr << "Neplatná --websocket-url: " << url << std::endl;
                std::exit(1);
            }
            o.websocket_host = host;
            o.websocket_port = port;
            o.websocket_path = path;
        } else if (arg == "--sync") {
            o.sync = true;
        } else if (arg == "--no-sync") {
            o.sync = false;
        } else if (arg == "--sync-dest") {
            o.sync_dest = need_value(i, "--sync-dest");
        } else if (arg == "--upload-service") {
            o.upload_service = true;
        } else if (arg == "--no-upload-service") {
            o.upload_service = false;
        } else if (arg == "--upload-trigger-file") {
            o.upload_trigger_file = need_value(i, "--upload-trigger-file");
        } else if (arg == "--brightness-threshold") {
            o.brightness_threshold = std::stof(need_value(i, "--brightness-threshold"));
        } else if (arg == "--brightness-debounce") {
            o.brightness_debounce = std::stoi(need_value(i, "--brightness-debounce"));
        } else if (arg == "--save-8bit") {
            o.save_8bit = true;
        } else if (arg == "--no-save-8bit") {
            o.save_8bit = false;
        } else if (arg == "--save-packed10") {
            o.save_packed10 = true;
        } else if (arg == "--no-save-packed10") {
            o.save_packed10 = false;
        } else if (!arg.empty() && arg[0] != '-') {
            // Poziční argument = cesta ke config.ini (zpětná kompatibilita)
            o.cfg_path = arg;
        } else {
            std::cerr << "Neznámý argument: " << arg << std::endl;
            print_usage(argv[0]);
            std::exit(1);
        }
    }
    return o;
}

void apply_cli_overrides(Config &cfg, const CliOverrides &o) {
    if (o.keyboard)        cfg.trigger_keyboard = *o.keyboard;
    if (o.gpio)            cfg.trigger_gpio = *o.gpio;
    if (o.brightness)      cfg.trigger_brightness = *o.brightness;
    if (o.websocket)       cfg.trigger_websocket = *o.websocket;
    if (o.websocket_host)  cfg.websocket_host = *o.websocket_host;
    if (o.websocket_port)  cfg.websocket_port = *o.websocket_port;
    if (o.websocket_path)  cfg.websocket_path = *o.websocket_path;
    if (o.sync)            cfg.sync_enabled = *o.sync;
    if (o.sync_dest)       cfg.rsync_dest = *o.sync_dest;
    if (o.upload_service)      cfg.upload_service_enabled = *o.upload_service;
    if (o.upload_trigger_file) cfg.upload_trigger_file = *o.upload_trigger_file;
    if (o.brightness_threshold) cfg.trigger_brightness_threshold = *o.brightness_threshold;
    if (o.brightness_debounce)  cfg.trigger_brightness_debounce_sec = *o.brightness_debounce;
    if (o.save_8bit)       cfg.save_as_8bit = *o.save_8bit;
    if (o.save_packed10)   cfg.save_as_packed10 = *o.save_packed10;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    CliOverrides cli = parse_cli(argc, argv);

    // Načti konfiguraci
    std::string cfg_path = cli.cfg_path.value_or("config.ini");
    if (!fs::exists(cfg_path)) cfg_path = "/etc/circularpicam.conf";
    Config cfg = load_config(cfg_path);
    apply_cli_overrides(cfg, cli);

    std::cout << "[INIT] Konfigurace: " << cfg_path << std::endl;
    std::cout << "[INIT] Rozlišení: " << cfg.resolution_width << "x" << cfg.resolution_height
              << " @ " << cfg.fps << " FPS, buffer "
              << (cfg.pre_seconds + cfg.post_seconds) << "s" << std::endl;
    std::cout << "[INIT] Expozice: " << (1000000LL / cfg.fps) << "us (max pro "
              << cfg.fps << " FPS), auto-gain: " << (cfg.auto_gain ? "ON" : "OFF") << std::endl;

    int buf_size = (cfg.pre_seconds + cfg.post_seconds) * cfg.fps;
    CircularBuffer circ_buf(buf_size);
    std::atomic<int64_t> last_snapshot_us{0};
    g_current_gain.store(cfg.analogue_gain);

    // Spuštění trigger vláken
    std::vector<std::thread> threads;

    if (cfg.trigger_keyboard)
        threads.emplace_back(keyboard_trigger_thread);
    if (cfg.trigger_gpio)
        threads.emplace_back(gpio_trigger_thread, std::cref(cfg));
    if (cfg.trigger_websocket)
        threads.emplace_back(websocket_trigger_thread, std::cref(cfg));

    // libcamera setup
    CameraManager cm;
    cm.start();
    if (cm.cameras().empty()) {
        std::cerr << "[CAM] Žádná kamera nenalezena!" << std::endl;
        return 1;
    }

    std::shared_ptr<Camera> camera = cm.cameras()[0];
    camera->acquire();

    auto config_cam = camera->generateConfiguration({ StreamRole::Raw });
    config_cam->at(0).pixelFormat = libcamera::PixelFormat::fromString("SRGGB10_CSI2P");
    config_cam->at(0).size = libcamera::Size(cfg.resolution_width, cfg.resolution_height);
    config_cam->validate();
    StreamConfiguration &stream_cfg = config_cam->at(0);
    std::cout << "[CAM] Formát: " << stream_cfg.pixelFormat.toString()
              << " " << stream_cfg.size.toString()
              << " bufferCount(navrženo)=" << stream_cfg.bufferCount << std::endl;

    // Výchozí počet bufferů z validate() bývá nízký (často 4) a při jakékoli
    // latenci v recyklaci requestu (mutex/cv/wake hlavní smyčky) způsobí
    // zahazování snímků na úrovni senzoru/unicam driveru, i když je zpracování
    // v handleRequest rychlé. Větší fronta bufferů tuto latenci absorbuje.
    stream_cfg.bufferCount = std::max(stream_cfg.bufferCount, 8u);
    std::cout << "[CAM] bufferCount(použito)=" << stream_cfg.bufferCount << std::endl;

    if (camera->configure(config_cam.get()) < 0) {
        std::cerr << "[CAM] Konfigurace selhala!" << std::endl;
        return 1;
    }

    // ScalerCrop: centrovaný výřez ze senzoru 1456x1088
    int crop_x = (1456 - cfg.resolution_width) / 2;
    int crop_y = (1088 - cfg.resolution_height) / 2;
    int64_t frame_dur = 1000000LL / cfg.fps;

    libcamera::ControlList cam_controls(libcamera::controls::controls);
    // Bez vypnutí AE by IPA mohla na dalších snímcích přepsat ruční ExposureTime/
    // AnalogueGain svým vlastním 3A algoritmem (na rozdíl od raw.py, kde je AeEnable
    // vždy false) — pak by se expozice neudržela na maximu pro daný FPS.
    cam_controls.set(libcamera::controls::AeEnable,             false);
    cam_controls.set(libcamera::controls::ExposureTime,        (int32_t)frame_dur);  // max expozice pro daný FPS
    cam_controls.set(libcamera::controls::AnalogueGain,        cfg.analogue_gain);
    {
        std::array<int64_t, 2> fd = { frame_dur, frame_dur };
        cam_controls.set(libcamera::controls::FrameDurationLimits,
                         libcamera::Span<const int64_t, 2>(fd));
    }
    cam_controls.set(libcamera::controls::ScalerCrop,
                     libcamera::Rectangle(crop_x, crop_y,
                                          cfg.resolution_width, cfg.resolution_height));

    Stream *stream = stream_cfg.stream();
    FrameBufferAllocator allocator(camera);
    if (allocator.allocate(stream) < 0) {
        std::cerr << "[CAM] Alokace bufferů selhala!" << std::endl;
        return 1;
    }

    std::queue<Request*> completed_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;

    std::vector<std::unique_ptr<Request>> requests;
    for (const auto &fb : allocator.buffers(stream)) {
        auto req = camera->createRequest();
        if (!req || req->addBuffer(stream, fb.get()) < 0) {
            std::cerr << "[CAM] Vytvoření requestu selhalo!" << std::endl;
            return 1;
        }
        requests.push_back(std::move(req));
    }

    RequestHandler handler(completed_queue, queue_mutex, queue_cv,
                           circ_buf, cfg, last_snapshot_us);
    // connect bez libcamera::Object → přímé volání v camera threadu
    camera->requestCompleted.connect(&handler, &RequestHandler::handleRequest);

    if (camera->start(&cam_controls) < 0) {
        std::cerr << "[CAM] Start kamery selhal!" << std::endl;
        return 1;
    }

    // Pozn.: camera->controls() po start() vrací absolutní hardwarový rozsah
    // ExposureTime (řádově sekundy), NE rozsah omezený aktuálním FrameDurationLimits
    // — proto se nepoužívá k výpočtu cíle. Skutečně dosaženou hodnotu ověřujeme
    // ze metadat snímku (log [EXP] níže), driver expozici sám oclampuje na max
    // dovolené pro daný FPS.
    for (auto &req : requests)
        camera->queueRequest(req.get());

    // Save thread
    threads.emplace_back(save_thread_fn, std::cref(cfg), std::ref(circ_buf), camera);

    std::cout << "[RUN] Bufferuji..." << std::endl;

    // Hlavní smyčka: recykluje dokončené requesty
    while (g_running) {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cv.wait_for(lock, 50ms, [&]{ return !completed_queue.empty(); });
        while (!completed_queue.empty()) {
            Request *req = completed_queue.front();
            completed_queue.pop();
            lock.unlock();
            req->reuse(Request::ReuseBuffers);
            req->controls().set(libcamera::controls::AnalogueGain, g_current_gain.load());
            camera->queueRequest(req);
            lock.lock();
        }
    }

    camera->stop();
    camera->release();
    cm.stop();

    for (auto &t : threads)
        if (t.joinable()) t.join();

    return 0;
}
