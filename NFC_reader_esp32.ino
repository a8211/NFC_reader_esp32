#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>
#include <base64.h>

#define SD_CS 5
#define MISO_PIN 19
#define MOSI_PIN 23
#define SCK_PIN 18

// Dane sieci Wi-Fi
const char* ssid = "Orange_Swiatlowod_8F90";
const char* password = "3h9N3QLXKHomQ7n5su";

// Dane repozytorium
const char* owner = "a8211";
const char* repo = "NFC_reader_esp32";
const char* branch = "main";

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
  {"firmware/NFC_reader_esp32.ino.merged.bin", "/firmware/NFC_reader_esp32.bin"}
};
const int fileCountDownload = sizeof(filesToDownload) / sizeof(filesToDownload[0]);

////////////////////////////////////////////////////////////////////////////////////////////////////

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

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n🔌 Start ESP32...");
  WiFi.begin(ssid, password);
  Serial.print("🔗 Łączenie z WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Połączono!");
  Serial.println(WiFi.localIP());

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("❌ Błąd SD!");
    return;
  }
  Serial.println("💾 Karta SD gotowa!");

  client.setInsecure();

  downloadAllFiles();
  
  
  for (int i = 0; i < fileCount; i++) {
    uploadFileToGitHub(filesToUpload[i][0], filesToUpload[i][1]);
  }
}

void loop() {}
