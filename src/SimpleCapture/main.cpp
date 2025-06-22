#include <libcamera/libcamera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/request.h>
#include <libcamera/control_ids.h>

#include <tiffio.h>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <sys/mman.h>
#include <vector>

namespace fs = std::filesystem;
using namespace libcamera;

std::string current_date()
{
    std::ostringstream oss;
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    oss << std::put_time(std::localtime(&t), "%Y-%m-%d");
    return oss.str();
}

bool save_raw_to_tiff(const std::string &filename, const uint8_t *data, int width, int height)
{
    TIFF *tif = TIFFOpen(filename.c_str(), "w");
    if (!tif)
        return false;

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8); // RAW10 by potřeboval rozbalit na 16 bitů
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);

    for (uint32_t row = 0; row < height; row++)
        TIFFWriteScanline(tif, (void *)(data + row * width), row);

    TIFFClose(tif);
    return true;
}

bool save_raw10_to_tiff(const std::string &filename, const uint8_t *data, int width, int height)
{
    // RAW10: 4 pixels = 5 bytes
    int packed_stride = ((width * 10 + 7) / 8);
    std::vector<uint16_t> unpacked(width * height);

    for (int row = 0; row < height; ++row) {
        const uint8_t* src = data + row * packed_stride;
        uint16_t* dst = &unpacked[row * width];
        int col = 0;
        for (; col + 3 < width; col += 4) {
            uint8_t b0 = src[0];
            uint8_t b1 = src[1];
            uint8_t b2 = src[2];
            uint8_t b3 = src[3];
            uint8_t b4 = src[4];
            dst[0] = ((b0 << 2) | ((b4 >> 0) & 0x3));
            dst[1] = ((b1 << 2) | ((b4 >> 2) & 0x3));
            dst[2] = ((b2 << 2) | ((b4 >> 4) & 0x3));
            dst[3] = ((b3 << 2) | ((b4 >> 6) & 0x3));
            src += 5;
        }
        // Handle remaining pixels (if width not divisible by 4)
        int rem = width - col;
        if (rem > 0) {
            uint8_t b[5] = {0,0,0,0,0};
            for (int i = 0; i < (rem == 1 ? 2 : rem == 2 ? 3 : rem == 3 ? 4 : 5); ++i)
                b[i] = src[i];
            for (int i = 0; i < rem; ++i)
                dst[i] = ((b[i] << 2) | ((b[4] >> (2 * i)) & 0x3));
        }
    }

    TIFF *tif = TIFFOpen(filename.c_str(), "w");
    std::cout << "[DEBUG] TIFFOpen: " << filename << std::endl;
    if (!tif)
        return false;

    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, height);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);

    for (uint32_t row = 0; row < height; row++)
        TIFFWriteScanline(tif, (void *)(&unpacked[row * width]), row);

    TIFFClose(tif);
    return true;
}

// Pomocná třída pro callback
class RequestHandler : public libcamera::Object
{
public:
    RequestHandler(std::queue<Request *> &q, std::mutex &m, std::condition_variable &c)
        : queue(q), mutex(m), cv(c) {}

    void handleRequest(Request *request)
    {
        std::cout << "[DEBUG] handleRequest called for request: " << request << std::endl;
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push(request);
        }
        cv.notify_one();
    }

private:
    std::queue<Request *> &queue;
    std::mutex &mutex;
    std::condition_variable &cv;
};

int main()
{
    const int MAX_FRAMES = 100;
    std::atomic<int> captured_frames = 0;
    std::queue<Request *> completed_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;

    std::string out_dir = "./out/" + current_date();
    std::cout << "[DEBUG] Output directory: " << out_dir << std::endl;
    fs::create_directories(out_dir);

    CameraManager cm;
    std::cout << "[DEBUG] Starting CameraManager..." << std::endl;
    cm.start();
    if (cm.cameras().empty())
    {
        std::cerr << "[DEBUG] No camera found!" << std::endl;
        return 1;
    }

    std::shared_ptr<Camera> camera = cm.cameras()[0];
    std::cout << "[DEBUG] Acquiring camera: " << camera->id() << std::endl;
    camera->acquire();

    std::cout << "[DEBUG] Camera status: " << static_cast<int>(camera->status()) << std::endl;

    std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration({ StreamRole::Raw });
    std::cout << "[DEBUG] Generated configuration." << std::endl;
    config->at(0).pixelFormat = libcamera::PixelFormat::fromString("SBGGR10_CSI2P");
    config->at(0).size = libcamera::Size(1456, 1088);
    config->validate();
    StreamConfiguration &cfg = config->at(0);
    std::cout << "[DEBUG] After validate - pixel format: " << cfg.pixelFormat.toString() << std::endl;
    std::cout << "[DEBUG] After validate - resolution: " << cfg.size.toString() << std::endl;

    if (camera->configure(config.get()) < 0)
    {
        std::cerr << "[DEBUG] Failed to configure camera!" << std::endl;
        return 1;
    }

    camera->setControls({
        { libcamera::controls::ExposureTime, 5000 }, // Nastav expozici na 10 ms
        { libcamera::controls::AnalogueGain, 4.0 },  // Nastav gain na 1.0
        { libcamera::controls::FrameDurationLimits, { 16667, 16667 } } // Nastav frame rate na 60 FPS
    });

    Stream *stream = cfg.stream();
    FrameBufferAllocator allocator(camera);
    std::cout << "[DEBUG] Allocating buffers..." << std::endl;
    if (allocator.allocate(stream) < 0)
    {
        std::cerr << "[DEBUG] Buffer allocation failed!" << std::endl;
        return 1;
    }

    std::vector<std::unique_ptr<Request>> requests;
    const auto &buffers = allocator.buffers(stream);
    std::cout << "[DEBUG] Number of buffers: " << buffers.size() << std::endl;

    for (const auto &fb : buffers)
    {
        std::unique_ptr<Request> req = camera->createRequest();
        if (!req || req->addBuffer(stream, fb.get()) < 0)
        {
            std::cerr << "[DEBUG] Failed to create request!" << std::endl;
            return 1;
        }
        requests.push_back(std::move(req));
    }

    RequestHandler handler(completed_queue, queue_mutex, queue_cv);
    camera->requestCompleted.connect(&handler, &RequestHandler::handleRequest);


    std::cout << "[DEBUG] Starting camera..." << std::endl;
    if (camera->start() < 0) {
        std::cerr << "[DEBUG] camera->start() failed!" << std::endl;
        return 1;
    }

    for (auto &req : requests) {
        camera->queueRequest(req.get());
        std::cout << "[DEBUG] Queued request: " << req.get() << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "[DEBUG] Entering capture loop..." << std::endl;
    while (captured_frames < MAX_FRAMES)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (completed_queue.empty())
            continue;

        Request *req = completed_queue.front();
        completed_queue.pop();
        lock.unlock();

        // Zpracování requestu...
    }

    std::cout << "[DEBUG] Stopping camera..." << std::endl;
    camera->stop();
    camera->release();
    cm.stop();

    std::cout << "[DEBUG] Done." << std::endl;
    return 0;
}
