#include <ModbusMaster.h>
#include <dhtnew.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <WiFiManager.h>    // Setup WiFi via portal
#include <HTTPClient.h>     // Untuk POST ke Supabase REST
#include <ArduinoJson.h>    // Format JSON

// ==== Pin Definition ====
#define RXD2 16
#define TXD2 17
#define DE_RE 4
#define DHTPIN 15
#define BUZZER 5  // Buzzer aktif HIGH
#define OLED_RESET -1

// ==== OLED Config ====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
// Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// === Menu system ===
#define JOY_X 34
#define JOY_Y 35
#define JOY_BTN 32

int menuIndex = 0;
const char* menuItems[] = {
  "Realtime Monitoring",
  "Mulai Rekomendasi",
  "Cek Hasil Terakhir",
  "Set WiFi"
};
const int menuCount = sizeof(menuItems) / sizeof(menuItems[0]);

String rekomText = "";
int scrollOffset = 0;     // posisi baris awal teks
int totalLines = 0;       // total baris teks rekomendasi
unsigned long lastMove = 0;
const int scrollDelay = 200; // jeda antar scroll
long lastRekomendasiId = 0;


enum AppMode { MENU, MONITORING, RECOMMENDATION, LAST_RESULT, WIFI_SETUP };
AppMode currentMode = MENU;


ModbusMaster node;
DHTNEW dht(DHTPIN);

// ==== Supabase ====
const char* SUPABASE_URL = "https://rxthvgmkbxgblszhqnnv.supabase.co";
const char* SUPABASE_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InJ4dGh2Z21rYnhnYmxzemhxbm52Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjI0MjAzNjAsImV4cCI6MjA3Nzk5NjM2MH0.18hZwWhUCQJoH83kBRjV5PKGxIUgfs9D9f1Hea-pYeY";  // Ganti dengan key kamu (sebaiknya disimpan aman)
const char* DEVICE_ID = "2384180d-c274-4308-8ca3-c2270302be82";  // UUID dari tabel devices Supabase

// ==== RS485 Control ====
void preTransmission() { digitalWrite(DE_RE, HIGH); }
void postTransmission() { digitalWrite(DE_RE, LOW); }

void setup() {
  Serial.begin(115200);
  delay(500);

  // ==== WiFi Manager ====
  WiFiManager wm;
  bool res = wm.autoConnect("SoilMonitorAP", "12345678");
  if (!res) {
    Serial.println("⚠️ Gagal connect WiFi!");
  } else {
    Serial.println("✅ WiFi connected!");
  }
   pinMode(JOY_BTN, INPUT_PULLUP);
  analogReadResolution(12); // pastikan pembacaan akurat (0-4095)

  // ==== RS485 init ====
  Serial2.begin(4800, SERIAL_8N1, RXD2, TXD2);
  pinMode(DE_RE, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(DE_RE, LOW);
  digitalWrite(BUZZER, LOW);
  node.begin(1, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  delay(1000); // delay penting untuk stabilisasi sensor RS485

  // ==== OLED Init ====
  Wire.begin(21, 22); // SDA, SCL
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("❌ OLED not found!");
    while (true);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("RS-ECTH-N01-TR-1");
  display.println("Initializing...");
  display.display();
  delay(1500);
  drawMenu();
}

void loop() {
  if (currentMode == MENU) {
    checkJoystickMenu();
  }
  else if (currentMode == MONITORING) {
    showRealtime();
    checkBackToMenu();
  }
  else if (currentMode == RECOMMENDATION) {
    checkBackToMenu();
  }
  else if (currentMode == LAST_RESULT) {
    handleJoystickScroll();   // ✅ tambahkan ini
    checkBackToMenu();
  }
  else if (currentMode == WIFI_SETUP) {
    checkBackToMenu();
  }
}



void showRealtime() {
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate < 3000) return;  // update tiap 3 detik
  lastUpdate = millis();

  uint8_t result = node.readInputRegisters(0x0000, 5); 
  float soilMoist = NAN, soilTemp = NAN, soilEC = NAN, soilSalinity = NAN, soilTDS = NAN;

  if (result == node.ku8MBSuccess) {
    soilMoist    = node.getResponseBuffer(0) / 10.0;
    soilTemp     = node.getResponseBuffer(1) / 10.0;
    soilEC       = node.getResponseBuffer(2);
    soilSalinity = node.getResponseBuffer(3);
    soilTDS      = node.getResponseBuffer(4);
  }

  dht.read();
  float airTemp = dht.getTemperature();
  float airHum  = dht.getHumidity();

  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println("Soil Monitor (RS485)");
  display.println("--------------------");
  display.printf("M:%.1f%% T:%.1fC\n", soilMoist, soilTemp);
  display.printf("EC:%d SAL:%d\n", (int)soilEC, (int)soilSalinity);
  display.printf("TDS:%d mg/L\n", (int)soilTDS);
  display.println("--------------------");
  display.printf("AirT:%.1fC H:%.1f%%", airTemp, airHum);
  display.display();

  sendToSupabase(soilMoist, soilTemp, soilEC, soilSalinity, soilTDS, airTemp, airHum, false);
}


void checkJoystickMenu() {
  static unsigned long lastMove = 0;
  static int lastBtn = HIGH;

  int x = analogRead(JOY_X);
  int y = analogRead(JOY_Y);
  int btn = digitalRead(JOY_BTN);

  if (currentMode == MENU) {
    if (millis() - lastMove > 300) {
      if (y < 1000) { // naik
        menuIndex = (menuIndex - 1 + menuCount) % menuCount;
        drawMenu();
        lastMove = millis();
      } else if (y > 3800) { // turun
        menuIndex = (menuIndex + 1) % menuCount;
        drawMenu();
        lastMove = millis();
      }
    }

    if (btn == LOW && lastBtn == HIGH) {
      handleMenuSelect(menuIndex);
      delay(300);
    }
  }
  lastBtn = btn;
}

void checkBackToMenu() {
  if (digitalRead(JOY_BTN) == LOW) {
    currentMode = MENU;
    drawMenu();
    delay(300);
  }
}

void resetWiFi() {
  WiFiManager wm;
  wm.resetSettings();
  displayMsg("Reset WiFi...\nConnect ulang");
  wm.startConfigPortal("SoilMonitorAP", "12345678");
  displayMsg("WiFi OK!\nTekan OK utk Menu");
}

void startRecommendation() {
  displayMsg("Mengukur data (1 menit)...");
  unsigned long startTime = millis();

  float sumMoist = 0, sumTemp = 0, sumEC = 0, sumSal = 0, sumTDS = 0;
  float sumAirT = 0, sumAirH = 0;
  int count = 0;

  // ambil data selama ± 60 detik
  while (millis() - startTime < 60000) {
    uint8_t result = node.readInputRegisters(0x0000, 5);
    if (result == node.ku8MBSuccess) {
      sumMoist += node.getResponseBuffer(0) / 10.0;
      sumTemp  += node.getResponseBuffer(1) / 10.0;
      sumEC    += node.getResponseBuffer(2);
      sumSal   += node.getResponseBuffer(3);
      sumTDS   += node.getResponseBuffer(4);
      count++;
    }

    dht.read();
    sumAirT += dht.getTemperature();
    sumAirH += dht.getHumidity();

    // tampilkan progress di OLED
    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.println("Mengukur data...");
    int progress = map(millis() - startTime, 0, 60000, 0, 100);
    display.printf("Progress: %d%%", progress);
    display.display();

    delay(1000); // ambil data tiap 1 detik
  }

  if (count == 0) {
    displayMsg("❌ Tidak ada data sensor!");
    return;
  }

  // hitung rata-rata hasil selama 1 menit
  float soilMoist = sumMoist / count;
  float soilTemp = sumTemp / count;
  float soilEC = sumEC / count;
  float soilSalinity = sumSal / count;
  float soilTDS = sumTDS / count;
  float airTemp = sumAirT / count;
  float airHum = sumAirH / count;

  // tampilkan ringkasan di OLED
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  display.println("Rata-rata hasil:");
  display.printf("M:%.1f%% T:%.1fC\n", soilMoist, soilTemp);
  display.printf("EC:%.0f SAL:%.0f\n", soilEC, soilSalinity);
  display.printf("TDS:%.0f\n", soilTDS);
  display.display();
  delay(3000);

  // kirim ke Supabase satu kali saja
  displayMsg("Kirim ke server...");
  sendToSupabase(soilMoist, soilTemp, soilEC, soilSalinity, soilTDS, airTemp, airHum, true);

  // tunggu rekomendasi hanya sekali
  displayMsg("Tunggu rekomendasi...");
  delay(10000);  // beri waktu server JS memproses

  String rekom = getRecommendation();

  // tampilkan rekomendasi akhir
  displayMsg("Rekomendasi:\n" + rekom);
  tone(BUZZER, 1500, 200);
}

String getRecommendation() {
  Serial.println("\n===============================");
  Serial.println("🔍 Memulai pengambilan rekomendasi dari Supabase...");
  unsigned long startTime = millis(); // untuk ukur waktu

  HTTPClient http;
  String url = String(SUPABASE_URL) +
               "/rest/v1/rekomendasi?device_id=eq." + DEVICE_ID +
               "&select=id,title,bullets&order=created_at.desc&limit=1";

  Serial.println("🌐 URL: " + url);

  http.begin(url);
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));

  int httpCode = http.GET();
  String result = "Belum ada hasil";

  Serial.printf("📡 HTTP Response Code: %d\n", httpCode);

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println("📥 Respons Supabase:");
    Serial.println(payload);
    Serial.printf("📦 Ukuran respons: %d bytes\n", payload.length());

    // Perbaikan: beri ukuran JsonDocument
    StaticJsonDocument<1024> doc; 
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      Serial.println("✅ JSON berhasil di-parse!");
      if (doc.size() > 0) {
        long id = doc[0]["id"] | 0; 
        String title = doc[0]["title"] | "Tanpa judul";
        String bullets = doc[0]["bullets"] | "";

        Serial.println("🆔 ID: " + String(id)); // harus convert ke String
        Serial.println("📌 Title: " + title);
        Serial.println("📝 Bullets (mentah): " + bullets);

        // Simpan ID terakhir agar bisa dicek lagi nanti
        if (id != 0) {
          lastRekomendasiId = id;
          Serial.println("💾 Disimpan sebagai lastRekomendasiId: " + String(lastRekomendasiId));
        }

        // Rapikan bullet list biar mudah dibaca
        bullets.replace("[", "");
        bullets.replace("]", "");
        bullets.replace("\"", "");
        bullets.replace(",", "\n• ");

        result = title + "\n• " + bullets;
      } else {
        Serial.println("⚠️ JSON kosong, tidak ada data rekomendasi!");
      }
    } else {
      Serial.print("⚠️ Gagal parsing JSON! Error: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.printf("❌ HTTP error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();

  unsigned long duration = millis() - startTime;
  Serial.printf("⏱️ Waktu total proses: %lu ms\n", duration);
  Serial.println("===============================\n");

  return result;
}


String getRecommendationById(long rekomId) {
  if (rekomId == 0) return "Belum ada ID rekomendasi.";

  Serial.println("\n===============================");
  Serial.println("🔍 Mengambil rekomendasi berdasarkan ID...");
  unsigned long startTime = millis();

  HTTPClient http;
  String url = String(SUPABASE_URL) +
               "/rest/v1/rekomendasi?id=eq." + rekomId +
               "&select=title,bullets";

  Serial.println("🌐 URL: " + url);

  http.begin(url);
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));

  int httpCode = http.GET();
  String result = "Belum ada hasil";

  Serial.printf("📡 HTTP Response Code: %d\n", httpCode);

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println("📥 Respons Supabase:");
    Serial.println(payload);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error && doc.size() > 0) {
      String title = doc[0]["title"] | "Tanpa judul";
      String bullets = doc[0]["bullets"] | "";

      Serial.println("📌 Title: " + title);
      Serial.println("📝 Bullets (mentah): " + bullets);

      // Rapikan bullet list
      bullets.replace("[", "");
      bullets.replace("]", "");
      bullets.replace("\"", "");
      bullets.replace(",", "\n• ");

      result = title + "\n• " + bullets;
    } else if (error) {
      Serial.print("⚠️ Gagal parsing JSON! Error: ");
      Serial.println(error.c_str());
    } else {
      Serial.println("⚠️ Tidak ada data untuk ID ini!");
    }
  } else {
    Serial.printf("❌ HTTP error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();

  unsigned long duration = millis() - startTime;
  Serial.printf("⏱️ Waktu total proses: %lu ms\n", duration);
  Serial.println("===============================\n");

  return result;
}



// === Fungsi tampilkan teks rekomendasi di OLED dengan scroll ===
void showRecommendation() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Pisahkan teks jadi baris
  std::vector<String> lines;
  int start = 0;
  while (true) {
    int newline = rekomText.indexOf('\n', start);
    if (newline == -1) {
      lines.push_back(rekomText.substring(start));
      break;
    }
    lines.push_back(rekomText.substring(start, newline));
    start = newline + 1;
  }
  totalLines = lines.size();

  // tampilkan hanya 6 baris mulai dari scrollOffset
  for (int i = 0; i < 6; i++) {
    int idx = scrollOffset + i;
    if (idx < totalLines) {
      display.setCursor(0, i * 10);
      display.println(lines[idx]);
    }
  }

  // indikator posisi di bawah layar
  display.setCursor(0, 56);
  display.setTextSize(1);
  display.printf("[%d/%d]", scrollOffset + 1, totalLines);

  display.display();
}

// === Fungsi kontrol joystick scroll ===
void handleJoystickScroll() {
  int y = analogRead(JOY_Y);

  if (millis() - lastMove > scrollDelay) {
    if (y < 1000 && scrollOffset > 0) { // atas
      scrollOffset--;
      showRecommendation();
      lastMove = millis();
    } else if (y > 3800 && scrollOffset < totalLines - 6) { // bawah
      scrollOffset++;
      showRecommendation();
      lastMove = millis();
    }
  }
}




void showLastResult() {
  if (lastRekomendasiId == 0) {
    displayMsg("Belum ada rekomendasi terakhir.");
    return;
  }

  // Ambil rekomendasi berdasarkan ID terakhir
  rekomText = getRecommendationById(lastRekomendasiId);

  // Reset scroll
  scrollOffset = 0;

  // Tampilkan teks di OLED dengan fungsi scroll
  showRecommendation();
}

void displayMsg(String msg) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);
  int y = 0;
  String line = "";
  for (int i = 0; i < msg.length(); i++) {
    if (msg[i] == '\n' || line.length() > 20) {
      display.setCursor(0, y);
      display.println(line);
      line = "";
      y += 10;
    } else {
      line += msg[i];
    }
  }
  if (line.length() > 0) {
    display.setCursor(0, y);
    display.println(line);
  }
  display.display();
}


void drawMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("=== MENU ===");
  display.println("--------------------");
  for (int i = 0; i < menuCount; i++) {
    if (i == menuIndex) display.print("> ");
    else display.print("  ");
    display.println(menuItems[i]);
  }
  display.println("--------------------");
  long rssi = WiFi.RSSI();
  display.printf("WiFi: %s (%lddBm)", WiFi.SSID().c_str(), rssi);
  display.display();
}


void handleMenuSelect(int index) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(1);

  switch (index) {
    case 0:
      currentMode = MONITORING;
      displayMsg("Realtime monitoring...");
      break;
    case 1:
      currentMode = RECOMMENDATION;
      startRecommendation();
      break;
    case 2:
      currentMode = LAST_RESULT;
      showLastResult();
      break;
    case 3:
      currentMode = WIFI_SETUP;
      resetWiFi();
      break;
  }
}


// Tambahkan parameter 'bool isRec' di ujung
void sendToSupabase(float soilM, float soilT, float EC, float SAL, float TDS, float airT, float airH, bool isRec) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi disconnected, skipping upload...");
    return; [cite_start]// [cite: 97]
  }

  HTTPClient http;
  http.begin(String(SUPABASE_URL) + "/rest/v1/readings");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", "Bearer " + String(SUPABASE_KEY));

  // Naikkan sedikit buffer jika perlu
  StaticJsonDocument<512> doc;
  doc["device_id"] = DEVICE_ID;     [cite_start]// [cite: 98]
  doc["soil_moisture"] = soilM;
  doc["soil_temp"] = soilT;
  doc["soil_ec"] = EC;
  doc["soil_salinity"] = SAL;
  doc["soil_tds"] = TDS;
  doc["air_temp"] = airT;           [cite_start]// [cite: 99]
  doc["air_humidity"] = airH;
  
  // === BAGIAN BARU ===
  // Mengirim status is_recommendation (true/false)
  doc["is_recommendation"] = isRec; 
  // ===================

  String json;
  serializeJson(doc, json);

  int httpResponseCode = http.POST(json);

  if (httpResponseCode > 0) {
    Serial.printf("✅ Supabase response: %d\n", httpResponseCode); [cite_start]// [cite: 100]
  } else {
    Serial.printf("❌ POST failed: %s\n", http.errorToString(httpResponseCode).c_str());
  }

  http.end();
}
