/*
 * SIDEBAN - Sistem Deteksi Dini Banjir (ESP32 + MQTT)
 * ------------------------------------------------------------
 * Logika sensor, LED, LCD, dan buzzer sama dengan versi offline.
 * Tambahan di versi ini:
 *   - WiFi + MQTT (publish data sensor & status, terima perintah)
 *   - Alarm lokal TETAP jalan walau WiFi / broker putus
 *   - Ambang batas bisa diubah dari dashboard / Telegram (disimpan di flash)
 *   - Buzzer bisa dibisukan dari dashboard / Telegram
 *
 * Library yang perlu di-install (Library Manager Arduino IDE):
 *   - PubSubClient  (Nick O'Leary)
 *   - LiquidCrystal I2C (Frank de Brabander)
 *
 * Board: "ESP32 Dev Module" (DOIT ESP32 DEVKIT V1 juga bisa)
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>

// ===== WAJIB DIISI =====
const char* WIFI_SSID   = "NAMA_WIFI";
const char* WIFI_PASS   = "PASSWORD_WIFI";
const char* MQTT_SERVER = "192.168.1.10";   // IP laptop yang menjalankan Mosquitto
const int   MQTT_PORT   = 1883;
const char* MQTT_USER   = "";               // kosongkan jika broker tanpa password
const char* MQTT_PASS   = "";

// ===== MQTT TOPIC =====
const char* CLIENT_ID       = "sideban-titik01";
const char* TOPIC_AIR       = "sideban/titik01/sensor/air";
const char* TOPIC_HUJAN     = "sideban/titik01/sensor/hujan";
const char* TOPIC_STATUS    = "sideban/titik01/status";
const char* TOPIC_PERANGKAT = "sideban/titik01/perangkat";
const char* TOPIC_CMD_BUZZ  = "sideban/titik01/perintah/buzzer";
const char* TOPIC_CMD_AMBANG= "sideban/titik01/perintah/ambang";

// Interval kirim data
const unsigned long INTERVAL_NORMAL = 10000;  // 10 detik saat AMAN
const unsigned long INTERVAL_CEPAT  = 2000;   // 2 detik saat SIAGA/BAHAYA/hujan deras

// ===== KONFIGURASI PIN =====
#define LED_MERAH 2
#define LED_KUNING 18
#define LED_HIJAU 19
#define BUZZER 27       // Active-LOW: LOW = bunyi, HIGH = mati

#define I2C_SDA 22      // LCD SDA ke D22
#define I2C_SCL 21      // LCD SCL ke D21

#define RAIN_PIN 34     // Raindrop Sensor pin AO ke D34
#define TRIG_PIN 32     // Ultrasonik TRIG ke D32 (OUTPUT)
#define ECHO_PIN 35     // Ultrasonik ECHO ke D35 (INPUT-only)

// ===== KONFIGURASI AMBANG BATAS (nilai awal, bisa diubah lewat MQTT) =====
int batasJarakBahaya = 20;    // <= 20 cm     = MERAH (Bahaya)
int batasJarakSiaga  = 100;   // 21 - 100 cm  = KUNING (Siaga), > 100 cm = HIJAU (Aman)

// Kalibrasi Raindrop Sensor (ADC 0-4095: makin banyak air, angka makin KECIL)
#define ADC_KERING 4095
#define ADC_BASAH_MAX 800

// Batas Persentase Hujan
#define PERSEN_GERIMIS 15     // 16% s/d batas deras = Gerimis (tetap AMAN)
int persenDeras = 65;         // > 65% = Hujan Deras (SIAGA)

LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient espClient;
PubSubClient mqtt(espClient);
Preferences prefs;

bool buzzerBisu = false;            // true = buzzer dibisukan dari dashboard/Telegram
int  statusTerakhir = -1;           // untuk deteksi perubahan status
unsigned long waktuKirimTerakhir = 0;
unsigned long waktuCobaMqtt = 0;

const char* NAMA_STATUS[] = {"AMAN", "SIAGA", "BAHAYA"};

// ------------------------------------------------------------
void bunyiBuzzer(int durasiMs) {
  if (buzzerBisu) {           // dibisukan: tunggu saja tanpa bunyi
    delay(durasiMs);
    return;
  }
  digitalWrite(BUZZER, LOW);   // Bunyi
  delay(durasiMs);
  digitalWrite(BUZZER, HIGH);  // Mati
}

long bacaJarakCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long durasi = pulseIn(ECHO_PIN, HIGH, 30000); // Timeout ~5 meter
  if (durasi == 0) return 400; // Tidak ada pantulan -> dianggap jauh (Aman)

  long cm = durasi * 0.034 / 2;
  if (cm < 2) cm = 2;
  if (cm > 400) cm = 400;
  return cm;
}

int bacaRaindropStabil() {
  long total = 0;
  for (int i = 0; i < 10; i++) {
    total += analogRead(RAIN_PIN);
    delay(5);
  }
  return total / 10;
}

// Ambil angka dari JSON sederhana, contoh: {"bahaya":20,"siaga":100,"deras":65}
// Kalau kunci tidak ada, kembalikan nilaiDefault.
int ambilAngkaJson(const String& json, const char* kunci, int nilaiDefault) {
  String cari = String("\"") + kunci + "\"";
  int pos = json.indexOf(cari);
  if (pos < 0) return nilaiDefault;
  pos = json.indexOf(':', pos + cari.length());
  if (pos < 0) return nilaiDefault;
  pos++;
  while (pos < (int)json.length() && (json[pos] == ' ' || json[pos] == '"')) pos++;
  if (pos >= (int)json.length()) return nilaiDefault;
  if (!(isDigit(json[pos]) || json[pos] == '-')) return nilaiDefault;
  return json.substring(pos).toInt();
}

// ------------------------------------------------------------
void terimaPesan(char* topic, byte* payload, unsigned int length) {
  String isi;
  for (unsigned int i = 0; i < length; i++) isi += (char)payload[i];
  isi.trim();
  Serial.print("[MQTT] Pesan masuk ");
  Serial.print(topic);
  Serial.print(" : ");
  Serial.println(isi);

  if (strcmp(topic, TOPIC_CMD_BUZZ) == 0) {
    isi.toUpperCase();
    if (isi == "MUTE") {
      buzzerBisu = true;
      digitalWrite(BUZZER, HIGH);
    } else if (isi == "AUTO") {
      buzzerBisu = false;
    }
    statusTerakhir = -1;      // paksa kirim status baru
    return;
  }

  if (strcmp(topic, TOPIC_CMD_AMBANG) == 0) {
    int b = ambilAngkaJson(isi, "bahaya", batasJarakBahaya);
    int s = ambilAngkaJson(isi, "siaga", batasJarakSiaga);
    int d = ambilAngkaJson(isi, "deras", persenDeras);

    // Validasi: 2 <= bahaya < siaga <= 400, gerimis < deras <= 100
    if (b >= 2 && b < s && s <= 400 && d > PERSEN_GERIMIS && d <= 100) {
      batasJarakBahaya = b;
      batasJarakSiaga = s;
      persenDeras = d;
      prefs.putInt("bahaya", b);
      prefs.putInt("siaga", s);
      prefs.putInt("deras", d);
      Serial.println("[MQTT] Ambang baru disimpan");
    } else {
      Serial.println("[MQTT] Ambang ditolak (nilai tidak valid)");
    }
    statusTerakhir = -1;      // paksa kirim status dengan ambang terbaru
  }
}

// Sambung ulang MQTT tanpa menghentikan alarm lokal (dicoba tiap 5 detik)
void jagaKoneksi() {
  if (WiFi.status() != WL_CONNECTED) return;   // WiFi sambung ulang otomatis
  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }
  if (millis() - waktuCobaMqtt < 5000) return;
  waktuCobaMqtt = millis();

  Serial.print("[MQTT] Menyambung ke broker... ");
  const char* user = strlen(MQTT_USER) ? MQTT_USER : NULL;
  const char* pass = strlen(MQTT_PASS) ? MQTT_PASS : NULL;
  // Last Will: kalau ESP32 mati/putus, broker otomatis kirim "offline"
  if (mqtt.connect(CLIENT_ID, user, pass, TOPIC_PERANGKAT, 1, true, "offline")) {
    Serial.println("tersambung");
    mqtt.publish(TOPIC_PERANGKAT, "online", true);
    mqtt.subscribe(TOPIC_CMD_BUZZ, 1);
    mqtt.subscribe(TOPIC_CMD_AMBANG, 1);
    statusTerakhir = -1;      // kirim status lengkap setelah tersambung
  } else {
    Serial.print("gagal, rc=");
    Serial.println(mqtt.state());
  }
}

void kirimData(long jarak, int nilaiAir, int persenHujan, const String& teksHujan, int statusAkhir) {
  if (!mqtt.connected()) return;
  char buf[256];

  snprintf(buf, sizeof(buf), "{\"jarak_cm\":%ld}", jarak);
  mqtt.publish(TOPIC_AIR, buf);

  String kategori = teksHujan;
  kategori.trim();
  snprintf(buf, sizeof(buf), "{\"adc\":%d,\"persen\":%d,\"kategori\":\"%s\"}",
           nilaiAir, persenHujan, kategori.c_str());
  mqtt.publish(TOPIC_HUJAN, buf);

  snprintf(buf, sizeof(buf),
           "{\"status\":\"%s\",\"level\":%d,\"jarak_cm\":%ld,\"hujan_persen\":%d,"
           "\"hujan\":\"%s\",\"buzzer\":\"%s\",\"ambang\":{\"bahaya\":%d,\"siaga\":%d,\"deras\":%d}}",
           NAMA_STATUS[statusAkhir], statusAkhir, jarak, persenHujan, kategori.c_str(),
           buzzerBisu ? "MUTE" : "AUTO", batasJarakBahaya, batasJarakSiaga, persenDeras);
  mqtt.publish(TOPIC_STATUS, buf, true);   // retained: dashboard langsung tahu status terakhir
}

// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(LED_MERAH, OUTPUT);
  pinMode(LED_KUNING, OUTPUT);
  pinMode(LED_HIJAU, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, HIGH);  // Pastikan buzzer mati di awal

  pinMode(RAIN_PIN, INPUT);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // Ambang terakhir yang tersimpan di flash
  prefs.begin("sideban", false);
  batasJarakBahaya = prefs.getInt("bahaya", batasJarakBahaya);
  batasJarakSiaga  = prefs.getInt("siaga", batasJarakSiaga);
  persenDeras      = prefs.getInt("deras", persenDeras);

  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("SIDEBAN Siap...");
  lcd.setCursor(0, 1);
  lcd.print("Hubungkan WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long mulai = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - mulai < 10000) {
    delay(250);   // tunggu maksimal 10 detik, setelah itu tetap jalan offline
  }
  lcd.setCursor(0, 1);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Tersambung, IP: ");
    Serial.println(WiFi.localIP());
    lcd.print("WiFi OK         ");
  } else {
    Serial.println("[WiFi] Belum tersambung, alarm lokal tetap jalan");
    lcd.print("Mode offline    ");
  }

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(terimaPesan);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(3);

  delay(1000);
  lcd.clear();
}

void loop() {
  jagaKoneksi();

  int nilaiAir = bacaRaindropStabil();
  long jarak = bacaJarakCm();

  int persenHujan = map(nilaiAir, ADC_KERING, ADC_BASAH_MAX, 0, 100);
  persenHujan = constrain(persenHujan, 0, 100);

  int statusAir = 0;
  String teksHujan = "Kering";

  if (persenHujan > persenDeras) {
    statusAir = 1;
    teksHujan = "Deras ";
  } else if (persenHujan > PERSEN_GERIMIS) {
    statusAir = 0;
    teksHujan = "Grimis";
  }

  int statusJarak = 0;
  if (jarak <= batasJarakBahaya) {
    statusJarak = 2;
  } else if (jarak <= batasJarakSiaga) {
    statusJarak = 1;
  }

  int statusAkhir = max(statusAir, statusJarak);

  // Setelah kembali AMAN, buzzer otomatis aktif lagi untuk kejadian berikutnya
  if (statusAkhir == 0 && buzzerBisu) {
    buzzerBisu = false;
    statusTerakhir = -1;
  }

  // ===== Kirim MQTT: langsung saat status berubah, atau sesuai interval =====
  unsigned long interval = (statusAkhir > 0 || statusAir > 0) ? INTERVAL_CEPAT : INTERVAL_NORMAL;
  if (statusAkhir != statusTerakhir || millis() - waktuKirimTerakhir >= interval) {
    kirimData(jarak, nilaiAir, persenHujan, teksHujan, statusAkhir);
    if (mqtt.connected()) {
      statusTerakhir = statusAkhir;
      waktuKirimTerakhir = millis();
    }
  }

  // Baris 1 LCD: Jarak & Persentase Hujan
  lcd.setCursor(0, 0);
  lcd.print("J:");
  lcd.print(jarak);
  lcd.print("cm H:");
  lcd.print(persenHujan);
  lcd.print("%    ");

  // Debug Serial Monitor
  Serial.print("Jarak: ");
  Serial.print(jarak);
  Serial.print(" cm | ADC: ");
  Serial.print(nilaiAir);
  Serial.print(" (");
  Serial.print(persenHujan);
  Serial.print("% - ");
  Serial.print(teksHujan);
  Serial.print(")");
  Serial.print(mqtt.connected() ? " [MQTT]" : " [offline]");

  // Eksekusi LED, LCD Baris 2, dan Buzzer
  if (statusAkhir == 2) {
    digitalWrite(LED_MERAH, HIGH);
    digitalWrite(LED_KUNING, LOW);
    digitalWrite(LED_HIJAU, LOW);
    lcd.setCursor(0, 1);
    lcd.print("BAHAYA | ");
    lcd.print(teksHujan);
    lcd.print(" ");
    Serial.println(" -> BAHAYA (MERAH)");
    bunyiBuzzer(1000);
    delay(200);
  } else if (statusAkhir == 1) {
    digitalWrite(LED_MERAH, LOW);
    digitalWrite(LED_KUNING, HIGH);
    digitalWrite(LED_HIJAU, LOW);
    lcd.setCursor(0, 1);
    lcd.print("SIAGA  | ");
    lcd.print(teksHujan);
    lcd.print(" ");
    Serial.println(" -> SIAGA (KUNING)");
    for (int i = 0; i < 3; i++) {
      bunyiBuzzer(150);
      delay(150);
    }
  } else {
    digitalWrite(LED_MERAH, LOW);
    digitalWrite(LED_KUNING, LOW);
    digitalWrite(LED_HIJAU, HIGH);
    digitalWrite(BUZZER, HIGH);
    lcd.setCursor(0, 1);
    lcd.print("AMAN   | ");
    lcd.print(teksHujan);
    lcd.print(" ");
    Serial.println(" -> AMAN (HIJAU)");
    delay(500);
  }
}
