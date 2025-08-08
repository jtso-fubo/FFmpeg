/*
 * Copyright (c) 2012 Stefano Sabatini
 * Copyright (c) 2014 Clément Bœsch
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file libavcodec motion vectors extraction API usage example
 * @example extract_mvs.c
 *
 * Read from input file, decode video stream and print a motion vectors
 * representation to stdout.
 */

#include <libavutil/motion_vector.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include "libavutil/timestamp.h"

static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *video_dec_ctx = NULL;
static AVStream *video_stream = NULL;
static const char *src_filename = NULL;

static int video_stream_idx = -1;
static AVFrame *frame = NULL;
static int video_frame_count = 0;

typedef struct FrameData {
    int64_t pkt_pos;
    int     pkt_size;
} FrameData;

static int decode_packet(AVPacket *pkt)
{
    FrameData *fd;

    pkt->opaque_ref = av_buffer_allocz(sizeof(*fd));
    if (!pkt->opaque_ref) {
        fprintf(stderr, "Error while allocating for opaque ref\n");
        return 0;
    }
    fd = (FrameData*)pkt->opaque_ref->data;
    fd->pkt_pos  = pkt->pos;
    fd->pkt_size = pkt->size;
    
    int ret = avcodec_send_packet(video_dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error while sending a packet to the decoder: %s\n", av_err2str(ret));
        return ret;
    }

    while (ret >= 0)  {
        ret = avcodec_receive_frame(video_dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Error while receiving a frame from the decoder: %s\n", av_err2str(ret));
            return ret;
        }

        if (ret >= 0) {
            int i;
            AVFrameSideData *sd;

            video_frame_count++;
            sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);

            FrameData *fd = frame->opaque_ref ? (FrameData*)frame->opaque_ref->data : NULL;
            if (fd == NULL) {
                printf("FD is NULL");
            }
            int pkt_size = fd && fd->pkt_size != -1 ? fd->pkt_size : 0;
            if (sd) {
                const AVMotionVector *mvs = (const AVMotionVector *)sd->data;
                int sources[2];
                int sources_count = 0;
                for (i = 0; i < sd->size / sizeof(*mvs); i++) {
                    const AVMotionVector *mv = &mvs[i];
                    int is_duplicate = 0;

                    // Check if input[i] is already in unique[]
                    for (int j = 0; j < sources_count; j++) {
                        if (mv->source == sources[j]) {
                            is_duplicate = 1;
                            break;
                        }
                    }

                    // If not found, add to unique[]
                    if (!is_duplicate) {
                        sources[sources_count] = mv->source;
                        sources_count++;
                    }
                    // printf("%d,%2d,%2d,%2d,%4d,%4d,%4d,%4d,0x%"PRIx64",%4d,%4d,%4d\n",
                    //     video_frame_count, mv->source,
                    //     mv->w, mv->h, mv->src_x, mv->src_y,
                    //     mv->dst_x, mv->dst_y, mv->flags,
                    //     mv->motion_x, mv->motion_y, mv->motion_scale);
                }
                printf("index=%d|sources=", video_frame_count);
                for (int k = 0; k < sources_count; k++) {
                    printf("%d", sources[k]);
                    if (k != sources_count - 1) {
                        printf(",");
                    }
                }
                printf("|pts=%s|pts_time=%s|dts_time=%s|pict_type=%c|pkt_size=%d\n", av_ts2str(frame->pts), av_ts2timestr(frame->pts, &video_stream->time_base), av_ts2timestr(frame->pkt_dts, &video_stream->time_base), av_get_picture_type_char(frame->pict_type), pkt_size);
            } else {
                printf("index=%d|pts=%s|pts_time=%s|dts_time=%s|pict_type=%c|pkt_size=%d\n", video_frame_count, av_ts2str(frame->pts), av_ts2timestr(frame->pts, &video_stream->time_base), av_ts2timestr(frame->pkt_dts, &video_stream->time_base), av_get_picture_type_char(frame->pict_type), pkt_size);
            }
            av_frame_unref(frame);
        }
    }

    return 0;
}

static int open_codec_context(AVFormatContext *fmt_ctx, enum AVMediaType type)
{
    int ret;
    AVStream *st;
    AVCodecContext *dec_ctx = NULL;
    const AVCodec *dec = NULL;
    AVDictionary *opts = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, &dec, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename);
        return ret;
    } else {
        int stream_idx = ret;
        st = fmt_ctx->streams[stream_idx];

        dec_ctx = avcodec_alloc_context3(dec);
        if (!dec_ctx) {
            fprintf(stderr, "Failed to allocate codec\n");
            return AVERROR(EINVAL);
        }

        ret = avcodec_parameters_to_context(dec_ctx, st->codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters to codec context\n");
            return ret;
        }

        /* Init the video decoder */
        av_dict_set(&opts, "flags2", "+export_mvs", 0);
        ret = avcodec_open2(dec_ctx, dec, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }

        video_stream_idx = stream_idx;
        video_stream = fmt_ctx->streams[video_stream_idx];
        video_dec_ctx = dec_ctx;
        video_dec_ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    }

    return 0;
}

int main(int argc, char **argv)
{
    int ret = 0;
    AVPacket *pkt = NULL;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <video>\n", argv[0]);
        exit(1);
    }
    src_filename = argv[1];

    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open source file %s\n", src_filename);
        exit(1);
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        exit(1);
    }

    open_codec_context(fmt_ctx, AVMEDIA_TYPE_VIDEO);

    av_dump_format(fmt_ctx, 0, src_filename, 0);

    if (!video_stream) {
        fprintf(stderr, "Could not find video stream in the input, aborting\n");
        ret = 1;
        goto end;
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate frame\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    printf("framenum,source,blockw,blockh,srcx,srcy,dstx,dsty,flags,motion_x,motion_y,motion_scale\n");

    /* read frames from the file */
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_stream_idx)
            ret = decode_packet(pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            break;
    }

    /* flush cached frames */
    decode_packet(NULL);

end:
    avcodec_free_context(&video_dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0;
}
