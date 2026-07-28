/*
  ESP32-C3 Supermini WAV Player (streaming version)
  --------------------------------------------------
  Hosts a WiFi hotspot + web page where you can upload a WAV file, then
  plays it back through a speaker driven by a 2N2222 transistor on GPIO10.

  This version STREAMS audio from flash (LittleFS) instead of loading the
  whole file into RAM, so file size is limited by your flash storage
  (megabytes), not by the ~320KB of usable heap on the ESP32-C3.

  WIRING:
    GPIO10 --[1k resistor]--> Base (2N2222)
    Emitter -> GND
    Collector -> Speaker (+)
    Speaker (-) -> 3.3V or 5V supply
    Optional: 0.1uF cap across speaker terminals to smooth the PWM carrier

  REQUIREMENTS:
    - Arduino-ESP32 core 3.x (uses the newer ledcAttach/ledcWrite API)
    - Board: "ESP32C3 Dev Module" (or your specific Supermini variant)
    - Tools > Partition Scheme: pick one that includes a filesystem
      (e.g. "Default 4MB with spiffs") so LittleFS has space to mount
    - Tools > USB CDC On Boot: Enabled (so Serial prints show up)

  WAV FILE NOTES:
    - Supports 8-bit or 16-bit PCM WAV (mono or stereo; stereo just
      uses the left channel, no downmixing).
    - Max file size is basically whatever your flash's LittleFS
      partition can hold (a few MB typically).
    - If you have ffmpeg, an easy way to prep a file:
        ffmpeg -i input.mp3 -ar 8000 -ac 1 -sample_fmt u8 output.wav
*/

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include "esp_timer.h"

// ---------- WiFi Access Point ----------
// The ESP32 creates its OWN WiFi network. Connect your phone/laptop to this
// network name, then browse to 192.168.4.1
const char* ap_ssid     = "ESP32-Speaker";
const char* ap_password = "speaker123"; // must be 8+ characters

// ---------- Speaker config ----------
#define SPEAKER_PIN      10
#define LEDC_RESOLUTION  8          // 8-bit duty (0-255)
#define LEDC_CARRIER_HZ  62500      // PWM carrier freq, well above audio band

WebServer server(80);

const char* WAV_PATH = "/audio.wav";
File uploadFile;

// ---------- Streaming playback state ----------
#define RING_SIZE 8192              // ~1 sec of buffering at 8kHz mono
uint8_t ringBuf[RING_SIZE];
volatile size_t ringHead = 0;       // fill task writes here
volatile size_t ringTail = 0;       // timer reads here
volatile bool   isPlaying = false;
volatile bool   streamDone = false; // true once no more file data remains to read

File     playFile;
uint32_t sampleRateHz = 8000;
uint16_t fileBitsPerSample = 8;
uint16_t fileChannels = 1;
uint32_t fileFrameBytes = 1;        // bytesPerSample * channels
size_t   remainingDataBytes = 0;

esp_timer_handle_t playTimer = nullptr;

// ---------- Volume / gain ----------
// 100 = normal (unity) volume, 0 = mute, up to 1000 = 10x boost (will clip/distort at high values)
volatile int gainPercent = 100;

// ---------- Battery sense ----------
// Voltage divider: Battery+ --[10k]-- GPIO3 --[10k]-- GND
// This halves the battery voltage so it's safe for the ADC.
#define BATTERY_ADC_PIN   3
#define DIVIDER_RATIO     2.0f   // multiply the reading by this to get actual battery voltage

// ---------------- Web page ----------------
const char* PAGE_HTML = R"HTML(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 Speaker</title>
<style>
body{font-family:sans-serif;text-align:center;margin-top:40px;background:#111;color:#eee}
button{padding:12px 24px;font-size:18px;margin:8px;border-radius:8px;border:none;background:#2b7de9;color:#fff;cursor:pointer}
input[type=file]{margin:12px;color:#eee}
#status{margin-top:10px;color:#9f9}
.battery{width:52px;height:24px;border:2px solid #ccc;border-radius:4px;position:relative;display:inline-block;vertical-align:middle;margin-right:6px}
.battery:after{content:'';position:absolute;right:-6px;top:6px;width:4px;height:10px;background:#ccc;border-radius:0 2px 2px 0}
.battery-fill{height:100%;border-radius:1px;transition:width .4s, background-color .4s}
#battWrap{margin-top:18px;display:flex;align-items:center;justify-content:center}
</style></head><body>
<h2>ESP32-C3 WAV Player</h2>
<div id="battWrap">
  <div class="battery"><div class="battery-fill" id="battFill" style="width:100%;background:#4caf50"></div></div>
  <span id="battText">-- %</span>
</div>
<form id="f" method="POST" action="/upload" enctype="multipart/form-data">
  <input type="file" name="wav" accept=".wav" required><br>
  <button type="submit">Upload</button>
</form>
<button onclick="fetch('/play')">Play</button>
<button onclick="fetch('/stop')">Stop</button>
<br>
<label for="vol">Volume boost: <span id="volLabel">100</span></label><br>
<input type="range" id="vol" min="0" max="1000" value="100" style="width:80%">
<p id="status"></p>
<script>
document.getElementById('f').addEventListener('submit', async (e) => {
  e.preventDefault();
  const data = new FormData(e.target);
  document.getElementById('status').innerText = 'Uploading...';
  const res = await fetch('/upload', { method: 'POST', body: data });
  document.getElementById('status').innerText = await res.text();
});

const vol = document.getElementById('vol');
const volLabel = document.getElementById('volLabel');
let volTimeout = null;
vol.addEventListener('input', () => {
  volLabel.innerText = vol.value;
  clearTimeout(volTimeout);
  volTimeout = setTimeout(() => fetch('/volume?level=' + vol.value), 80);
});

async function updateBattery() {
  try {
    const res = await fetch('/battery');
    const data = await res.json();
    const fill = document.getElementById('battFill');
    const text = document.getElementById('battText');
    const pct = Math.max(0, Math.min(100, data.percent));
    fill.style.width = pct + '%';
    fill.style.background = pct > 50 ? '#4caf50' : (pct > 20 ? '#ff9800' : '#f44336');
    text.innerText = pct + '% (' + data.voltage.toFixed(2) + 'V)';
  } catch (e) { /* ignore a missed poll */ }
}
updateBattery();
setInterval(updateBattery, 5000);
</script>
</body></html>
)HTML";

// ---------------- WAV parsing ----------------
struct WavInfo {
  uint32_t sampleRate;
  uint16_t bitsPerSample;
  uint16_t numChannels;
  uint32_t dataOffset;
  uint32_t dataSize;
};

bool parseWavHeader(File &f, WavInfo &info) {
  uint8_t riff[12];
  if (f.read(riff, 12) != 12) return false;
  if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) return false;

  bool gotFmt = false, gotData = false;
  info.numChannels = 1;
  info.bitsPerSample = 8;
  info.sampleRate = 8000;

  uint8_t chunkHeader[8];
  while (f.read(chunkHeader, 8) == 8) {
    char id[5] = {0};
    memcpy(id, chunkHeader, 4);
    uint32_t size;
    memcpy(&size, chunkHeader + 4, 4);

    if (memcmp(id, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      uint32_t toRead = size < 16 ? size : 16;
      f.read(fmt, toRead);
      memcpy(&info.numChannels, fmt + 2, 2);
      memcpy(&info.sampleRate, fmt + 4, 4);
      memcpy(&info.bitsPerSample, fmt + 14, 2);
      if (size > toRead) f.seek(f.position() + (size - toRead));
      gotFmt = true;
    } else if (memcmp(id, "data", 4) == 0) {
      info.dataOffset = f.position();
      info.dataSize = size;
      gotData = true;
      break; // we only care about the first data chunk
    } else {
      f.seek(f.position() + size); // skip unknown chunk
    }
    if (size % 2 == 1) f.seek(f.position() + 1); // chunks are word-aligned
  }
  return gotFmt && gotData;
}

// Just peek the header for validation/info display after upload (doesn't load audio)
bool peekWavInfo(WavInfo &info) {
  File f = LittleFS.open(WAV_PATH, "r");
  if (!f) return false;
  bool ok = parseWavHeader(f, info);
  f.close();
  return ok && (info.bitsPerSample == 8 || info.bitsPerSample == 16) && info.numChannels > 0;
}

// ---------------- Battery reading ----------------
// Rough discharge curve for 3x AAA alkaline in series (total pack voltage -> %).
// Not perfectly linear - alkaline cells sag slowly then drop off near the end.
float voltageToPercent(float v) {
  struct Point { float volts; float pct; };
  static const Point curve[] = {
    {4.5f, 100.0f},
    {4.2f,  90.0f},
    {3.9f,  70.0f},
    {3.6f,  50.0f},
    {3.3f,  30.0f},
    {3.0f,  10.0f},
    {2.7f,   0.0f}
  };
  const int n = sizeof(curve) / sizeof(curve[0]);

  if (v >= curve[0].volts) return 100.0f;
  if (v <= curve[n - 1].volts) return 0.0f;

  for (int i = 0; i < n - 1; i++) {
    if (v <= curve[i].volts && v >= curve[i + 1].volts) {
      float span = curve[i].volts - curve[i + 1].volts;
      float frac = (v - curve[i + 1].volts) / span;
      return curve[i + 1].pct + frac * (curve[i].pct - curve[i + 1].pct);
    }
  }
  return 0.0f;
}

float readBatteryVoltage() {
  uint32_t mv = analogReadMilliVolts(BATTERY_ADC_PIN);
  return (mv / 1000.0f) * DIVIDER_RATIO;
}


void fillRingBuffer() {
  if (!playFile || remainingDataBytes == 0) return;
  uint8_t frame[8];

  while (remainingDataBytes >= fileFrameBytes) {
    size_t used = (ringHead + RING_SIZE - ringTail) % RING_SIZE;
    size_t freeSpace = RING_SIZE - used - 1; // leave 1 slot open to distinguish full/empty
    if (freeSpace == 0) break; // buffer is full, try again next loop() pass

    if (playFile.read(frame, fileFrameBytes) != fileFrameBytes) {
      remainingDataBytes = 0;
      break;
    }

    uint8_t rawSample;
    if (fileBitsPerSample == 8) {
      rawSample = frame[0]; // WAV 8-bit PCM is already unsigned
    } else {
      int16_t s16;
      memcpy(&s16, frame, 2); // left channel, first 2 bytes of the frame
      rawSample = (uint8_t)((s16 >> 8) + 128); // signed 16-bit -> unsigned 8-bit
    }

    // Apply gain around the center point (128 = silence), then clip to 0-255
    int centered = (int)rawSample - 128;
    centered = (centered * gainPercent) / 100;
    if (centered > 127) centered = 127;
    if (centered < -128) centered = -128;

    ringBuf[ringHead] = (uint8_t)(centered + 128);
    ringHead = (ringHead + 1) % RING_SIZE;
    remainingDataBytes -= fileFrameBytes;
  }

  if (remainingDataBytes == 0) streamDone = true;
}

// ---------------- Playback timer (fires once per audio sample) ----------------
void IRAM_ATTR onTimer(void* arg) {
  if (ringTail != ringHead) {
    ledcWrite(SPEAKER_PIN, ringBuf[ringTail]);
    ringTail = (ringTail + 1) % RING_SIZE;
  } else if (streamDone) {
    // ring buffer drained and no more file data coming - playback finished
    isPlaying = false;
    ledcWrite(SPEAKER_PIN, 0);
    esp_timer_stop(playTimer);
  }
  // else: buffer underrun (fill task fell behind) - just hold, next tick will retry
}

bool startPlayback() {
  if (isPlaying) return false;

  WavInfo info;
  if (playFile) playFile.close();
  playFile = LittleFS.open(WAV_PATH, "r");
  if (!playFile) return false;
  if (!parseWavHeader(playFile, info) ||
      (info.bitsPerSample != 8 && info.bitsPerSample != 16) ||
      info.numChannels == 0) {
    playFile.close();
    return false;
  }

  sampleRateHz = info.sampleRate;
  fileBitsPerSample = info.bitsPerSample;
  fileChannels = info.numChannels;
  fileFrameBytes = (fileBitsPerSample / 8) * fileChannels;
  remainingDataBytes = info.dataSize;
  playFile.seek(info.dataOffset);

  ringHead = 0;
  ringTail = 0;
  streamDone = false;

  fillRingBuffer(); // pre-fill before starting the timer, to avoid an initial underrun

  isPlaying = true;
  if (!playTimer) {
    esp_timer_create_args_t args = {};
    args.callback = &onTimer;
    args.arg = nullptr;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "audio_timer";
    esp_timer_create(&args, &playTimer);
  }
  uint64_t periodUs = 1000000ULL / sampleRateHz;
  esp_timer_start_periodic(playTimer, periodUs);
  return true;
}

void stopPlayback() {
  isPlaying = false;
  if (playTimer) esp_timer_stop(playTimer);
  ledcWrite(SPEAKER_PIN, 0);
  if (playFile) playFile.close();
}

// ---------------- Upload handlers ----------------
void handleUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    stopPlayback(); // don't write over a file that's currently streaming
    uploadFile = LittleFS.open(WAV_PATH, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
  }
}

void handleUploadDone() {
  WavInfo info;
  if (peekWavInfo(info)) {
    server.send(200, "text/plain",
      "Upload OK. Sample rate: " + String(info.sampleRate) +
      " Hz, bits: " + String(info.bitsPerSample) +
      ", channels: " + String(info.numChannels) +
      ", data size: " + String(info.dataSize) + " bytes");
  } else {
    server.send(400, "text/plain",
      "Failed to parse WAV. Needs 8 or 16-bit PCM.");
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  ledcAttach(SPEAKER_PIN, LEDC_CARRIER_HZ, LEDC_RESOLUTION);
  ledcWrite(SPEAKER_PIN, 0);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  Serial.println("Access Point started");
  Serial.print("Network name: ");
  Serial.println(ap_ssid);
  Serial.print("Connect to it, then browse to: ");
  Serial.println(WiFi.softAPIP()); // normally 192.168.4.1

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", PAGE_HTML); });
  server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  server.on("/play", HTTP_GET, []() {
    bool ok = startPlayback();
    server.send(200, "text/plain", ok ? "Playing" : "Could not start playback (bad file or already playing)");
  });
  server.on("/stop", HTTP_GET, []() { stopPlayback(); server.send(200, "text/plain", "Stopped"); });
  server.on("/volume", HTTP_GET, []() {
    if (server.hasArg("level")) {
      int lvl = server.arg("level").toInt();
      if (lvl < 0) lvl = 0;
      if (lvl > 1000) lvl = 1000;
      gainPercent = lvl;
    }
    server.send(200, "text/plain", "Volume set to " + String(gainPercent));
  });
  server.on("/battery", HTTP_GET, []() {
    float v = readBatteryVoltage();
    float pct = voltageToPercent(v);
    String json = "{\"voltage\":" + String(v, 2) + ",\"percent\":" + String((int)pct) + "}";
    server.send(200, "application/json", json);
  });

  server.begin();
  Serial.println("Web server started. Go to the IP address above in your browser.");
}

void loop() {
  server.handleClient();
  fillRingBuffer(); // keep topping up the ring buffer while playing
}
