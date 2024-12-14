/*
 * webrtc demuxer
 * Copyright (c) 2021, Tencent. Fei Wei<weifei@tencent.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <pthread.h>

#include "config.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavformat/url.h"
#include "libswresample/swresample.h"

#include "leb_connection_api.h"

#if LIBAVFORMAT_VERSION_INT < ((57 << 16) | (25 << 8) | 100) // < ffmpeg3.0
#define AV_OPT_TYPE_BOOL AV_OPT_TYPE_INT
#endif

#if LIBAVFORMAT_VERSION_INT > ((57 << 16) | (25 << 8) | 100) // > ffmpeg3.0
#define codecpar(avstream)   ((avstream)->codecpar)
#else
#define codecpar(avstream)   ((avstream)->codec)
#endif

#if LIBAVFORMAT_VERSION_MAJOR > 58
#include "libavformat/internal.h"
#else
#define ffstream(avstream)   (avstream)
#endif

#ifndef  AV_INPUT_BUFFER_PADDING_SIZE
#define AV_INPUT_BUFFER_PADDING_SIZE FF_INPUT_BUFFER_PADDING_SIZE
#endif

/**
 * Guide for webrtc_demuxer
 * 1. depends on libLebConnection SDK from https://github.com/tencentyun/libLebConnectionSDK/
 * 2. ffmpeg integration example, refer to https://docs.qq.com/doc/DWExGWnZacnZWR2JW
 * 3. ijkplayer integration example, refer to https://mp.weixin.qq.com/s/f3ct29ydzAjdJ1fIdOmHmQ
 * 4. parameters in the AVOption table can be set or read from ffplay through option interface
 * 5. offer_ufrag_pwd reported by OnStatsCallback() should be saved and provided for log tracing
 * 6. for Android, must use System.loadLibrary() to load LibLebConnection_so.so explicitly
 */

#define DEBUG 0
#define MAX_ABR_NUM 3

typedef struct PacketQueue {
    AVPacketList *first_pkt, *last_pkt;
    int is_started;
    int nb_packets;
    int abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
} PacketQueue;

typedef struct WEBRTCContext {
    const AVClass *class;
    AVFormatContext *avctx;
    int open_timeout;
    int read_timeout;
    int reopen_count;
    int max_reopen_count;
    int is_opened;
    LebConnectionHandle *handle;
    LebConfig config;
    int video_stream_index_in;
    int video_stream_index_out;
    int audio_stream_index_in;
    int audio_stream_index_out;
    LebVideoCodecType video_codec;
    LebAudioCodecType audio_codec;
    int enable_aac;
    int receive_video;
    int receive_audio;

    // video info
    int width;
    int height;
    char *video_extradata;
    int video_extradata_size;
    // audio info
    int sample_rate;
    int num_channels;
    char *audio_extradata;
    int audio_extradata_size;

    AVPacket video_pkt;
    AVPacket audio_pkt;
    PacketQueue queue;
    int max_queue_size;
    int  log_offset;
    int  error_code;

    char *server_address;
    int enable_minisdp;
    int minisdp_server_port;
    int allowed_avsync_diff;
    int max_jitter_delay_ms;
    int min_jitter_delay_ms;
    int enable_smoothing_output;
    float max_output_speed;
    float min_output_speed;

    // 0:关闭abr, 1:abr本地控制, 2:abr服务端控制(要和后台域名配置配合)
    int abr_mode;
    // 从高到低，不同abr清晰度等级，最多3级：high，medium，low，对应不同转码模板，需要后台配置
    // high可以为“origin”， 表示原始流，不转码
    // 最典型应用就是低码率开播，然后根据网络和播放状态调整到合适码率或原始流
    char *video_definitions[MAX_ABR_NUM];
    char *current_video_definition;
    char *requested_video_definition;
    int  requested_definition_index;
    int  max_definition_index;

    char *client_ip;
    char *signal_server_ip;
    char *data_server_ip;
    int jitter_delay_ms;

    int dump_file;
    FILE *vfp;
    FILE *afp;
    int64_t video_pts;
    int64_t audio_pts;

    // 内部播控需要注册外部音频解码器回调
    int enable_play_control;
    pthread_mutex_t audio_decoder_mutex;
    AVCodec* audio_decoder;
    AVCodecContext* audio_decoder_ctx;
    struct SwrContext* audio_resample_ctx;
    AVPacket audio_decoder_packet;
    AVFrame* audio_decoder_frame;

    // bitmap for av_mode:两位bit, 低位表示是否下发音频数据，高位表示是否下发视频数据
    // 0(00):pause, 不下发音视频数据，但维持连接; 1(01):audioonly, 只下发音频数据; 2(10):videoonly, 只下发视频数据; 3(11):resume, 下发音视频数据
    int current_av_mode;
    int last_av_mode;
    int init_av_mode;

    int enable_ipv6_first;
} WEBRTCContext;

static int packet_queue_init(PacketQueue *q, void *logctx)
{
    int ret;

    memset(q, 0, sizeof(PacketQueue));
    ret = pthread_mutex_init(&q->mutex, NULL);
    if (ret != 0) {
        ret = AVERROR(ret);
        av_log(logctx, AV_LOG_ERROR, "pthread_mutex_init failed : %s\n", av_err2str(ret));
        return ret;
    }
    ret = pthread_cond_init(&q->condition, NULL);
    if (ret != 0) {
        pthread_mutex_destroy(&q->mutex);
        ret = AVERROR(ret);
        av_log(logctx, AV_LOG_FATAL, "pthread_cond_init failed : %s\n", av_err2str(ret));
        return ret;
    }

    return 0;
}

static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *nextpkt;
    pthread_mutex_lock(&q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = nextpkt) {
        nextpkt = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    pthread_mutex_unlock(&q->mutex);
}

static void packet_queue_reset(PacketQueue *q) {
    packet_queue_flush(q);
    pthread_mutex_lock(&q->mutex);
    q->abort_request = 0;
    q->is_started = 0;
    pthread_mutex_unlock(&q->mutex);
}

static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_reset(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->condition);
}

static void packet_queue_abort(PacketQueue *q)
{
    pthread_mutex_lock(&q->mutex);
    q->abort_request = 1;
    pthread_cond_signal(&q->condition);
    pthread_mutex_unlock(&q->mutex);
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt, WEBRTCContext *s)
{
    AVPacketList *pkt1;

    pthread_mutex_lock(&q->mutex);
    if (q->abort_request) {
        av_packet_unref(pkt);
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    if (s->max_queue_size > 0 && q->nb_packets >= s->max_queue_size) {
        av_log(s->avctx, AV_LOG_INFO, "packet queue overflow, %d/%d, drop old packet\n", q->nb_packets, s->max_queue_size);
        pkt1 = q->first_pkt;
        q->first_pkt = pkt1->next;
        av_packet_unref(&pkt1->pkt);
        av_free(pkt1);
        q->nb_packets--;
    }
    
    pkt1 = av_malloc(sizeof(AVPacketList));
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;

    q->is_started = 1;
    q->nb_packets++;
    pthread_cond_signal(&q->condition);

    pthread_mutex_unlock(&q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, WEBRTCContext *s, AVPacket *pkt, int64_t timeout)
{
    AVPacketList *pkt1;
    int ret = 0;
    int loop = (timeout >= 0) ? FFMAX(timeout / 100000, 1) : -1;

    for (int i = 0; loop > 0 && i < loop; i++) {
        if (ff_check_interrupt(&s->avctx->interrupt_callback)) {
            packet_queue_abort(q);
            ret = AVERROR_EXIT;
            break;
        }

        pthread_mutex_lock(&q->mutex);
        if (q->abort_request) {
            pthread_mutex_unlock(&q->mutex);
            return AVERROR_EXIT;
        }
        if (s->error_code) {
             pthread_mutex_unlock(&q->mutex);
             return s->error_code;
        }
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            pthread_mutex_unlock(&q->mutex);
            break;
        } else {
            int64_t t = av_gettime() + 100000;
            struct timespec tv = { .tv_sec  =  t / 1000000,
                                   .tv_nsec = (t % 1000000) * 1000 };
            ret = AVERROR(ETIMEDOUT);
            pthread_cond_timedwait(&q->condition, &q->mutex, &tv);
        }
        pthread_mutex_unlock(&q->mutex);
    }

    return ret;
}

/* keep it the same as avpriv_set_pts_info */
static void set_stream_pts_info(AVStream *s, int pts_wrap_bits,
                         unsigned int pts_num, unsigned int pts_den)
{
    AVRational new_tb;
    if (av_reduce(&new_tb.num, &new_tb.den, pts_num, pts_den, INT_MAX)) {
        if (new_tb.num != pts_num)
            av_log(NULL, AV_LOG_DEBUG,
                   "st:%d removing common factor %d from timebase\n",
                   s->index, pts_num / new_tb.num);
    } else
        av_log(NULL, AV_LOG_WARNING,
               "st:%d has too large timebase, reducing\n", s->index);

    if (new_tb.num <= 0 || new_tb.den <= 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Ignoring attempt to set invalid timebase %d/%d for st:%d\n",
               new_tb.num, new_tb.den,
               s->index);
        return;
    }
    s->time_base     = new_tb;
#if FF_API_LAVF_AVCTX
    s->codec->pkt_timebase = new_tb;
#endif
    s->pts_wrap_bits = pts_wrap_bits;
}

static int to_ff_log_level(WEBRTCContext *s, LebLogLevel level)
{
    int v = FFMIN((int)level + s->log_offset, (int)kError);

    switch(v) {
    case kDebug:
        return AV_LOG_DEBUG;
    case kInfo:
        return AV_LOG_INFO;
    case kWarning:
        return AV_LOG_WARNING;
    case kError:
        return AV_LOG_ERROR;
    default:
        return AV_LOG_TRACE;
    }
}

static LebLogLevel to_leb_log_level(WEBRTCContext *s)
{
    AVFormatContext *h = s->avctx;
    int v = av_log_get_level();
    LebLogLevel lebloglevel;

    if (v <= AV_LOG_ERROR) {
        lebloglevel = kError;
    } else if (v <= AV_LOG_WARNING) {
        lebloglevel = kWarning;
    } else if (v < AV_LOG_VERBOSE) {
        lebloglevel = kInfo;
    } else { // >= AV_LOG_VERBOSE
        lebloglevel = kDebug;
    }
    av_log(h, AV_LOG_INFO, "av_log_level %d, leb_log_level %d\n", v, lebloglevel);
    return lebloglevel;
}

#define MAX_LOG_LENGTH 1024 // 分段打印的最大长度
static void OnLogCallback(void* context, const char* tag, LebLogLevel level, const char* msg) {
    WEBRTCContext *s = context;
    AVFormatContext *h = s->avctx;
    size_t message_length = strlen(msg);
    int ff_log_level = to_ff_log_level(s, level);
    for (size_t i = 0; i < message_length; i += MAX_LOG_LENGTH) {
        size_t current_length = (i + MAX_LOG_LENGTH > message_length) ? (message_length - i) : MAX_LOG_LENGTH;
        char buffer[MAX_LOG_LENGTH + 1];
        strncpy(buffer, msg + i, current_length);
        buffer[current_length] = '\0';
        if (i == 0) {
            av_log(h, ff_log_level, "[lebconnection]%s%s", tag, buffer);
        } else {
            av_log(h, ff_log_level, "%s", buffer);
        }
    }
    av_log(h, ff_log_level, "\n");
}

static void OnVideoInfoCallback(void* context, LebVideoInfo info) {
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVFormatContext *h = s->avctx;

    s->video_codec = info.codec_type;
    if (s->video_codec != kH264 && s->video_codec != kH265 && s->video_codec != kH266 && s->video_codec != kAV1) {
        av_log(h, AV_LOG_ERROR,
                "OnVideoInfoCallback, unknown video codec %d\n",
                s->video_codec);
        return;
    }
    s->width = info.width;
    s->height = info.height;
    av_log(h, AV_LOG_INFO, "OnVideoInfoCallback, video codec %d, width %zu, height %zu, extradata_size %zu\n",
           info.codec_type, info.width, info.height, info.extra_size);

    if (info.extra_size > 0 && !s->video_extradata) {
        s->video_extradata = av_malloc(info.extra_size);
        if (!s->video_extradata) {
            s->error_code = AVERROR(ENOMEM);
            return;
        }
        memcpy(s->video_extradata, info.extra_data, info.extra_size);
        s->video_extradata_size = info.extra_size;

        //just a fake video packet for create stream
        if (h->ctx_flags & AVFMTCTX_NOHEADER) {
            AVPacket *pkt = &s->video_pkt;
            av_new_packet(pkt, 0);
            pkt->stream_index = s->video_stream_index_in;
            packet_queue_put(&s->queue, pkt, s);
        }
    }
}

static void OnAudioInfoCallback(void* context, LebAudioInfo info) {
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVFormatContext *h = s->avctx;

    s->audio_codec = info.codec_type;
    if (s->audio_codec != kAAC && s->audio_codec != kOpus && s->audio_codec != kPCM) {
        av_log(h, AV_LOG_ERROR,
                "OnAudioInfoCallback, unknown audio codec %d\n",
                s->audio_codec);
        return;
    }
    s->sample_rate = info.sample_rate;
    s->num_channels = info.num_channels;
    av_log(h, AV_LOG_INFO, "OnAudioInfoCallback, audio codec %d, sample rate %d, num channels %d, extradata_size %zu\n",
           info.codec_type, info.sample_rate, info.num_channels, info.extra_size);

    if (info.extra_size > 0 && !s->audio_extradata) {
        s->audio_extradata = av_malloc(info.extra_size);
        if (!s->audio_extradata) {
            s->error_code = AVERROR(ENOMEM);
            return;
        }
        memcpy(s->audio_extradata, info.extra_data, info.extra_size);
        s->audio_extradata_size = info.extra_size;

        //just a fake audio packet for create stream
        if (h->ctx_flags & AVFMTCTX_NOHEADER) {
            AVPacket *pkt = &s->audio_pkt;
            av_new_packet(pkt, 0);
            pkt->stream_index = s->audio_stream_index_in;
            packet_queue_put(&s->queue, pkt, s);
        }
    }
}

static void OnVideoDataCallback(void* context, LebEncodedVideoFrame videoframe)
{
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVPacket *pkt = &s->video_pkt;
#if DEBUG
    av_log(s->avctx, AV_LOG_INFO, "OnVideoDataCallback, size:%zu, is_keyframe %d, dts:%lld, pts:%lld, a-v diff:%lld, audio pts:%lld\n",
           videoframe.size, videoframe.is_keyframe, videoframe.dts, videoframe.pts, s->audio_pts - videoframe.pts, s->audio_pts);
#endif
    av_new_packet(pkt, videoframe.size);
    memcpy(pkt->data, videoframe.data, videoframe.size);
    pkt->stream_index = s->video_stream_index_in;
    pkt->dts = videoframe.dts;
    pkt->pts = videoframe.pts;
    if (videoframe.is_keyframe)
        pkt->flags |= AV_PKT_FLAG_KEY;
    s->video_pts = videoframe.pts;
    packet_queue_put(&s->queue, pkt, s);
}

static void OnAudioDataCallback(void* context, LebEncodedAudioFrame audioframe)
{
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVPacket *pkt = &s->audio_pkt;
#if DEBUG
    av_log(s->avctx, AV_LOG_INFO, "OnAudioDataCallback, size:%zu, dts:%lld, pts:%lld, a-v diff:%lld, video pts:%lld\n",
           audioframe.size, audioframe.dts, audioframe.pts, audioframe.pts - s->video_pts, s->video_pts);
#endif
    av_new_packet(pkt, audioframe.size);
    memcpy(pkt->data, audioframe.data, audioframe.size);
    pkt->stream_index = s->audio_stream_index_in;
    pkt->dts = audioframe.dts;
    pkt->pts = audioframe.pts;
    s->audio_pts = audioframe.pts;
    packet_queue_put(&s->queue, pkt, s);
}

static void OnMetaDataCallback(void* context, LebMetaData metadata)
{
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVFormatContext *h = s->avctx;
    av_log(h, AV_LOG_INFO, "OnMetaDataCallback %zu\n", metadata.size);
}

static void OnStatsCallback(void* context, LebStats stats)
{
    WEBRTCContext *s = context;
    AVFormatContext *h = s->avctx;
    /**
     * 请用户保存或上报offer_ufrag_pwd信息，能对应每次播放记录，排查线上问题时需提供此信息，用于查询相关日志
     */
    av_log(h, to_ff_log_level(s, kInfo),
      "OnStats "
      "offer_ufrag_pwd: %s, "
      "client_ip: %s, signal_ip: %s, media_ip:%s, dns_cost: %d, "
      "first video packet delay:%d, first audio packet delay: %d, "
      "rtt:%d audio_bytes:%" PRIu64", audio_nack:%u, audio_lost:%d, audio_pkts:%u, audio_output:%u, "
      "video_bytes:%" PRIu64", video_nack:%u, video_lost:%d, video_pkts:%u, "
      "video_frames_received:%u, video_frame_rate_received %.2f, video_frames_output:%u, video_frame_rate_output:%.2f, "
      "jitter_delay:%d, transport_loss_rate:%.4f\n",
      stats.offer_ufrag_pwd, stats.client_ip,
      stats.signal_server_ip, stats.data_server_ip, stats.signal_dns_cost_ms,
      stats.first_video_received_cost_ms, stats.first_audio_received_cost_ms,
      stats.rtt_ms, stats.audio_bytes_received, stats.audio_nack_count,
      stats.audio_packets_lost, stats.audio_packets_received, stats.audio_frames_output,
      stats.video_bytes_received, stats.video_nack_count,
      stats.video_packets_lost, stats.video_packets_received,
      stats.video_frames_received, stats.video_frame_rate_received, stats.video_frames_output, stats.video_frame_rate_output,
      stats.jitter_delay_ms, stats.transport_loss_rate);

    // 保存客户端IP，信令服务器IP和数据服务器IP
    if (!s->client_ip || av_strcasecmp(s->client_ip, stats.client_ip)) {
        av_free(s->client_ip);
        s->client_ip = av_strdup(stats.client_ip);
    }
    if (!s->signal_server_ip || av_strcasecmp(s->signal_server_ip, stats.signal_server_ip)) {
        av_free(s->signal_server_ip);
        s->signal_server_ip = av_strdup(stats.signal_server_ip);
    }
    if (!s->data_server_ip || av_strcasecmp(s->data_server_ip, stats.data_server_ip)) {
        av_free(s->data_server_ip);
        s->data_server_ip = av_strdup(stats.data_server_ip);
    }
    s->jitter_delay_ms = stats.jitter_delay_ms;
}

static void OnErrorCallbck(void* context, LebErrorCode error)
{
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVFormatContext *h = s->avctx;

    pthread_mutex_lock(&s->queue.mutex);
    if (error == kUnknow_error) {
        s->error_code = AVERROR_UNKNOWN;
    } else if (error == kNotFound_error) {
        s->error_code = AVERROR_STREAM_NOT_FOUND;
    } else if (error == kAuth_error) {
        s->error_code = AVERROR(EINVAL);
    } else if (error == kSignal_timeout) {
        s->error_code = AVERROR(ETIMEDOUT);
    } else if (error == kData_timeout) {
        s->error_code = AVERROR(ETIMEDOUT);
    } else if (error == kReceive_bye) {
        s->error_code = AVERROR_EOF;
    } else if (error != kNone_error) {
        s->error_code = AVERROR(EINVAL);
    } else {
        s->error_code = 0;
    }
    pthread_mutex_unlock(&s->queue.mutex);

    av_log(h, AV_LOG_ERROR, "OnErrorCallbck %d, %s\n", error, av_err2str(s->error_code));
}

static void OnConnectionChangeCallback(void* context, LebNetState state)
{
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVFormatContext *h = s->avctx;

    av_log(h, AV_LOG_INFO, "OnConnectionChangeCallback %d\n", state);
    pthread_mutex_lock(&s->queue.mutex);

    if (state == kDisconnected) {
        // webrtc stun ping超时导致connection断开，进行reopen
        s->error_code = AVERROR(ETIMEDOUT);
    } else if (state == kConnectFailed) {
        s->error_code = AVERROR_EXIT;
    }
    pthread_mutex_unlock(&s->queue.mutex);
}

// used for internal play control when enable_play_control is on
static int OnOpenAudioDecoderCallback(void* context, LebAudioInfo info)
{
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVFormatContext *h = s->avctx;
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;
    av_log(h, AV_LOG_INFO, "OnOpenAudioDecoderCallback sample_rate:%d, num_channels:%d, extradata_size %zu\n",
           info.sample_rate, info.num_channels, info.extra_size);

    pthread_mutex_lock(&s->audio_decoder_mutex);

    if (s->audio_decoder_ctx) {
        av_log(NULL, AV_LOG_ERROR, "external audio decoder already opened\n");
        pthread_mutex_unlock(&s->audio_decoder_mutex);
        return -1;
    }

    if (info.codec_type == kAAC) {
        codec_id = AV_CODEC_ID_AAC;
    } else if (info.codec_type == kOpus) {
        codec_id = AV_CODEC_ID_OPUS;
    } else {
        av_log(NULL, AV_LOG_ERROR, "not expected audio codec type\n");
        pthread_mutex_unlock(&s->audio_decoder_mutex);
        return -1;
    }
    s->audio_decoder = avcodec_find_decoder(codec_id);
    if (!s->audio_decoder) {
        av_log(NULL, AV_LOG_ERROR, "external find audio decoder failed\n");
        pthread_mutex_unlock(&s->audio_decoder_mutex);
        return -1;
    }
    av_init_packet(&s->audio_decoder_packet);
    s->audio_decoder_ctx = avcodec_alloc_context3(s->audio_decoder);
    if (!s->audio_decoder_ctx) {
        av_log(NULL, AV_LOG_ERROR, "external alloc audio decoder context failed\n");
        pthread_mutex_unlock(&s->audio_decoder_mutex);
        return AVERROR(ENOMEM);
    }

    s->audio_decoder_ctx->channel_layout = av_get_default_channel_layout(info.num_channels);
    s->audio_decoder_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
    s->audio_decoder_ctx->sample_rate = info.sample_rate;
    s->audio_decoder_ctx->channels = info.num_channels;
    s->audio_decoder_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
    if (info.extra_size > 0) {
        s->audio_decoder_ctx->extradata = av_malloc(info.extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (s->audio_decoder_ctx->extradata) {
            memset(s->audio_decoder_ctx->extradata + info.extra_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(s->audio_decoder_ctx->extradata, info.extra_data, info.extra_size);
            s->audio_decoder_ctx->extradata_size = info.extra_size;
        } else {
            s->audio_decoder_ctx->extradata_size = 0;
        }
    }

    if (avcodec_open2(s->audio_decoder_ctx, s->audio_decoder, NULL) < 0) {
        av_log(NULL, AV_LOG_ERROR, "external open audio decoder failed\n");
        pthread_mutex_unlock(&s->audio_decoder_mutex);
        return -1;
    }

    s->audio_decoder_frame = av_frame_alloc();
    pthread_mutex_unlock(&s->audio_decoder_mutex);
    av_log(h, AV_LOG_INFO, "OnOpenAudioDecoderCallback success\n");
    return 0;
}

// used for internal play control when enable_play_control is on
static int OnDecodeAudioFrameCallback(void* context, const uint8_t* in_data, int in_size, uint8_t* out_data)
{
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVFormatContext *h;
    int out_len = 0;
    int64_t out_channel_layout, in_channel_layout;
    enum AVSampleFormat out_sample_format, in_sample_format;
    int out_sample_rate, in_sample_rate;
    const uint8_t** in_buffer;
    uint8_t *out_buffer;
    int out_num_samples, samples;

    if (context == NULL || in_data == NULL || in_size <= 0 || out_data == NULL) {
        av_log(NULL, AV_LOG_ERROR, "OnAudioDecodeCallback bad parameter \n");
        return -1;
    }

    h = s->avctx;

    pthread_mutex_lock(&s->audio_decoder_mutex);
    if (!s->audio_decoder_ctx || !s->audio_decoder || !s->audio_decoder_frame) {
        av_log(NULL, AV_LOG_INFO, "external audio decoder not opened \n");
        pthread_mutex_unlock(&s->audio_decoder_mutex);
        return -1;
    }

    s->audio_decoder_packet.data = (uint8_t*)in_data;
    s->audio_decoder_packet.size = in_size;
    int ret;
#if LIBAVFORMAT_VERSION_INT > ((57 << 16) | (25 << 8) | 100) // > ffmpeg3.0
    if (ret = avcodec_send_packet(s->audio_decoder_ctx, &s->audio_decoder_packet) < 0) {
        av_log(NULL, AV_LOG_ERROR, "avcodec_send_packet failed %d\n", ret);
        pthread_mutex_unlock(&s->audio_decoder_mutex);
        return -1;
    }

    if (ret = avcodec_receive_frame(s->audio_decoder_ctx, s->audio_decoder_frame) < 0) {
        av_log(NULL, AV_LOG_ERROR, "avcodec_receive_frame failed %d\n", ret);
        pthread_mutex_unlock(&s->audio_decoder_mutex);
        return -1;
    }
#else
    int got_frame = 0;
    if (ret = avcodec_decode_audio4(s->audio_decoder_ctx, s->audio_decoder_frame, &got_frame, &s->audio_decoder_packet) < 0) {
        av_log(NULL, AV_LOG_ERROR, "avcodec_decode_audio4 failed %d\n", ret);
        pthread_mutex_unlock(&s->audio_decoder_mutex);
        return -1;
    }

    if (!got_frame) {
        pthread_mutex_unlock(&s->audio_decoder_mutex);
        return 0;
    }
#endif
    out_channel_layout = av_get_default_channel_layout(s->num_channels);
    out_sample_format = AV_SAMPLE_FMT_S16;
    out_sample_rate = s->sample_rate;

    in_channel_layout = av_get_default_channel_layout(s->audio_decoder_frame->channels);
    in_sample_format = s->audio_decoder_frame->format;
    in_sample_rate = s->audio_decoder_frame->sample_rate;
    if (!s->audio_resample_ctx) {
        av_log(h, AV_LOG_INFO, "swr_alloc_set_opts, "
            "out_channel_layout: %d, out_sample_format: %d, out_sample_rate: %d "
            "in_channel_layout: %d, in_sample_format: %d, in_sample_rate: %d\n",
            out_channel_layout, out_sample_format, out_sample_rate,
            in_channel_layout, in_sample_format, in_sample_rate);
        s->audio_resample_ctx = swr_alloc_set_opts(NULL, out_channel_layout, out_sample_format, out_sample_rate,
                                                 in_channel_layout, in_sample_format, in_sample_rate,
                                                 0, NULL);
        if (!s->audio_resample_ctx) {
            av_log(h, AV_LOG_ERROR, "swr_alloc_set_opts failed!\n");
            pthread_mutex_unlock(&s->audio_decoder_mutex);
            return -1;
        }

        if (swr_init(s->audio_resample_ctx) < 0) {
            av_log(h, AV_LOG_ERROR, "swr_init failed!\n");
            swr_free(&s->audio_resample_ctx);
            pthread_mutex_unlock(&s->audio_decoder_mutex);
            return -1;
        }
    }

    in_buffer = (const uint8_t**)s->audio_decoder_frame->data;
    out_buffer = out_data;
    out_num_samples = av_rescale_rnd(swr_get_delay(s->audio_resample_ctx, in_sample_rate) + s->audio_decoder_frame->nb_samples,
                                     out_sample_rate, in_sample_rate, AV_ROUND_UP);
    samples = swr_convert(s->audio_resample_ctx,
                          &out_buffer,
                          out_num_samples,
                          in_buffer,
                          s->audio_decoder_frame->nb_samples);
    if (samples < 0) {
        av_log(h, AV_LOG_ERROR, "swr_convert failed!\n");
        pthread_mutex_unlock(&s->audio_decoder_mutex);
        return -1;
    }
    out_len = samples * s->num_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
    pthread_mutex_unlock(&s->audio_decoder_mutex);
#if DEBUG
    av_log(h, AV_LOG_INFO, "OnDecodeAudioFrameCallback, out_len:%d\n", out_len);
#endif
    return out_len;
}

// used for internal play control when enable_play_control is on
static int OnCloseAudioDecoderCallback(void* context)
{
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVFormatContext *h = s->avctx;
    av_log(h, AV_LOG_INFO, "OnCloseAudioDecoderCallback\n");

    pthread_mutex_lock(&s->audio_decoder_mutex);
    if (s->audio_decoder_ctx) {
        if (s->audio_decoder_ctx->extradata) {
            av_freep(&s->audio_decoder_ctx->extradata);
        }
        avcodec_close(s->audio_decoder_ctx);
        avcodec_free_context(&s->audio_decoder_ctx);
        s->audio_decoder_ctx = NULL;
    }

    if (s->audio_decoder_frame) {
        av_frame_free(&s->audio_decoder_frame);
        s->audio_decoder_frame = NULL;
    }

    if (s->audio_resample_ctx) {
        swr_free(&s->audio_resample_ctx);
        s->audio_resample_ctx = NULL;
    }

    av_packet_unref(&s->audio_decoder_packet);
    pthread_mutex_unlock(&(s->audio_decoder_mutex));
    av_log(h, AV_LOG_INFO, "OnCloseAudioDecoderCallback success\n");
    return 0;
}

static int webrtc_open(AVFormatContext *h, const char *uri)
{
    WEBRTCContext *s = h->priv_data;
    LebLogLevel level = to_leb_log_level(s);//SDK输出日志等级
    char* new_uri = NULL;
    char* new_signal_server_address = NULL;
    char hostname[1024], proto[1024], path[1024];
    int port = -1;
    static const LebCallback callback = {
        .onLogInfo = OnLogCallback,
        .onVideoInfo = OnVideoInfoCallback,
        .onAudioInfo = OnAudioInfoCallback,
        .onEncodedVideo = OnVideoDataCallback,
        .onEncodedAudio = OnAudioDataCallback,
        .onMetaData = OnMetaDataCallback,
        .onStatsInfo = OnStatsCallback,
        .onError = OnErrorCallbck,
        .onConnectionChange = OnConnectionChangeCallback,
        .onOpenAudioDecoder = OnOpenAudioDecoderCallback,
        .onDecodeAudioFrame = OnDecodeAudioFrameCallback,
        .onCloseAudioDecoder = OnCloseAudioDecoderCallback
    };

    if (s->is_opened) {
        av_log(h, AV_LOG_INFO, "already opened, webrtc_open exit\n");
        return 0;
    }

    av_log(h, AV_LOG_INFO, "webrtc_open %s\n", uri);
    packet_queue_init(&s->queue, h);
    av_init_packet(&s->video_pkt);
    av_init_packet(&s->audio_pkt);
    pthread_mutex_init(&s->audio_decoder_mutex, NULL);

    memset(&s->config, -1, sizeof(LebConfig));// init as -1 to check if some configs are missed
    s->video_codec = kUnknownVideo;
    s->audio_codec = kUnknownAudio;

    av_url_split(proto, sizeof(proto), NULL, 0,
                 hostname, sizeof(hostname), &port,
                 path, sizeof(path), uri);
    av_log(h, AV_LOG_INFO, "stream url parsed hostname:%s\n", hostname);

    if (av_strstart(uri, "http://", NULL) && strstr(uri, ".sdp")) {
        new_uri = av_strireplace(uri, "http://", "webrtc://");
        new_signal_server_address = hostname;
    } else if (av_strstart(uri, "https://", NULL) && strstr(uri, ".sdp")) {
        new_uri = av_strireplace(uri, "https://", "webrtc://");
        new_signal_server_address = hostname;
    } else if (av_strstart(uri, "webrtc://", NULL))  {
        new_uri = av_strdup(uri);
    } else {
        packet_queue_destroy(&s->queue);
        pthread_mutex_destroy(&s->audio_decoder_mutex);
        return AVERROR(EINVAL);
    }
    av_log(h, AV_LOG_INFO, "new uri %s\n", new_uri);

    s->last_av_mode = s->current_av_mode;
    s->init_av_mode = s->current_av_mode;
    s->receive_audio = s->current_av_mode & 1;
    s->receive_video = (s->current_av_mode & 2) >> 1;

    /**
     * 配置参数
     */
    s->config.stream_url = new_uri;
    // 默认ipv4和ipv6双栈域名：webrtc-dk.tliveplay.com
    // 对于 http(s)://xxxx/xx.sdp的播放，使用码流url中的域名作为信令域名
    if (new_signal_server_address) {
        s->config.signal_address = new_signal_server_address;
    } else {
        s->config.signal_address = s->server_address;
    }
    s->config.open_timeout_ms = s->open_timeout; // sdk内部信令或数据超时会有异步错误回调
    s->config.enable_minisdp = s->enable_minisdp;
    s->config.enable_aac = s->enable_aac;
    s->config.enable_flex_fec = 1;
    s->config.receive_audio = s->receive_audio;
    s->config.receive_video = s->receive_video;
    s->config.stats_period_ms = 1000;
    s->config.enable_audio_plc = 1;
    s->config.enable_0rtt = 0;
    s->config.allowed_avsync_diff = s->allowed_avsync_diff;
    s->config.max_jitter_delay_ms = s->max_jitter_delay_ms;
    s->config.min_jitter_delay_ms = s->min_jitter_delay_ms;
    s->config.enable_smoothing_output = s->enable_smoothing_output;
    s->config.max_output_speed = s->max_output_speed;
    s->config.min_output_speed = s->min_output_speed;
    s->config.enable_play_control = s->enable_play_control;
    s->config.enable_ipv6_first = s->enable_ipv6_first;

    //init for abr
    s->config.abr_mode = s->abr_mode;
    if (s->config.abr_mode > 0) {
        s->max_definition_index = -1;
        for (int i = 0; i < MAX_ABR_NUM; i++) {
            if (s->video_definitions[i]) {
                s->config.abr_transcode_names[i] = s->video_definitions[i];
                if (s->current_video_definition
                    && !av_strcasecmp(s->current_video_definition, s->video_definitions[i])) {
                    s->config.start_transcode_name = s->current_video_definition;
                    s->requested_definition_index = i;
                    s->requested_video_definition = s->video_definitions[s->requested_definition_index];
                }
                if (s->current_video_definition
                    && !av_strcasecmp(s->current_video_definition, "AUTO")) {
                    s->requested_definition_index = 1;
                    s->config.start_transcode_name = s->video_definitions[s->requested_definition_index];
                    s->requested_video_definition = s->video_definitions[s->requested_definition_index];
                }
                s->max_definition_index = i;
            } else {
                break;
            }
        }
        if (s->max_definition_index > 0 && !s->config.start_transcode_name) {
            // 默认以最低码率为起始模板
            s->config.start_transcode_name = s->video_definitions[s->max_definition_index];
            s->requested_definition_index = s->max_definition_index;
            s->requested_video_definition = s->video_definitions[s->requested_definition_index];
        }
        // 没有用到的模板要设置为NULL
        for (int i = MAX_ABR_NUM; i < MAX_ABR_NUM_ALLOWED; i++) {
            s->config.abr_transcode_names[i] = NULL;
        }
        av_log(h, AV_LOG_INFO, "init abr definiton %s, index %d\n", s->requested_video_definition, s->requested_definition_index);
    } else {
        s->config.start_transcode_name = NULL;
        for(int i = 0; i < MAX_ABR_NUM_ALLOWED; i++) {
            s->config.abr_transcode_names[i] = NULL;
        }
    }

    /**
     * 1. 创建LebConnecton, 并注册回调函数
     */
    s->handle = OpenLebConnection((void*)s, level);
    RegisterLebCallback(s->handle, &callback);

    /**
     * 2. 开始连接，内部走信令流程，直接建联拉流；
     */
    StartLebConnection(s->handle, s->config);

    s->is_opened = 1;

    av_log(h, AV_LOG_INFO, "webrtc_open exit\n");
    return 0;
}

static int webrtc_close(AVFormatContext *h)
{
    WEBRTCContext *s = h->priv_data;

    if (!s->is_opened) {
        av_log(h, AV_LOG_INFO, "already closed, webrtc_close exit\n");
        return 0;
    }

    av_log(h, AV_LOG_INFO, "webrtc_close\n");
    if (s->vfp) {
        fclose(s->vfp);
        s->vfp = NULL;
    }
    if (s->afp) {
        fclose(s->afp);
        s->afp = NULL;
    }

    packet_queue_abort(&s->queue);

    /**
     * 3. 停止和关闭LebConnection
     */
    StopLebConnection(s->handle);
    CloseLebConnection(s->handle);
    s->handle = NULL;

    packet_queue_destroy(&s->queue);
    pthread_mutex_destroy(&s->audio_decoder_mutex);

    av_free((void*)s->video_extradata);
    av_free((void*)s->audio_extradata);
    s->video_extradata = NULL;
    s->audio_extradata = NULL;

    av_free((void*)s->config.stream_url);
    s->config.stream_url = NULL;

    s->is_opened = 0;

    av_log(h, AV_LOG_INFO, "webrtc_close exit\n");
    return 0;
}

static AVStream *create_stream(AVFormatContext *s, int codec_type)
{
    WEBRTCContext *h   = s->priv_data;
    av_log(s, AV_LOG_INFO, "create_stream, codec_type %d\n", codec_type);

    AVStream *st = avformat_new_stream(s, NULL);
    if (!st)
        return NULL;
    codecpar(st)->codec_type = codec_type;

    if (s->nb_streams >= h->receive_audio + h->receive_video)
        s->ctx_flags &= ~AVFMTCTX_NOHEADER;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        if (h->video_codec == kH264) {
            codecpar(st)->codec_id = AV_CODEC_ID_H264;
            if (h->dump_file) {
                h->vfp = fopen("video.h264", "wb");
            }
        } else if (h->video_codec == kH265) {
            codecpar(st)->codec_id = AV_CODEC_ID_H265;
            if (h->dump_file) {
                h->vfp = fopen("video.h265", "wb");
            }
        } else if (h->video_codec == kH266) {
#if CONFIG_VVC_PARSER || LIBAVFORMAT_VERSION_INT >= ((58 << 16) | (76 << 8) | 100) // >= ffmpeg4.4
            codecpar(st)->codec_id = AV_CODEC_ID_H266;
            if (h->dump_file) {
                h->vfp = fopen("video.h266", "wb");
            }
#endif
        } else if (h->video_codec == kAV1) {
#if CONFIG_AV1_PARSER || LIBAVFORMAT_VERSION_INT >= ((57 << 16) | (71 << 8) | 100) // >= ffmpeg3.3
            codecpar(st)->codec_id = AV_CODEC_ID_AV1;
            if (h->dump_file) {
                h->vfp = fopen("video.av1", "wb");
            }
#endif
        }
        codecpar(st)->width = h->width;
        codecpar(st)->height = h->height;
        ffstream(st)->need_parsing = AVSTREAM_PARSE_NONE;
        h->video_stream_index_out = st->index;

        if (h->video_extradata_size > 0) {
            codecpar(st)->extradata = av_malloc(h->video_extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (codecpar(st)->extradata != NULL) {
                memset(codecpar(st)->extradata + h->video_extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                memcpy(codecpar(st)->extradata, h->video_extradata, h->video_extradata_size);
                codecpar(st)->extradata_size = h->video_extradata_size;
            } else {
                codecpar(st)->extradata_size = 0;
            }
        }
    }

    if (codec_type == AVMEDIA_TYPE_AUDIO) {
        if (h->audio_codec == kAAC) {
            codecpar(st)->codec_id = AV_CODEC_ID_AAC;
            if (h->dump_file) {
                h->afp = fopen("audio.aac", "wb");
            }
        } else if (h->audio_codec == kOpus) {
            codecpar(st)->codec_id = AV_CODEC_ID_OPUS;
            if (h->dump_file) {
                h->afp = fopen("audio.opus", "wb");
            }
        } else if (h->audio_codec == kPCM) {
            codecpar(st)->codec_id = AV_CODEC_ID_PCM_S16LE;
            if (h->dump_file) {
                h->afp = fopen("audio.pcm", "wb");
            }
        }
        codecpar(st)->sample_rate = h->sample_rate;
        codecpar(st)->channels = h->num_channels;
        ffstream(st)->need_parsing = AVSTREAM_PARSE_NONE;
        h->audio_stream_index_out = st->index;

        if (h->audio_extradata_size > 0) {
            codecpar(st)->extradata = av_malloc(h->audio_extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
            if (codecpar(st)->extradata != NULL) {
                memset(codecpar(st)->extradata + h->audio_extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                memcpy(codecpar(st)->extradata, h->audio_extradata, h->audio_extradata_size);
                codecpar(st)->extradata_size = h->audio_extradata_size;
            } else {
                codecpar(st)->extradata_size = 0;
            }
        }
    }
    set_stream_pts_info(st, 64, 1, 1000);/* 64 bit pts in ms */
    return st;
}

static int webrtc_read_probe(const AVProbeData *p)
{
    /**
     * 快直播webrtc stream url形式
     * 1. webrtc://xxxx/xx(.sdp/.flv)
     * 2. http(s)://xxxx/xx.sdp
     */
    if (av_strstart(p->filename, "webrtc://", NULL) ||
        (av_strstart(p->filename, "http://", NULL) && strstr(p->filename, ".sdp")) ||
        (av_strstart(p->filename, "https://", NULL) && strstr(p->filename, ".sdp"))) {
        return AVPROBE_SCORE_MAX;
    }
    return 0;
}

static int webrtc_read_header(AVFormatContext *s)
{
    WEBRTCContext *h = s->priv_data;
#if LIBAVFORMAT_VERSION_MAJOR > 57
    const char *url = s->url;
#else
    const char *url = s->filename;
#endif
    int ret;

    av_log(s, AV_LOG_INFO, "webrtc_read_header, url %s\n", url);

    s->flags |= AVFMT_FLAG_GENPTS;
    s->ctx_flags |= AVFMTCTX_NOHEADER;
    s->fps_probe_size = 0;
    s->max_analyze_duration = FFMAX(s->max_analyze_duration, 5*AV_TIME_BASE);
    s->probesize = FFMAX(s->probesize, 512*1024);
    h->avctx = s;
    h->video_stream_index_in = 0;
    h->audio_stream_index_in = 1;
    h->video_stream_index_out = -1;
    h->audio_stream_index_out = -1;

    // 异步回调更新webrtc状态或错误码
    ret = webrtc_open(s, url);
    if (ret) {
        av_log(s, AV_LOG_ERROR, "webrtc_read_header: webrtc_open failed, %s\n", av_err2str(ret));
        return ret;
    }

    av_log(s, AV_LOG_INFO, "webrtc_read_header exit\n");
    return ret;
}

static int webrtc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret, i;
    WEBRTCContext *h = s->priv_data;
    AVStream *st;

    if (!h->is_opened) {
        av_log(h, AV_LOG_INFO, "already closed, webrtc_read_packet exit\n");
        return AVERROR_EXIT;
    }

    // adjust abr target
    if (h->abr_mode == 1) {
        // 端侧控制abr, 播放器可以通过setoption设置current_video_definition来切换码率
        if (h->current_video_definition && h->requested_video_definition
            && av_strcasecmp(h->current_video_definition, h->requested_video_definition)) {
            for (int i = 0; i <= h->max_definition_index; i++) {
                if (!av_strcasecmp(h->current_video_definition, h->video_definitions[i])) {
                    h->requested_definition_index = i;
                    h->requested_video_definition = h->video_definitions[h->requested_definition_index];
                    DoLebCommand(h->handle, "request_abr", (void*)h->requested_video_definition);
                    av_log(h, AV_LOG_INFO, "select abr defintion %s, index %d\n", h->requested_video_definition, h->requested_definition_index);
                    break;
                }
            }
            if (!av_strcasecmp(h->current_video_definition, "AUTO")) {
                h->requested_video_definition = "AUTO";
                DoLebCommand(h->handle, "request_abr", (void*)h->requested_video_definition);
                av_log(h, AV_LOG_INFO, "enable abr auto");
            }
        }
    }

    if (h->current_av_mode != h->last_av_mode) {
        int av_mode_for_request = (h->current_av_mode & h->init_av_mode);
        DoLebCommand(h->handle, "request_avm", (void*)&av_mode_for_request);
        h->last_av_mode = h->current_av_mode;
        av_log(h, AV_LOG_INFO, "request_avm:%d", av_mode_for_request);
        if (h->current_av_mode == 0) {
            packet_queue_flush(&h->queue);
            return AVERROR(EAGAIN);
        }
    } else if (h->current_av_mode == 0) {
        return AVERROR(EAGAIN);
    }

reopen:
    do {
        int timeout = !h->queue.is_started ? h->open_timeout : h->read_timeout;
        ret = packet_queue_get(&h->queue, h, pkt, INT64_C(1000) * timeout);
        if (ret < 0)
            break;

        /* now find stream */
        for (i = 0; i < s->nb_streams; i++) {
            st = s->streams[i];
            if (pkt->stream_index == h->video_stream_index_in
                && codecpar(st)->codec_type == AVMEDIA_TYPE_VIDEO) {
                break;
            } else if (pkt->stream_index == h->audio_stream_index_in
                       && codecpar(st)->codec_type == AVMEDIA_TYPE_AUDIO) {
                break;
            }
        }
        if (i == s->nb_streams) {
            static const enum AVMediaType stream_types[] = {AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO};
            st = create_stream(s, stream_types[pkt->stream_index]);
            if (!st) {
                av_packet_unref(pkt);
                ret = AVERROR(ENOMEM);
                break;
            }
        }

        if (pkt->size <= 0) {
            // drop fake packet and continue to read packet
            av_packet_unref(pkt);
            ret = 0;
            continue;
        }

        if (pkt->stream_index == h->video_stream_index_in) {
            pkt->stream_index = h->video_stream_index_out;
            if (h->vfp) {
                fwrite(pkt->data, 1, pkt->size, h->vfp);
                fflush(h->vfp);
            }
        } else if (pkt->stream_index == h->audio_stream_index_in) {
            pkt->stream_index = h->audio_stream_index_out;
            if (h->afp) {
                fwrite(pkt->data, 1, pkt->size, h->afp);
                fflush(h->afp);
            }
        } else {
            ret = 0;
        }

        if (!ret) {
            av_log(s, AV_LOG_INFO, "drop pkt with index %d and continue\n",
                   pkt->stream_index);
            av_packet_unref(pkt);
        }
    } while (!ret);

    ret = ret > 0 ? 0 : ret;
    if (ret) {
        av_log(s, AV_LOG_WARNING, "webrtc_read_packet, %s\n", av_err2str(ret));
        if (ret == AVERROR(ETIMEDOUT) && (h->reopen_count++ < h->max_reopen_count)) {
            av_log(s, AV_LOG_WARNING, "webrtc_read_packet timeout reopen %d\n", h->reopen_count);
            StopLebConnection(h->handle);
            packet_queue_reset(&h->queue);
            h->error_code = 0;
            StartLebConnection(h->handle, h->config);
            goto reopen;
        } else {
            webrtc_close(s);
            return ret;
        }
    } else {
        h->reopen_count = 0;
    }
    return ret;
}

static int webrtc_read_close(AVFormatContext *s)
{
    av_log(s, AV_LOG_INFO, "webrtc_read_close\n");
    webrtc_close(s);
    av_log(s, AV_LOG_INFO, "webrtc_read_close exit\n");
    return 0;
}


#define OFFSET(x) offsetof(WEBRTCContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "webrtc_log_offset", "Adjust WebRTC to FFmpeg logging level mapping, negative to reduce verbosity and vice versa", OFFSET(log_offset), AV_OPT_TYPE_INT, { .i64 = 0 }, -4, 3, D },
    { "webrtc_max_queue_size", "The maximum number of packets can hold before dropping, default to -1 for unlimited", OFFSET(max_queue_size), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT_MAX, D },
    { "webrtc_dump_file", "Dump video and audio raw data to file", OFFSET(dump_file), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, D },
    { "webrtc_server_address", "Webrtc server address", OFFSET(server_address), AV_OPT_TYPE_STRING, { .str = "webrtc-dk.tliveplay.com" }, 0, 0, D },
    { "webrtc_allowed_avsync_diff", "webrtc allowed avsync diff", OFFSET(allowed_avsync_diff), AV_OPT_TYPE_INT, { .i64 = 100 }, -1, INT_MAX, D },
    { "webrtc_abr_mode", "webrtc abr mode", OFFSET(abr_mode), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 2, D },
    { "webrtc_high_video_definition", "Webrtc high video definition", OFFSET(video_definitions[0]), AV_OPT_TYPE_STRING, { .str = "1080P" }, 0, 0, D },
    { "webrtc_medium_video_definition", "Webrtc medium video definition", OFFSET(video_definitions[1]), AV_OPT_TYPE_STRING, { .str = "720P" }, 0, 0, D },
    { "webrtc_low_video_definition", "Webrtc low video definition", OFFSET(video_definitions[2]), AV_OPT_TYPE_STRING, { .str = "480P" }, 0, 0, D },
    { "webrtc_current_video_definition", "Webrtc current video definition", OFFSET(current_video_definition), AV_OPT_TYPE_STRING, { .str = "480P" }, 0, 0, D },
    { "webrtc_current_av_mode", "Webrtc current av mode", OFFSET(current_av_mode), AV_OPT_TYPE_INT, { .i64 = 3 }, 0, 3, D },
    { "webrtc_enable_aac", "Webrtc enable aac instead of opus", OFFSET(enable_aac),  AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, D },
    { "webrtc_open_timeout", "Webrtc open timeout in ms", OFFSET(open_timeout),  AV_OPT_TYPE_INT, { .i64 = 5000 }, 2000, INT_MAX, D },
    { "webrtc_read_timeout", "Webrtc read timeout in ms", OFFSET(read_timeout),  AV_OPT_TYPE_INT, { .i64 = 5000 }, 2000, INT_MAX, D },
    { "webrtc_max_jitter_delay", "Webrtc max internal jitter delay in ms", OFFSET(max_jitter_delay_ms), AV_OPT_TYPE_INT, { .i64 = 3000 }, 1000, 10000, D },
    { "webrtc_min_jitter_delay", "Webrtc min internal jitter delay in ms", OFFSET(min_jitter_delay_ms), AV_OPT_TYPE_INT, { .i64 = 200 }, 0, 1000, D },
    { "webrtc_enable_smoothing_output", "Webrtc enable smoothing output", OFFSET(enable_smoothing_output), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, D },
    { "webrtc_max_output_speed", "Webrtc max output speed in 1.x", OFFSET(max_output_speed), AV_OPT_TYPE_FLOAT, { .dbl= 1.2f }, 1.05f, 1.5f, D },
    { "webrtc_min_output_speed", "Webrtc min output speed in 0.x", OFFSET(min_output_speed), AV_OPT_TYPE_FLOAT, { .dbl = 0.9f }, 0.75f, 1.0f, D },
    { "webrtc_enable_play_control", "Webrtc enable internal play control", OFFSET(enable_play_control), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, D },
    { "webrtc_enable_minisdp", "Webrtc enable minisdp", OFFSET(enable_minisdp), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, D },
    { "webrtc_max_reopen_count", "Webrtc max retry count", OFFSET(max_reopen_count), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 3, D },
    { "webrtc_enable_ipv6_first", "Webrtc enable ipv6 first", OFFSET(enable_ipv6_first), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 1, D },

    // belows are read only
    { "webrtc_client_ip", "Webrtc client ip", OFFSET(client_ip), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_EXPORT | AV_OPT_FLAG_READONLY },
    { "webrtc_signal_server_ip", "Webrtc signal server ip", OFFSET(signal_server_ip), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_EXPORT | AV_OPT_FLAG_READONLY },
    { "webrtc_data_server_ip", "Webrtc data server ip", OFFSET(data_server_ip), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_EXPORT | AV_OPT_FLAG_READONLY },
    { "webrtc_jitter_delay", "Webrtc internal jitter delay in ms", OFFSET(jitter_delay_ms), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, INT_MAX, AV_OPT_FLAG_EXPORT | AV_OPT_FLAG_READONLY },
    { NULL }
};

static const AVClass webrtc_class = {
    .class_name = "webrtc",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVInputFormat ff_webrtc_demuxer = {
    .name           = "webrtc",
    .long_name      = "webrtc demuxer",
    .priv_data_size = sizeof(WEBRTCContext),
    .read_probe     = webrtc_read_probe,
    .read_header    = webrtc_read_header,
    .read_packet    = webrtc_read_packet,
    .read_close     = webrtc_read_close,
    .extensions      = "webrtc",
    .priv_class     = &webrtc_class,
    .flags          = AVFMT_NOFILE,
};

#ifdef BUILD_AS_PLUGIN
void register_webrtc_demuxer()
{
    av_log(NULL, AV_LOG_INFO, "register_webrtc_demuxer\n");
    av_register_input_format(&ff_webrtc_demuxer);
}
#endif
