#include "Arduino.h"               // Arduino框架的基本库
#include "WiFi.h"                  // WiFi库，用于WiFi连接
#include "Audio.h"                 // 音频库，用于音频播放
#include "AiEsp32RotaryEncoder.h"  // 旋转编码器库，用于旋转编码器输入
#include <U8g2lib.h>               // 用于OLED显示屏的库
#include <SPI.h>                   // SPI通信库
#include <WiFiManager.h>           // WiFi管理库，简化WiFi连接过程
#include <Preferences.h>           // 用于保存设备的配置信息
#include <time.h>                  // 用于获取和显示当前时间

// 常量定义
#define SHOW_TIME_PERIOD 1000  // 每秒刷新一次显示
#define I2S_DOUT 39            // I2S数据输出引脚
#define I2S_BCLK 40            // I2S时钟引脚
#define I2S_LRC 41             // I2S左/右时钟引脚
#define VOLUME_PIN 5           // 音量调节引脚

// 旋转编码器引脚定义
#define ROTARY_ENCODER_A_PIN 16       // 旋转编码器A相引脚
#define ROTARY_ENCODER_B_PIN 17       // 旋转编码器B相引脚
#define ROTARY_ENCODER_BUTTON_PIN 18  // 旋转编码器按钮引脚
#define ROTARY_ENCODER_STEPS 4        // 每步的旋转量

// 定义不同的工作模式
enum Mode { PLAY_MODE,      // 播放模式
            TUNE_MODE,      // 调谐模式
            VOLUME_MODE };  // 音量调节模式

Mode currentMode = PLAY_MODE;            // 当前工作模式初始化为播放模式
unsigned long lastInteraction = 0;       // 上一次交互的时间
const unsigned long modeTimeout = 3000;  // 模式切换的超时时间

Preferences preferences;                                                                                                              // 用于存储用户设置（电台、音量等）
U8G2_ST7565_LM6059_F_4W_SW_SPI u8g2(U8G2_R0, 11, 10, 14, 12, 13);                                                                     // OLED显示屏
AiEsp32RotaryEncoder rotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, -1, ROTARY_ENCODER_STEPS);  // 旋转编码器对象
Audio audio;                                                                                                                          // 音频对象

// 电台信息结构体
struct Station {
  const char* url;   // 电台的流媒体URL
  const char* name;  // 电台名称
  int gain;          // 电台的增益（影响音量）
};

// 电台列表
const Station stations[] = {
  { "http://icecast.ndr.de/ndr/njoy/live/mp3/128/stream.mp3", "n Joy", 1 },
  { "http://icecast.ndr.de/ndr/ndr2/niedersachsen/mp3/128/stream.mp3", "NDR 2", 1 },
  { "https://icecast.ndr.de/ndr/ndr1niedersachsen/oldenburg/mp3/128/stream.mp3", "NDR Niedersachsen", 1 },
  { "http://stream.ffn.de/ffn/mp3-192/stream.mp3", "FFN", 1 },
  { "https://lhttp.qtfm.cn/live/1275/64k.mp3", "珠海交通", 1 },
  { "https://lhttp.qtfm.cn/live/1274/64k.mp3", "珠海先锋", 1 },
  { "http://live.bjradio.com:8000/bjfm91.mp3", "北京音乐广播", 1 },
  { "http://www.eastday.com/efm8.m3u", "上海东方广播", 1 },
  { "http://live.gzyfm.com:8000/guangzhou_news.mp3", "广州电台新闻频道", 1 },
  { "http://tianjin.radiostream.migu.cn/stream/1034.m3u", "天津广播音乐频道", 1 },
  { "http://live.zzradio.com:8000/zzradio_news.m3u", "郑州电台新闻综合广播", 1 },
  { "http://zzfm.music.com:8000/zzfm_music.m3u", "郑州电台音乐广播", 1 },
  { "https://lhttp.qingting.fm/live/1223/64k.mp3", "郑州怀旧音乐", 1 },
  { "https://live.bjradio.com:8000/bjfm91.mp3", "北京文艺广播", 1 },
  { "https://www.eastday.com/efm8.m3u", "上海文艺广播", 1 },
  { "https://live.gzyfm.com:8000/guangzhou_news.mp3", "广州文艺广播", 1 },
  { "https://www.szrt.com:8000/szrt.m3u", "深圳文艺广播", 1 },
  { "https://www.chengdufm.com/live/cdfm5.m3u", "成都文艺广播", 1 },
  { "https://live.bjradio.com:8000/bjfm93.mp3", "书香北京", 1 },
  { "https://tianjin.radiostream.migu.cn/stream/1034.m3u", "天津文学广播", 1 },
  { "https://hz1.migu.cn/stream/hunan.fm1.m3u", "湖南文艺广播", 1 },
  { "https://www.zjradio.com:8000/zzfm.m3u", "浙江文艺广播", 1 },
  { "https://live.kmradio.com:8000/kmradio.m3u", "云南文艺广播", 1 } 
  { "https://live.bjradio.com:8000/bjfm91.mp3", "北京广播综合频道", 1 },
  { "https://www.eastday.com/efm8.m3u", "上海东方广播电台", 1 },
  { "https://live.gzyfm.com:8000/guangzhou_news.mp3", "广州娱乐广播", 1 },
  { "https://tianjin.radiostream.migu.cn/stream/1034.m3u", "天津广播娱乐频道", 1 },
  { "https://61.139.93.67:8000/chongqing_traffic.m3u", "重庆娱乐广播", 1 },
  { "https://hz1.migu.cn/stream/hunan.fm1.m3u", "湖南娱乐广播", 1 },
  { "https://www.chengdufm.com/live/cdfm5.m3u", "成都娱乐广播", 1 },
  { "https://live.zzradio.com:8000/zzradio_news.m3u", "郑州娱乐广播", 1 },
  { "https://www.xafm.cn:8000/xa_music.m3u", "西安娱乐广播", 1 },
  { "https://61.144.226.186:8000/shenyang_radio.mp3", "沈阳娱乐广播", 1 }


};

const int NUM_STATIONS = sizeof(stations) / sizeof(stations[0]);  // 电台数量
int currentStation = 0;                                           // 当前电台索引
int browseIndex = 0;                                              // 当前浏览电台的索引
String currentSong = "未知歌曲";                                  // 当前歌曲
String currentArtist = "未知歌手";                                // 当前歌手

// 旋转编码器的中断服务程序，用于读取旋转编码器
void IRAM_ATTR readEncoderISR() {
  rotaryEncoder.readEncoder_ISR();  // 读取编码器状态
}

// 元数据回调函数，用于解析歌曲和歌手信息
void metadataCallback(const char* type, const char* value) {
  if (strcmp(type, "Title") == 0) {
    String metaData = String(value);  // 获取歌曲标题
    int sepIndex = metaData.indexOf(" - ");
    if (sepIndex != -1) {
      currentArtist = metaData.substring(0, sepIndex);  // 提取歌手
      currentSong = metaData.substring(sepIndex + 3);   // 提取歌曲
    } else {
      currentArtist = "未知歌手";
      currentSong = metaData;
    }
  }
}

// 设置函数，初始化硬件和WiFi连接
void setup() {
  Serial.begin(115200);                                  // 初始化串口
  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");  // 配置NTP时间服务器

  rotaryEncoder.begin();                                   // 初始化旋转编码器
  rotaryEncoder.setup(readEncoderISR);                     // 设置中断服务函数
  rotaryEncoder.setBoundaries(0, NUM_STATIONS - 1, true);  // 设置电台浏览的边界

  u8g2.begin();            // 初始化显示屏
  u8g2.enableUTF8Print();  // 启用UTF-8打印支持

  WiFiManager wm;                   // 创建WiFi管理对象
  wm.autoConnect("ESP Radio", "");  // 自动连接WiFi，默认使用"ESP Radio"作为热点

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);           // 设置音频输出引脚
  preferences.begin("radio", false);                      // 打开偏好设置，读取上次保存的电台和音量
  currentStation = preferences.getInt("lastStation", 0);  // 获取上次保存的电台
  audio.setVolume(preferences.getInt("lastVolume", 2));   // 设置上次保存的音量
  connectToStation(currentStation);                       // 连接到上次保存的电台
}

// 主循环函数，持续处理音频播放、用户输入和显示更新
void loop() {
  audio.loop();       // 处理音频播放
  handleUserInput();  // 处理用户输入（旋转编码器操作）
  updateDisplay();    // 更新显示
}

// 用户输入处理函数
void handleUserInput() {
  if (rotaryEncoder.encoderChanged()) {                         // 如果旋转编码器的值发生变化
    lastInteraction = millis();                                 // 更新上次交互时间
    static int lastEncoderValue = rotaryEncoder.readEncoder();  // 记录上次的编码器值
    int newEncoderValue = rotaryEncoder.readEncoder();          // 获取新的编码器值
    int change = newEncoderValue - lastEncoderValue;            // 计算旋转的变化量

    if (change != 0) {  // 如果旋转了
      lastEncoderValue = newEncoderValue;

      // 根据当前模式处理旋转编码器的值
      if (currentMode == PLAY_MODE) {
        currentMode = TUNE_MODE;  // 切换到调谐模式
      } else if (currentMode == TUNE_MODE) {
        browseIndex += (change > 0 ? 1 : -1);  // 根据旋转方向浏览电台
        // 循环模式
        if (browseIndex < 0) {
          browseIndex = NUM_STATIONS - 1;
        } else if (browseIndex >= NUM_STATIONS) {
          browseIndex = 0;
        }
      } else if (currentMode == VOLUME_MODE) {
        int volume = audio.getVolume() + (change > 0 ? 1 : -1);  // 调节音量
        volume = constrain(volume, 0, 21);                       // 限制音量范围
        audio.setVolume(volume);                                 // 设置新的音量
        preferences.putInt("lastVolume", volume);                // 保存音量
      }
    }
  }

  if (rotaryEncoder.isEncoderButtonClicked()) {  // 如果按钮被点击
    lastInteraction = millis();                  // 更新上次交互时间
    if (currentMode == PLAY_MODE) {
      currentMode = VOLUME_MODE;  // 切换到音量调节模式
    } else if (currentMode == TUNE_MODE) {
      connectToStation(browseIndex);  // 连接到选择的电台
      currentMode = PLAY_MODE;        // 切换回播放模式
    } else if (currentMode == VOLUME_MODE) {
      currentMode = PLAY_MODE;  // 切换回播放模式
    }
  }

  // 如果长时间没有交互，自动切换回播放模式
  if (millis() - lastInteraction > modeTimeout && currentMode != PLAY_MODE) {
    currentMode = PLAY_MODE;
  }
}

// 更新显示内容的函数
void updateDisplay() {
  static int last = 0;
  if (millis() - last < SHOW_TIME_PERIOD) return;  // 限制更新频率
  last = millis();

  u8g2.clearBuffer();  // 清屏，防止显示重叠

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {  // 获取当前时间
    char dateStr[32], timeStr[16], volumeStr[8];
    const char* weekDays[] = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };
    snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d %s",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, weekDays[timeinfo.tm_wday]);
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    snprintf(volumeStr, sizeof(volumeStr), "V: %d", audio.getVolume());  // 获取音量

    u8g2.setFont(u8g2_font_wqy12_t_gb2312b);  // 设置中文字体

    if (currentMode != TUNE_MODE) {
      u8g2.setCursor(16, 11);
      u8g2.print(dateStr);  // 显示日期
      u8g2.setCursor(20, 24);
      u8g2.print(timeStr);  // 显示时间
      u8g2.setCursor(98, 24);
      u8g2.print(volumeStr);  // 显示音量
    }

    if (currentMode == PLAY_MODE) {  // 在播放模式下显示电台信息和歌曲信息
      u8g2.setCursor(0, 38);
      u8g2.printf("第%d台: %s", currentStation, stations[currentStation].name);
      u8g2.setCursor(0, 50);
      u8g2.printf("歌曲: %s", currentSong.c_str());
      u8g2.setCursor(0, 62);
      u8g2.printf("歌手: %s", currentArtist.c_str());
    } else if (currentMode == TUNE_MODE) {  // 在调谐模式下显示电台列表
      int startY = 11;
      int halfWindow = 2;
      int displayCount = min(5, NUM_STATIONS);  // 显示最多5个电台
      for (int i = 0; i < displayCount; i++) {
        int displayIndex = browseIndex - halfWindow + i;
        if (displayIndex < 0) {
          displayIndex += NUM_STATIONS;
        } else if (displayIndex >= NUM_STATIONS) {
          displayIndex -= NUM_STATIONS;
        }
        u8g2.setCursor(10, startY + i * 13);
        if (displayIndex == browseIndex) u8g2.print(">> ");
        u8g2.print(stations[displayIndex].name);  // 显示电台名称
      }
    } else if (currentMode == VOLUME_MODE) {  // 在音量调节模式下显示音量
      u8g2.setCursor(30, 38);
      u8g2.printf("调节音量: %d", audio.getVolume());
    }

    u8g2.sendBuffer();  // 更新显示屏
  }
}

// 连接到指定电台并设置音量的函数
void connectToStation(int stationIndex) {
  // 获取当前音量和增益
  int currentVolume = audio.getVolume();
  int currentGain = stations[currentStation].gain;
  int newGain = stations[stationIndex].gain;

  // 计算新的音量
  int newVolume = currentVolume - currentGain + newGain;
  newVolume = constrain(newVolume, 0, 21);  // 限制音量范围

  // 更新当前电台索引并保存电台设置
  currentStation = stationIndex;
  audio.setVolume(newVolume);
  preferences.putInt("lastStation", stationIndex);
  preferences.putInt("lastVolume", newVolume);

  // 连接到新的电台
  audio.connecttohost(stations[stationIndex].url);
}

// 用于显示电台元数据（如歌曲和歌手信息）
void audio_showstreamtitle(const char* info) {
  // 假设元数据格式是 "歌手 - 歌曲"
  String metaData = String(info);
  int sepIndex = metaData.indexOf(" - ");
  if (sepIndex != -1) {
    currentArtist = metaData.substring(0, sepIndex);  // 提取歌手
    currentSong = metaData.substring(sepIndex + 3);   // 提取歌曲
  } else {
    currentArtist = "未知歌手";
    currentSong = metaData;
  }
}
