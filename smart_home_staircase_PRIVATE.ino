/*
  *** ไฟล์นี้มีรหัสจริงของคุณ (WiFi, AWS, private key) ห้ามส่งให้ใครหรืออัปโหลดที่ไหน ***
*/
/*
  Voice Controlled Smart Home (ESP32) — FINAL VERSION for the real build
  WiFi/AWS IoT Core + EXTERNAL HC-05 Bluetooth module, 3 AC lights + 1 DC
  fan. STAIRCASE (two-way) VERSION: each wall switch and its relay are wired
  as a two-way "switch here / switch there" pair, like a staircase light.
  ---------------------------------------------------------------------
  Command path A (cloud, when WiFi is available):
      Phone app --> AWS IoT Core (MQTT/TLS, over WiFi)
                --> ESP32 (subscribed) --> Relay --> Bulb/Fan
  Command path B (local, always available regardless of WiFi):
      Phone app --> HC-05 (external module, plain UART) --> ESP32
                --> Relay --> Bulb/Fan

  WHY HC-05 INSTEAD OF THE ESP32's BUILT-IN BLUETOOTH:
  The ESP32's own classic-Bluetooth radio reserves a large, fixed chunk of
  RAM (~100KB) the moment it starts, competing with WiFi/TLS for heap, and
  the two radios also have to share the ESP32's single antenna — this was
  observed to cause the phone's "app not responding" dialog while
  connecting/using Bluetooth whenever WiFi was also scanning/connecting at
  the same time. HC-05 is a separate chip talking to the ESP32 over a plain
  UART (two data wires + power), so it does NOT touch the ESP32's own
  Bluetooth radio or RAM, and does not contend for the antenna — it can run
  continuously alongside WiFi/AWS with no interference.

  Wiring (confirm with the no-multimeter GND/line test described in
  PROJECT_HANDOFF.md before trusting this — GPIO25/26 previously tested as
  dead on this exact board for reasons never fully root-caused; GPIO4/13
  were the last pins tried):
    HC-05 TXD -> ESP32 GPIO4  (ESP32 receives here)
    HC-05 RXD -> ESP32 GPIO13 (ESP32 transmits here)
    HC-05 VCC -> 5V (3.3V was tried first and was not reliable)
    HC-05 GND -> ESP32 GND (shared, confirmed with a direct wire, not just
                             a breadboard rail)

  Pair with "HC-05" in Android's Bluetooth settings and in the app's
  ListPicker (NOT "ESP32_SmartHome" — that name only applies to the
  built-in-Bluetooth version of this sketch).

  วงจรบันได (สวิตช์สองทาง) ต่อช่องละชุด ×4 — สวิตช์ไม่ได้ต่อเข้า ESP32:
    สาย Live (ไฟ 1-3) หรือ +12V (พัดลม) -> ขา L (กลาง) ของสวิตช์ BTicino
    ขา L1 ของสวิตช์ -> ขา NO ของรีเลย์ช่องนั้น
    ขา L2 ของสวิตช์ -> ขา NC ของรีเลย์ช่องนั้น
    ขา COM ของรีเลย์ -> หลอด/พัดลม -> Neutral (ไฟ) หรือ GND (พัดลม)
  หลอดติดเมื่อสวิตช์กับรีเลย์ชี้ทางเดียวกัน กดสวิตช์ผนังหรือสั่งให้รีเลย์สลับ
  หลอดก็เปลี่ยนสถานะ บอร์ดดับก็ยังใช้สวิตช์ผนังได้ แต่บอร์ด "ไม่รู้" ว่าหลอดติดหรือดับ
  (ไม่มีวงจรตรวจไฟ) และสวิตช์ผนังไม่ใช่ตัวบังคับปิดอีกต่อไป

  Fan wiring (12V DC — see PROJECT_HANDOFF.md for the full power-supply
  chain: 12V 5A switching PSU -> LM2596 step-down -> 5V logic rail):
    - Fan reuses the 4th relay channel (relay4 = GPIO19) — the light that
      used to be "light four" is now the fan. No new GPIO pins are used.
    - Put a flyback diode (e.g. 1N5408) directly across the fan's two
      power terminals, cathode (banded end) toward +12V.

  You still need to fill in 5 things below (marked "FILL IN"):
    1. WIFI_SSID / WIFI_PASSWORD   (can be a mobile hotspot)
    2. AWS_IOT_ENDPOINT            (from AWS IoT Core console)
    3. AWS_CERT_CA                 (Amazon Root CA 1)
    4. AWS_CERT_CRT                (your device certificate)
    5. AWS_CERT_PRIVATE            (your device private key)

  Library required (install via Arduino Library Manager):
    - "PubSubClient" by Nick O'Leary
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
// No BluetoothSerial here — HC-05 talks over a plain UART (HardwareSerial),
// so the ESP32's internal Bluetooth radio is never started and never
// competes with WiFi/TLS for RAM or antenna time.

// HC-05 wiring — see header comment above before trusting these pins.
const int HC05_RX_PIN = 4;  // HC-05 TXD connects here
const int HC05_TX_PIN = 13; // HC-05 RXD connects here
const unsigned long HC05_BAUD = 9600;
HardwareSerial hc05(2); // UART2

// ---------- 1) FILL IN: WiFi credentials (hotspot is fine) ----------
const char* WIFI_SSID     = "KO";
const char* WIFI_PASSWORD = "eeeeeeee";

// ---------- 2) FILL IN: AWS IoT endpoint ----------
// Find this in AWS IoT Core Console -> Settings -> "Device data endpoint"
const char* AWS_IOT_ENDPOINT = "a93k4os3jgcbu-ats.iot.ap-southeast-1.amazonaws.com";
// Port 8883 (plain MQTT-TLS) is blocked by many mobile-carrier hotspots as a
// non-web port. Port 443 is the standard HTTPS port that is never blocked,
// and AWS IoT Core accepts MQTT over 443 too if the client asks for it via
// ALPN (see the setAlpnProtocols call in connectAWS() below).
const int   AWS_IOT_PORT = 443;
static const char* ALPN_PROTOCOL_LIST[] = { "x-amzn-mqtt-ca", NULL };

const char* MQTT_TOPIC = "smarthome/lights";
const char* MQTT_CLIENT_ID = "esp32_smart_home";

// ---------- 3,4,5) FILL IN: Certificates (from AWS IoT "Create Thing") ----------
static const char AWS_CERT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
)EOF";

static const char AWS_CERT_CRT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDWTCCAkGgAwIBAgIUYDJN7B5E8BbSMW0C7YErn85svrEwDQYJKoZIhvcNAQEL
BQAwTTFLMEkGA1UECwxCQW1hem9uIFdlYiBTZXJ2aWNlcyBPPUFtYXpvbi5jb20g
SW5jLiBMPVNlYXR0bGUgU1Q9V2FzaGluZ3RvbiBDPVVTMB4XDTI2MDkyNTE0NTM0
OVoXDTQ5MTIzMTIzNTk1OVowHjEcMBoGA1UEAwwTQVdTIElvVCBDZXJ0aWZpY2F0
ZTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAL2Lnjz+faKsY/Sy1CY+
BUVcubZDGca7rHfuAD7qIpT38MAYLKN/Wu++1EVL0JryfR7NAVtuusRyttT3y0zh
KWPHLnfIbZOdWBEl/+UC+uwNQnHI0Woc818bSoDZAs2VuuvY3pqEFFRZ8i9rrq1i
1PcpDsUu4WqeXXHH7OCJdYAyTkxIbk/GPzuIEJqAH7Ho+yxkJfPzumDw0fj33YCH
q+q1jSpd3Ml+t05lJtr4cSJ4hqCmfvG3FiDsSRQ2hU9sbNXhw5+vOylKrQKW4j6O
Y7kMp+pWXIxiMpDsLfwrf0ydkJSEdwlmdJYkSWoCyQgNottj95ufDmdpghkMpvGR
690CAwEAAaNgMF4wHwYDVR0jBBgwFoAU1H1fSToQiCuazlqQL0CfC97xYI0wHQYD
VR0OBBYEFMZ+yyysooRVaMA2Q4CS6mkN/xzCMAwGA1UdEwEB/wQCMAAwDgYDVR0P
AQH/BAQDAgeAMA0GCSqGSIb3DQEBCwUAA4IBAQA2We66Co1vkyU05JzTlhGzYIoH
UWqDMAP+sYql7iUVgEA9y3/IB2NBXBl4I6VvneQIeFC/IKOvcMCIF8COTxY0AKq9
V0zaigGZ2qTeoJVam7uigYH0qvTINbvCHTrr2QYeGVYgQ5EnF896V0XJczuX2SmK
yunWSLtPa3wZdwX6v5FYw+/YcHZO8hFU2E152EmwepCrPwWAhRP0pNJI6RPB+486
snxe5uHFgr3/cMlK85BoUG+Fmku7AWqMGYKwCkveOvlyfhLiVD0QtSU1vrKez+Wq
n+tS+syS2sbamTLiT2Be7dsxWeTJq8ZSRvHqyg4Y+WeXb4Ka3MPCPd9d2vwT
-----END CERTIFICATE-----
)EOF";

static const char AWS_CERT_PRIVATE[] PROGMEM = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
MIIEpAIBAAKCAQEAvYuePP59oqxj9LLUJj4FRVy5tkMZxrusd+4APuoilPfwwBgs
o39a777URUvQmvJ9Hs0BW266xHK21PfLTOEpY8cud8htk51YESX/5QL67A1CccjR
ahzzXxtKgNkCzZW669jemoQUVFnyL2uurWLU9ykOxS7hap5dccfs4Il1gDJOTEhu
T8Y/O4gQmoAfsej7LGQl8/O6YPDR+PfdgIer6rWNKl3cyX63TmUm2vhxIniGoKZ+
8bcWIOxJFDaFT2xs1eHDn687KUqtApbiPo5juQyn6lZcjGIykOwt/Ct/TJ2QlIR3
CWZ0liRJagLJCA2i22P3m58OZ2mCGQym8ZHr3QIDAQABAoIBAHdr3v/Y67kxWizS
+v05OELyYAOolTX0Cn7pUfhRtgYbjPDKil1coSPKHJ2oXb8yJojgawl7Y2rgR7c4
hDtk5Zn65ZkMi5Vknhb31Rkp3oQY+pMMDn0HzoJOhItXrv9uZf2wftflfZWDsiYU
d1LAoSM0GB0+I0H0kzVb3C/AC2r/r/ej1GJh0S0St7HjkKX0EloeNPmLOnZ1hSj8
KOkWJUxkfuVaeQlOvuUy6yA3GMX2QygBnT2xwG0GmXyGbkc+LY+Z3g5xxaqlx7Mb
4XrjK7AfagoPZAvRzY592F3xgbPpmrb7CbcXjS7CExezPY3kpbGOuCO90jhpWtNx
XgHJG/UCgYEA899gZppB681U6ExsGCtWRMBNcYn5dQI7gHpXuvFy8o94c8Uj1Fwx
2qCxF9brDx70+TZe9i1iVAVTuZYDvaqRxli2hnzbS7JQkY7PY9yBCwn5T2nC8siZ
gZPxFuOzs+NLP8zU4k7wiXCcY4Z+L2HULLqcSYE2UrpRPPFCbLKWEKcCgYEAxvig
55xhml7RjDTWO09qEgMbyb/WuIEGWnZ26ASX7hulyF6wkPy+EJDZpYk/3/oItPZv
jZUTVHhdZ3QH8ivMWsbzhCj/chNN/V1mdi0AdAk4sWGMgxJLUI8yXPnXWxXTWzkl
QcXd5TSG3RFTszRX7igklmK8xErlsjNTptSBi9sCgYAwTOmW6K5tuXk7mQc/sjWf
4EZ2WlFh71NBmvmNOQz/JZNBuqXiBXCKUJFCm3pqwELcqPwd3dc/tN5kP2X2RrYg
hql/8XKth1+VtKc2rBGS0iLiq5vE4h4lH9v8IJUS582bkUIIoIHbdsZCn/yV/dY8
FammlUThFa3JWC9DwAsguQKBgQClSJoZbE2j98oopyejY0fgqJZn1KWg5QWLFkeW
W2J+GJdxyD7yYhHdHwa9vTLDcyfSc7idK05AaDBnJPsI0BtU+VoG2ivA7oplwzW/
S4jJ23Mt3rPjmcnYpOuYUq7z6kCF0gXCCoVWxniVobQe9IdqvgwOpPzcLr96daqy
tmra6wKBgQCI9KwqmwFNEDa6oimpeaaTymTZ8xIU0C7fmiQQ/DNBo78ABTSDoWUz
ZhRifseXF0DNxjAk9sdLw0MFD+HwRxpJ5hGisJ9alHCbxgRGWWQ4+zwtmMkJdnZh
4KhcbabDJuFNEyVULZAFWKMrg4FGbP1WSnlwcfGEeYkf+xY/7XEBTA==
-----END RSA PRIVATE KEY-----
)EOF";

// ---------- Pin definitions ----------
// NOTE (วงจรบันได): สวิตช์ BTicino 2 ทางทั้ง 4 ตัวไม่ได้ต่อเข้า ESP32 แต่ต่อกับรีเลย์
// แบบ "เปิดตรงนี้ ปิดตรงโน้น": Live/+12V -> ขา L ของสวิตช์, L1 -> รีเลย์ NO,
// L2 -> รีเลย์ NC, รีเลย์ COM -> หลอด/พัดลม -> Neutral/GND (ภาพ staircase_wiring.png)
// กดสวิตช์หรือสั่งรีเลย์สลับ หลอดก็เปลี่ยนสถานะ บอร์ดดับยังใช้สวิตช์ผนังได้
// บอร์ด "ไม่รู้" สถานะหลอดจริง (ไม่มีวงจรตรวจไฟ) จึงจำจากคำสั่งล่าสุดแทน
// ไม่ต้องใช้ขา GPIO หรือโค้ด debounce สำหรับสวิตช์
// โค้ดนี้สั่งเฉพาะคอยล์รีเลย์ด้านล่าง
const int relay1 = 23, relay2 = 22, relay3 = 21, relay4 = 19;

// You only have 4 physical relay channels: 3 drive lights, and the 4th
// (relay4, previously "light four") now drives the fan instead.
const int NUM_LIGHTS = 3;
const int relayPins[NUM_LIGHTS] = {relay1, relay2, relay3};

// Fan reuses the 4th relay channel — not a separate new pin.
const int FAN_RELAY_PIN = relay4; // GPIO19
bool fanState = false;

const bool RELAY_ACTIVE_LOW = true; // most relay boards are active-low

bool lightState[NUM_LIGHTS] = {false, false, false};

WiFiClientSecure net;
PubSubClient client(net);

// ---------- Relay helpers ----------
void writeRelay(int pin, bool on) {
  digitalWrite(pin, RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
}

// ---------- วงจรบันได: รีเลย์ทำหน้าที่เป็น "สวิตช์ตัวที่ 2" ----------
// STAIR_MODE_TOGGLE = true  : ทุกคำสั่ง (เปิด/ปิด/สลับ) กลับสถานะรีเลย์ 1 ครั้ง เหมือนกดสวิตช์บันได
//                             ไม่มีวันค้าง แต่ถ้าหลอดติดอยู่แล้ว คำว่า "เปิด" จะทำให้ดับ
// STAIR_MODE_TOGGLE = false : "เปิด/ปิด" ใช้สถานะที่บอร์ดจำไว้ (ถูกต้องถ้าไม่มีใครกดสวิตช์ผนัง)
//                             ถ้ามีคนกดสวิตช์ผนัง บอร์ดจะจำผิด ให้บอกสถานะจริง เช่น "ไฟ 1 ติดอยู่" / "ไฟ 1 ดับอยู่"
//                             (บอร์ดจำค่าใหม่โดยไม่สลับรีเลย์) หรือพูด "สลับ" ซึ่งกลับสถานะรีเลย์เสมอ
const bool STAIR_MODE_TOGGLE = false;   // ค่าเริ่มต้น: เปิด/ปิดตามที่จำไว้ (ดูคำอธิบายด้านบน)
bool lampBelief[NUM_LIGHTS] = {false, false, false};   // ใช้เฉพาะโหมด false: สถานะหลอดที่บอร์ด "คิดว่า" เป็น (เริ่มต้นสมมุติว่าดับ)
bool fanBelief = false;

// lightState[] / fanState = ตำแหน่งรีเลย์ที่บอร์ดสั่ง (true = NO, false = NC) ไม่ใช่สถานะหลอด
void flipLight(int index) {
  if (index < 0 || index >= NUM_LIGHTS) return;
  lightState[index] = !lightState[index];
  writeRelay(relayPins[index], lightState[index]);
  lampBelief[index] = !lampBelief[index];
  Serial.printf("Light %d: relay -> %s (lamp toggled)\n", index + 1, lightState[index] ? "NO" : "NC");
}

void flipFan() {
  fanState = !fanState;
  writeRelay(FAN_RELAY_PIN, fanState);
  fanBelief = !fanBelief;
  Serial.printf("Fan: relay -> %s (fan toggled)\n", fanState ? "NO" : "NC");
}

void setLight(int index, bool on) {
  if (index < 0 || index >= NUM_LIGHTS) return;
  if (!STAIR_MODE_TOGGLE && lampBelief[index] == on) {
    Serial.printf("Light %d: already %s (from memory). If the lamp is really %s, say 'ไฟ %d %s' then repeat\n",
                  index + 1, on ? "ON" : "OFF", on ? "OFF" : "ON", index + 1, on ? "ดับอยู่" : "ติดอยู่");
    return;
  }
  flipLight(index);
}

void setFan(bool on) {
  if (!STAIR_MODE_TOGGLE && fanBelief == on) {
    Serial.printf("Fan: already %s (from memory). If the fan is really %s, say 'พัดลม %s' then repeat\n",
                  on ? "ON" : "OFF", on ? "OFF" : "ON", on ? "ดับอยู่" : "ติดอยู่");
    return;
  }
  flipFan();
}

void setAllLights(bool on) {
  for (int i = 0; i < NUM_LIGHTS; i++) setLight(i, on);
  setFan(on);
}

void toggleAll() {
  for (int i = 0; i < NUM_LIGHTS; i++) flipLight(i);
  flipFan();
}

// ---------- Command parsing (English + Thai) ----------
String normalize(String s) { s.trim(); s.toLowerCase(); return s; }

int findLightIndex(const String &cmd) {
  if (cmd.indexOf("1") != -1 || cmd.indexOf("one") != -1 || cmd.indexOf("หนึ่ง") != -1 || cmd.indexOf("๑") != -1) return 0;
  if (cmd.indexOf("2") != -1 || cmd.indexOf("two") != -1 || cmd.indexOf("สอง") != -1 || cmd.indexOf("๒") != -1) return 1;
  if (cmd.indexOf("3") != -1 || cmd.indexOf("three") != -1 || cmd.indexOf("สาม") != -1 || cmd.indexOf("๓") != -1) return 2;
  // No "light four" anymore — that channel is the fan now (see meansFan()).
  return -1;
}

// Match a whole English word only ("one" must not count as "on")
bool hasWord(const String &cmd, const char* word) {
  int len = strlen(word);
  int from = 0;
  while (true) {
    int i = cmd.indexOf(word, from);
    if (i == -1) return false;
    bool startOk = (i == 0) || cmd[i - 1] == ' ';
    bool endOk = (i + len == (int)cmd.length()) || cmd[i + len] == ' ';
    if (startOk && endOk) return true;
    from = i + 1;
  }
}

// Returns 1 = ON, 0 = OFF, -1 = unknown.
// Thai "เปิด" contains the substring "ปิด", so "เปิด" must be tested first.
int parseAction(const String &c) {
  if (c.indexOf("สลับ") != -1 || hasWord(c, "toggle") || hasWord(c, "switch")) return 2;   // สลับสถานะ
  if (c.indexOf("เปิด") != -1) return 1;
  if (c.indexOf("ปิด") != -1) return 0;
  if (c.indexOf("ติด") != -1) return 1;
  if (c.indexOf("ดับ") != -1) return 0;
  if (hasWord(c, "off")) return 0;
  if (hasWord(c, "on")) return 1;
  return -1;
}

bool meansAll(const String &c) { return c.indexOf("all") != -1 || c.indexOf("ทั้งหมด") != -1 || c.indexOf("ทุกดวง") != -1; }
// พัดลม = รีเลย์ช่องที่ 4 จึงรับทั้งคำว่า "พัดลม"/"fan" และเลข 4 (เช่น "สลับ ไฟ 4", "ปิด 4", "light four", "ไฟสี่", "๔")
bool meansFan(const String &c) {
  return c.indexOf("fan") != -1 || c.indexOf("พัดลม") != -1 ||
         c.indexOf("4") != -1 || hasWord(c, "four") || c.indexOf("สี่") != -1 || c.indexOf("๔") != -1;
}

void handleCommand(String raw) {
  String cmd = normalize(raw);
  if (cmd.length() == 0) return;
  Serial.print("Command: "); Serial.println(cmd);

  int action = parseAction(cmd);
  if (action == -1) { Serial.println("Not recognized"); return; }

  // บอกสถานะจริงของหลอด (หลังมีคนกดสวิตช์ผนัง): จำค่าใหม่ ไม่สลับรีเลย์
  if ((action == 0 || action == 1) && (cmd.indexOf("อยู่") != -1 || hasWord(cmd, "is"))) {
    bool isOn = (action == 1);
    if (meansFan(cmd)) fanBelief = isOn;
    else if (meansAll(cmd)) { for (int i = 0; i < NUM_LIGHTS; i++) lampBelief[i] = isOn; fanBelief = isOn; }
    else { int k = findLightIndex(cmd); if (k != -1) lampBelief[k] = isOn; else { Serial.println("No light number or 'fan' found"); return; } }
    Serial.printf("Noted: %s (memory updated, relay not changed)\n", isOn ? "ON" : "OFF");
    return;
  }

  if (action == 2) {                               // "สลับ": กลับสถานะเสมอ ไม่ว่าโหมดไหน
    if (meansFan(cmd)) { flipFan(); return; }
    if (meansAll(cmd)) { toggleAll(); return; }
    int t = findLightIndex(cmd);
    if (t != -1) flipLight(t);
    else Serial.println("No light number or 'fan' found");
    return;
  }
  bool on = (action == 1);

  if (meansFan(cmd)) { setFan(on); return; }
  if (meansAll(cmd)) { setAllLights(on); return; }

  int idx = findLightIndex(cmd);
  if (idx != -1) setLight(idx, on);
  else Serial.println("No light number or 'fan' found");
}

// ---------- MQTT callback: runs when AWS IoT Core sends a message ----------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  handleCommand(msg);
}

// ---------- Connect to WiFi (bounded — never blocks forever) ----------
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 8000;

bool connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("\nNo WiFi found (timed out) — continuing without it. "
                      "Bluetooth fallback and physical switches still work.");
      return false;
    }
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
  return true;
}

// ---------- Sync clock via NTP ----------
// REQUIRED before AWS IoT TLS will succeed: the ESP32 boots with no idea what
// the current date/time is, and TLS certificate validation checks the cert's
// valid-from/valid-until dates against the device's clock. Without this, AWS
// IoT Core connections fail silently and retry forever.
void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time via NTP");
  time_t now = time(nullptr);
  unsigned long start = millis();
  // 8*3600*2 is just "sometime in 1970" — wait until the clock looks real.
  while (now < 8 * 3600 * 2) {
    delay(300);
    Serial.print(".");
    now = time(nullptr);
    if (millis() - start > 15000) {
      Serial.println("\nNTP sync taking too long (mobile hotspots sometimes "
                      "block UDP port 123). Continuing anyway — if AWS still "
                      "won't connect, try a different WiFi/hotspot for NTP.");
      return;
    }
  }
  Serial.println();
  Serial.print("Time synced: ");
  Serial.println(ctime(&now));
}

// ---------- Connect to AWS IoT Core (MQTT over TLS) ----------
void connectAWS() {
  net.setCACert(AWS_CERT_CA);
  net.setCertificate(AWS_CERT_CRT);
  net.setPrivateKey(AWS_CERT_PRIVATE);
  net.setAlpnProtocols(ALPN_PROTOCOL_LIST); // required for AWS IoT Core over port 443

  client.setServer(AWS_IOT_ENDPOINT, AWS_IOT_PORT);
  client.setCallback(mqttCallback);

  Serial.print("Connecting to AWS IoT Core");
  unsigned long start = millis();
  while (!client.connected()) {
    if (client.connect(MQTT_CLIENT_ID)) {
      Serial.println("\nConnected to AWS IoT Core!");
      client.subscribe(MQTT_TOPIC);
      Serial.print("Subscribed to topic: ");
      Serial.println(MQTT_TOPIC);
    } else {
      // PubSubClient error codes, printed so we know WHICH problem it is:
      //  -4 timeout reaching server   -2 TCP/TLS connect failed (endpoint/port/cert/firewall)
      //   5 connected but AWS rejected it (IoT Policy doesn't allow this clientId/actions)
      Serial.print(" [state="); Serial.print(client.state()); Serial.print("]");
      delay(2000);
      if (millis() - start > 30000) {
        Serial.println("\nAWS connect taking too long — giving up this attempt, will retry in loop()");
        return; // don't block forever; loop() will call connectAWS() again
      }
    }
  }
}

// How often loop() is allowed to retry WiFi while it's down.
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
unsigned long lastWifiAttempt = 0;

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_LIGHTS; i++) {
    pinMode(relayPins[i], OUTPUT);
    writeRelay(relayPins[i], false);
  }
  pinMode(FAN_RELAY_PIN, OUTPUT);
  writeRelay(FAN_RELAY_PIN, false);

  hc05.begin(HC05_BAUD, SERIAL_8N1, HC05_RX_PIN, HC05_TX_PIN);
  Serial.println("HC-05 UART started (Serial2). Pair with 'HC-05' in Android Bluetooth settings.");
  Serial.print("[heap] free after HC-05 UART start="); Serial.println(ESP.getFreeHeap());

  if (connectWiFi()) {
    syncTime();
    connectAWS();
  }
  Serial.print("[heap] free after WiFi/AWS setup="); Serial.println(ESP.getFreeHeap());
  // If WiFi isn't found, that's fine — HC-05 above already works regardless,
  // and the loop() below retries WiFi periodically without blocking it.
}

void loop() {
  // No antenna-contention guard needed here — HC-05 is a separate chip with
  // its own radio, so WiFi can scan/connect freely without affecting it at
  // all. WiFi retry runs on its own normal schedule.
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifiAttempt > WIFI_RETRY_INTERVAL_MS) {
      lastWifiAttempt = millis();
      if (connectWiFi()) {
        syncTime();
        connectAWS();
      }
    }
  } else if (!client.connected()) {
    connectAWS();
  } else {
    client.loop();
  }

  // Commands over HC-05 — always listened to, regardless of WiFi status.
  if (hc05.available()) {
    String incoming = hc05.readStringUntil('\n');
    handleCommand(incoming);
  }

  // Watch free heap continuously. Should stay essentially flat in this
  // version (HC-05 doesn't touch the ESP32's heap the way its internal
  // Bluetooth radio did) — if it still trends toward 0, something else is
  // leaking and is worth investigating independently of the BT-vs-HC05
  // question.
  static unsigned long lastHeapPrint = 0;
  if (millis() - lastHeapPrint > 5000) {
    lastHeapPrint = millis();
    Serial.print("[heap] free="); Serial.print(ESP.getFreeHeap()); Serial.println(" bytes");
  }

  // Type a command directly into the Serial Monitor (e.g. "fan on") and press
  // Enter — useful for testing without needing the app, Bluetooth, or AWS.
  if (Serial.available()) {
    String incoming = Serial.readStringUntil('\n');
    handleCommand(incoming);
  }
}
