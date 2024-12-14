// Copyright (c) 2020 Tencent. All rights reserved.


#ifndef LEB_CONNECTION_API_H_
#define LEB_CONNECTION_API_H_

#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define WEBRTC_EXPORT_API __declspec(dllexport)
#else
#define WEBRTC_EXPORT_API __attribute__((visibility("default")))
#endif

typedef enum LebVideoCodecType {
  kNoVideo = -1,
  kH264,
  kH265,
} LebVideoCodecType;

typedef enum LebAudioCodecType {
  kNoAudio = -1,
  kOpus,
  kAac,
} LebAudioCodecType;

typedef enum LebMetaDataType {
  kAudio,
  kVideo,
  kData,
} LebMetaDataType;

typedef enum LebNetState {
  // 正在连接中
  kConnecting,
  // 连接成功
  kConnected,
  // 当前已经断网，正在尝试重新连接
  kDisconnected,
  // 重连失败，网络不可恢复
  kConnectFailed,
  // 客户主动关闭连接
  kConnectClosed,
} LebNetState;

typedef enum LebLogLevel {
  kDebug,
  kInfo,
  kWarning,
  kError,
} LebLogLevel;

typedef enum LebSdpType {
  kOffer,
  kAnswer,
} LebSdpType;

typedef struct LebSdpInfo {
  LebSdpType type;
  const char* sdp;
} LebSdpInfo;

typedef struct LebVideoInfo {
  LebVideoCodecType codec_type;
  uint8_t extra_data[4096];
  size_t extra_size;
} LebVideoInfo;

typedef struct LebAudioInfo {
  LebAudioCodecType codec_type;
  int32_t sample_rate;
  int32_t num_channels;
} LebAudioInfo;

typedef struct LebEncodedVideoFrame {
  LebVideoCodecType codec_type;
  const uint8_t* data;
  size_t size;
  int64_t pts;
  int64_t dts;
  int32_t output_delay_ms;
} LebEncodedVideoFrame;

typedef struct LebEncodedAudioFrame {
  LebAudioCodecType codec_type;
  const uint8_t* data;
  size_t size;
  int64_t dts;
  int64_t pts;
  int32_t sample_rate;
  int32_t num_channels;
  int32_t output_delay_ms;
} LebEncodedAudioFrame;

typedef struct LebMetaData {
  const uint8_t* data;
  size_t size;
  LebMetaDataType type;
} LebMetaData;

typedef struct LebStats {
  // network
  int32_t rtt_ms;
  // 信令服务器 IP
  const char* signal_server_ip;
  // 信令服务器域名解析耗时
  int32_t signal_dns_cost_ms;
  // 数据服务器 IP
  const char* data_server_ip;
  // 从开始拉流到收到首个视频帧的时间
  int32_t first_video_received_cost_ms;
  // 从开始拉流到收到首个音频帧的时间
  int32_t first_audio_received_cost_ms;

  // video
  uint32_t video_packets_received;
  uint64_t video_bytes_received;
  int32_t video_packets_lost;
  uint32_t video_nack_count;

  // audio
  uint32_t audio_packets_received;
  uint64_t audio_bytes_received;
  int32_t audio_packets_lost;
  uint32_t audio_nack_count;
} LebStats;

typedef void (*OnLogInfo)(void* context, const char* tag, LebLogLevel, const char* message);
typedef void (*OnSdpInfo)(void* context, LebSdpInfo info);
typedef void (*OnVideoInfo)(void* context, LebVideoInfo info);
typedef void (*OnAudioInfo)(void* context, LebAudioInfo info);
typedef void (*OnEncodedVideo)(void* context, LebEncodedVideoFrame video_frame);
typedef void (*OnEncodedAudio)(void* context, LebEncodedAudioFrame audio_frame);
typedef void (*OnMetaData)(void* context, LebMetaData data);
typedef void (*OnStatsInfo)(void* context, LebStats stats);

typedef struct LebConfig {
    // 拉流地址，如: "webrtc://5664.liveplay.myqcloud.com/live/5664_harchar1"，给内部udp信令用
    const char* stream_url;

    // 远程ip或域名，默认为"webrtc.liveplay.myqcloud.com"，给内部udp信令用
    const char* remote_address;
  
    // 是否开启udp信令，udp信令内部执行对外透明，关闭时外部信令交互
    int enable_udp_signal;

    // 视频开关
    int receive_video;

    // 音频开关
    int receive_audio;

    // 加密开关
    int enable_encryption;

    // 是否开启 AAC，开启时CDN下发原始流，否则默认音频转码成Opus
    int enable_aac;

    // Flex FEC 开关
    int enable_flex_fec;

    // 统计回调周期，内部默认5秒
    int stats_period_ms;
} LebConfig;

typedef struct LebConnectionHandle {
    void* context;
    void* internal_handle;
    LebConfig config;

    OnSdpInfo onOfferCreated;//only used for http signal
    OnVideoInfo onVideoInfo;
    OnAudioInfo onAudioInfo;
    OnEncodedVideo onVideoData;
    OnEncodedAudio onAudioData;
    OnMetaData  onMetaData;
    OnStatsInfo onStatsInfo;
    OnLogInfo onLogInfo;
} LebConnectionHandle;

// 创建快直播连接
WEBRTC_EXPORT_API LebConnectionHandle* OpenLebConnection(void* context, LebLogLevel loglevel);

// 开始连接，内部完成信令后，直接建联拉流
WEBRTC_EXPORT_API void StartLebConnection(LebConnectionHandle* handle, LebConfig config);

// 开始信令，回调onOfferCreated输出offer sdp，通过http交互获取answer sdp
WEBRTC_EXPORT_API void CreateOffer(LebConnectionHandle* handle, LebConfig config);

// 设置获取的answer sdp，开始建联拉流
WEBRTC_EXPORT_API void SetRemoteSDP(LebConnectionHandle* handle, LebSdpInfo info);

// 停止连接
WEBRTC_EXPORT_API void StopLebConnection(LebConnectionHandle* handle);

// 关闭连接
WEBRTC_EXPORT_API void CloseLebConnection(LebConnectionHandle* handle);

// 播放过程中查询统计数据，onStatsInfo异步回调输出
WEBRTC_EXPORT_API void GetStats(LebConnectionHandle* handle);

// 注册日志回调函数
WEBRTC_EXPORT_API void RegisterLogInfoCallback(LebConnectionHandle* handle, OnLogInfo callback);
// 注册sdp回调函数，获取offer sdp
WEBRTC_EXPORT_API void RegisterSdpInfoCallback(LebConnectionHandle* handle, OnSdpInfo callback);
// 注册视频信息回调函数，获取视频信息
WEBRTC_EXPORT_API void RegisterVideoInfoCallback(LebConnectionHandle* handle, OnVideoInfo callback);
// 注册音频信息回调函数，获取音频信息
WEBRTC_EXPORT_API void RegisterAudioInfoCallback(LebConnectionHandle* handle, OnAudioInfo callback);
// 注册视频数据回调函数，获取视频帧裸数据
WEBRTC_EXPORT_API void RegisterVideoDataCallback(LebConnectionHandle* handle, OnEncodedVideo callback);
// 注册音频数据回调函数，获取音频帧裸数据
WEBRTC_EXPORT_API void RegisterAudioDataCallback(LebConnectionHandle* handle, OnEncodedAudio callback);
// 注册MetaData回调函数，获取MetaData数据
WEBRTC_EXPORT_API void RegisterMetaDataCallback(LebConnectionHandle* handle, OnMetaData callback);
// 注册统计回调函数，获取统计数据
WEBRTC_EXPORT_API void RegisterStatsInfoCallback(LebConnectionHandle* handle, OnStatsInfo callback);

#ifdef __cplusplus
}
#endif

#endif  // LEB_CONNECTION_API_H_
