//启动双核 优化程序。

#include "Arduino.h"
#include "WiFi.h"
#include "Audio.h"
#include "AiEsp32RotaryEncoder.h"
#include <U8g2lib.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>

#define SHOW_TIME_PERIOD 1000         // 每隔1000毫秒更新一次显示
#define I2S_DOUT 39                   // I2S 数据输出引脚
#define I2S_BCLK 40                   // I2S 时钟引脚
#define I2S_LRC 41                    // I2S 左右声道控制引脚
#define VOLUME_PIN 5                  // 音量调节模拟输入引脚
#define ROTARY_ENCODER_A_PIN 16       // 编码器A脚引脚
#define ROTARY_ENCODER_B_PIN 17       // 编码器B脚引脚
#define ROTARY_ENCODER_BUTTON_PIN 18  // 编码器按钮引脚
#define ROTARY_ENCODER_STEPS 4        // 编码器旋转步数

// 设置显示屏和编码器
U8G2_ST7565_LM6059_F_4W_SW_SPI u8g2(U8G2_R0, 11, 10, 14, 12, 13);
AiEsp32RotaryEncoder rotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, -1, ROTARY_ENCODER_STEPS);
Audio audio;
Preferences preferences;

struct Station {
  const char *url;   // 电台URL
  const char *name;  // 电台名称
  int gain;          // 电台音量增益
};

const Station stations[] = {
  { "http://icecast.ndr.de/ndr/njoy/live/mp3/128/stream.mp3", "n Joy", 1 },
  { "https://icecast.ndr.de/ndr/ndr1niedersachsen/oldenburg/mp3/128/stream.mp3", "NDR 1 Niedersachsen", 1 },
  { "http://stream.ffn.de/ffn/mp3-192/stream.mp3", "FFN", 1 },
  { "http://icecast.ndr.de/ndr/ndr2/niedersachsen/mp3/128/stream.mp3", "NDR 2", 1 },
  { "https://lhttp.qtfm.cn/live/1275/64k.mp3", "珠海交通", 1 },
  { "https://lhttp.qtfm.cn/live/1274/64k.mp3", "珠海先锋", 1 },
  { "https://rfichinois96k.ice.infomaniak.ch/rfichinois-96k.mp3", "法国 RFI", 4 },
  { "http://voa-11.akacast.akamaistream.net/7/317/322023/v1/ibb.akacast.akamaistream.net/voa-11.m3u", "美国之音", 6 },
  { "http://rme.stream.dicast.fr:8000/rme-192.mp3", "RME 欧洲", 1 },
  { "https://lhttp.qingting.fm/live/5021912/64k.mp3", "FM 亚洲经典", 1 },
  { "http://lhttp.qingting.fm/live/4915/64k.mp3", "清晨音乐台", 1 },
  { "https://lhttp.qingting.fm/live/5022405/64k.mp3", "Asia FM", 1 },
  { "https://lhttp.qingting.fm/live/5021381/64k.mp3", "北京好音乐", 1 },
  { "http://livestream.1766.today:1769/live1.mp3", "1766 私房音乐", 1 },
  { "https://lhttp.qingting.fm/live/5022038/64k.mp3", "CRI 怀旧金曲", 1 },
  { "http://playerservices.streamtheworld.com/api/livestream-redirect/HAO_963.mp3", "好 FM", 1 },
  { "https://lhttp.qingting.fm/live/1223/64k.mp3", "郑州怀旧音乐", 1 },
  { "https://lhttp.qingting.fm/live/20071/64k.mp3", "香港亚洲天空", 1 },
  { "http://lhttp.qingting.fm/live/274/64k.mp3", "上海动感 101", 1 }
};

const int NUM_STATIONS = sizeof(stations) / sizeof(stations[0]);  // 获取电台数量
int currentStation = 0, browseIndex = 0;                          // 当前电台索引和浏览电台列表的索引
bool browsingStations = false, isMuted = false;                   // 是否在浏览电台，是否静音
String currentSong = "未知歌曲", currentArtist = "未知歌手";      // 当前播放的歌曲和歌手信息

int lastVolume = 5;  // 初始音量设置

void IRAM_ATTR readEncoderISR() {
  rotaryEncoder.readEncoder_ISR();
}

void audioTask(void *pvParameters) {
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);  // 设置音频输出引脚
  while (true) {
    audio.loop();  // 继续播放音频
    delay(10);     // 给其他任务腾出执行时间
  }
}

void controlTask(void *pvParameters) {
  while (true) {
    // 如果正在浏览电台列表，调用浏览电台的函数；否则，检查电台是否发生变化
    if (browsingStations) {
      browseStationList();
    } else {
      checkStationChange();
    }

    // 读取模拟输入的音量值
    int rawVolume = analogRead(VOLUME_PIN);
    // 将模拟输入的值映射到音量范围（0-21）并加上当前电台的增益
    int volume = map(rawVolume, 0, 4095, 0, 21) + stations[currentStation].gain;
    volume = constrain(volume, 0, 21);  // 限制音量范围在0到21之间

    // 如果音量变化较大，才更新音量
    if (abs(volume - lastVolume) > 1) {
      lastVolume = volume;
      audio.setVolume(isMuted ? 0 : volume);  // 根据是否静音来设置音量
    }

    static int last = 0;
    unsigned long currentMillis = millis();
    if (currentMillis - last >= SHOW_TIME_PERIOD) {  // 每隔SHOW_TIME_PERIOD毫秒更新显示
      last = currentMillis;
      updateDisplay();
    }

    delay(10);  // 给音频任务执行的时间
  }
}

void setup() {
  Serial.begin(115200);        // 初始化串口通信
  pinMode(VOLUME_PIN, INPUT);  // 设置音量调节引脚为输入
  //if (psramInit()) Serial.printf("PSRAM 启用: 总大小 %d bytes\n", ESP.getPsramSize());  // 如果启用了PSRAM，打印其大小
  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");  // 配置NTP服务器，用于获取时间

  rotaryEncoder.begin();                                   // 初始化旋转编码器
  rotaryEncoder.setup(readEncoderISR);                     // 设置中断服务程序
  rotaryEncoder.setBoundaries(0, NUM_STATIONS - 1, true);  // 设置电台切换的边界（0到最大电台数量）

  u8g2.begin();  // 初始化显示屏

  // 使用WiFiManager自动连接到Wi-Fi网络
  WiFiManager wifiManager;
  wifiManager.autoConnect("ESP Radio", "");

  // Wi-Fi 连接状态检查
  if (WiFi.status() != WL_CONNECTED) {
    // 如果没有连接Wi-Fi，显示提示信息
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_wqy12_t_gb2312b);  // 设置中文字体
    u8g2.setCursor(10, 20);
    u8g2.print("未连接Wi-Fi!");
    u8g2.sendBuffer();  // 更新显示屏
    while (true) {
      // 这里可以选择加入延迟，或者继续检查Wi-Fi状态
      delay(1000);
    }
  }

  preferences.begin("radio", false);                      // 启用Preferences库，保存电台和音量设置
  currentStation = preferences.getInt("lastStation", 0);  // 获取最后一次播放的电台
  audio.setVolume(preferences.getInt("lastVolume", 2));   // 获取最后一次设置的音量
  connectToStation(currentStation);                       // 连接到最后一次播放的电台

  // 创建两个任务，绑定到核心 0 和核心 1
  xTaskCreatePinnedToCore(audioTask, "Audio Task", 4096, NULL, 1, NULL, 0);      // 音频播放任务，绑定到核心 0
  xTaskCreatePinnedToCore(controlTask, "Control Task", 4096, NULL, 1, NULL, 1);  // 控制任务，绑定到核心 1
}

void loop() {
  // 主循环为空，任务已经通过多核实现
}

void updateDisplay() {
  u8g2.clearBuffer();  // 清空显示缓冲区

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {  // 获取当前时间
    char dateStr[32], timeStr[16], volumeStr[8];

    // 格式化日期： 年-月-日 星期几
    strftime(dateStr, sizeof(dateStr), "%Y年%m月%d日 %a", &timeinfo);
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);  // 格式化时间： 时:分:秒
    snprintf(volumeStr, sizeof(volumeStr), "音量: %d", audio.getVolume());                                     // 获取当前音量并格式化为 "音量: X"

    u8g2.setFont(u8g2_font_wqy12_t_gb2312b);  // 设置中文字体

    // 显示日期： 居中显示
    u8g2.setCursor((128 - u8g2.getStrWidth(dateStr)) / 2, 11);  // 设置日期显示位置
    u8g2.print(dateStr);                                        // 显示日期

    // 显示时间： 在日期下方显示
    u8g2.setCursor(20, 24);  // 设置时间显示位置
    u8g2.print(timeStr);     // 显示时间

    // 显示音量： 在右上角显示
    u8g2.setCursor(98, 24);  // 设置音量显示位置
    u8g2.print(volumeStr);   // 显示音量
  }

  // 更新显示
  u8g2.sendBuffer();
}


void checkStationChange() {
  if (rotaryEncoder.encoderChanged()) {                              // 如果旋转编码器发生变化
    int newStation = rotaryEncoder.readEncoder();                    // 获取新的电台索引
    if (newStation != currentStation) connectToStation(newStation);  // 如果电台发生变化，切换电台
  }
  if (rotaryEncoder.isEncoderButtonClicked()) browsingStations = true;  // 如果按下按钮，进入浏览电台模式
}

void browseStationList() {
  if (rotaryEncoder.encoderChanged()) browseIndex = rotaryEncoder.readEncoder();  // 获取编码器的值，更新浏览电台索引
  browseIndex = (browseIndex + NUM_STATIONS) % NUM_STATIONS;                      // 确保浏览索引在有效范围内

  u8g2.clearBuffer();  // 清空显示缓冲区
  for (int i = 0; i < 5; i++) {
    int displayIndex = (browseIndex - 2 + i + NUM_STATIONS) % NUM_STATIONS;  // 计算显示的电台索引
    u8g2.setCursor(displayIndex == browseIndex ? 0 : 12, 11 + i * 13);       // 设置显示位置
    if (displayIndex == browseIndex) u8g2.print(">>");                       // 如果是当前浏览的电台，显示" >> "
    u8g2.print(stations[displayIndex].name);                                 // 显示电台名称
  }
  u8g2.sendBuffer();  // 更新显示屏

  if (rotaryEncoder.isEncoderButtonClicked()) {  // 如果按下按钮，选择当前浏览的电台
    browsingStations = false;
    connectToStation(browseIndex);  // 切换到选中的电台
  }
}

void connectToStation(int stationIndex) {
  if (!browsingStations) {                                                            // 如果不在浏览电台模式
    Serial.printf("切换至电台 %d: %s\n", stationIndex, stations[stationIndex].name);  // 打印切换电台的信息
    audio.connecttohost(stations[stationIndex].url);                                  // 连接到指定电台
    preferences.putInt("lastStation", stationIndex);                                  // 保存当前电台索引
    currentStation = stationIndex;                                                    // 更新当前电台索引
  }
}
