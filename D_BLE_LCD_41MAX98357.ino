#include "Arduino.h"
#include "WiFi.h"
#include "Audio.h"
#include "AiEsp32RotaryEncoder.h"
#include <U8g2lib.h>
#include <SPI.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// 常量定义
#define SHOW_TIME_PERIOD 200  // 减小到 200ms，提升流畅性
#define I2S_DOUT 39
#define I2S_BCLK 40
#define I2S_LRC 41  // // PCM5102 = 38  MAX98357 = 41
#define MAX_STATIONS 50
#define TUNE_DELAY 2000
#define WEATHER_TIMEOUT 30000
#define MODE_TIMEOUT 5000
#define UPDATE_MESSAGE_TIMEOUT 3000
#define DISPLAY_SWITCH_INTERVAL 5000
#define MAX_DISPLAY_WIDTH 128
#define SRAM_BUFFER_SIZE 32768
#define PSRAM_BUFFER_SIZE 524288

// SPI 接口引脚
#define SPI_CLK_PIN  12
#define SPI_MOSI_PIN 11
#define SPI_CS_PIN   14
#define SPI_DC_PIN   10
#define SPI_RST_PIN  13

// 旋转编码器引脚
static const uint8_t ROTARY_ENCODER_A_PIN = 16;
static const uint8_t ROTARY_ENCODER_B_PIN = 17;
static const uint8_t ROTARY_ENCODER_BUTTON_PIN = 18;
static const uint8_t ROTARY_ENCODER_STEPS = 4;

// BLE UUID
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// 工作模式
enum class Mode : uint8_t { PLAY, TUNE, VOLUME, WEATHER, GAIN };

// 全局变量
Mode currentMode = Mode::PLAY;
unsigned long lastInteraction = 0, lastTuneChange = 0, updateMessageTime = 0;
bool isUpdatingStations = false;
volatile uint8_t browseIndex = 0;
uint8_t lastVolume = 2;  // 用于保存静音前的音量

// 硬件对象
Preferences preferences;
U8G2_ST7565_LM6059_F_4W_HW_SPI u8g2(U8G2_R0, SPI_CS_PIN, SPI_DC_PIN, SPI_RST_PIN);
AiEsp32RotaryEncoder rotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, -1, ROTARY_ENCODER_STEPS);
Audio audio;
BLEServer* pServer = nullptr;
BLECharacteristic* pCharacteristic = nullptr;

// 数据结构体
struct Station { String url, name; int8_t gain; };
struct WeatherData { String condition; float temp, tempMin, tempMax, feelsLike; int humidity; float windSpeed; int windDeg; String windDir; int pressure; };
struct ForecastData { String condition; float tempMin, tempMax, windSpeed; String windDir; };

// 全局变量
Station stations[MAX_STATIONS];
uint8_t numStations = 0, currentStation = 0;
String currentSong = "未知歌曲", currentArtist = "未知歌手";
WeatherData currentWeather = { "N/A", 0, 0, 0, 0, 0, 0, 0, "", 0 };
ForecastData forecast[3];
String songPart1, songPart2, artistPart1, artistPart2;
bool songIsLong = false, artistIsLong = false;
uint8_t displayPhase = 0;

// 中断服务
IRAM_ATTR void readEncoderISR() { rotaryEncoder.readEncoder_ISR(); }

// 工具函数
String degToDirection(int deg) {
  const char* dirs[] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};
  return dirs[(deg + 22) / 45 % 8];
}

void showUpdateMessage(const String& message) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312b);
  u8g2.setCursor((128 - u8g2.getUTF8Width(message.c_str())) / 2, 32);
  u8g2.print(message);
  u8g2.sendBuffer();
  updateMessageTime = millis();
  isUpdatingStations = true;
}

// BLE 回调
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    String value = pCharacteristic->getValue().c_str();
    if (value == "NEXT") { browseIndex = (currentStation + 1) % numStations; connectToStation(browseIndex); }
    else if (value == "PREV") { browseIndex = (currentStation - 1 + numStations) % numStations; connectToStation(browseIndex); }
    else if (value == "VOL+") { uint8_t vol = min(audio.getVolume() + 1, 21); audio.setVolume(vol); preferences.putUChar("lastVolume", vol); }
    else if (value == "VOL-") { uint8_t vol = max(audio.getVolume() - 1, 0); audio.setVolume(vol); preferences.putUChar("lastVolume", vol); }
    else if (value == "MUTE") { lastVolume = audio.getVolume(); audio.setVolume(0); }
    else if (value == "UNMUTE") { audio.setVolume(lastVolume); preferences.putUChar("lastVolume", lastVolume); }
  }
};

// 音频流回调
void audio_eof_stream(const char* info) {  // 音频流中断或结束
  Serial.println("音频流中断，尝试重连...");
  connectToStation(currentStation);
}

// 网络请求
void fetchStationsFromGitHub() {
  if (WiFi.status() != WL_CONNECTED) return;
  showUpdateMessage("正在更新电台列表...");
  HTTPClient http;
  http.begin("https://raw.githubusercontent.com/buch1234/Radio-list/main/3cn");
  http.setTimeout(10000);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    numStations = 0;
    int startIdx = 0;
    while (startIdx < payload.length() && numStations < MAX_STATIONS) {
      int endIdx = payload.indexOf('\n', startIdx);
      if (endIdx == -1) endIdx = payload.length();
      String line = payload.substring(startIdx, endIdx);
      line.trim();
      int firstComma = line.indexOf(','), secondComma = line.indexOf(',', firstComma + 1);
      if (firstComma != -1 && secondComma != -1) {
        stations[numStations++] = { line.substring(firstComma + 1, secondComma), line.substring(0, firstComma), constrain(line.substring(secondComma + 1).toInt(), -10, 10) };
      }
      startIdx = endIdx + 1;
    }
    saveStations();
    rotaryEncoder.setBoundaries(0, numStations - 1, true);
    showUpdateMessage("电台列表更新完成");
  } else {
    showUpdateMessage("更新失败: HTTP " + String(httpCode));
  }
  http.end();
}

void fetchWeatherData() {
  if (WiFi.status() != WL_CONNECTED) { currentWeather.condition = "WiFi 未连接"; return; }
  HTTPClient http;
  http.begin("https://api.openweathermap.org/data/2.5/forecast?lat=52.6&lon=7.2667&units=metric&appid=841b67ea454cf7956d9d8cf4c4896b03");
  http.setTimeout(10000);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    DynamicJsonDocument doc(4096);
    if (!deserializeJson(doc, http.getString())) {
      auto weatherMatch = [](const String& cond) -> String {
        String lowerCond = cond;
        lowerCond.toLowerCase();
        if (lowerCond.indexOf("clear") != -1) return "晴天";
        if (lowerCond.indexOf("cloud") != -1) return "多云";
        if (lowerCond.indexOf("rain") != -1) return "雨天";
        if (lowerCond.indexOf("snow") != -1) return "雪天";
        if (lowerCond.indexOf("thunderstorm") != -1) return "雷雨";
        if (lowerCond.indexOf("drizzle") != -1) return "小雨";
        if (lowerCond.indexOf("mist") != -1) return "薄雾";
        if (lowerCond.indexOf("fog") != -1) return "雾天";
        if (lowerCond.indexOf("haze") != -1) return "霾";
        if (lowerCond.indexOf("storm") != -1) return "风暴";
        if (lowerCond.indexOf("overcast") != -1) return "阴天";
        if (lowerCond.indexOf("sleet") != -1) return "雨夹雪";
        if (lowerCond.indexOf("dust") != -1 || lowerCond.indexOf("sand") != -1) return "沙尘";
        if (lowerCond.indexOf("smoke") != -1) return "烟雾";
        return "其他";
      };

      currentWeather.condition = weatherMatch(doc["list"][0]["weather"][0]["description"].as<String>());
      currentWeather.temp = doc["list"][0]["main"]["temp"];
      currentWeather.tempMin = doc["list"][0]["main"]["temp_min"];
      currentWeather.tempMax = doc["list"][0]["main"]["temp_max"];
      currentWeather.feelsLike = doc["list"][0]["main"]["feels_like"];
      currentWeather.humidity = doc["list"][0]["main"]["humidity"];
      currentWeather.windSpeed = doc["list"][0]["wind"]["speed"];
      currentWeather.windDeg = doc["list"][0]["wind"]["deg"];
      currentWeather.windDir = degToDirection(currentWeather.windDeg);
      currentWeather.pressure = doc["list"][0]["main"]["pressure"];

      for (int i = 0; i < 3; i++) {
        int idx = (i + 1) * 8;
        if (idx < doc["list"].size()) {
          forecast[i].condition = weatherMatch(doc["list"][idx]["weather"][0]["description"].as<String>());
          forecast[i].tempMin = doc["list"][idx]["main"]["temp_min"];
          forecast[i].tempMax = doc["list"][idx]["main"]["temp_max"];
          forecast[i].windSpeed = doc["list"][idx]["wind"]["speed"];
          forecast[i].windDir = degToDirection(doc["list"][idx]["wind"]["deg"]);
        }
      }
    } else {
      currentWeather.condition = "数据解析失败";
    }
  } else {
    currentWeather.condition = "天气获取失败";
  }
  http.end();
}

// 存储管理
void loadStations() {
  numStations = preferences.getUChar("stationCount", 0);
  if (numStations == 0) fetchStationsFromGitHub();
  else {
    char key[12];
    for (uint8_t i = 0; i < numStations; i++) {
      sprintf(key, "station%d", i);
      stations[i] = { preferences.getString((String(key) + "_url").c_str(), ""),
                      preferences.getString((String(key) + "_name").c_str(), ""),
                      preferences.getChar((String(key) + "_gain").c_str(), 0) };
    }
  }
  rotaryEncoder.setBoundaries(0, numStations - 1, true);
}

void saveStations() {
  preferences.putUChar("stationCount", numStations);
  char key[12];
  for (uint8_t i = 0; i < numStations; i++) {
    sprintf(key, "station%d", i);
    preferences.putString((String(key) + "_url").c_str(), stations[i].url);
    preferences.putString((String(key) + "_name").c_str(), stations[i].name);
    preferences.putChar((String(key) + "_gain").c_str(), stations[i].gain);
  }
}

// 双核任务
void audioTask(void* pvParameters) {
  while (1) {
    audio.loop();
    delay(1);
  }
}

void displayTask(void* pvParameters) {
  while (1) {
    handleUserInput();
    updateDisplay();
    delay(1);
  }
}

// 主程序
void setup() {
  Serial.begin(115200);
  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");
  WiFiManager wm;
  wm.autoConnect("ESP Radio", "");
  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  u8g2.begin();
  u8g2.enableUTF8Print();
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setBufsize(SRAM_BUFFER_SIZE, PSRAM_BUFFER_SIZE);
  audio.setConnectionTimeout(10000, 15000);
  // 移除 setStreamEndCallback，直接依赖 audio_eof_stream 回调
  preferences.begin("radio", false);
  loadStations();
  currentStation = constrain(preferences.getUChar("lastStation", 0), 0, numStations - 1);
  lastVolume = preferences.getUChar("lastVolume", 2);
  audio.setVolume(lastVolume);
  connectToStation(currentStation);
  fetchWeatherData();

  // 初始化 BLE
  BLEDevice::init("ESP32 Radio");
  pServer = BLEDevice::createServer();
  BLEService* pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->setValue("Ready");
  pService->start();
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->start();
  Serial.println("BLE 已启动");

  // 创建双核任务
  xTaskCreatePinnedToCore(audioTask, "AudioTask", 10000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(displayTask, "DisplayTask", 10000, NULL, 1, NULL, 1);
}

void loop() {
  delay(1000);  // 主循环空闲
}

void handleUserInput() {
  static unsigned long buttonPressTime = 0;
  static bool buttonPressed = false;
  static int16_t lastEncoderValue = rotaryEncoder.readEncoder();

  if (rotaryEncoder.encoderChanged()) {
    lastInteraction = millis();
    int16_t newValue = rotaryEncoder.readEncoder();
    int8_t change = newValue - lastEncoderValue;
    if (change) {
      lastEncoderValue = newValue;
      switch (currentMode) {
        case Mode::PLAY: currentMode = Mode::TUNE; lastTuneChange = millis(); break;
        case Mode::TUNE: browseIndex = (browseIndex + change + numStations) % numStations; lastTuneChange = millis(); break;
        case Mode::VOLUME: audio.setVolume(newValue); preferences.putUChar("lastVolume", audio.getVolume()); break;
        case Mode::GAIN: stations[currentStation].gain = newValue; connectToStation(currentStation); saveStations(); break;
        default: break;
      }
    }
  }

  if (!digitalRead(ROTARY_ENCODER_BUTTON_PIN)) {
    if (!buttonPressed) { buttonPressTime = millis(); buttonPressed = true; }
  } else if (buttonPressed) {
    buttonPressed = false;
    unsigned long pressDuration = millis() - buttonPressTime;
    lastInteraction = millis();
    if (pressDuration >= 3000 && currentMode == Mode::PLAY) { fetchStationsFromGitHub(); connectToStation(currentStation); }
    else if (pressDuration >= 1000) {
      currentMode = (currentMode != Mode::WEATHER) ? Mode::WEATHER : Mode::PLAY;
      if (currentMode == Mode::WEATHER) fetchWeatherData();
      rotaryEncoder.setBoundaries(0, numStations - 1, true);
    } else {
      switch (currentMode) {
        case Mode::PLAY: currentMode = Mode::VOLUME; rotaryEncoder.setBoundaries(0, 21, false); rotaryEncoder.setEncoderValue(audio.getVolume()); break;
        case Mode::TUNE: connectToStation(browseIndex); currentMode = Mode::PLAY; break;
        case Mode::VOLUME: currentMode = Mode::GAIN; rotaryEncoder.setBoundaries(-10, 10, false); rotaryEncoder.setEncoderValue(stations[currentStation].gain); break;
        case Mode::GAIN: case Mode::WEATHER: currentMode = Mode::PLAY; rotaryEncoder.setBoundaries(0, numStations - 1, true); break;
      }
    }
  }

  if (currentMode == Mode::TUNE && millis() - lastTuneChange >= TUNE_DELAY) connectToStation(browseIndex);
  if (currentMode == Mode::WEATHER && millis() - lastInteraction > WEATHER_TIMEOUT) currentMode = Mode::PLAY;
  else if (currentMode != Mode::PLAY && currentMode != Mode::WEATHER && millis() - lastInteraction > MODE_TIMEOUT) currentMode = Mode::PLAY;
}

void updateDisplay() {
  static unsigned long lastUpdate = 0, lastWeatherSwitch = 0, lastSongSwitch = 0, lastWeatherUpdate = 0;
  static bool showWeatherGroup1 = true;
  if (millis() - lastUpdate < SHOW_TIME_PERIOD) return;
  lastUpdate = millis();

  if (isUpdatingStations && millis() - updateMessageTime < UPDATE_MESSAGE_TIMEOUT) return;
  isUpdatingStations = false;

  u8g2.clearBuffer();
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char dateStr[32], timeStr[16], volumeStr[16];
    static const char* weekDays[] PROGMEM = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d %s", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, weekDays[timeinfo.tm_wday]);
    snprintf(timeStr, sizeof(timeStr), "%02d.%02d.%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    snprintf(volumeStr, sizeof(volumeStr), "Vol: %02d", audio.getVolume());
    u8g2.setFont(u8g2_font_wqy12_t_gb2312b);

    switch (currentMode) {
      case Mode::PLAY:
        u8g2.setCursor((128 - u8g2.getUTF8Width(dateStr)) / 2, 12); u8g2.print(dateStr);
        u8g2.setCursor(20, 24); u8g2.print(timeStr);
        u8g2.setCursor(128 - u8g2.getUTF8Width(volumeStr), 24); u8g2.print(volumeStr);
        u8g2.drawHLine(0, 26, 128);
        u8g2.setCursor(0, 38); u8g2.printf("第%d台: %s", currentStation + 1, stations[currentStation].name.c_str());
        if (millis() - lastSongSwitch >= DISPLAY_SWITCH_INTERVAL) {
          displayPhase = (songIsLong && artistIsLong) ? (displayPhase + 1) % 4 : (songIsLong || artistIsLong) ? (displayPhase + 1) % 3 : (displayPhase + 1) % 2;
          lastSongSwitch = millis();
        }
        u8g2.setCursor(0, 50);
        if (songIsLong && artistIsLong) {
          if (displayPhase == 0) u8g2.print("歌名: " + songPart1);
          else if (displayPhase == 1) u8g2.print("歌名: " + songPart2);
          else if (displayPhase == 2) u8g2.print("歌手: " + artistPart1);
          else u8g2.print("歌手: " + artistPart2);
        } else if (songIsLong) {
          if (displayPhase == 0) u8g2.print("歌名: " + songPart1);
          else if (displayPhase == 1) u8g2.print("歌名: " + songPart2);
          else u8g2.print("歌手: " + artistPart1);
        } else if (artistIsLong) {
          if (displayPhase == 0) u8g2.print("歌名: " + songPart1);
          else if (displayPhase == 1) u8g2.print("歌手: " + artistPart1);
          else u8g2.print("歌手: " + artistPart2);
        } else {
          if (displayPhase == 0) u8g2.print("歌名: " + songPart1);
          else u8g2.print("歌手: " + artistPart1);
        }
        if (millis() - lastWeatherSwitch >= DISPLAY_SWITCH_INTERVAL) { showWeatherGroup1 = !showWeatherGroup1; lastWeatherSwitch = millis(); }
        u8g2.setCursor(0, 62);
        showWeatherGroup1 ? u8g2.printf("%s %.1f°C 体感%.1f°C", currentWeather.condition.c_str(), currentWeather.temp, currentWeather.feelsLike)
                          : u8g2.printf("%s风%.1fm/s %d%% %dhPa", currentWeather.windDir.c_str(), currentWeather.windSpeed, currentWeather.humidity, currentWeather.pressure);
        break;
      case Mode::TUNE:
        for (uint8_t i = 0; i < min((uint8_t)5, numStations); i++) {
          int8_t idx = (browseIndex - 2 + i + numStations) % numStations;
          u8g2.setCursor(10, 12 + i * 12);
          u8g2.print((idx == browseIndex ? "> " : "") + stations[idx].name);
        }
        break;
      case Mode::VOLUME:
        u8g2.setCursor(0, 12); u8g2.print(dateStr);
        u8g2.setCursor(0, 24); u8g2.print(timeStr);
        u8g2.setCursor(80, 24); u8g2.print(volumeStr);
        u8g2.setCursor(0, 36); u8g2.print("调节音量: " + String(audio.getVolume()));
        break;
      case Mode::GAIN:
        u8g2.setCursor(0, 12); u8g2.print(dateStr);
        u8g2.setCursor(0, 24); u8g2.print(timeStr);
        u8g2.setCursor(80, 24); u8g2.print(volumeStr);
        u8g2.setCursor(0, 36); u8g2.printf("电台: %s", stations[currentStation].name.c_str());
        u8g2.setCursor(0, 48); u8g2.printf("增益: %d", stations[currentStation].gain);
        break;
      case Mode::WEATHER:
        u8g2.setCursor(0, 14); u8g2.printf("%s %.1f-%.1f°C %s风%.1fm/s", currentWeather.condition.c_str(), currentWeather.tempMin, currentWeather.tempMax, currentWeather.windDir.c_str(), currentWeather.windSpeed);
        for (int i = 0; i < 3; i++) {
          u8g2.setCursor(0, 26 + i * 12);
          u8g2.printf("%s %.1f-%.1f°C %s风%.1fm/s", forecast[i].condition.c_str(), forecast[i].tempMin, forecast[i].tempMax, forecast[i].windDir.c_str(), forecast[i].windSpeed);
        }
        break;
    }
    u8g2.sendBuffer();
    if (millis() - lastWeatherUpdate >= 300000) { fetchWeatherData(); lastWeatherUpdate = millis(); }
  }
}

void connectToStation(uint8_t stationIndex) {
  if (stationIndex >= numStations) return;
  int8_t prevGain = stations[currentStation].gain;
  currentStation = stationIndex;
  audio.setVolume(constrain(audio.getVolume() - prevGain + stations[currentStation].gain, 0, 21));
  preferences.putUChar("lastStation", currentStation);
  preferences.putUChar("lastVolume", audio.getVolume());
  audio.stopSong();
  audio.connecttohost(stations[stationIndex].url.c_str());
}

void audio_showstreamtitle(const char* info) {
  String meta = info;
  int sep = meta.indexOf(" - ");
  currentArtist = (sep != -1) ? meta.substring(0, sep) : "未知歌手";
  currentSong = (sep != -1) ? meta.substring(sep + 3) : meta;
  u8g2.setFont(u8g2_font_wqy12_t_gb2312b);

  auto splitText = [&](const String& text, String& part1, String& part2, bool& isLong, const char* prefix) {
    String full = String(prefix) + text;
    isLong = u8g2.getUTF8Width(full.c_str()) > MAX_DISPLAY_WIDTH;
    if (isLong) {
      int lastSpace = -1;
      for (int i = text.length() - 1; i >= 0; i--) {
        if (text[i] == ' ' && u8g2.getUTF8Width((String(prefix) + text.substring(0, i)).c_str()) <= MAX_DISPLAY_WIDTH) {
          lastSpace = i;
          break;
        }
      }
      part1 = (lastSpace != -1) ? text.substring(0, lastSpace) : text.substring(0, text.length() / 2);
      part2 = (lastSpace != -1) ? text.substring(lastSpace + 1) : text.substring(text.length() / 2);
    } else {
      part1 = text;
      part2 = "";
    }
  };
  splitText(currentSong, songPart1, songPart2, songIsLong, "歌名: ");
  splitText(currentArtist, artistPart1, artistPart2, artistIsLong, "歌手: ");
  displayPhase = 0;
}