#include <WiFi.h>
extern "C" {
  #include "esp_wifi.h"
}
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// === Button Pins ===
#define BTN_UP     14
#define BTN_DOWN   27
#define BTN_SELECT 26

// === SD Card Pins ===
#define SD_CS 5

// === Menu State ===
enum Screen { HOME, SCAN, SELECTED_INFO, FILTER_SELECT, MONITOR, FAKE_AP};
Screen currentScreen = HOME;

enum PacketFilter { FILTER_BEACON, FILTER_PROBE, FILTER_ALL };
PacketFilter selectedFilterType = FILTER_BEACON;

String apList[20];
int apCount = 0;
int selectedAP = -1;
int menuIndex = 0;
int scanIndex = 0;

File logFile;
bool showFinishedMessage = false;
unsigned long finishedTime = 0;

String latestMAC = "";
int latestRSSI = 0;
unsigned long lastAnimTime = 0;
int animStep = 0;

// === Button Debounce ===
bool wasPressed(int pin) {
  static uint32_t lastPressed[3] = {0, 0, 0};
  int index = (pin == BTN_UP) ? 0 : (pin == BTN_DOWN) ? 1 : 2;
  if (digitalRead(pin) == LOW && millis() - lastPressed[index] > 300) {
    lastPressed[index] = millis();
    return true;
  }
  return false;
}

// === OLED Display Functions ===
void drawHome() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("== Main Menu ==");
  String options[] = {"1. Scan WiFi", "2. Selected AP", "3. Packet Monitor","4.Eviltwin"};
  for (int i = 0; i < 4; i++) {
    if (i == menuIndex) display.print("> ");
    else display.print("  ");
    display.println(options[i]);
  }
  display.display();
}
void drawFakeAP() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Fake AP Active!");
  display.println("SSID:");
  display.println(apList[selectedAP]);
  display.println("Press SELECT to stop");
  display.display();
}
void startFakeAP() {
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  String fakeSSID = apList[selectedAP];
  WiFi.softAP(fakeSSID.c_str());
  Serial.println("Fake AP started with SSID: " + fakeSSID);
}

void scanWiFi() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Scanning...");
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(1000);

  apCount = WiFi.scanNetworks();
  for (int i = 0; i < apCount && i < 20; i++) {
    apList[i] = WiFi.SSID(i);
  }
}

void drawScanList() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("== Select AP ==");
  for (int i = 0; i < 4 && (i + scanIndex) < apCount; i++) {
    int idx = i + scanIndex;
    if (idx == selectedAP) display.print("*");
    else display.print(" ");
    if (idx == menuIndex) display.print("> ");
    else display.print("  ");
    display.println(apList[idx]);
  }
  display.display();
}

void drawSelectedInfo() {
  display.clearDisplay();
  display.setCursor(0, 0);
  if (selectedAP == -1) {
    display.println("No AP selected.");
  } else {
    display.println("== Selected AP ==");
    display.println(apList[selectedAP]);
    display.print("RSSI: ");
    display.println(WiFi.RSSI(selectedAP));
    display.print("MAC: ");
    display.println(WiFi.BSSIDstr(selectedAP));
  }
  display.display();
}

void drawFilterMenu() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("== Select Filter ==");
  String filters[] = {"Beacon Frames", "Probe Requests", "All Mgmt Frames"};
  for (int i = 0; i < 3; i++) {
    if (i == menuIndex) display.print("> ");
    else display.print("  ");
    display.println(filters[i]);
  }
  display.display();
}

void drawLoggingScreen() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Packet Monitor");
  display.println("Logging...");

  display.print("MAC: ");
  display.println(latestMAC);
  display.print("RSSI: ");
  display.println(latestRSSI);

  display.setCursor(0, 54);
  String dots = "";
  for (int i = 0; i <= animStep; i++) dots += "• ";
  display.println(dots);
  display.display();
}

// === Packet Sniffer ===
void packetSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;

  wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  int rssi = pkt->rx_ctrl.rssi;
  uint8_t* payload = pkt->payload;
  uint8_t subtype = payload[0];

  bool logIt = false;
  switch (selectedFilterType) {
    case FILTER_BEACON:
      logIt = (subtype == 0x80);
      break;
    case FILTER_PROBE:
      logIt = (subtype == 0x40);
      break;
    case FILTER_ALL:
      logIt = true;
      break;
  }

  if (!logIt) return;

  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
          payload[10], payload[11], payload[12],
          payload[13], payload[14], payload[15]);
  latestMAC = String(macStr);
  latestRSSI = rssi;

  String logLine = latestMAC + ", RSSI: " + rssi + "\n";
  Serial.print(logLine);

  if (logFile) {
    logFile.print(logLine);
    logFile.flush();
  }
}

void startPacketMonitor() {
  latestMAC = "";
  latestRSSI = 0;
  animStep = 0;
  lastAnimTime = 0;

  WiFi.disconnect(true,true);
  delay(100);
  WiFi.mode(WIFI_MODE_NULL);
  delay(100);
  esp_wifi_set_promiscuous_rx_cb(&packetSniffer);
  esp_wifi_set_promiscuous(true);
}

void stopPacketMonitor() {
  esp_wifi_set_promiscuous(false);
  if (logFile) logFile.close();
  showFinishedMessage = true;
  finishedTime = millis();
}

// === Setup ===
void setup() {
  Serial.begin(115200);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED init failed"));
    while (1);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Starting...");
  display.display();
  delay(1000);

  if (!SD.begin(SD_CS)) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("SD init failed!");
    display.display();
    while (1);
  }

  logFile = SD.open("/packet_log.txt", FILE_WRITE);
  if (!logFile) {
    display.println("Log open failed!");
    display.display();
  }
}

// === Loop ===
void loop() {
  if (showFinishedMessage) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Logging Finished!");
    display.display();
    if (millis() - finishedTime > 2000) {
      showFinishedMessage = false;
      currentScreen = HOME;
      menuIndex = 0;
    }
    return;
  }

  switch (currentScreen) {
    case HOME:
      drawHome();
      if (wasPressed(BTN_UP)) menuIndex = (menuIndex > 0) ? menuIndex - 1 : 3;
      if (wasPressed(BTN_DOWN)) menuIndex = (menuIndex < 3) ? menuIndex + 1 : 0;
      if (wasPressed(BTN_SELECT)) {
        if (menuIndex == 0) {
          scanWiFi();
          menuIndex = 0;
          scanIndex = 0;
          currentScreen = SCAN;
        } else if (menuIndex == 1) {
          currentScreen = SELECTED_INFO;
        } else if (menuIndex == 2) {
          currentScreen = FILTER_SELECT;
          menuIndex = 0;
        }
          else if (menuIndex == 3){
          startFakeAP();
          currentScreen = FAKE_AP;
          menuIndex = 0;
      }
      break;

    case SCAN:
      drawScanList();
      if (wasPressed(BTN_UP)) {
        if (menuIndex > 0) menuIndex--;
        if (menuIndex < scanIndex) scanIndex--;
      }
      if (wasPressed(BTN_DOWN)) {
        if (menuIndex < apCount - 1) menuIndex++;
        if (menuIndex >= scanIndex + 4) scanIndex++;
      }
      if (wasPressed(BTN_SELECT)) {
        selectedAP = menuIndex;
        currentScreen = HOME;
        menuIndex = 0;
      }
      break;

    case SELECTED_INFO:
      drawSelectedInfo();
      if (wasPressed(BTN_SELECT)) {
        currentScreen = HOME;
        menuIndex = 0;
      }
      break;

    case FILTER_SELECT:
      drawFilterMenu();
      if (wasPressed(BTN_UP)) menuIndex = (menuIndex > 0) ? menuIndex - 1 : 2;
      if (wasPressed(BTN_DOWN)) menuIndex = (menuIndex < 2) ? menuIndex + 1 : 0;
      if (wasPressed(BTN_SELECT)) {
        selectedFilterType = (PacketFilter)menuIndex;
        currentScreen = MONITOR;
        startPacketMonitor();
        menuIndex = 0;
      }
      break;

    case MONITOR:
      if (wasPressed(BTN_SELECT)) {
        stopPacketMonitor();
        currentScreen = HOME;
        menuIndex = 0;
      }
      if (millis() - lastAnimTime > 500) {
        animStep = (animStep + 1) % 4;
        lastAnimTime = millis();
        drawLoggingScreen();
      }
      break;
    case FAKE_AP:
      drawFakeAP();
      if (wasPressed(BTN_SELECT)) {
        WiFi.softAPdisconnect(true);
        currentScreen = HOME;
      }
      break;
      }
  }

  delay(20);
}
