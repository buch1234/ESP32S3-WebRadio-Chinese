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

// 常量定义
#define SHOW_TIME_PERIOD 1000
#define I2S_DOUT 39
#define I2S_BCLK 40
#define I2S_LRC 41
#define MAX_STATIONS 50
#define TUNE_DELAY 2000
#define WEATHER_TIMEOUT 30000  // 天气模式30秒超时
#define MODE_TIMEOUT 5000      // 其他模式5秒超时
#define UPDATE_MESSAGE_TIMEOUT 3000
#define DISPLAY_SWITCH_INTERVAL 5000  // 每5秒切换显示
#define MAX_DISPLAY_WIDTH 128         // 显示屏宽度（像素）
#define MAX_BUFFER_SIZE (2 * 1024 * 1024) // 2MB PSRAM 缓冲区
#define MAX_RECONNECT_ATTEMPTS 3      // 最大重连次数
#define RECONNECT_DELAY 2000          // 重连延迟（毫秒）
#define BUFFER_CHECK_INTERVAL 500     // 缓冲区检查间隔（毫秒）
#define CHUNK_SIZE 1024               // 每次读取的数据块大小（字节）

// 旋转编码器引脚
static const uint8_t ROTARY_ENCODER_A_PIN = 16;
static const uint8_t ROTARY_ENCODER_B_PIN = 17;
static const uint8_t ROTARY_ENCODER_BUTTON_PIN = 18;
static const uint8_t ROTARY_ENCODER_STEPS = 4;

// 工作模式
enum class Mode : uint8_t {
  PLAY,
  TUNE,
  VOLUME,
  WEATHER,
  GAIN
};

// 全局变量
Mode currentMode = Mode::PLAY;
unsigned long lastInteraction = 0;
unsigned long lastTuneChange = 0;
bool isUpdatingStations = false;
unsigned long updateMessageTime = 0;
uint8_t currentStationIndex = 0; // 当前电台索引
uint8_t bufferPosition = 0;      // 当前缓冲区填充位置（以 CHUNK_SIZE 为单位计数）
uint8_t* audioBuffer;            // 2MB PSRAM 缓冲区指针

// 硬件对象
Preferences preferences;
U8G2_ST7565_LM6059_F_4W_SW_SPI u8g2(U8G2_R0, 11, 10, 14, 12, 13);
AiEsp32RotaryEncoder rotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, -1, ROTARY_ENCODER_STEPS);
Audio audio;

// 电台和天气结构体
struct Station {
  String url;
  String name;
  int8_t gain;
};

struct WeatherData {
  String condition;
  float temp;
  float tempMin;
  float tempMax;
  float feelsLike;
  int humidity;
  float windSpeed;
  int windDeg;     // 风向角度
  String windDir;  // 风向中文
  int pressure;
};

struct ForecastData {
  String condition;
  float tempMin;
  float tempMax;
  float windSpeed;
  String windDir;  // 风向中文
};

// 全局变量
Station stations[MAX_STATIONS];
uint8_t numStations = 0;
uint8_t currentStation = 0;
uint8_t browseIndex = 0;
String currentSong = "未知歌曲";
String currentArtist = "未知歌手";
WeatherData currentWeather = { "N/A", 0.0, 0.0, 0.0, 0.0, 0, 0.0, 0, "", 0 };
ForecastData forecast[3];  // 未来 3 天预报

// 显示控制变量
String songPart1 = "";
String songPart2 = "";
String artistPart1 = "";
String artistPart2 = "";
bool songIsLong = false;
bool artistIsLong = false;
uint8_t displayPhase = 0;  // 0: 歌曲部分1, 1: 歌曲部分2, 2: 歌手部分1, 3: 歌手部分2（若适用）

// 中断服务
void IRAM_ATTR readEncoderISR() {
  rotaryEncoder.readEncoder_ISR();
}

// 将风向角度转换为中文方向
String degToDirection(int deg) {
  if (deg >= 337.5 || deg < 22.5) return "北";
  else if (deg >= 22.5 && deg < 67.5) return "东北";
  else if (deg >= 67.5 && deg < 112.5) return "东";
  else if (deg >= 112.5 && deg < 157.5) return "东南";
  else if (deg >= 157.5 && deg < 202.5) return "南";
  else if (deg >= 202.5 && deg < 247.5) return "西南";
  else if (deg >= 247.5 && deg < 292.5) return "西";
  else if (deg >= 292.5 && deg < 337.5) return "西北";
  return "未知";
}

// 显示更新消息
void showUpdateMessage(String message) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312b);
  u8g2.setCursor((128 - u8g2.getUTF8Width(message.c_str())) / 2, 32);
  u8g2.print(message);
  u8g2.sendBuffer();
  updateMessageTime = millis();
  isUpdatingStations = true;
}

// 从 GitHub 获取电台列表
void fetchStationsFromGitHub() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 未连接，无法获取 GitHub 电台列表");
    return;
  }

  showUpdateMessage("正在更新电台列表...");
  HTTPClient http;
  String url = "https://raw.githubusercontent.com/buch1234/Radio-list/main/3cn";
  http.begin(url);
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

      int firstComma = line.indexOf(',');
      int secondComma = line.indexOf(',', firstComma + 1);
      if (firstComma != -1 && secondComma != -1) {
        String name = line.substring(0, firstComma);
        String url = line.substring(firstComma + 1, secondComma);
        String gainStr = line.substring(secondComma + 1);
        int8_t gain = gainStr.toInt();

        stations[numStations] = { url, name, constrain(gain, -10, 10) };
        numStations++;
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

// 加载和保存电台
void loadStations() {
  numStations = preferences.getUChar("stationCount", 0);
  if (numStations == 0) {
    fetchStationsFromGitHub();
  } else {
    char keyBase[12];
    for (uint8_t i = 0; i < numStations; i++) {
      sprintf(keyBase, "station%d", i);
      stations[i].url = preferences.getString((String(keyBase) + "_url").c_str(), "");
      stations[i].name = preferences.getString((String(keyBase) + "_name").c_str(), "");
      stations[i].gain = preferences.getChar((String(keyBase) + "_gain").c_str(), 0);
    }
  }
  rotaryEncoder.setBoundaries(0, numStations - 1, true);
}

void saveStations() {
  preferences.putUChar("stationCount", numStations);
  char keyBase[12];
  for (uint8_t i = 0; i < numStations; i++) {
    sprintf(keyBase, "station%d", i);
    preferences.putString((String(keyBase) + "_url").c_str(), stations[i].url);
    preferences.putString((String(keyBase) + "_name").c_str(), stations[i].name);
    preferences.putChar((String(keyBase) + "_gain").c_str(), stations[i].gain);
  }
}

// 获取天气数据
void fetchWeatherData() {
  if (WiFi.status() != WL_CONNECTED) {
    currentWeather.condition = "WiFi 未连接";
    return;
  }

  HTTPClient http;
  String apiKey = "841b67ea454cf7956d9d8cf4c4896b03";
  String url = "https://api.openweathermap.org/data/2.5/forecast?lat=52.6&lon=7.2667&units=metric&appid=" + apiKey;
  http.begin(url);
  http.setTimeout(10000);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      // 当前天气（取第一个数据点）
      currentWeather.condition = doc["list"][0]["weather"][0]["description"].as<String>();
      currentWeather.temp = doc["list"][0]["main"]["temp"];
      currentWeather.feelsLike = doc["list"][0]["main"]["feels_like"];
      currentWeather.windSpeed = doc["list"][0]["wind"]["speed"];
      currentWeather.windDeg = doc["list"][0]["wind"]["deg"];
      currentWeather.windDir = degToDirection(currentWeather.windDeg);  // 转换为中文风向
      currentWeather.humidity = doc["list"][0]["main"]["humidity"];
      currentWeather.pressure = doc["list"][0]["main"]["pressure"];
      currentWeather.tempMin = doc["list"][0]["main"]["temp_min"];
      currentWeather.tempMax = doc["list"][0]["main"]["temp_max"];

      if (currentWeather.condition.indexOf("clear") != -1) currentWeather.condition = "晴天";
      else if (currentWeather.condition.indexOf("cloud") != -1) currentWeather.condition = "多云";
      else if (currentWeather.condition.indexOf("rain") != -1) currentWeather.condition = "雨天";
      else if (currentWeather.condition.indexOf("snow") != -1) currentWeather.condition = "雪天";
      else if (currentWeather.condition.indexOf("thunderstorm") != -1) currentWeather.condition = "雷雨";
      else if (currentWeather.condition.indexOf("drizzle") != -1) currentWeather.condition = "小雨";
      else if (currentWeather.condition.indexOf("mist") != -1) currentWeather.condition = "薄雾";
      else if (currentWeather.condition.indexOf("fog") != -1) currentWeather.condition = "雾天";
      else if (currentWeather.condition.indexOf("haze") != -1) currentWeather.condition = "霾";
      else if (currentWeather.condition.indexOf("storm") != -1) currentWeather.condition = "风暴";
      else currentWeather.condition = "未知";

      // 提取未来 3 天预报（每天取第8个数据点，约24小时后）
      for (int i = 0; i < 3; i++) {
        int index = (i + 1) * 8;  // 每24小时取一个点
        if (index < doc["list"].size()) {
          forecast[i].condition = doc["list"][index]["weather"][0]["description"].as<String>();
          forecast[i].tempMin = doc["list"][index]["main"]["temp_min"];
          forecast[i].tempMax = doc["list"][index]["main"]["temp_max"];
          forecast[i].windSpeed = doc["list"][index]["wind"]["speed"];
          int windDeg = doc["list"][index]["wind"]["deg"];
          forecast[i].windDir = degToDirection(windDeg);

          if (forecast[i].condition.indexOf("clear") != -1) forecast[i].condition = "晴天";
          else if (forecast[i].condition.indexOf("cloud") != -1) forecast[i].condition = "多云";
          else if (forecast[i].condition.indexOf("rain") != -1) forecast[i].condition = "雨天";
          else if (forecast[i].condition.indexOf("snow") != -1) forecast[i].condition = "雪天";
          else if (forecast[i].condition.indexOf("thunderstorm") != -1) forecast[i].condition = "雷雨";
          else if (forecast[i].condition.indexOf("drizzle") != -1) forecast[i].condition = "小雨";
          else if (forecast[i].condition.indexOf("mist") != -1) forecast[i].condition = "薄雾";
          else if (forecast[i].condition.indexOf("fog") != -1) forecast[i].condition = "雾天";
          else if (forecast[i].condition.indexOf("haze") != -1) forecast[i].condition = "霾";
          else if (forecast[i].condition.indexOf("storm") != -1) forecast[i].condition = "风暴";
          else forecast[i].condition = "未知";
        }
      }
    }
  }
  http.end();
}

// 连接电台并管理缓冲
void connectToStation(uint8_t stationIndex) {
  if (stationIndex >= numStations) return;

  uint8_t currentVolume = audio.getVolume();
  int8_t previousGain = stations[currentStation].gain;
  int8_t newGain = stations[stationIndex].gain;
  currentStation = stationIndex;
  currentStationIndex = stationIndex;

  int16_t adjustedVolume = currentVolume - previousGain + newGain;
  audio.setVolume(constrain(adjustedVolume, 0, 21));
  preferences.putUChar("lastStation", currentStation);
  preferences.putUChar("lastVolume", audio.getVolume());

  // 重置缓冲区
  bufferPosition = 0;
  audio.connecttohost(stations[stationIndex].url.c_str());
  Serial.printf("连接到电台: %s\n", stations[stationIndex].name.c_str());
}

// 处理音频数据填充缓冲区
void audio_id3data(const char *info) {  // ID3 元数据回调
  audio_showstreamtitle(info);  // 更新歌曲和歌手信息
}

void audio_eof_mp3(const char *info) {  // 播放结束回调
  Serial.println("电台流结束，尝试重连...");
  connectWithRetry();
}

void audio_process(uint8_t *data, size_t len) {  // 自定义回调处理音频数据
  if (bufferPosition * CHUNK_SIZE + len <= MAX_BUFFER_SIZE) {
    memcpy(audioBuffer + (bufferPosition * CHUNK_SIZE), data, len);
    bufferPosition += (len + CHUNK_SIZE - 1) / CHUNK_SIZE;  // 按 CHUNK_SIZE 向上取整
    Serial.printf("缓冲区填充: %d/%d 字节\n", bufferPosition * CHUNK_SIZE, MAX_BUFFER_SIZE);
  } else {
    Serial.println("缓冲区已满，等待播放消耗");
  }
}

// 带重连机制的连接
void connectWithRetry() {
  uint8_t attempts = 0;
  while (attempts < MAX_RECONNECT_ATTEMPTS) {
    if (audio.connecttohost(stations[currentStationIndex].url.c_str())) {
      Serial.printf("重连成功: %s\n", stations[currentStationIndex].name.c_str());
      bufferPosition = 0;  // 重置缓冲区
      return;
    }
    attempts++;
    Serial.printf("重连尝试 %d/%d 失败\n", attempts, MAX_RECONNECT_ATTEMPTS);
    delay(RECONNECT_DELAY);
  }

  // 重连失败，切换到下一电台
  Serial.println("达到最大重连次数，切换到下一电台");
  switchToNextStation();
}

// 切换到下一电台
void switchToNextStation() {
  currentStationIndex = (currentStationIndex + 1) % numStations;
  Serial.printf("切换到电台: %s\n", stations[currentStationIndex].name.c_str());
  connectToStation(currentStationIndex);
}

void setup() {
  Serial.begin(115200);
  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");

  WiFiManager wm;
  wm.autoConnect("ESP Radio", "");
  Serial.println("WiFi 已连接，IP: " + WiFi.localIP().toString());

  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);

  u8g2.begin();
  u8g2.enableUTF8Print();

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  preferences.begin("radio", false);

  // 分配 2MB PSRAM 缓冲区
  audioBuffer = (uint8_t*)ps_malloc(MAX_BUFFER_SIZE);
  if (audioBuffer == NULL) {
    Serial.println("PSRAM 分配失败！");
    while (1);  // 停止运行
  }
  Serial.printf("成功分配 %d 字节 PSRAM 缓冲区\n", MAX_BUFFER_SIZE);

  loadStations();
  uint8_t lastStation = preferences.getUChar("lastStation", 0);
  uint8_t lastVolume = preferences.getUChar("lastVolume", 2);
  currentStation = lastStation < numStations ? lastStation : 0;
  audio.setVolume(lastVolume);
  connectToStation(currentStation);
  fetchWeatherData();
}

void loop() {
  audio.loop();  // 处理音频播放和缓冲
  handleUserInput();
  updateDisplay();
}

void handleUserInput() {
  static unsigned long buttonPressTime = 0;
  static bool buttonPressed = false;

  if (rotaryEncoder.encoderChanged()) {
    lastInteraction = millis();
    static int16_t lastEncoderValue = rotaryEncoder.readEncoder();
    int16_t newEncoderValue = rotaryEncoder.readEncoder();
    int8_t change = newEncoderValue - lastEncoderValue;

    if (change) {
      lastEncoderValue = newEncoderValue;
      switch (currentMode) {
        case Mode::PLAY:
          currentMode = Mode::TUNE;
          lastTuneChange = millis();
          rotaryEncoder.setBoundaries(0, numStations - 1, true);
          break;
        case Mode::TUNE:
          browseIndex = (browseIndex + change + numStations) % numStations;
          lastTuneChange = millis();
          break;
        case Mode::VOLUME:
          rotaryEncoder.setBoundaries(0, 21, false);
          audio.setVolume(rotaryEncoder.readEncoder());
          preferences.putUChar("lastVolume", audio.getVolume());
          break;
        case Mode::GAIN:
          rotaryEncoder.setBoundaries(-10, 10, false);
          stations[currentStation].gain = rotaryEncoder.readEncoder();
          connectToStation(currentStation);
          saveStations();
          break;
        case Mode::WEATHER:
          break;
      }
    }
  }

  if (digitalRead(ROTARY_ENCODER_BUTTON_PIN) == LOW) {
    if (!buttonPressed) {
      buttonPressTime = millis();
      buttonPressed = true;
    }
  } else if (buttonPressed) {
    buttonPressed = false;
    unsigned long pressDuration = millis() - buttonPressTime;

    lastInteraction = millis();
    if (pressDuration >= 3000 && currentMode == Mode::PLAY) {
      fetchStationsFromGitHub();
      connectToStation(currentStation);
    } else if (pressDuration >= 1000) {
      if (currentMode != Mode::WEATHER) {
        currentMode = Mode::WEATHER;
        fetchWeatherData();
      } else {
        currentMode = Mode::PLAY;
      }
      rotaryEncoder.setBoundaries(0, numStations - 1, true);
    } else {
      switch (currentMode) {
        case Mode::PLAY:
          currentMode = Mode::VOLUME;
          rotaryEncoder.setBoundaries(0, 21, false);
          rotaryEncoder.setEncoderValue(audio.getVolume());
          break;
        case Mode::TUNE:
          connectToStation(browseIndex);
          currentMode = Mode::PLAY;
          rotaryEncoder.setBoundaries(0, numStations - 1, true);
          break;
        case Mode::VOLUME:
          currentMode = Mode::GAIN;
          rotaryEncoder.setBoundaries(-10, 10, false);
          rotaryEncoder.setEncoderValue(stations[currentStation].gain);
          break;
        case Mode::GAIN:
          currentMode = Mode::PLAY;
          rotaryEncoder.setBoundaries(0, numStations - 1, true);
          break;
        case Mode::WEATHER:
          currentMode = Mode::PLAY;
          rotaryEncoder.setBoundaries(0, numStations - 1, true);
          break;
      }
    }
  }

  if (currentMode == Mode::TUNE && millis() - lastTuneChange >= TUNE_DELAY) {
    connectToStation(browseIndex);
    lastTuneChange = millis() + TUNE_DELAY;
  }

  if (currentMode == Mode::WEATHER && millis() - lastInteraction > WEATHER_TIMEOUT) {
    currentMode = Mode::PLAY;
    rotaryEncoder.setBoundaries(0, numStations - 1, true);
    Serial.println("天气模式超时，返回播放模式");
  } else if (currentMode != Mode::PLAY && currentMode != Mode::WEATHER && millis() - lastInteraction > MODE_TIMEOUT) {
    currentMode = Mode::PLAY;
    rotaryEncoder.setBoundaries(0, numStations - 1, true);
    Serial.println("其他模式超时，返回播放模式");
  }
}

void updateDisplay() {
  static unsigned long lastUpdate = 0;
  static unsigned long lastWeatherSwitch = 0;
  static unsigned long lastSongSwitch = 0;
  static bool showWeatherGroup1 = true;

  if (millis() - lastUpdate < SHOW_TIME_PERIOD) return;
  lastUpdate = millis();

  if (isUpdatingStations && millis() - updateMessageTime < UPDATE_MESSAGE_TIMEOUT) {
    return;
  } else if (isUpdatingStations) {
    isUpdatingStations = false;
  }

  u8g2.clearBuffer();
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char dateStr[32], timeStr[16], volumeStr[16];
    static const char* weekDays[] PROGMEM = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };
    snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d %s", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, weekDays[timeinfo.tm_wday]);
    // 修改时间格式为“小时:分钟:秒”
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    snprintf(volumeStr, sizeof(volumeStr), "Vol: %02d", audio.getVolume());

    u8g2.setFont(u8g2_font_wqy12_t_gb2312b);

    switch (currentMode) {
      case Mode::PLAY:
        {
          u8g2.setCursor((128 - u8g2.getUTF8Width(dateStr)) / 2, 12);
          u8g2.print(dateStr);
          u8g2.setCursor(20, 24);
          u8g2.print(timeStr);  // 显示“13:10:10”格式
          u8g2.setCursor(128 - u8g2.getUTF8Width(volumeStr), 24);
          u8g2.print(volumeStr);
          u8g2.drawHLine(0, 26, 128);
          u8g2.setCursor(0, 38);
          u8g2.printf("第%d台: %s", currentStation + 1, stations[currentStation].name.c_str());

          // 每5秒切换显示阶段
          if (millis() - lastSongSwitch >= DISPLAY_SWITCH_INTERVAL) {
            if (songIsLong && !artistIsLong) {
              displayPhase = (displayPhase + 1) % 3;
            } else if (!songIsLong && artistIsLong) {
              displayPhase = (displayPhase + 1) % 3;
            } else if (songIsLong && artistIsLong) {
              displayPhase = (displayPhase + 1) % 4;
            } else {
              displayPhase = (displayPhase + 1) % 2;
            }
            lastSongSwitch = millis();
          }

          // 根据阶段显示内容
          u8g2.setCursor(0, 50);
          if (songIsLong && !artistIsLong) {
            if (displayPhase == 0) u8g2.print("歌名: " + songPart1);
            else if (displayPhase == 1) u8g2.print("歌名: " + songPart2);
            else u8g2.print("歌手: " + artistPart1);
          } else if (!songIsLong && artistIsLong) {
            if (displayPhase == 0) u8g2.print("歌名: " + songPart1);
            else if (displayPhase == 1) u8g2.print("歌手: " + artistPart1);
            else u8g2.print("歌手: " + artistPart2);
          } else if (songIsLong && artistIsLong) {
            if (displayPhase == 0) u8g2.print("歌名: " + songPart1);
            else if (displayPhase == 1) u8g2.print("歌名: " + songPart2);
            else if (displayPhase == 2) u8g2.print("歌手: " + artistPart1);
            else u8g2.print("歌手: " + artistPart2);
          } else {
            if (displayPhase == 0) u8g2.print("歌名: " + songPart1);
            else u8g2.print("歌手: " + artistPart1);
          }

          // 天气信息切换
          if (millis() - lastWeatherSwitch >= DISPLAY_SWITCH_INTERVAL) {
            showWeatherGroup1 = !showWeatherGroup1;
            lastWeatherSwitch = millis();
          }
          u8g2.setCursor(0, 62);
          if (showWeatherGroup1) {
            u8g2.printf("%s %.1f°C 体感%.1f°C",
                        currentWeather.condition.c_str(),
                        currentWeather.temp,
                        currentWeather.feelsLike);
          } else {
            u8g2.printf("%s风%.1fm/s %d%% %dhPa",
                        currentWeather.windDir.c_str(),
                        currentWeather.windSpeed,
                        currentWeather.humidity,
                        currentWeather.pressure);
          }
          break;
        }
      case Mode::TUNE:
        for (uint8_t i = 0; i < min((uint8_t)5, numStations); i++) {
          int8_t displayIndex = (browseIndex - 2 + i + numStations) % numStations;
          int yPos = 12 + i * 12;
          u8g2.setCursor(10, yPos);
          if (displayIndex == browseIndex) {
            u8g2.print("> " + stations[displayIndex].name);
          } else {
            u8g2.print(stations[displayIndex].name);
          }
        }
        break;
      case Mode::VOLUME:
        u8g2.setCursor(0, 12);
        u8g2.print(dateStr);
        u8g2.setCursor(0, 24);
        u8g2.print(timeStr);  // 显示“13:10:10”格式
        u8g2.setCursor(80, 24);
        u8g2.print(volumeStr);
        u8g2.setCursor(0, 36);
        u8g2.print("调节音量: " + String(audio.getVolume()));
        break;
      case Mode::GAIN:
        u8g2.setCursor(0, 12);
        u8g2.print(dateStr);
        u8g2.setCursor(0, 24);
        u8g2.print(timeStr);  // 显示“13:10:10”格式
        u8g2.setCursor(80, 24);
        u8g2.print(volumeStr);
        u8g2.setCursor(0, 36);
        u8g2.printf("电台: %s", stations[currentStation].name.c_str());
        u8g2.setCursor(0, 48);
        u8g2.printf("增益: %d", stations[currentStation].gain);
        break;
      case Mode::WEATHER:
        {
          u8g2.setFont(u8g2_font_wqy12_t_gb2312b);

          u8g2.setCursor(0, 14);
          u8g2.printf("%s %.1f-%.1f°C %s风%.1fm/s",
                      currentWeather.condition.c_str(),
                      currentWeather.tempMin,
                      currentWeather.tempMax,
                      currentWeather.windDir.c_str(),
                      currentWeather.windSpeed);

          u8g2.setCursor(0, 26);
          u8g2.printf("%s %.1f-%.1f°C %s风%.1fm/s",
                      forecast[0].condition.c_str(),
                      forecast[0].tempMin,
                      forecast[0].tempMax,
                      forecast[0].windDir.c_str(),
                      forecast[0].windSpeed);

          u8g2.setCursor(0, 38);
          u8g2.printf("%s %.1f-%.1f°C %s风%.1fm/s",
                      forecast[1].condition.c_str(),
                      forecast[1].tempMin,
                      forecast[1].tempMax,
                      forecast[1].windDir.c_str(),
                      forecast[1].windSpeed);

          u8g2.setCursor(0, 50);
          u8g2.printf("%s %.1f-%.1f°C %s风%.1fm/s",
                      forecast[2].condition.c_str(),
                      forecast[2].tempMin,
                      forecast[2].tempMax,
                      forecast[2].windDir.c_str(),
                      forecast[2].windSpeed);

          break;
        }
    }
    u8g2.sendBuffer();

    // 天气每5分钟更新一次
    static unsigned long lastWeatherUpdate = 0;
    if (millis() - lastWeatherUpdate >= 5 * 60 * 1000) {
      fetchWeatherData();
      lastWeatherUpdate = millis();
    }
  }
}

void audio_showstreamtitle(const char* info) {
  String metaData = info;
  int sepIndex = metaData.indexOf(" - ");
  currentArtist = (sepIndex != -1) ? metaData.substring(0, sepIndex) : "未知歌手";
  currentSong = (sepIndex != -1) ? metaData.substring(sepIndex + 3) : metaData;

  u8g2.setFont(u8g2_font_wqy12_t_gb2312b);

  // 处理歌名
  String songPrefix = "歌名: " + currentSong;
  int songWidth = u8g2.getUTF8Width(songPrefix.c_str());
  songIsLong = (songWidth > MAX_DISPLAY_WIDTH);
  if (songIsLong) {
    String tempSong = currentSong;
    int lastSpace = -1;
    int currentWidth = u8g2.getUTF8Width("歌名: ");  // 前缀宽度

    for (int i = tempSong.length() - 1; i >= 0; i--) {
      if (tempSong[i] == ' ') {
        String testPart = "歌名: " + tempSong.substring(0, i);
        int testWidth = u8g2.getUTF8Width(testPart.c_str());
        if (testWidth <= MAX_DISPLAY_WIDTH) {
          lastSpace = i;
          break;
        }
      }
    }

    if (lastSpace != -1) {
      songPart1 = currentSong.substring(0, lastSpace);
      songPart2 = currentSong.substring(lastSpace + 1);
    } else {
      int halfLength = currentSong.length() / 2;
      songPart1 = currentSong.substring(0, halfLength);
      songPart2 = currentSong.substring(halfLength);
    }
  } else {
    songPart1 = currentSong;
    songPart2 = "";
  }

  // 处理歌手名
  String artistPrefix = "歌手: " + currentArtist;
  int artistWidth = u8g2.getUTF8Width(artistPrefix.c_str());
  artistIsLong = (artistWidth > MAX_DISPLAY_WIDTH);
  if (artistIsLong) {
    String tempArtist = currentArtist;
    int lastSpace = -1;
    int currentWidth = u8g2.getUTF8Width("歌手: ");  // 前缀宽度

    for (int i = tempArtist.length() - 1; i >= 0; i--) {
      if (tempArtist[i] == ' ') {
        String testPart = "歌手: " + tempArtist.substring(0, i);
        int testWidth = u8g2.getUTF8Width(testPart.c_str());
        if (testWidth <= MAX_DISPLAY_WIDTH) {
          lastSpace = i;
          break;
        }
      }
    }

    if (lastSpace != -1) {
      artistPart1 = currentArtist.substring(0, lastSpace);
      artistPart2 = currentArtist.substring(lastSpace + 1);
    } else {
      int halfLength = currentArtist.length() / 2;
      artistPart1 = currentArtist.substring(0, halfLength);
      artistPart2 = currentArtist.substring(halfLength);
    }
  } else {
    artistPart1 = currentArtist;
    artistPart2 = "";
  }

  displayPhase = 0;  // 重置显示阶段
}