#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>

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

// Bufor JSON
StaticJsonDocument<2048> doc;

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

  String url = String("https://api.github.com/repos/") + owner + "/" + repo + "/contents?ref=" + branch;
  Serial.println("📁 Pobieranie listy plików...");
  downloadFolder(url, "/");
}

void loop() {}

void downloadFolder(String apiUrl, String localPath) {
  HTTPClient http;
  http.begin(apiUrl);
  http.addHeader("User-Agent", "ESP32");
  http.addHeader("Authorization", String("token ") + githubToken);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("⚠️ Błąd HTTP: %d przy pobieraniu %s\n", httpCode, apiUrl.c_str());
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("❌ Błąd JSON: ");
    Serial.println(error.c_str());
    return;
  }

  for (JsonObject file : doc.as<JsonArray>()) {
    const char* type = file["type"];
    const char* name = file["name"];
    const char* path = file["path"];
    const char* download_url = file["download_url"];
    const char* url = file["url"];

if (strcmp(type, "file") == 0) {
    // sprawdzamy, czy download_url istnieje i nie jest pusty
    if (download_url && strlen(download_url) > 0) {
        String localFilePath = localPath + name;
        Serial.printf("⬇️ Pobieranie pliku: %s\n", path);
        downloadFile(download_url, localFilePath);
    } else {
        Serial.printf("⚠️ Pomijam plik (brak download_url): %s\n", path);
    }
} 
else if (strcmp(type, "dir") == 0 && url) {
    String nameStr = String(name);
    // opcjonalnie pomijamy foldery systemowe typu .github
    if (nameStr.startsWith(".github")) {
        Serial.printf("⚠️ Pomijam folder systemowy: %s\n", path);
        continue; // przejdź do następnego elementu w pętli
    }


    String subDirPath = localPath + name + "/";
    if (!SD.exists(subDirPath)) {
        SD.mkdir(subDirPath);
        Serial.printf("📂 Utworzono folder: %s\n", subDirPath.c_str());
    }

    // URL już zawiera ?ref=main, więc nie dodajemy nic dodatkowego
    String subApiUrl = String(url);
    downloadFolder(subApiUrl, subDirPath);
}
  }
}

void downloadFile(const char* fileURL, String localPath) {
  Serial.printf("\n🔗 Rozpoczynam pobieranie: %s\n", fileURL);

  HTTPClient http;
  http.begin(fileURL);
  http.addHeader("User-Agent", "ESP32");
  http.setTimeout(30000); // 30 s timeout na połączenie/odczyt

  int httpCode = http.GET();
  Serial.printf("📄 HTTP GET code: %d\n", httpCode);
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("⚠️ Błąd HTTP %d przy pobieraniu %s\n", httpCode, fileURL);
    http.end();
    return;
  }

  int totalLength = http.getSize();
  Serial.printf("📦 Rozmiar pliku: %d bajtów\n", totalLength);

  File file = SD.open(localPath, FILE_WRITE);
  if (!file) {
    Serial.printf("❌ Nie można otworzyć pliku %s do zapisu\n", localPath.c_str());
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[2048]; // bufor 4 KB
  int downloaded = 0;
  int lastPercent = -1;
  unsigned long lastDataTime = millis();

  while (downloaded < totalLength) {
    size_t size = stream->available();
    if (size) {
      int c = stream->readBytes(buffer, (size > sizeof(buffer) ? sizeof(buffer) : size));
      file.write(buffer, c);
      file.flush(); // od razu zapisujemy na SD
      downloaded += c;
      lastDataTime = millis();

      int percent = (downloaded * 100) / totalLength;
      if (percent != lastPercent) {
        Serial.printf("⬇️ %s: %d%% (%d bajtów)\n", localPath.c_str(), percent, downloaded);
        lastPercent = percent;
      }
    } else {
      // brak danych chwilowo, czekamy max 1h
      if ((millis() - lastDataTime) > 3600000) {
        Serial.println("❌ Timeout: brak danych > 1h");
        break;
      }
      delay(1);
    }
  }

  if (downloaded >= totalLength) {
    Serial.println("✅ Pobieranie zakończone.");
  } else {
    Serial.println("⚠️ Pobieranie przerwane przed ukończeniem pliku.");
  }

  file.close();
  http.end();
  Serial.printf("💾 Zapisano %s (%d bajtów)\n", localPath.c_str(), downloaded);
}
