/*当使用八线 PSRAM 时，GPIO33~37 会连接到 SPIIO4 ~ SPIIO7 和 SPIDQS。因此，GPIO33~37 也不可用于其他用途
定义输出 I2S_DOUT 39   I2S_BCLK 40  I2S_LRC 41
开发板 esp32 2.0.15,  
必要的库：
ESP32-aduioii2s/master by schreibfaul1 2.0.0,  
Ai_Esp32_Rotary_Encoder by igor Antolic 1,7  ,
U8g2 bei Oliver 2.35.30 ,
WiFiManager by tzapu 2.0.17

02.20205 in Geeste Germany
*/
#include "Arduino.h"
#include "WiFi.h"
#include "Audio.h"
#include "AiEsp32RotaryEncoder.h"
#include <U8g2lib.h>
#include <SPI.h>
#include <WiFiManager.h>
#include <Preferences.h>  // 引入Preferences库
#include <time.h>         // 引入time.h库用于时间管理

// 显示更新时间的周期（毫秒）
#define SHOW_TIME_PERIOD 1000

// I2S接口的引脚定义
#define I2S_DOUT 39   // I2S数据输出引脚
#define I2S_BCLK 40   // I2S时钟引脚
#define I2S_LRC 41    // I2S左右声道选择引脚
#define VOLUME_PIN 5  // 音量电位器读取引脚

// 旋转编码器引脚定义
#define ROTARY_ENCODER_A_PIN 16
#define ROTARY_ENCODER_B_PIN 17
#define ROTARY_ENCODER_BUTTON_PIN 18
#define ROTARY_ENCODER_STEPS 4  // 每个步骤的脉冲数

// OLED显示屏初始化（基于U8g2库）
U8G2_ST7565_LM6059_F_4W_SW_SPI u8g2(U8G2_R0, /* clock=*/11, /* data=*/10, /* cs=*/14, /* dc=*/12, /* reset=*/13);

// 创建旋转编码器实例
AiEsp32RotaryEncoder rotaryEncoder = AiEsp32RotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, -1, ROTARY_ENCODER_STEPS);

// 创建音频处理实例
Audio audio;

// 定义电台信息结构体
struct Station {
  const char *url;   // 电台流媒体URL
  const char *name;  // 电台名称
  int gain;          // 音量增益
};

const Station stations[] = {
  { "http://icecast.ndr.de/ndr/njoy/live/mp3/128/stream.mp3", "n Joy", 1 },
  { "http://icecast.ndr.de/ndr/ndr2/niedersachsen/mp3/128/stream.mp3", "NDR 2", 1 },
  { "https://icecast.ndr.de/ndr/ndr1niedersachsen/oldenburg/mp3/128/stream.mp3", "NDR 1 Niedersachsen", 1 },
  { "http://stream.ffn.de/ffn/mp3-192/stream.mp3", "FFN", 1 },
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

const int NUM_STATIONS = sizeof(stations) / sizeof(stations[0]);  // 电台数量
int currentStation = 0;                                           // 当前播放的电台索引
bool browsingStations = false;                                    // 是否处于浏览模式
int browseIndex = 0;                                              // 浏览电台的索引

// 当前歌曲信息
String currentSong = "未知歌曲";
String currentArtist = "未知歌手";

// 创建 Preferences 实例用于保存数据
Preferences preferences;

// 静音功能相关
bool isMuted = false;  // 是否静音

// 旋转编码器中断服务程序
void IRAM_ATTR readEncoderISR() {
  rotaryEncoder.readEncoder_ISR();
}

void setup() {
  delay(1000);
  Serial.begin(115200);
  pinMode(VOLUME_PIN, INPUT);

  
  // **手动初始化 PSRAM**
  if (psramInit()) {
    Serial.printf("PSRAM 启用: 总大小 %d bytes, 可用 %d bytes\n", ESP.getPsramSize(), ESP.getFreePsram());
  } else {
    Serial.println("PSRAM 初始化失败！");
  }

  // 使用 esp_spiram_init 来初始化 PSRAM
  if (esp_spiram_init() == ESP_OK) {
    Serial.println("PSRAM 初始化成功！");
  } else {
    Serial.println("PSRAM 初始化失败！");
  }

  // 设置时区
  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");  // 配置NTP服务器

  rotaryEncoder.begin();  // 初始化旋转编码器
  rotaryEncoder.setup(readEncoderISR);
  rotaryEncoder.setBoundaries(0, NUM_STATIONS - 1, true);
  rotaryEncoder.setAcceleration(0);

  u8g2.begin();            // 初始化OLED
  u8g2.enableUTF8Print();  // 启用UTF-8打印支持

  WiFiManager wm;  // 创建WiFiManager实例
  heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
  wm.autoConnect("ESP Radio", "");  // 自动连接或创建热点

  Serial.println("\nWiFi connected");
  Serial.println("IP address: " + WiFi.localIP().toString());

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);  // 设置I2S引脚
  audio.setBufsize(64 * 1024, 6 * 1024 * 1024);  // 设置缓冲区大小

  preferences.begin("radio", false);                      // 初始化Preferences
  currentStation = preferences.getInt("lastStation", 0);  // 恢复上次的电台索引
  int lastVolume = preferences.getInt("lastVolume", 2);   // 恢复上次的音量设置
  audio.setVolume(lastVolume);                            // 恢复音量

  connectToStation(currentStation);  // 连接到当前电台
}

// 主循环函数
void loop() {
  audio.loop();  // 处理音频流

  if (browsingStations) {
    browseStationList();  // 浏览电台列表
  } else {
    checkStationChange();  // 检查电台切换
  }

  // 控制音量：通过模拟输入（电位器）读取音量值
  int volumeReading = analogRead(VOLUME_PIN);
  int volume = map(volumeReading, 0, 1023, 0, 21);  // 最大音量为21
  // 获取当前电台的音量增益
  int stationGain = stations[currentStation].gain;

// 如果未静音，设置音量
  if (!isMuted) {
    int totalVolume = volume + stationGain;
    // 限制音量范围 (0 - 21)
    if (totalVolume > 21) {
      totalVolume = 21;
    }
    if (totalVolume < 0) {
      totalVolume = 0;
    }
    audio.setVolume(totalVolume);  // 设置音量
  } else {
    // 如果是静音状态，音量设置为 0
    audio.setVolume(0);
  }

  static int last = 0;
  if ((millis() - last) >= SHOW_TIME_PERIOD) {
    last = millis();
    u8g2.clearBuffer();
    //
    Serial.printf("Heap: Free %d bytes, Min Free %d bytes\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
    Serial.printf("PSRAM: Free %d bytes\n", ESP.getFreePsram());


    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      // 第一行：日期和星期
      char dateStr[32];
      strftime(dateStr, sizeof(dateStr), "%Y-%m-%d %a", &timeinfo);
      u8g2.setFont(u8g2_font_wqy12_t_gb2312b);
      u8g2.setCursor((128 - u8g2.getStrWidth(dateStr)) / 2, 11);  // 居中显示
      u8g2.print(dateStr);

      // 第二行：时间和音量
      char timeStr[16];
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      u8g2.setCursor(20, 24);  // 靠左显示时间
      u8g2.print(timeStr);

      // 显示当前音量（右对齐）
      char volumeStr[8];
      snprintf(volumeStr, sizeof(volumeStr), "V: %d", audio.getVolume());
      u8g2.setCursor(98, 24);  // 靠右显示音量
      u8g2.print(volumeStr);

      // 第三行：电台编号和名称
      u8g2.setCursor(0, 38);
      u8g2.print("第");
      u8g2.print(currentStation);
      u8g2.print("台:");
      u8g2.print(stations[currentStation].name);

      // 第四行：歌曲名称
      u8g2.setCursor(0, 50);
      u8g2.print("歌曲: ");
      u8g2.print(currentSong);

      // 第五行：歌手名称
      u8g2.setCursor(0, 62);
      u8g2.print("歌手: ");
      u8g2.print(currentArtist);
    }

    u8g2.sendBuffer();
  }
}

void audio_showstreamtitle(const char *info) {
  Serial.println("Stream Title Info: ");
  Serial.println(info);

  currentSong = "未知歌曲";
  currentArtist = "未知歌手";

  String streamInfo = String(info);
  int separatorIndex = streamInfo.indexOf(" - ");
  if (separatorIndex > 0) {
    currentArtist = streamInfo.substring(0, separatorIndex);
    currentSong = streamInfo.substring(separatorIndex + 3);
  } else {
    currentSong = streamInfo;
  }

  Serial.println("解析后的歌曲名: " + currentSong);
  Serial.println("解析后的歌手: " + currentArtist);
}
// 检查电台切换
void checkStationChange() {
  if (rotaryEncoder.encoderChanged()) {
    int newStation = rotaryEncoder.readEncoder();
    if (newStation != currentStation) {
      currentStation = newStation;
      connectToStation(currentStation);
    }
  }

  if (rotaryEncoder.isEncoderButtonClicked()) {
    browsingStations = true;
    browseIndex = currentStation;
  }
}
// 浏览电台列表（5行显示），保持当前电台在中间
void browseStationList() {
  if (rotaryEncoder.encoderChanged()) {
    browseIndex = rotaryEncoder.readEncoder();
    // 让电台列表滚动
    if (browseIndex < 0) {
      browseIndex = NUM_STATIONS - 1;
    } else if (browseIndex >= NUM_STATIONS) {
      browseIndex = 0;
    }
  }

  // 计算当前显示的电台索引，确保当前电台在中间
  int startIndex = browseIndex - 2;  // 从当前电台前两个电台开始显示

  // 防止 startIndex 越界
  if (startIndex < 0) {
    startIndex = 0;
  } else if (startIndex + 4 >= NUM_STATIONS) {  // 显示5行，最后一行应该是 NUM_STATIONS - 1
    startIndex = NUM_STATIONS - 5;
  }

  // 手动刷新电台列表显示
  u8g2.clearBuffer();

  for (int i = 0; i < 5; i++) {
    int displayIndex = startIndex + i;
    if (displayIndex >= NUM_STATIONS) {
      break;  // 如果超出了电台的数量，退出循环
    }

    // 高亮显示当前电台
    if (displayIndex == browseIndex) {
      u8g2.setFont(u8g2_font_wqy12_t_gb2312b);;
      u8g2.setCursor(0, 11 + i * 13);
      u8g2.print(">>");  // 高亮标识符
    } else {
      u8g2.setFont(u8g2_font_wqy12_t_gb2312b);
      u8g2.setCursor(12, 11 + i * 13);
    }

    u8g2.print(stations[displayIndex].name);  // 显示电台名称
  }

  u8g2.sendBuffer();

  // 按下旋转编码器按钮时选择电台
  if (rotaryEncoder.isEncoderButtonClicked()) {
    browsingStations = false;
    currentStation = browseIndex;
    connectToStation(currentStation);  // 使用正确的方法连接电台
  }
}

// 连接到指定电台
void connectToStation(int stationIndex) {
  Serial.printf("切换至电台 %d: %s\n", stationIndex, stations[stationIndex].name);
  Serial.printf("URL: %s\n", stations[stationIndex].url);
  Serial.printf("当前音量: %d\n", audio.getVolume());

  audio.connecttohost(stations[stationIndex].url);  // 使用 connecttohost() 连接电台
  preferences.putInt("lastStation", stationIndex);  // 保存当前电台到Preferences
  }
