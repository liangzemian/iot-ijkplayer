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

#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"

#include "libavformat/avformat.h"
#include "libavformat/url.h"
#include <pthread.h>

#include "leb_connection_api.h"

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
    LebConnectionHandle *handle;
    int video_stream_index_in;
    int video_stream_index_out;
    int audio_stream_index_in;
    int audio_stream_index_out;
    LebVideoCodecType video_codec;
    LebAudioCodecType audio_codec;
    AVPacket video_pkt;
    AVPacket audio_pkt;
    PacketQueue queue;
    int max_queue_size;
    int64_t current_dts;
    int64_t current_pts;
    char video_header[1024];
    int  video_header_size;
    int  video_header_repeated;
    int  log_offset;
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

static void packet_queue_destroy(PacketQueue *q)
{
    AVPacketList *pkt, *nextpkt;

    for (pkt = q->first_pkt; pkt; pkt = nextpkt) {
        nextpkt = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
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

static int packet_queue_wait_start(PacketQueue *q, AVFormatContext *s, int64_t timeout) {
    int started;
    int loop = (timeout >= 0) ? FFMAX(timeout / 100000, 1) : -1;

    for (int i = 0; loop > 0 && i < loop; i++) {
        if (ff_check_interrupt(&s->interrupt_callback))
            return AVERROR_EXIT;

        pthread_mutex_lock(&q->mutex);
        if (!q->is_started) {
            int64_t t = av_gettime() + 100000;
            struct timespec tv = { .tv_sec  =  t / 1000000,
                    .tv_nsec = (t % 1000000) * 1000 };
            pthread_cond_timedwait(&q->condition, &q->mutex, &tv);
        }
        started = q->is_started;
        pthread_mutex_unlock(&q->mutex);
        if (started)
            return 0;
    }

    return AVERROR(ETIMEDOUT);
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

static int packet_queue_get(AVFormatContext *s, PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;
    int ret = 0;

    while (!ret) {
        if (ff_check_interrupt(&s->interrupt_callback)) {
            packet_queue_abort(q);
            ret = -1;
            break;
        }

        pthread_mutex_lock(&q->mutex);
        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
        } else {
            int64_t t = av_gettime() + 100000;
            struct timespec tv = { .tv_sec  =  t / 1000000,
                                   .tv_nsec = (t % 1000000) * 1000 };
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

static void OnLogCallback(void* context, const char* tag, LebLogLevel level, const char* msg) {
    WEBRTCContext *s = context;
    AVFormatContext *h = s->avctx;

    av_log(h, to_ff_log_level(s, level), "[lebconnection]%s%s\n", tag, msg);
}

static void OnVideoInfoCallback(void* context, LebVideoInfo info) {
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVFormatContext *h = s->avctx;
    AVStream *st;

    s->video_codec = info.codec_type;
    if (s->video_codec != kH264 && s->video_codec != kH265) {
        av_log(h, AV_LOG_ERROR,
                "OnVideoInfoCallback, unknown video codec %d\n",
                s->video_codec);
        return;
    }
    av_log(h, AV_LOG_INFO, "video codec %d\n", info.codec_type);

    st = avformat_new_stream(s->avctx, NULL);
    s->video_stream_index_out = st->index;
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    if (s->video_codec == kH264) {
        st->codecpar->codec_id = AV_CODEC_ID_H264;
    } else if (s->video_codec == kH265) {
        st->codecpar->codec_id = AV_CODEC_ID_H265;
    }
    st->need_parsing = AVSTREAM_PARSE_FULL;
    set_stream_pts_info(st, 64, 1, 1000);/* 64 bit pts in ms */

    if (info.extra_size > 0) {
        st->codecpar->extradata = av_malloc(info.extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (st->codecpar->extradata != NULL) {
            memset(st->codecpar->extradata + info.extra_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            memcpy(st->codecpar->extradata, info.extra_data, info.extra_size);
            st->codecpar->extradata_size = info.extra_size;
        } else {
            st->codecpar->extradata_size = 0;
        }
    }
}

static void OnAudioInfoCallback(void* context, LebAudioInfo info) {
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVFormatContext *h = s->avctx;
    AVStream *st;

    s->audio_codec = info.codec_type;
    if (s->audio_codec != kAac && s->audio_codec != kOpus) {
        av_log(h, AV_LOG_ERROR,
                "OnAudioInfoCallback, unknown audio codec %d\n",
                s->audio_codec);
        return;
    }
    av_log(h, AV_LOG_INFO, "audio codec %d\n", info.codec_type);

    st = avformat_new_stream(s->avctx, NULL);
    s->audio_stream_index_out = st->index;
    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    if (s->audio_codec == kAac) {
        st->codecpar->codec_id = AV_CODEC_ID_AAC;
    } else if (s->audio_codec == kOpus) {
        st->codecpar->codec_id = AV_CODEC_ID_OPUS;
    }
    st->need_parsing = AVSTREAM_PARSE_HEADERS;
    set_stream_pts_info(st, 64, 1, 1000);/* 64 bit pts in ms */
}

static void OnVideoDataCallback(void* context, LebEncodedVideoFrame videoframe)
{
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVPacket *pkt = &s->video_pkt;

    if (!s->current_dts && !s->current_pts && videoframe.size < 1024) {
        s->current_dts = videoframe.dts;
        s->current_pts = videoframe.pts;
        s->video_header_size = videoframe.size;
        memcpy(s->video_header, videoframe.data, videoframe.size);
    }

    if (!s->video_header_repeated && s->current_pts != videoframe.pts) {
        av_new_packet(pkt, s->video_header_size);
        memcpy(pkt->data, s->video_header, s->video_header_size);
        pkt->stream_index = s->video_stream_index_in;
        pkt->dts = s->current_dts;
        pkt->pts = s->current_pts;
        packet_queue_put(&s->queue, pkt, s);
        s->video_header_repeated = 1;
    }

    av_new_packet(pkt, videoframe.size);
    memcpy(pkt->data, videoframe.data, videoframe.size);
    pkt->stream_index = s->video_stream_index_in;
    pkt->dts = videoframe.dts;
    pkt->pts = videoframe.pts;
    packet_queue_put(&s->queue, pkt, s);
}

static void OnAudioDataCallback(void* context, LebEncodedAudioFrame audioframe)
{
    WEBRTCContext *s = (WEBRTCContext*)context;
    AVPacket *pkt = &s->audio_pkt;
    av_new_packet(pkt, audioframe.size);
    memcpy(pkt->data, audioframe.data, audioframe.size);
    pkt->stream_index = s->audio_stream_index_in;
    pkt->dts = audioframe.dts;
    pkt->pts = audioframe.pts;
    packet_queue_put(&s->queue, pkt, s);
}

static void OnMetaDataCallback(void* context, LebMetaData metadata) {
    av_log(context, AV_LOG_INFO, "OnMetaDataInfo %s\n", metadata.data);
}

static void OnStatsCallback(void* context, LebStats stats) {
    WEBRTCContext *s = context;
    AVFormatContext *h = s->avctx;

    av_log(h, to_ff_log_level(s, kInfo),
      "OnStats signal_ip: %s, media_ip:%s, dns_cost: %d, "
      "first video packet delay:%d, first audio packet delay: %d, "
      "rtt:%d audio_bytes:%" PRIu64" audio_nack:%u,audio_lost:%d,audio_pkts:%u, "
      "video_bytes:%" PRIu64" video_nack:%u video_lost:%d,video_pkts:%u\n",
      stats.signal_server_ip, stats.data_server_ip, stats.signal_dns_cost_ms,
      stats.first_video_received_cost_ms, stats.first_audio_received_cost_ms,
      stats.rtt_ms, stats.audio_bytes_received, stats.audio_nack_count,
      stats.audio_packets_lost, stats.audio_packets_received,
      stats.video_bytes_received, stats.video_nack_count,
      stats.video_packets_lost, stats.video_packets_received);
}

static int webrtc_open(AVFormatContext *h, const char *uri)
{
    WEBRTCContext *s = h->priv_data;
    LebConfig config;
    LebLogLevel level = kWarning;//SDK输出日志等级
    char* new_uri1 = NULL;
    char* new_uri2 = NULL;

    av_log(h, AV_LOG_INFO, "webrtc_open %s\n", uri);
    packet_queue_init(&s->queue, h);
    av_init_packet(&s->video_pkt);
    av_init_packet(&s->audio_pkt);

    memset(&config, 0, sizeof(LebConfig));
    s->video_codec = kNoVideo;
    s->audio_codec = kNoAudio;

    if(av_strstart(uri, "http://", NULL) && strstr(uri, ".sdp")) {
        new_uri1 = av_strireplace(uri, "http://", "webrtc://");
        new_uri2 = av_strireplace(new_uri1, ".sdp", "");
    } else if (av_strstart(uri, "https://", NULL) && strstr(uri, ".sdp")) {
        new_uri1 = av_strireplace(uri, "https://", "webrtc://");
        new_uri2 = av_strireplace(new_uri1, ".sdp", "");
    } else if (av_strstart(uri, "webrtc://", NULL))  {
        new_uri2 = av_strireplace(uri, ".sdp", "");
    } else {
        packet_queue_destroy(&s->queue);
        return AVERROR(EINVAL);
    }
    av_log(h, AV_LOG_INFO, "new uri %s\n", new_uri2);

    config.stream_url = new_uri2;
    config.remote_address = "webrtc.liveplay.myqcloud.com";//通用快直播信令域名或专属域名
    config.enable_udp_signal = 1;
    config.enable_aac = 1;
    config.enable_encryption = 0;
    config.enable_flex_fec = 1;
    config.receive_audio = 1;
    config.receive_video = 1;
    config.stats_period_ms = 1000;

    /**
     * 1. 创建LebConnecton, 并注册回调函数
     */
    s->handle = OpenLebConnection((void*)s, level);
    RegisterLogInfoCallback(s->handle, OnLogCallback);
    RegisterVideoInfoCallback(s->handle, OnVideoInfoCallback);
    RegisterAudioInfoCallback(s->handle, OnAudioInfoCallback);
    RegisterVideoDataCallback(s->handle, OnVideoDataCallback);
    RegisterAudioDataCallback(s->handle, OnAudioDataCallback);
    RegisterMetaDataCallback(s->handle, OnMetaDataCallback);
    RegisterStatsInfoCallback(s->handle, OnStatsCallback);

    /**
     * 2. 开始连接，内部走信令流程，直接建联拉流；
     */
    StartLebConnection(s->handle, config);

    av_free(new_uri1);
    av_free(new_uri2);
    av_log(h, AV_LOG_INFO, "webrtc_open exit\n");
    return 0;
}

static int webrtc_close(AVFormatContext *h)
{
    av_log(h, AV_LOG_INFO, "webrtc_close\n");
    WEBRTCContext *s = h->priv_data;
#ifdef DEBUG
    // 测试GetStats， 统计结果通过异步回调OnStatsCallback返回
    GetStats(s->handle);
    av_usleep(1000000);
#endif
    packet_queue_abort(&s->queue);
    /**
     * 3. 停止和关闭LebConnection
     */
    StopLebConnection(s->handle);
    CloseLebConnection(s->handle);

    packet_queue_destroy(&s->queue);

    av_log(h, AV_LOG_INFO, "webrtc_close exit\n");
    return 0;
}

static int webrtc_probe(AVProbeData *p)
{
    if (av_strstart(p->filename, "webrtc:", NULL))
        return AVPROBE_SCORE_MAX;
    return 0;
}

static int webrtc_read_header(AVFormatContext *s)
{
    WEBRTCContext *h = s->priv_data;
    int ret;

    av_log(s, AV_LOG_INFO, "webrtc_read_header, filename %s\n", s->filename);

    s->flags |= AVFMT_FLAG_GENPTS;
    s->fps_probe_size = 0;
    s->probesize = 32*1024;
    h->avctx = s;
    h->video_stream_index_in = 0;
    h->audio_stream_index_in = 1;
    h->video_stream_index_out = -1;
    h->audio_stream_index_out = -1;
    ret = webrtc_open(s, s->filename);
    if (ret) {
        av_log(s, AV_LOG_ERROR, "webrtc_read_header: webrtc_open failed, %s\n", av_err2str(ret));
        return ret;
    }

    // 5秒收不到数据，超时退出
    ret = packet_queue_wait_start(&h->queue, s, INT64_C(1000) * 5000);
    if (ret) {
        av_log(s, AV_LOG_ERROR, "webrtc_read_header wait failed, %s\n", av_err2str(ret));
        webrtc_close(s);
        return ret;
    }

    av_log(s, AV_LOG_INFO, "webrtc_read_header exit\n");
    return 0;
}

static int webrtc_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    int ret;
    WEBRTCContext *h = s->priv_data;

    do {
        ret = packet_queue_get(s, &h->queue, pkt);
        if (ret < 0)
            break;

        if (pkt->stream_index == h->video_stream_index_in) {
            if (h->video_stream_index_out < 0)
                ret = 0; /* drop video packet and continue */
            else
                pkt->stream_index = h->video_stream_index_out;
        } else if (pkt->stream_index == h->audio_stream_index_in) {
            if (h->audio_stream_index_out < 0)
                ret = 0; /* drop audio packet and continue */
            else
                pkt->stream_index = h->audio_stream_index_out;
        } else {
            ret = 0;
        }

        if (!ret) {
            av_log(s, AV_LOG_INFO, "drop pkt with index %d and continue\n",
                   pkt->stream_index);
            av_packet_unref(pkt);
        }
    } while (!ret);

    ret = ret > 0 ? 0 : AVERROR_EOF;
    if (ret)
        av_log(s, AV_LOG_WARNING, "webrtc_read_packet, %s\n", av_err2str(ret));
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
    .read_probe     = webrtc_probe,
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
