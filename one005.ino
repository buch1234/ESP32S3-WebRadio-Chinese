#include "Arduino.h"
#include "WiFi.h"
#include "Audio.h"
#include "AiEsp32RotaryEncoder.h"
#include <U8g2lib.h>
#include <SPI.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>

#define SHOW_TIME_PERIOD 1000
#define I2S_DOUT 39
#define I2S_BCLK 40
#define I2S_LRC 41
#define VOLUME_PIN 5

#define ROTARY_ENCODER_A_PIN 16
#define ROTARY_ENCODER_B_PIN 17
#define ROTARY_ENCODER_BUTTON_PIN 18
#define ROTARY_ENCODER_STEPS 4

enum Mode { PLAY_MODE,
            TUNE_MODE,
            VOLUME_MODE };
Mode currentMode = PLAY_MODE;
unsigned long lastInteraction = 0;
const unsigned long modeTimeout = 3000;

Preferences preferences;
U8G2_ST7565_LM6059_F_4W_SW_SPI u8g2(U8G2_R0, 11, 10, 14, 12, 13);
AiEsp32RotaryEncoder rotaryEncoder(ROTARY_ENCODER_A_PIN, ROTARY_ENCODER_B_PIN, ROTARY_ENCODER_BUTTON_PIN, -1, ROTARY_ENCODER_STEPS);
Audio audio;

struct Station {
  const char* url;
  const char* name;
  int gain;  // 电台增益
};

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
  { "http://hz1.migu.cn/stream/hunan.fm1.m3u", "湖南交通广播", 1 },
  { "http://www.chengdufm.com/live/cdfm5.m3u", "成都电台音乐广播", 1 },
  { "http://tianjin.radiostream.migu.cn/stream/1034.m3u", "天津广播音乐频道", 1 },
  { "http://113.105.115.17:8000/njfm1.mp3", "南京电台交通频道", 1 },
  { "http://61.139.93.67:8000/chongqing_traffic.m3u", "重庆交通广播", 1 },
  { "http://live.wuhanradio.com:8000/wuhanfm.m3u", "武汉交通广播", 1 },
  { "http://61.144.226.186:8000/shenyang_radio.mp3", "沈阳人民广播电台", 1 },
  { "http://live.zzradio.com:8000/zzradio_news.m3u", "郑州电台新闻综合广播", 1 },
  { "http://www.szrt.com:8000/suzhou_fm.m3u", "苏州广播电视台", 1 },
  { "http://live.xmradio.cn:8000/xmfm_music.m3u", "厦门电台音乐广播", 1 },
  { "http://www.qdradio.com:8000/qingdao_radio.m3u", "青岛电台", 1 },
  { "http://live.jnradio.com:8000/jn_radio.m3u", "济南人民广播电台", 1 },
  { "http://hbdx.hljradio.com:8000/hbdxfm1.mp3", "哈尔滨电台", 1 },
  { "http://live.fzradio.com:8000/fzfm1.mp3", "福州电台音乐广播", 1 },
  { "http://www.hfmm.cn:8000/hf_radio.m3u", "合肥交通广播", 1 },
  { "http://live.lzradio.com:8000/lzfm1.mp3", "兰州人民广播电台", 1 },
  { "http://live.nc.radio.cn:8000/ncfm1.mp3", "南昌人民广播电台", 1 },
  { "http://t.yt.sxcn.cn:8000/taiyuan_news.m3u", "太原电台新闻频道", 1 },
  { "http://www.syfm.cn:8000/trafficfm.mp3", "沈阳交通广播", 1 },
  { "http://www.xafm.cn:8000/xa_music.m3u", "西安电台音乐广播", 1 },
  { "http://live.kmradio.com:8000/kmradio.m3u", "昆明人民广播电台", 1 },
  { "http://zzfm.music.com:8000/zzfm_music.m3u", "郑州电台音乐广播", 1 },
  { "http://live.gyfm.cn:8000/gyfm_news.mp3", "贵阳电台新闻频道", 1 },
  { "http://live.qhfm.cn:8000/qhfmmusic.m3u", "青海电台音乐广播", 1 },
  { "http://live.jlradio.com:8000/jl_radio.m3u", "吉林人民广播电台", 1 },
  { "http://imgradio.cdn.nmgnews.com:8000/im_radio_news.mp3", "内蒙古电台新闻广播", 1 },
  { "http://dlfm.cdn.dlradio.com:8000/dlfm.m3u", "大连电台", 1 },
  { "http://www.qd12345.com:8000/qd_radio.m3u", "青岛人民广播电台", 1 },
  { "http://live.czfm.com:8000/czfm_music.m3u", "常州电台音乐广播", 1 },
  { "http://yzradio.cdn.yzfm.com:8000/yzfm.m3u", "扬州电台", 1 },
  { "http://nnfm.cdn.nannradio.com:8000/nnfm_radio.m3u", "南宁电台", 1 },
  { "http://xmradio.xm.com:8000/xmfm_news.m3u", "厦门电台新闻广播", 1 },
  { "http://live.hrbfm.com:8000/hrbfm_music.m3u", "哈尔滨电台音乐广播", 1 },
  { "https://lhttp.qingting.fm/live/1223/64k.mp3", "郑州怀旧音乐", 1 },

};
const int NUM_STATIONS = sizeof(stations) / sizeof(stations[0]);
int currentStation = 0;
int browseIndex = 0;
String currentSong = "未知歌曲";
String currentArtist = "未知歌手";

void IRAM_ATTR readEncoderISR() {
  rotaryEncoder.readEncoder_ISR();
}

void metadataCallback(const char* type, const char* value) {
  if (strcmp(type, "Title") == 0) {
    String metaData = String(value);
    int sepIndex = metaData.indexOf(" - ");
    if (sepIndex != -1) {
      currentArtist = metaData.substring(0, sepIndex);
      currentSong = metaData.substring(sepIndex + 3);
    } else {
      currentArtist = "未知歌手";
      currentSong = metaData;
    }
  }
}

void setup() {
  Serial.begin(115200);
  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");

  rotaryEncoder.begin();
  rotaryEncoder.setup(readEncoderISR);
  rotaryEncoder.setBoundaries(0, NUM_STATIONS - 1, true);

  u8g2.begin();
  u8g2.enableUTF8Print();

  WiFiManager wm;
  wm.autoConnect("ESP Radio", "");

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  preferences.begin("radio", false);
  currentStation = preferences.getInt("lastStation", 0);
  audio.setVolume(preferences.getInt("lastVolume", 2));
  connectToStation(currentStation);
}

void loop() {
  audio.loop();
  handleUserInput();
  updateDisplay();
}

void handleUserInput() {
  if (rotaryEncoder.encoderChanged()) {
    lastInteraction = millis();
    static int lastEncoderValue = rotaryEncoder.readEncoder();
    int newEncoderValue = rotaryEncoder.readEncoder();
    int change = newEncoderValue - lastEncoderValue;

    if (change != 0) {
      lastEncoderValue = newEncoderValue;

      if (currentMode == PLAY_MODE) {
        currentMode = TUNE_MODE;
      } else if (currentMode == TUNE_MODE) {
        browseIndex += (change > 0 ? 1 : -1);

        // **修改为循环模式**
        if (browseIndex < 0) {
          browseIndex = NUM_STATIONS - 1;
        } else if (browseIndex >= NUM_STATIONS) {
          browseIndex = 0;
        }
      } else if (currentMode == VOLUME_MODE) {
        int volume = audio.getVolume() + (change > 0 ? 1 : -1);
        volume = constrain(volume, 0, 21);
        audio.setVolume(volume);
        preferences.putInt("lastVolume", volume);
      }
    }
  }

  if (rotaryEncoder.isEncoderButtonClicked()) {
    lastInteraction = millis();
    if (currentMode == PLAY_MODE) {
      currentMode = VOLUME_MODE;
    } else if (currentMode == TUNE_MODE) {
      connectToStation(browseIndex);
      currentMode = PLAY_MODE;
    } else if (currentMode == VOLUME_MODE) {
      currentMode = PLAY_MODE;
    }
  }

  if (millis() - lastInteraction > modeTimeout && currentMode != PLAY_MODE) {
    currentMode = PLAY_MODE;
  }
}

void updateDisplay() {
  static int last = 0;
  if (millis() - last < SHOW_TIME_PERIOD) return;
  last = millis();

  u8g2.clearBuffer();  // 清屏，防止重叠

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char dateStr[32], timeStr[16], volumeStr[8];
    const char* weekDays[] = { "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六" };
    snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d %s",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, weekDays[timeinfo.tm_wday]);
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    snprintf(volumeStr, sizeof(volumeStr), "V: %d", audio.getVolume());

    u8g2.setFont(u8g2_font_wqy12_t_gb2312b);

    if (currentMode != TUNE_MODE) {
      u8g2.setCursor(16, 11);
      u8g2.print(dateStr);
      u8g2.setCursor(20, 24);
      u8g2.print(timeStr);
      u8g2.setCursor(98, 24);
      u8g2.print(volumeStr);
    }

    if (currentMode == PLAY_MODE) {
      u8g2.setCursor(0, 38);
      u8g2.printf("第%d台: %s", currentStation, stations[currentStation].name);

      u8g2.setCursor(0, 50);
      u8g2.printf("歌曲: %s", currentSong.c_str());
      u8g2.setCursor(0, 62);
      u8g2.printf("歌手: %s", currentArtist.c_str());
    } else if (currentMode == TUNE_MODE) {
      int startY = 11;
      int halfWindow = 2;                       // 显示窗口的一半
      int displayCount = min(5, NUM_STATIONS);  // 确保不会超出可用电台数量

      for (int i = 0; i < displayCount; i++) {
        int displayIndex = browseIndex - halfWindow + i;

        // 让列表循环滚动而不会在边界处重复
        if (displayIndex < 0) {
          displayIndex += NUM_STATIONS;
        } else if (displayIndex >= NUM_STATIONS) {
          displayIndex -= NUM_STATIONS;
        }

        u8g2.setCursor(10, startY + i * 13);
        if (displayIndex == browseIndex) u8g2.print(">> ");
        u8g2.print(stations[displayIndex].name);
      }
    } else if (currentMode == VOLUME_MODE) {
      u8g2.setCursor(30, 38);
      u8g2.printf("调节音量: %d", audio.getVolume());
    }

    u8g2.sendBuffer();  // 发送数据到屏幕
  }
}

// 修改后的连接电台函数，调整音量
void connectToStation(int stationIndex) {
  // 获取当前音量
  int currentVolume = audio.getVolume();

  // 获取当前电台的增益
  int currentGain = stations[currentStation].gain;

  // 获取新电台的增益
  int newGain = stations[stationIndex].gain;

  // 计算新的音量
  int newVolume = currentVolume - currentGain + newGain;

  // 限制音量范围（假设最大音量是 21）
  newVolume = constrain(newVolume, 0, 21);

  // 更新当前电台索引
  currentStation = stationIndex;

  // 设置新的音量
  audio.setVolume(newVolume);

  // 保存当前电台和音量
  preferences.putInt("lastStation", stationIndex);
  preferences.putInt("lastVolume", newVolume);

  // 连接到新电台
  audio.connecttohost(stations[stationIndex].url);
}

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
