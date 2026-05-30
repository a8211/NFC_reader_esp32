/*  update z wifi d
 *  karta sd d
 *  display d
 *  ntp d
 *  zegar d
 *  zczytywanie kart d
 *  zapis uid do pliku o nazwie miesiąca z rokiem, ale max co 2 godziny na uid
 *  
 *  logs ~ nie wysyła plików do githuba
* można dodać że drugie esp podłączone na chwile zapisuje logs i może wgrać zmiany
 *  konsola w pliku txt (pobieranie pliku txt z githuba co 1 min i sprawdzanie komend w nim)
 *  zeby bez wifi tez dzialalo (wpisywanie wifi cred bez modyfikowania ino) d
 *  restart o polnocy d
 *  wersja na display
 *  
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
#include <time.h>
#include <Preferences.h>

////

#define SD_CS 5
#define MISO_PIN 19
#define MOSI_PIN 23
#define SCK_PIN 18



#define FIRMWARE_PATH "/firmware/NFC_reader_esp32.bin"
#define SHA_PATH "/last_sha.txt"
#define MAX_LOG_SIZE 30720  // 30 KB



// Dane repozytorium
const char* owner = "a8211";
const char* repo = "NFC_reader_esp32";
const char* branch = "main";

const char* monthNames[] = {
  "none",
  "styczen", "luty", "marzec", "kwiecien",
  "maj", "czerwiec", "lipiec", "sierpien",
  "wrzesien", "pazdziernik", "listopad", "grudzien"
};


WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "europe.pool.ntp.org", 7200);
RTC_DS3231 rtc;
Preferences prefs;

// Twój token GitHub (PRYWATNY!)
//const char* githubToken = "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";
String githubToken;


WiFiClientSecure client;

// Bufor JSON
StaticJsonDocument<2048> doc;

// Pierwszy argument = ścieżka na SD (z /), drugi = ścieżka w repo (bez /)
const char* filesToUpload[][2] = {
  {"/example.txt", "example.txt"},
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
bool dev;

#define NFC_SDA 14
#define NFC_SCL 27

TwoWire WireNFC = TwoWire(1);

#define NFC_INTERFACE_I2C
  #include <Wire.h>
  #include <PN532_I2C.h>
  #include <PN532_I2C.cpp>
  #include <PN532.h>
  
  PN532_I2C pn532i2c(WireNFC);
  PN532 nfc(pn532i2c);

/////////////////////////////////////////////////////////////////////////////////////////////////////



void Logs(String x) { 
  DateTime now = rtc.now(); 
  String tme; 
  tme += "[" + String(now.year()) + "-" + String(now.month()) + "-" + String(now.day()) + " "; 
  tme += String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second()) + "] " + x; 
  int miesiac = now.month(); 
  Serial.println(tme); 
  String path = String("/logs/") + now.day() + monthNames[miesiac] + now.year() + ".txt"; 
  File Log = SD.open(path, FILE_APPEND); 
  if (Log) { 
    Log.println(tme); 
    Log.close(); 
  } 
}



String getFileSHA(const String& repoPath) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.github.com/repos/" + String(owner) + "/" + repo +
               "/contents/" + repoPath + "?ref=" + branch;

  http.begin(client, url);
  http.addHeader("User-Agent", "ESP32");
  http.addHeader("Authorization", String("token ") + githubToken);
  int code = http.GET();
  String sha = "";
  if (code == 200) {
    StaticJsonDocument<1024> doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    if (!err) {
      sha = doc["sha"] | "";
    } else {
      Serial.printf("❌ Błąd JSON przy SHA: %s\n", err.c_str());
    }
  } else if (code == 404) {
    // Plik nie istnieje → nowy plik, SHA pozostaje pusty
    sha = "";
  } else {
    Serial.printf("❌ Nie udało się pobrać SHA (%d)\n", code);
  }
  http.end();
  return sha;
}



// ---- Upload jednego pliku do GitHub (SD -> GitHub) ----
bool uploadFileToGitHub(const char* sdPath, const char* repoPath) {
  if (!SD.exists(sdPath)) {
    Serial.printf("❌ Brak pliku na SD: %s\n", sdPath);
    return false;
  }

  File file = SD.open(sdPath, FILE_READ);
  if (!file) {
    Serial.printf("❌ Nie można otworzyć pliku: %s\n", sdPath);
    return false;
  }

  size_t fileSize = file.size();
  if (fileSize == 0) {
    Serial.println("⚠️ Plik pusty – pomijam");
    file.close();
    return false;
  }

  // ---- wczytaj cały plik ----
  uint8_t* buffer = (uint8_t*)malloc(fileSize);
  if (!buffer) {
    Serial.println("❌ Brak RAM na plik");
    file.close();
    return false;
  }
  file.read(buffer, fileSize);
  file.close();

  // ---- Base64 ----
  String base64Content = base64::encode(buffer, fileSize);
  free(buffer);

  if (base64Content.length() == 0) {
    Serial.println("❌ Base64 puste");
    return false;
  }

  // ---- pobierz SHA jeśli istnieje ----
  String sha = getFileSHA(String(repoPath));

  // ---- JSON ----
  StaticJsonDocument<4096> doc;
  doc["message"] = "Update from ESP32";
  doc["content"] = base64Content;
  if (sha.length() > 0) doc["sha"] = sha; // dodajemy SHA tylko jeśli istnieje

  String payload;
  serializeJson(doc, payload);

  // ---- HTTP PUT ----
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String("https://api.github.com/repos/") + owner + "/" + repo + "/contents/" + repoPath;

  http.begin(client, url);
  http.addHeader("Authorization", String("token ") + githubToken);
  http.addHeader("User-Agent", "ESP32");
  http.addHeader("Content-Type", "application/json");

  int code = http.PUT(payload);
  String response = http.getString();
  http.end();

  if (code == 200 || code == 201) {
    Serial.printf("✅ GitHub OK: %s\n", repoPath);
    return true;
  }

  Serial.printf("❌ GitHub error %d\n%s\n", code, response.c_str());
  return false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

bool downloadFile(const char* url, const char* path) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  Serial.printf("🔗 Pobieranie pliku: %s\n", url);
  if (!http.begin(client, url)) {
    Logs("Nie udało się rozpocząć połączenia HTTP!");
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
    Logs("Brak informacji o rozmiarze pliku!");
  } else {
    Serial.printf("📦 Rozmiar pliku: %d bajtów\n", totalSize);
  }

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Logs("Nie mogę otworzyć pliku do zapisu!");
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[2048];
  int downloaded = 0;
  unsigned long lastUpdate = millis();
  unsigned long startTime = millis();
  http.setTimeout(60000);
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
  if (githubToken.length() > 0) {
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

  Logs("Brak pola download_url (prawdopodobnie folder, nie plik)");
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
  Logs("Wszystkie pliki pobrane!");
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
    Logs("SHA nowego pliku: " + newSHA);
    Logs("SHA poprzedniego: " + oldSHA);
    return true;
  }
  return false;
}

// Aktualizacja firmware z SD
bool performUpdate() {
  File fw = SD.open(FIRMWARE_PATH);
  if (!fw) {
    Logs("Nie udało się otworzyć pliku firmware!");
    return false;
  }

  size_t fwSize = fw.size();
  if (fwSize == 0) {
    Logs("Plik firmware jest pusty!");
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
    Logs("Firmware zaktualizowany pomyślnie!");
    
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
    Logs("Brak poprzedniej wersji do rollbacku!");
    return false;
  }

  esp_err_t err = esp_ota_set_boot_partition(last);
  if (err == ESP_OK) {
    Logs("Rollback ustawiony pomyślnie!");
    return true;
  } else {
    Serial.printf("❌ Błąd rollbacku: %s\n", esp_err_to_name(err));
    return false;
  }
}

void debugFirmwareFile() {
  Logs("\nSprawdzanie pliku firmware...");

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
  Logs(" ");
  fw.close();

  // Sprawdzenie Update.begin()
  if (fwSize == 0) {
    Logs("Rozmiar pliku = 0, Update.begin() nie zostanie wywołane!");
  } else if (!Update.begin(fwSize)) {
    Serial.printf("❌ Update.begin() nie powiodło się: %s\n", Update.errorString());
  } else {
    Logs("Update.begin() działa poprawnie z tym plikiem.");
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

void saveWiFi(const char* ssid, const char* pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

bool loadWiFi(String& ssid, String& pass) {
  prefs.begin("wifi", true);      // true = tylko odczyt
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();

  return ssid.length() > 0;
}

void saveToken(const char* GToken) {
  prefs.begin("GToken", false);
  prefs.putString("GToken", GToken);
  prefs.end();
}

bool loadToken(String& GToken) {
  prefs.begin("GToken", true);      // true = tylko odczyt
  GToken = prefs.getString("GToken", "");
  prefs.end();

  return GToken;
}

void SerialCommands(void* parameter){

while(true){
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();

    if (line.startsWith("wifi ")) {
      int sp = line.indexOf(' ', 5);
      if (sp > 0) {
        String ssid = line.substring(5, sp);
        String pass = line.substring(sp + 1);
        saveWiFi(ssid.c_str(), pass.c_str());
        ESP.restart();
      }
    }
    
    if (line.startsWith("dev")) {
      Serial.println("dev mode");
      saveDevMode();
      }

  if (line.startsWith("GToken ")) {
    String GToken = line.substring(7);
    GToken.trim();
    if (GToken.length() > 0) {
     saveToken(GToken.c_str());
     Serial.println(GToken);
     ESP.restart();
    }
  }   

  if (line.startsWith("Restart")) {
    Logs("Restarting");
    delay(500);
     ESP.restart();
    }
  } 
      
    if (line.startsWith("help")) {
      Logs("help");
      Logs("dev");
      Logs("wifi");
      Logs("GToken");
      Logs("Restart");
     }
    
  }
}

}

void loadDevMode(){
  prefs.begin("devMode", true);
  dev = prefs.getBool("devM", "");
  prefs.end();
}


void saveDevMode(){
  if(dev == true){
    Logs("DevMode off");
    prefs.begin("devMode", false);
    prefs.putBool("devM", false);
    prefs.end();
  } else { 
    Logs("DevMode on");
    prefs.begin("devMode", false);
    prefs.putBool("devM", true);
    prefs.end();
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void TimeShit() {

  if (rtc.lostPower()) {
    Serial.println("RTC lost power, let's set the time!");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  if(WiFi.status() == WL_CONNECTED) {
    timeClient.update();
    time_t epoch = timeClient.getEpochTime();
  struct tm *t = gmtime(&epoch);
  
  if(timeClient.getMinutes() == 1 or iteration == 0){
    Logs("Updating Time");
    DateTime now = rtc.now();
    int Y = t->tm_year + 1900;
    int M = t->tm_mon + 1;
    int D = t->tm_mday;
    int H = timeClient.getHours();
    int Mi = timeClient.getMinutes();
    int S = timeClient.getSeconds();
    rtc.adjust(DateTime(Y, M, D, H, Mi, S));
    iteration = 1;
  } else { }
// raz na godzine jeśli wifi jest ^^
  }
}

void GetTime() {
    DateTime now = rtc.now();
    String tme;
    tme += "Aktualny czas: " + String(now.year()) + "-" + String(now.month()) + "-" + String(now.day()) + " ";
    tme += String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second());
    Logs(tme);
}
////////////////////////////////////////////////////////////////////////////////////////////////////





void CheckTimeForRestart(){
  DateTime now = rtc.now();
  if (now.hour() == 23 and now.minute() == 1) {
    Logs("Esp Restrat at 1:00");
    WiFi.disconnect();
    delay(1000);
    ESP.restart();
  }
}


////////////////////////////////////////////////////////////////////////////////////////////////////


String uidRead(){
  String uidOfCard;
  uint8_t uid[7];
  uint8_t uidLength;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
    Logs("Wyktyto Karte!");
    DateTime now = rtc.now();
    uidOfCard += "[" + String(now.year()) + "-" + String(now.month()) + "-" + String(now.day()) + " "; 
    uidOfCard += String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second()) + "] ";   
    for (uint8_t i = 0; i < uidLength; i++) {
      uidOfCard += String(uid[i], HEX);
      uidOfCard += " ";
    }
    int idx = uidOfCard.indexOf(']');
    String uid = uidOfCard.substring(idx + 1);
    display.clear();
    display.drawString(0, 0, "Wykryto karte");
    display.drawString(0, 15, uid);
    display.display();
  }
  return uidOfCard;
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void setup() {
  Serial.begin(115200);
  delay(1000);

 // ==========
  unsigned long myTime;
  myTime = millis();
  myTime = myTime / 1000;

  delay(1000);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  DisplayStart();
  
  if (!rtc.begin()) {
    Serial.println("DS3231 nie wykryty.");
  }

  loadDevMode();
 // ==========
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SD_CS);
  if (!SD.begin(SD_CS)) {
    Serial.println("❌ Błąd SD!");
    display.clear();
    display.drawString(0, 0, "Karta SD nie wykryta");
    display.display();
    return;
  }
  Logs("Karta SD gotowa!");
  Logs("Start ESP32...");

  String ssid, pass;
  if (loadWiFi(ssid, pass)) {
    Logs("Łączenie z ");
    Logs(ssid);
    WiFi.begin(ssid.c_str(), pass.c_str());
    if (WiFi.waitForConnectResult() == WL_CONNECTED or millis() > 5000) {
      Logs("WiFi połączone");
    } else {
      Logs("Nie udało się połączyć");
    }
  } else {
    Logs("Brak zapisanych danych WiFi");
  }

String GToken;
if (loadToken(GToken)) {
    Logs("Pobieranie Tokenu");
    githubToken = GToken;  // ← przypisuje do globalnej zmiennej
} else {
    Logs("Brak zapisanych danych Tokenu");
}

  
  Logs(WiFi.localIP().toString());

  if(WiFi.status() == WL_CONNECTED) {
  TimeShit();

  display.clear();
  display.drawString(0, 0, "Polaczono z WIFI");
  display.display();

 // ==========

  client.setInsecure();

    display.clear();
    display.drawString(0, 0, "Pobieranie plikow");
    display.display();

  if(dev == true){
    downloadAllFiles();
  }
  
  for (int i = 0; i < fileCount; i++) {
    uploadFileToGitHub(filesToUpload[i][0], filesToUpload[i][1]);
  }

 // ==========

  if (checkForNewFirmware()) {
    Logs("Nowe oprogramowanie wykryte! Aktualizacja...");

  display.clear();
  display.drawString(0, 0, "Aktualizowanie...");
  display.display();

    if (performUpdate()) {
      Logs("Aktualizacja zakończona sukcesem!");
      ESP.restart();
    } else {
      Logs("Aktualizacja nie powiodła się. Próba rollbacku...");
      if (rollbackFirmware()) {
        Logs("Rollback zakończony sukcesem. Restart...");
        ESP.restart();
      } else {
        Logs("Rollback nieudany! Urządzenie w stanie niepewnym.");
      }
    }
  } else {
    Logs("Brak nowej wersji firmware. Uruchamianie normalne...");
  }

 // ==========
  
  timeClient.begin();
  timeClient.update();
  }
  
  WireNFC.begin(NFC_SDA, NFC_SCL);
  delay(1000);
  nfc.begin();
  nfc.SAMConfig();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (! versiondata) {
    Serial.print("Didn't find PN53x board");
    while (1); // halt
  }
  nfc.setPassiveActivationRetries(20);
  Serial.println("Waiting for an ISO14443A card");

  xTaskCreatePinnedToCore(SerialCommands, "SerialCommands", 4096, NULL, 7, NULL, 0);
}

void loop() {
  TimeShit();
  GetTime();
  CheckTimeForRestart();
  Serial.println(uidRead());

  delay(500);

}
