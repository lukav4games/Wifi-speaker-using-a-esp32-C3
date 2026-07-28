# Wifi-speaker-using-a-esp32-C3
make a wifi speaker with a esp32 C3

wiring:

GPIO10 --[1k resistor]--> Base (2N2222)
Emitter -> GND
Collector -> Speaker (+)
Speaker (-) -> battery +

Optional: 0.1uF capacitor across the speaker terminals to smooth the audio




Battery voltage sense (GPIO3):

Battery+ --[10k]-- GPIO3 --[10k]-- GND

dont worry this isnt a short circut!




Setup
Open esp32_wav_speaker.ino in the Arduino IDE.
Install the ESP32 board package (core 3.x) if you haven't already.
Board settings:
Board: ESP32C3 Dev Module
Partition Scheme: something with a filesystem, e.g. Default 4MB with spiffs
USB CDC On Boot: Enabled
Flash the sketch.
On your phone, connect to the WiFi network ESP32-Speaker (password: speaker123).
Browse to http://192.168.4.1.
Upload a WAV file and hit Play.
