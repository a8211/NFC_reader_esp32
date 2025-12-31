/*  update z wifi d
 *  karta sd d
 *  display d
 *  ntp d
 *  zegar d
 *  zczytywanie kart
 *  logs
 *  konsola w pliku txt (pobieranie pliku txt z githuba co 1 min i sprawdzanie komend w nim)
 *  zeby bez wifi tez dzialalo
*/



#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>
#include <base64.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <SSD1306Wire.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <RTClib.h>
#include <Wire.h>

////

#define SD_CS 5
#define MISO_PIN 19
#define MOSI_PIN 23
#define SCK_PIN 18



#define FIRMWARE_PATH "/firmware/NFC_reader_esp32.bin"
#define SHA_PATH "/last_sha.txt"

// Dane sieci Wi-Fi
const char* ssid = "Orange_Swiatlowod_8F90";
const char* password = "3h9N3QLXKHomQ7n5su";

// Dane repozytorium
const char* owner = "a8211";
const char* repo = "NFC_reader_esp32";
const char* branch = "main";


WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 7200);
RTC_DS3231 rtc;

// Twój token GitHub (PRYWATNY!)
const char* githubToken = "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";


WiFiClientSecure client;

// Bufor JSON
StaticJsonDocument<2048> doc;

// Pierwszy argument = ścieżka na SD (z /), drugi = ścieżka w repo (bez /)
const char* filesToUpload[][2] = {
  {"/example.txt", "example.txt"}
};
const int fileCount = sizeof(filesToUpload) / sizeof(filesToUpload[0]);

// Repozytorium i pliki do pobrania:
// [repo path w GitHubie, nazwa pliku docelowego na SD]
const char* filesToDownload[][2] = {
  {"firmware/NFC_reader_esp32.ino.bin", "/firmware/NFC_reader_esp32.bin"}
};
const int fileCountDownload = sizeof(filesToDownload) / sizeof(filesToDownload[0]);

#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

#define SCREEN_ADDRESS 0x3C
SSD1306Wire display(SCREEN_ADDRESS, I2C_SDA_PIN, I2C_SCL_PIN);

int iteration = 0;


/////////////////////////////////////////////////////////////////////////////////////////////////////

String getFileSHA(String path) {
  HTTPClient http;
  String url = String("https://api.github.com/repos/") + owner + "/" + repo + "/contents/" + path + "?ref=" + branch;
  http.begin(client, url);
  http.addHeader("Authorization", String("token ") + githubToken);
  http.addHeader("User-Agent", "ESP32");

  int httpCode = http.GET();
  String sha = "";

  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      sha = doc["sha"].as<String>();
    }
  }
  http.end();
  return sha;
}

// ---- Funkcja do uploadu pliku z SD (strumieniowo, bez ładowania całego pliku do RAM) ----
void uploadFileToGitHub(const char* sdPath, const char* repoPath) {
  File file = SD.open(sdPath, FILE_READ);
  if (!file) {
    Serial.printf("❌ Nie można otworzyć pliku na SD: %s\n", sdPath);
    return;
  }

  // Pobieramy SHA jeśli plik istnieje
  String sha = getFileSHA(String(repoPath));

  // Tworzymy JSON z nagłówkami
  DynamicJsonDocument doc(4096);
  doc["message"] = sha.length() > 0 ? "Aktualizacja pliku z ESP32" : "Nowy plik z ESP32";

  // Strumieniowe kodowanie Base64
  String base64Content = "";
  uint8_t buffer[1024];
  while (file.available()) {
    size_t len = file.read(buffer, sizeof(buffer));
    base64Content += base64::encode(buffer, len);
    yield(); // aby watchdog nie resetował ESP32
  }
  file.close();

  doc["content"] = base64Content;
  if (sha.length() > 0) doc["sha"] = sha;

  String jsonPayload;
  serializeJson(doc, jsonPayload);

  // ---- Wysyłka ----
  HTTPClient http;
  String url = String("https://api.github.com/repos/") + owner + "/" + repo + "/contents/" + repoPath;
  http.begin(client, url);
  http.addHeader("Authorization", String("token ") + githubToken);
  http.addHeader("User-Agent", "ESP32");
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.PUT(jsonPayload);
  String response = http.getString();
  http.end();

  if (httpCode == 201 || httpCode == 200) {
    Serial.printf("✅ Plik wysłany do GitHub: %s\n", repoPath);
  } else {
    Serial.printf("❌ Błąd wysyłki pliku %s: %d\n%s\n", repoPath, httpCode, response.c_str());
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool downloadFile(const char* url, const char* path) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  Serial.printf("🔗 Pobieranie pliku: %s\n", url);
  if (!http.begin(client, url)) {
    Serial.println("❌ Nie udało się rozpocząć połączenia HTTP!");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("❌ Błąd HTTP GET: %d\n", httpCode);
    http.end();
    return false;
  }

  int totalSize = http.getSize();
  if (totalSize <= 0) {
    Serial.println("⚠️ Brak informacji o rozmiarze pliku!");
  } else {
    Serial.printf("📦 Rozmiar pliku: %d bajtów\n", totalSize);
  }

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("❌ Nie mogę otworzyć pliku do zapisu!");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[2048];
  int downloaded = 0;
  unsigned long lastUpdate = millis();
  unsigned long startTime = millis();
  http.setTimeout(240000);
  while (http.connected() && (downloaded < totalSize || totalSize == -1)) {
    size_t available = stream->available();
    if (available) {
      int len = stream->readBytes(buffer, std::min((size_t)sizeof(buffer), available));
      file.write(buffer, len);
      downloaded += len;

      // 🔄 co 500 ms pokazuj postęp
      if (millis() - lastUpdate >= 500) {
        if (totalSize > 0) {
          int percent = (downloaded * 100) / totalSize;
          Serial.printf("⬇️ %s: %d%% (%d/%d bajtów)\n", path, percent, downloaded, totalSize);
        } else {
          Serial.printf("⬇️ %s: %d bajtów pobrano (rozmiar nieznany)\n", path, downloaded);
        }
        lastUpdate = millis();
      }
    } else {
      delay(1);
    }
  }
  unsigned long duration = millis() - startTime;
  float speedKBs = (downloaded / 1024.0) / (duration / 1000.0);
  Serial.printf("✅ Pobieranie zakończone: %s (%d bajtów, %.2f KB/s)\n", path, downloaded, speedKBs);
  file.close();
  http.end();
  return true;
}

// --- Pobiera download_url z GitHub API ---
String getDownloadUrl(const char* repoPath) {
  String apiUrl = "https://api.github.com/repos/a8211/NFC_reader_esp32/contents/";
  apiUrl += repoPath;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, apiUrl);
  http.addHeader("User-Agent", "ESP32");
  if (strlen(githubToken) > 0) {
    http.addHeader("Authorization", String("token ") + githubToken);
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("❌ Błąd pobierania metadanych (%d): %s\n", httpCode, apiUrl.c_str());
    http.end();
    return "";
  }
  String payload = http.getString();
  http.end();
  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("❌ Błąd dekodowania JSON: %s\n", error.c_str());
    return "";
  }

  if (!doc["download_url"].isNull()) {
    return String(doc["download_url"].as<const char*>());
  }

  Serial.println("⚠️ Brak pola download_url (prawdopodobnie folder, nie plik)");
  return "";
}

// --- Pobieranie wszystkich plików z listy ---
void downloadAllFiles() {
  for (int i = 0; i < fileCountDownload; i++) {
    const char* repoPath = filesToDownload[i][0];
    const char* targetPath = filesToDownload[i][1];

    Serial.printf("\n📁 [%d/%d] Szukam %s...\n", i + 1, fileCountDownload, repoPath);
    String downloadUrl = getDownloadUrl(repoPath);

    if (downloadUrl == "") {
      Serial.printf("❌ Nie znaleziono pliku: %s\n", repoPath);
      continue;
    }

    downloadFile(downloadUrl.c_str(), targetPath);
  }
  Serial.println("\n📥 Wszystkie pliki pobrane!");
}

////////////////////////////////////////////////////////////////////////////////////////////////////

String calculateFileSHA(File file) {
  if (!file) return "";
  uint8_t shaResult[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);

  uint8_t buffer[512];
  while (file.available()) {
    size_t len = file.read(buffer, sizeof(buffer));
    mbedtls_sha256_update(&ctx, buffer, len);
  }

  mbedtls_sha256_finish(&ctx, shaResult);
  mbedtls_sha256_free(&ctx);

  String hex = "";
  for (int i = 0; i < 32; i++) {
    if (shaResult[i] < 16) hex += "0";
    hex += String(shaResult[i], HEX);
  }
  return hex;
}

// Sprawdź czy nowy firmware różni się SHA
bool checkForNewFirmware() {
  if (!SD.exists(FIRMWARE_PATH)) return false;
  File fw = SD.open(FIRMWARE_PATH, FILE_READ);
String newSHA = calculateFileSHA(fw);
fw.close();
  String oldSHA = "";
  if (SD.exists(SHA_PATH)) {
    File f = SD.open(SHA_PATH);
    oldSHA = f.readString();
    f.close();
    oldSHA.trim();
  }

  if (newSHA != oldSHA && newSHA.length() > 0) {
    Serial.println("💾 SHA nowego pliku: " + newSHA);
    Serial.println("📄 SHA poprzedniego: " + oldSHA);
    return true;
  }
  return false;
}

// Aktualizacja firmware z SD
bool performUpdate() {
  File fw = SD.open(FIRMWARE_PATH);
  if (!fw) {
    Serial.println("❌ Nie udało się otworzyć pliku firmware!");
    return false;
  }

  size_t fwSize = fw.size();
  if (fwSize == 0) {
    Serial.println("⚠️ Plik firmware jest pusty!");
    fw.close();
    return false;
  }

  if (!Update.begin(fwSize)) {
    Serial.printf("❌ Nie można rozpocząć aktualizacji! (%s)\n", Update.errorString());
    fw.close();
    return false;
  }
  size_t written = Update.writeStream(fw);
  fw.close();

  if (written == fwSize && Update.end(true)) {
    Serial.println("✅ Firmware zaktualizowany pomyślnie!");
    
    display.clear();
    display.drawString(0, 0, "Zaaktualizowano");
    display.display();

    // zapisanie SHA
    File fw2 = SD.open(FIRMWARE_PATH);
    String sha = calculateFileSHA(fw2);
    fw2.close();
    saveCurrentSHA(sha);
    return true;
  } else {
    Serial.printf("❌ Błąd aktualizacji: %s\n", Update.errorString());
    return false;
  }
}

// Zapisz aktualny SHA
void saveCurrentSHA(String sha) {
  File f = SD.open(SHA_PATH, FILE_WRITE);
  if (f) {
    f.print(sha);
    f.close();
  }
}

// Rollback do poprzedniej wersji
bool rollbackFirmware() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *last = esp_ota_get_last_invalid_partition();

  if (!last) {
    Serial.println("⚠️ Brak poprzedniej wersji do rollbacku!");
    return false;
  }

  esp_err_t err = esp_ota_set_boot_partition(last);
  if (err == ESP_OK) {
    Serial.println("🔁 Rollback ustawiony pomyślnie!");
    return true;
  } else {
    Serial.printf("❌ Błąd rollbacku: %s\n", esp_err_to_name(err));
    return false;
  }
}

void debugFirmwareFile() {
  Serial.println("\n🔍 Sprawdzanie pliku firmware...");

  // Sprawdź czy plik istnieje
  if (!SD.exists(FIRMWARE_PATH)) {
    Serial.printf("❌ Plik nie istnieje: %s\n", FIRMWARE_PATH);
    return;
  }

  // Otwórz plik
  File fw = SD.open(FIRMWARE_PATH, FILE_READ);
  if (!fw) {
    Serial.printf("❌ Nie można otworzyć pliku: %s\n", FIRMWARE_PATH);
    return;
  }

  // Rozmiar pliku
  size_t fwSize = fw.size();
  Serial.printf("📄 Plik otworzony: %s\n", FIRMWARE_PATH);
  Serial.printf("📏 Rozmiar pliku: %d bajtów\n", fwSize);

  // Spróbuj przeczytać pierwsze 64 bajty i wyświetlić w HEX
  Serial.print("🔢 Pierwsze 64 bajty pliku (HEX): ");
  uint8_t buffer[64];
  size_t readLen = fw.read(buffer, sizeof(buffer));
  for (size_t i = 0; i < readLen; i++) {
    if (buffer[i] < 16) Serial.print("0");
    Serial.print(buffer[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
  fw.close();

  // Sprawdzenie Update.begin()
  if (fwSize == 0) {
    Serial.println("⚠️ Rozmiar pliku = 0, Update.begin() nie zostanie wywołane!");
  } else if (!Update.begin(fwSize)) {
    Serial.printf("❌ Update.begin() nie powiodło się: %s\n", Update.errorString());
  } else {
    Serial.println("✅ Update.begin() działa poprawnie z tym plikiem.");
    Update.end(); // zamykamy od razu, bo to tylko test
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void DisplayStart(){
  display.init();
  display.clear();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "Inicjalizowanie...");
  display.display();
  delay(1000);
  display.clear();
  display.display();
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void TimeShit() {
  timeClient.update();
  if(timeClient.getMinutes() == 1 or iteration == 0){
    Serial.println("Updating Time");
    DateTime now = rtc.now();
    int Y = now.year();
    int M = now.month();
    int D = now.day();
    int H = timeClient.getHours();
    int Mi = timeClient.getMinutes();
    int S = timeClient.getSeconds();
    rtc.adjust(DateTime(Y, M, D, H, Mi, S));
    iteration = 1;
  }
// raz na godzine ^^
}

void GetTime() {
    DateTime now = rtc.now();
    String tme;
    tme += "Aktualny czas: " + String(now.year()) + "-" + String(now.month()) + "-" + String(now.day()) + " ";
    tme += String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second());
    Serial.println(tme);
}
////////////////////////////////////////////////////////////////////////////////////////////////////

String Logs(String x) {
  Serial.println(x);
  File Log = SD.open("Logs.txt", FILE_WRITE);
  if (Log) {
    Log.println(x);
    Log.flush();
    Log.close();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("❌ Błąd SD!");
    display.clear();
    display.drawString(0, 0, "Karta SD nie wykryta");
    display.display();
    return;
  }
  Serial.println("💾 Karta SD gotowa!");
  
  Serial.println("\n🔌 Start ESP32...");
  DisplayStart();
  WiFi.begin(ssid, password);
  Serial.print("🔗 Łączenie z WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Połączono!");
  Serial.println(WiFi.localIP());

  display.clear();
  display.drawString(0, 0, "Polaczono z WIFI");
  display.display();

  

 // ==========

  client.setInsecure();

    display.clear();
    display.drawString(0, 0, "Pobieranie plikow");
    display.display();
  
  downloadAllFiles();
  
  for (int i = 0; i < fileCount; i++) {
    uploadFileToGitHub(filesToUpload[i][0], filesToUpload[i][1]);
  }

 // ==========

  if (checkForNewFirmware()) {
    Serial.println("🆕 Nowe oprogramowanie wykryte! Aktualizacja...");

display.clear();
display.drawString(0, 0, "Aktualizowanie...");
display.display();

    if (performUpdate()) {
      Serial.println("✅ Aktualizacja zakończona sukcesem!");
      ESP.restart();
    } else {
      Serial.println("⚠️ Aktualizacja nie powiodła się. Próba rollbacku...");
      if (rollbackFirmware()) {
        Serial.println("🔁 Rollback zakończony sukcesem. Restart...");
        ESP.restart();
      } else {
        Serial.println("❌ Rollback nieudany! Urządzenie w stanie niepewnym.");
      }
    }
  } else {
    Serial.println("ℹ️ Brak nowej wersji firmware. Uruchamianie normalne...");
  }

 // ==========
 
  timeClient.begin();
  timeClient.update();
  
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  if (!rtc.begin()) {
    Serial.println("DS3231 nie wykryty.");
  }
  rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); 
  
 // ==========
}

void loop() {

  TimeShit();
  GetTime();

    
  delay(100);


}
