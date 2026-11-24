# SIDEBAN – Backend (MQTT + Node-RED + Telegram)

```
ESP32 (sensor) ──MQTT──> Mosquitto (laptop) ──MQTT──> Node-RED ──> Dashboard web
        ^                                                   │
        └──────────── perintah (bisukan, ambang) ◄──────────┴──> Bot Telegram (warga & petugas)
```

Isi folder:

| File | Fungsi |
|---|---|
| `esp32/sideban_mqtt/sideban_mqtt.ino` | Kode ESP32 kamu + WiFi + MQTT |
| `mosquitto/mosquitto.conf` | Konfigurasi broker MQTT |
| `node-red/sideban_flow.json` | Flow Node-RED (dashboard + bot Telegram), tinggal di-import |
| `contoh_dashboard.png` | Contoh tampilan dashboard |

Logika sensor, LED, LCD, dan buzzer **tidak diubah**. Kalau WiFi atau broker mati, alarm lokal tetap jalan.

---

## 1. Yang perlu disiapkan

- Laptop dan ESP32 di **WiFi yang sama** (hotspot HP juga bisa). ESP32 hanya bisa WiFi **2,4 GHz**.
- **Node.js 22 LTS** atau lebih baru (https://nodejs.org). Node Telegram butuh Node.js ≥ 22.19.
- **Mosquitto** (https://mosquitto.org/download).
- Arduino IDE dengan board ESP32 (Espressif) sudah terpasang.

## 2. Jalankan broker Mosquitto

1. Install Mosquitto (Windows: biasanya ke `C:\Program Files\mosquitto`).
2. Salin `mosquitto/mosquitto.conf` ke folder itu.
3. Buka **Command Prompt sebagai Administrator**, lalu:
   ```
   cd "C:\Program Files\mosquitto"
   net stop mosquitto
   mosquitto -c mosquitto.conf -v
   ```
   `net stop` mematikan service bawaan yang hanya menerima koneksi dari laptop sendiri. Biarkan jendela ini terbuka.
4. Saat Windows Firewall bertanya, klik **Allow** (centang Private network).
5. Cari IP laptop: buka Command Prompt baru, ketik `ipconfig`, catat **IPv4 Address** dari adapter WiFi (misal `192.168.1.10`).

## 3. Upload kode ESP32

1. Arduino IDE → Library Manager, install:
   - **PubSubClient** (Nick O'Leary)
   - **LiquidCrystal I2C** (Frank de Brabander), kalau belum
2. Buka `sideban_mqtt.ino`, isi bagian paling atas:
   ```cpp
   const char* WIFI_SSID   = "NAMA_WIFI";
   const char* WIFI_PASS   = "PASSWORD_WIFI";
   const char* MQTT_SERVER = "192.168.1.10";   // IP laptop dari langkah 2
   ```
3. Board: **DOIT ESP32 DEVKIT V1** (atau ESP32 Dev Module), lalu Upload.
4. Buka Serial Monitor (115200). Kalau berhasil, muncul `[MQTT] Menyambung ke broker... tersambung` dan tiap baris diakhiri `[MQTT]`.

Kode ini sudah saya compile untuk ESP32 DOIT DevKit V1 (core ESP32 3.3.6) tanpa error.

## 4. Jalankan Node-RED

1. Install dan jalankan:
   ```
   npm install -g --unsafe-perm node-red
   node-red
   ```
2. Buka http://localhost:1880 → menu ☰ → **Manage palette** → tab **Install**, install:
   - `@flowfuse/node-red-dashboard`
   - `node-red-contrib-telegrambot`
3. Supaya daftar warga Telegram tidak hilang saat Node-RED restart, buka file
   `C:\Users\<nama kamu>\.node-red\settings.js`, cari bagian `contextStorage`, ganti menjadi:
   ```js
   contextStorage: {
       default: { module: "localfilesystem" }
   },
   ```
   Simpan, lalu tutup dan jalankan ulang `node-red`.
4. Menu ☰ → **Import** → pilih file `node-red/sideban_flow.json` → **Import** → **Deploy**.
5. Buka dashboard: **http://localhost:1880/dashboard/sideban**
   HP di WiFi yang sama juga bisa membuka `http://<IP laptop>:1880/dashboard/sideban`.

## 5. Siapkan bot Telegram

1. Di Telegram, chat **@BotFather** → `/newbot` → beri nama → salin **token**.
2. Di Node-RED, klik dua kali node **Pesan masuk** → klik ikon pensil di samping *Bot* (`SidebanBot`) → tempel token di kolom **Token** → **Update** → **Done** → **Deploy**.
3. Buka bot kamu di Telegram, kirim `/id`. Bot membalas Chat ID kamu.
4. Klik dua kali node **Pengaturan admin**, isi Chat ID itu:
   ```js
   const ADMIN_CHAT_ID = [
       123456789,
   ];
   ```
   **Deploy**. Sekarang kamu admin (petugas).
5. Kirim `/daftar` supaya kamu juga menerima peringatan. Warga cukup kirim `/daftar` ke bot.

---

## Topic MQTT

| Topic | Arah | Isi | Keterangan |
|---|---|---|---|
| `sideban/titik01/sensor/air` | ESP32 → | `{"jarak_cm":45}` | tiap 10 s, atau 2 s saat SIAGA/BAHAYA/hujan deras |
| `sideban/titik01/sensor/hujan` | ESP32 → | `{"adc":3200,"persen":22,"kategori":"Grimis"}` | sama |
| `sideban/titik01/status` | ESP32 → | `{"status":"SIAGA","level":1,"jarak_cm":45,"hujan_persen":22,"hujan":"Grimis","buzzer":"AUTO","ambang":{...}}` | retained, dikirim langsung saat status berubah |
| `sideban/titik01/perangkat` | ESP32 → | `online` / `offline` | retained; `offline` dikirim broker otomatis (Last Will) |
| `sideban/titik01/perintah/buzzer` | → ESP32 | `MUTE` / `AUTO` | dari dashboard atau Telegram |
| `sideban/titik01/perintah/ambang` | → ESP32 | `{"bahaya":20,"siaga":100,"deras":65}` | retained; ESP32 juga menyimpannya di flash |
| `sideban/titik01/warga/W001/batas` | Node-RED → | `{"kategori":"rendah","batas_cm":120}` | retained, satu topic per warga; dihapus saat warga `/berhenti` |

Buzzer yang dibisukan otomatis aktif lagi setelah status kembali **AMAN**, supaya kejadian berikutnya tetap berbunyi.

## Perintah bot Telegram

| Perintah | Siapa | Fungsi |
|---|---|---|
| `/daftar` | semua | mulai menerima peringatan |
| `/berhenti` | semua | berhenti menerima peringatan |
| `/status` | semua | kondisi terkini |
| `/id` | semua | lihat Chat ID |
| `/ambang` | semua | lihat ambang batas |
| `/rumah rendah` / `sedang` / `tinggi` | warga | atur batas pribadi sesuai ketinggian rumah (otomatis ikut `/daftar`) |
| `/batas 90` | warga | atur batas pribadi dengan angka sendiri (cm) |
| `/rumah` | warga | lihat batas rumah sendiri dan pilihan kategori |
| `/bisukan` | admin | bisukan buzzer di lokasi |
| `/aktifkan` | admin | buzzer kembali otomatis |
| `/ambang 20 100 65` | admin | ubah ambang (bahaya cm, siaga cm, deras %) |

Peringatan otomatis:
- Naik ke **SIAGA** atau **BAHAYA**: langsung dikirim ke semua warga terdaftar.
- Turun level (misal kembali AMAN): dikirim setelah terbaca 2 kali berturut-turut, supaya tidak spam saat nilai naik-turun di sekitar ambang.
- Perangkat **offline / online lagi**: dikirim ke admin.

## Batas pribadi sesuai ketinggian rumah

Rumah warga tidak sama tingginya, jadi tiap warga bisa memilih kapan ia diberi peringatan pribadi:

| Kategori | Peringatan pribadi saat jarak sensor ke air ≤ | Untuk |
|---|---|---|
| `rendah` | 120 cm | rumah rendah / paling dekat sungai, diberi tahu paling awal |
| `sedang` | 80 cm | rumah dengan lantai sedang |
| `tinggi` | 40 cm | rumah dengan lantai tinggi / panggung |
| `khusus` | angka sendiri lewat `/batas` | warga yang tahu batas rumahnya sendiri |

- Batas ini dihitung di **Node-RED**, bukan di ESP32. ESP32 tetap hanya mengirim jarak air.
- Peringatan pribadi dikirim **sekali** saat batas terlampaui, lalu sekali lagi saat air turun (jarak > batas + 5 cm).
- Warga tetap menerima peringatan umum SIAGA/BAHAYA.
- Petugas melihat daftar warga di dashboard, kotak **Warga & Batas Rumah** (kode warga, kategori, batas, kondisi). Chat ID warga tidak ditampilkan.
- Angka 120/80/40 cocok untuk demo meja. Setelah kalibrasi di lokasi, ubah angka `BATAS_RUMAH` di node **Pengaturan admin** lalu Deploy. Warga yang memilih kategori otomatis ikut angka baru.

## Uji tanpa ESP32

Dengan Mosquitto dan Node-RED jalan, buka Command Prompt di folder Mosquitto:

```
mosquitto_pub -h localhost -t sideban/titik01/perangkat -r -m online
mosquitto_pub -h localhost -t sideban/titik01/sensor/air -m "{\"jarak_cm\":15}"
mosquitto_pub -h localhost -t sideban/titik01/status -r -m "{\"status\":\"BAHAYA\",\"level\":2,\"jarak_cm\":15,\"hujan_persen\":80,\"hujan\":\"Deras\",\"buzzer\":\"AUTO\",\"ambang\":{\"bahaya\":20,\"siaga\":100,\"deras\":65}}"
```

Dashboard harus berubah ke 🔴 BAHAYA dan warga terdaftar menerima pesan Telegram.
Untuk melihat perintah dari dashboard: `mosquitto_sub -h localhost -t "sideban/#" -v`.

## Kalau ada masalah

| Gejala | Penyebab umum |
|---|---|
| Serial Monitor: `gagal, rc=-2` | IP `MQTT_SERVER` salah, Mosquitto belum jalan dengan `mosquitto.conf`, atau firewall memblokir port 1883 |
| Serial Monitor: `rc=5` | Broker minta password, isi `MQTT_USER` / `MQTT_PASS` |
| WiFi tidak pernah tersambung | WiFi 5 GHz, SSID/password salah |
| Dashboard kosong | Node-RED belum tersambung broker (cek tulisan *connected* di bawah node MQTT) |
| Bot tidak membalas | Token belum diisi, atau Node.js versi lama (pakai 22 LTS) |
| Warga terdaftar hilang setelah restart | Langkah 4.3 (`contextStorage`) belum dilakukan |

## Catatan hardware

- **ECHO JSN-SR04M ke GPIO35 harus lewat pembagi tegangan** (ECHO → 1 kΩ → GPIO35 → 2×1 kΩ → GND), karena ECHO keluar 5 V dan pin ESP32 maksimal 3,3 V.
- Sensor hujan di GPIO34 (ADC1) aman dipakai bersamaan dengan WiFi. Pin ADC2 (GPIO 0, 2, 4, 12–15, 25–27) tidak bisa `analogRead` saat WiFi aktif, jadi jangan pindahkan sensor hujan ke pin itu.
