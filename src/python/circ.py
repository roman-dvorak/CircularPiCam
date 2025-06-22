import time
from picamera2 import Picamera2
from picamera2.encoders import H264Encoder
from picamera2.outputs import CircularOutput2, FileOutput

# Parametry pro Global Shutter (IMX296)
RES = (1456, 1088)
FPS = 60  # pokud zvládne HW encoder víc, klidně zvyši
BITRATE = 25000000  # hodně vysoký bitrate = minimální komprese

# Inicializace kamery
picam2 = Picamera2()
video_config = picam2.create_video_configuration(
    main={"size": RES, "format": "RGB888"},
    controls={"FrameDurationLimits": (int(1e6 // FPS/2), int(1e6 // FPS))}
)
config = picam2.create_video_configuration(raw={}, encode="raw")
picam2.configure(config)


encoder = H264Encoder(BITRATE)
circ = CircularOutput2(buffer_duration_ms=5000)  # buffer na 10 s (lze upravit)
encoder.output = circ

picam2.start()
picam2.start_encoder(encoder)

print(f"Buffer běží na {RES[0]}x{RES[1]} @ {FPS} FPS. Stiskni Enter pro záznam posledních 10 sekund...")
input()

# Při stisku Enter začne ukládat do souboru (např. 10 s zpět + 0 s po, pokud ihned stopneš)
print("TRIGGER")
filename = f"/mnt/ramdisk/video_{int(time.time())}.h264"
circ.open_output(FileOutput(filename))

time.sleep(5)  # můžeš zde čekat X sekund pro "po triggeru"
circ.close_output()
print(f"Hotovo! Video uložené jako {filename}")

picam2.stop_encoder()
picam2.stop()
