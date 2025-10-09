#include <Wire.h>
//#include <Adafruit_PN532.h>
#include <RTClib.h>
#include <SSD1306Wire.h>
#include <SPI.h>
#include <SD.h>
//#include <WiFi.h>
//#include <WebServer.h>
//#include <Update.h>

#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define SCREEN_ADDRESS 0x3C
#define CS_PIN 5
#define MISO_PIN 19
#define MOSI_PIN 23
#define SCK_PIN 18

#define DEBUG 1

RTC_DS3231 rtc;

String TimeAndDate() {
  DateTime now = rtc.now();
  int y = now.year();
  int mo = now.month();
  int d = now.day();
  int h = now.hour();
  int m = now.minute();
  int s = now.second();
  int dn = now.dayOfTheWeek();

  String Time = String(h) + ":" + String(m) + ":" + String(s);
  String Date = String(y) + "/" + String(mo) + "/" + String(d) + " " + String(dn);;

return Date + " " + Time;
}


void listDir(fs::FS &fs, const char * dirname, uint8_t levels) {
  if (DEBUG == 1) {
    Serial.printf("Listing directory: %s\n", dirname);

    File root = fs.open(dirname);
    if (!root) {
        Serial.println("Failed to open directory");
        return;
    }
    if (!root.isDirectory()) {
        Serial.println("Not a directory");
        return;
    }

    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            Serial.print("DIR : ");
            Serial.println(file.name());
            if (levels) {
                listDir(fs, file.name(), levels - 1);
            }
        } else {
            Serial.print("FILE: ");
            Serial.print(file.name());
            Serial.print("  SIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
  }
}






void SDFileWrite(String FilePos, String FileName, String Text) {
  if (!SD.exists(FilePos.c_str())) {
    if (!SD.mkdir(FilePos.c_str())) {
      Serial.println("Nie udało się utworzyć katalogu: " + FilePos);
      return;
    }
  }
  String path = FilePos + "/" + FileName;
  File file = SD.open(path.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("Nie można otworzyć pliku do zapisu: " + path);
    return;
  }
  file.seek(file.size());
  file.println(Text);
  file.flush();
  file.close();
  Serial.println("Zapisano do: " + path);
}

String SDFileRead(String FilePos, String FileName) {
  String path = FilePos + "/" + FileName;
  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) {
    Serial.println("Nie można otworzyć pliku do odczytu: " + path);
    return String();
  }

  Serial.print("  FILE: ");
  Serial.print(file.name());
  Serial.print("  SIZE: ");
  Serial.println(file.size());

  String result = "";
  while (file.available()) {
    char c = (char)file.read();
    if (DEBUG == 1) {
      Serial.write(c);    // wypisujemy surowy znak do portu szeregowego
    }
    result += c;        // zbieramy w Stringa
  }
  file.close();
  return result;
}

void setup() {
  Serial.begin(115200);
  //////////
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  rtc.begin();
  if (rtc.lostPower()) {
    Serial.println("RTC stracił zasilanie, ustawiam czas na kompilacji");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  //////////
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
  if (!SD.begin(CS_PIN)) {
    Serial.println("Błąd: karta SD nie wykryta lub SD.begin nie powiodło się!");
    while (1) { delay(1000); } // zatrzymaj - albo obsłuż inaczej
  }
  File dir = SD.open("/test");
  File file = dir.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
        SD.remove(file.name());  // usuń każdy plik
    }
    file = dir.openNextFile();
}
dir.close(); 
  //////////
  listDir(SD, "/", 1);
}

void loop() {
  Serial.println(TimeAndDate());
  SDFileWrite("/test", "tescik.txt", "testuje nowy kod");
  Serial.println(SDFileRead("/test", "tescik.txt"));
  delay(5000);
}
