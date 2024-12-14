// Copyright (c) 2022 Tencent. All rights reserved.


#ifndef LEB_CONNECTION_API_H_
#define LEB_CONNECTION_API_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define LEB_EXPORT_API __declspec(dllexport)
#else
#define LEB_EXPORT_API __attribute__((visibility("default")))
#endif

typedef enum LebVideoCodecType {
  kUnknownVideo = -1,
  kH264,
  kH265,
  kAV1,
  kH266,
} LebVideoCodecType;

typedef enum LebAudioCodecType {
  kUnknownAudio = -1,
  kOpus,
  kAAC,
  kPCM,
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
  kNone,
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
  size_t width;
  size_t height;
  uint8_t extra_data[4096];
  size_t extra_size;
  // 当前abr转码模板，abr未启用时为空字符串
  const char* abr_transcode_name;
} LebVideoInfo;

typedef struct LebAudioInfo {
  LebAudioCodecType codec_type;
  int32_t sample_rate;
  int32_t num_channels;
  uint8_t extra_data[1024];
  size_t extra_size;
} LebAudioInfo;

typedef struct LebEncodedVideoFrame {
  LebVideoCodecType codec_type;
  const uint8_t* data;
  size_t size;
  int64_t pts;
  int64_t dts;
  int32_t is_keyframe;
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
  // 每次拉流的标记，可用于查询前后端日志
  const char* offer_ufrag_pwd;

  // 客户端IP
  const char* client_ip;
  // 信令服务器 IP
  const char* signal_server_ip;
  // 信令服务器域名解析耗时
  int32_t signal_dns_cost_ms;
  // 数据服务器 IP
  const char* data_server_ip;

  // network
  int32_t rtt_ms;
  // 从拉流到收到信令answer的时间
  int32_t signal_answer_cost_ms;
  // 从开始拉流到收到首个视频帧的时间
  int32_t first_video_received_cost_ms;
  // 从开始拉流到收到首个音频帧的时间
  int32_t first_audio_received_cost_ms;

  // video
  uint32_t video_packets_received;
  uint64_t video_bytes_received;
  int32_t video_packets_lost;
  uint32_t video_nack_count;
  uint32_t video_frames_received;
  float video_frame_rate_received;
  uint32_t video_frames_output;
  float video_frame_rate_output;

  // audio
  uint32_t audio_packets_received;
  uint64_t audio_bytes_received;
  int32_t audio_packets_lost;
  uint32_t audio_nack_count;
  uint32_t audio_frames_output;

  // 内部jitterbuffer延时
  int32_t jitter_delay_ms;

  // 全局丢包率，包括音视频、及其重传和fec的所有数据包
  float transport_loss_rate;
} LebStats;

typedef enum LebErrorCode {
  kNone_error = 0,
  // config设置出错
  kConfig_error,
  // remote address解析出错
  kResolve_error,
  // stream url格式出错
  kScheme_error,
  // stream url鉴权失败
  kAuth_error,
  // stream不存在
  kNotFound_error,
  // 未知错误
  kUnknow_error,
  // 信令超时
  kSignal_timeout,
  // 数据超时
  kData_timeout,
  // 收到断流信号
  kReceive_bye,
} LebErrorCode;

typedef void (*OnLogInfo)(void* context, const char* tag, LebLogLevel, const char* message);
typedef void (*OnVideoInfo)(void* context, LebVideoInfo info);
typedef void (*OnAudioInfo)(void* context, LebAudioInfo info);
typedef void (*OnEncodedVideo)(void* context, LebEncodedVideoFrame video_frame);
typedef void (*OnEncodedAudio)(void* context, LebEncodedAudioFrame audio_frame);
typedef void (*OnMetaData)(void* context, LebMetaData data);
typedef void (*OnStatsInfo)(void* context, LebStats stats);
typedef void (*OnError)(void* context, LebErrorCode error);
typedef void (*OnConnectionChange)(void* context, LebNetState state);
typedef int (*OnOpenAudioDecoder)(void* context, LebAudioInfo info);
typedef int (*OnDecodeAudioFrame)(void* context, const uint8_t* in_data, int in_size, uint8_t* out_data);
typedef int (*OnCloseAudioDecoder)(void* context);

typedef struct LebCallback {
  // 日志回调
  OnLogInfo onLogInfo;
  // 视频信息回调
  OnVideoInfo onVideoInfo;
  // 音频信息回调
  OnAudioInfo onAudioInfo;
  // 视频数据回调
  OnEncodedVideo onEncodedVideo;
  // 音频数据回调
  OnEncodedAudio onEncodedAudio;
  // MetaData回调
  OnMetaData onMetaData;
  // 统计信息回调
  OnStatsInfo onStatsInfo;
  // 错误回调
  OnError onError;
  // 连接状态回调
  OnConnectionChange onConnectionChange;
  // 回调打开外部音频解码器
  OnOpenAudioDecoder onOpenAudioDecoder;
  // 回调外部音频解码器解码
  OnDecodeAudioFrame onDecodeAudioFrame;
  // 回调关闭外部音频解码器
  OnCloseAudioDecoder onCloseAudioDecoder;
} LebCallback;

// 用于用户自定义端侧ABR升降档控制算法
typedef int (*AbrDowngradeToNext)(void* context);
typedef int (*AbrDowngradeToLowest)(void* context);
typedef int (*AbrUpgradeToNext)(void* context);

typedef struct LebAbrCallback {
  // 降低一档清晰度的方法
  AbrDowngradeToNext abrDowngradeToNext;
  // 降低到最低档清晰度的方法
  AbrDowngradeToLowest abrDowngradeToLowest;
  // 升高一档清晰度的方法
  AbrUpgradeToNext abrUpgradeToNext;
} LebAbrCallback;

// 最多允许ABR模板为5个
#define MAX_ABR_NUM_ALLOWED  5

typedef struct LebConfig {
    // 码流地址，如: "webrtc://5664.liveplay.myqcloud.com/live/5664_harchar1"
    const char* stream_url;
    // 信令ip或域名，默认为"webrtc-dk.tliveplay.com"
    const char* signal_address;
    // 视频开关
    int receive_video;
    // 音频开关
    int receive_audio;
    // 是否开启 AAC，开启时CDN下发原始流，否则默认音频转码成Opus
    int enable_aac;
    // FlexFEC 开关
    int enable_flex_fec;
    // 统计回调周期，内部默认5秒
    int stats_period_ms;
    // 是否开启音频补包
    int enable_audio_plc;
    // 是否开启0RTT
    int enable_0rtt;
    // 最大允许av pts diff, >0时启用音视频同步输出, <=0时关闭
    int allowed_avsync_diff;
    // 0:关闭abr, 1:abr本地控制, 2:abr服务端控制(要和后台域名配置配合), >0时abr_transcode_names和start_transcode_name才有效
    int abr_mode;
    // abr多码率转码模板，需要用户后台配置转码模板，从高到低最多5个模板，没有配置的设为null
    const char* abr_transcode_names[MAX_ABR_NUM_ALLOWED];
    // abr起始转码模板
    const char* start_transcode_name;
    // 最大jitter delay, 取值范围[1000, 5000], 推荐3000
    int max_jitter_delay_ms;
    // 最小jitter delay, 取值范围[100, 1000], 推荐200
    int min_jitter_delay_ms;
    // 是否启用平滑输出, 启用时max_output_speed和min_output_speed参数生效，否则为尽快输出
    int enable_smoothing_output;
    // 最大输出倍速, 取值范围[1.05, 1.5], 推荐1.2
    float max_output_speed;
    // 最小输出倍速, 取值范围[0.8, 1.0], 推荐0.9
    float min_output_speed;
    // 是否启用内部播控，启用时需设置音频解码器回调接口，音频输出变为PCM
    int enable_play_control;
    // 是否启用minisdp，启用时使用minisdp，否则使用originsdp
    int enable_minisdp;
    // 拉流超时时间(信令或数据超时)
    int open_timeout_ms;
    // 是否启用ipv6优先，否则ipv4优先
    int enable_ipv6_first;
} LebConfig;

typedef struct LebConnectionHandle {
    void* context;
    void* internal_handle;
    LebConfig config;
    LebCallback callback;
    LebAbrCallback abr_callback;
} LebConnectionHandle;

// 创建快直播连接
LEB_EXPORT_API LebConnectionHandle* OpenLebConnection(void* context, LebLogLevel loglevel);

// 注册回调函数
LEB_EXPORT_API void RegisterLebCallback(LebConnectionHandle* handle, const LebCallback* callback);

// 开始连接，内部完成信令后，直接建联拉流
LEB_EXPORT_API void StartLebConnection(LebConnectionHandle* handle, LebConfig config);

// 停止连接
LEB_EXPORT_API void StopLebConnection(LebConnectionHandle* handle);

// 关闭连接
LEB_EXPORT_API void CloseLebConnection(LebConnectionHandle* handle);

// 注册abr回调函数
LEB_EXPORT_API void RegisterAbrCallback(LebConnectionHandle* handle, const LebAbrCallback* abr_callback);

/**
 * 拉流过程中命令执行接口
 * 1. 主动查询统计数据: command="get_stats", argument=NULL, OnStatsInfo异步回调输出
 * 2. 请求切换码率: command="request_abr", argument=transcode_name字符串, transcode_name为配置的转码模板列表之一，切换成功后OnVideoInfo回调有宽高值变化;
 *                如果argument=auto, 则打开端侧abr auto功能，此时如果有设置abrcallback，按外部算法执行端侧abr切换，否则按内部算法执行abr切换
 *                请求切换码率的command，只在config.abr_mode为1时生效;
 * 3. 请求动态音视频启停: command="request_avm", argument=0, 不下发音视频数据，但维持连接;
 *                                               argument=1, 只下发音频数据;
 *                                               argument=2, 只下发视频数据;
 *                                               argument=3, 下发音视频数据
 */
LEB_EXPORT_API void DoLebCommand(LebConnectionHandle* handle, const char* command, void* argument);

#ifdef __ANDROID__
// 安卓下在使用静态库或者不直接load动态库时，需要通过此接口设置JVM
LEB_EXPORT_API int Set_Leb_JVM(void *jvm);
#endif

#ifdef __cplusplus
}
#endif

#endif  // LEB_CONNECTION_API_H_
