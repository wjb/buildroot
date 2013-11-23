/*
 * Monkey's Audio APE demuxer
 * Copyright (c) 2007 Benjamin Zores <ben@geexbox.org>
 *  based upon libdemac from Dave Chapman.
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

#include <stdio.h>

#include "libavutil/intreadwrite.h"
#include "avformat.h"
#include "apetag.h"

/* The earliest and latest file formats supported by this library */
#define APE_MIN_VERSION 3950
#define APE_MAX_VERSION 3990

#define MAC_FORMAT_FLAG_8_BIT                 1 // is 8-bit [OBSOLETE]
#define MAC_FORMAT_FLAG_CRC                   2 // uses the new CRC32 error detection [OBSOLETE]
#define MAC_FORMAT_FLAG_HAS_PEAK_LEVEL        4 // uint32 nPeakLevel after the header [OBSOLETE]
#define MAC_FORMAT_FLAG_24_BIT                8 // is 24-bit [OBSOLETE]
#define MAC_FORMAT_FLAG_HAS_SEEK_ELEMENTS    16 // has the number of seek elements after the peak level
#define MAC_FORMAT_FLAG_CREATE_WAV_HEADER    32 // create the wave header on decompression (not stored)

#define MAC_SUBFRAME_SIZE 4608

#define APE_EXTRADATA_SIZE 6

typedef struct {
    int64_t pos;
    int nblocks;
    int size;
    int skip;
    int64_t pts;
} APEFrame;

typedef struct {
    /* Derived fields */
    uint32_t junklength;
    uint32_t firstframe;
    uint32_t totalsamples;
    int currentframe;
    APEFrame *frames;

    /* Info from Descriptor Block */
    char magic[4];
    int16_t fileversion;
    int16_t padding1;
    uint32_t descriptorlength;
    uint32_t headerlength;
    uint32_t seektablelength;
    uint32_t wavheaderlength;
    uint32_t audiodatalength;
    uint32_t audiodatalength_high;
    uint32_t wavtaillength;
    uint8_t md5[16];

    /* Info from Header Block */
    uint16_t compressiontype;
    uint16_t formatflags;
    uint32_t blocksperframe;
    uint32_t finalframeblocks;
    uint32_t totalframes;
    uint16_t bps;
    uint16_t channels;
    uint32_t samplerate;

    /* Seektable */
    uint32_t *seektable;
} APEContext;

static int ape_probe(AVProbeData * p)
{
    if (p->buf[0] == 'M' && p->buf[1] == 'A' && p->buf[2] == 'C' && p->buf[3] == ' ')
        return AVPROBE_SCORE_MAX;

    return 0;
}

static void ape_dumpinfo(AVFormatContext * s, APEContext * ape_ctx)
{
#ifdef DEBUG
    int i;

    av_log(s, AV_LOG_DEBUG, "Descriptor Block:\n\n");
    av_log(s, AV_LOG_DEBUG, "magic                = \"%c%c%c%c\"\n", ape_ctx->magic[0], ape_ctx->magic[1], ape_ctx->magic[2], ape_ctx->magic[3]);
    av_log(s, AV_LOG_DEBUG, "fileversion          = %"PRId16"\n", ape_ctx->fileversion);
    av_log(s, AV_LOG_DEBUG, "descriptorlength     = %"PRIu32"\n", ape_ctx->descriptorlength);
    av_log(s, AV_LOG_DEBUG, "headerlength         = %"PRIu32"\n", ape_ctx->headerlength);
    av_log(s, AV_LOG_DEBUG, "seektablelength      = %"PRIu32"\n", ape_ctx->seektablelength);
    av_log(s, AV_LOG_DEBUG, "wavheaderlength      = %"PRIu32"\n", ape_ctx->wavheaderlength);
    av_log(s, AV_LOG_DEBUG, "audiodatalength      = %"PRIu32"\n", ape_ctx->audiodatalength);
    av_log(s, AV_LOG_DEBUG, "audiodatalength_high = %"PRIu32"\n", ape_ctx->audiodatalength_high);
    av_log(s, AV_LOG_DEBUG, "wavtaillength        = %"PRIu32"\n", ape_ctx->wavtaillength);
    av_log(s, AV_LOG_DEBUG, "md5                  = ");
    for (i = 0; i < 16; i++)
         av_log(s, AV_LOG_DEBUG, "%02x", ape_ctx->md5[i]);
    av_log(s, AV_LOG_DEBUG, "\n");

    av_log(s, AV_LOG_DEBUG, "\nHeader Block:\n\n");

    av_log(s, AV_LOG_DEBUG, "compressiontype      = %"PRIu16"\n", ape_ctx->compressiontype);
    av_log(s, AV_LOG_DEBUG, "formatflags          = %"PRIu16"\n", ape_ctx->formatflags);
    av_log(s, AV_LOG_DEBUG, "blocksperframe       = %"PRIu32"\n", ape_ctx->blocksperframe);
    av_log(s, AV_LOG_DEBUG, "finalframeblocks     = %"PRIu32"\n", ape_ctx->finalframeblocks);
    av_log(s, AV_LOG_DEBUG, "totalframes          = %"PRIu32"\n", ape_ctx->totalframes);
    av_log(s, AV_LOG_DEBUG, "bps                  = %"PRIu16"\n", ape_ctx->bps);
    av_log(s, AV_LOG_DEBUG, "channels             = %"PRIu16"\n", ape_ctx->channels);
    av_log(s, AV_LOG_DEBUG, "samplerate           = %"PRIu32"\n", ape_ctx->samplerate);

    av_log(s, AV_LOG_DEBUG, "\nSeektable\n\n");
    if ((ape_ctx->seektablelength / sizeof(uint32_t)) != ape_ctx->totalframes) {
        av_log(s, AV_LOG_DEBUG, "No seektable\n");
    } else {
        for (i = 0; i < ape_ctx->seektablelength / sizeof(uint32_t); i++) {
            if (i < ape_ctx->totalframes - 1) {
                av_log(s, AV_LOG_DEBUG, "%8d   %d (%d bytes)\n", i, ape_ctx->seektable[i], ape_ctx->seektable[i + 1] - ape_ctx->seektable[i]);
            } else {
                av_log(s, AV_LOG_DEBUG, "%8d   %d\n", i, ape_ctx->seektable[i]);
            }
        }
    }

    av_log(s, AV_LOG_DEBUG, "\nFrames\n\n");
    for (i = 0; i < ape_ctx->totalframes; i++)
        av_log(s, AV_LOG_DEBUG, "%8d   %8"PRId64" %8d (%d samples)\n", i,
               ape_ctx->frames[i].pos, ape_ctx->frames[i].size,
               ape_ctx->frames[i].nblocks);

    av_log(s, AV_LOG_DEBUG, "\nCalculated information:\n\n");
    av_log(s, AV_LOG_DEBUG, "junklength           = %"PRIu32"\n", ape_ctx->junklength);
    av_log(s, AV_LOG_DEBUG, "firstframe           = %"PRIu32"\n", ape_ctx->firstframe);
    av_log(s, AV_LOG_DEBUG, "totalsamples         = %"PRIu32"\n", ape_ctx->totalsamples);
#endif
}

static int ape_read_header(AVFormatContext * s, AVFormatParameters * ap)
{
    AVIOContext *pb = s->pb;
    APEContext *ape = s->priv_data;
    AVStream *st;
    uint32_t tag;
    int i;
    int total_blocks, final_size = 0;
    int64_t pts, file_size;

    /* Skip any leading junk such as id3v2 tags */
    ape->junklength = avio_tell(pb);

    tag = avio_rl32(pb);
    if (tag != MKTAG('M', 'A', 'C', ' '))
        return -1;

    ape->fileversion = avio_rl16(pb);

    if (ape->fileversion < APE_MIN_VERSION || ape->fileversion > APE_MAX_VERSION) {
        av_log(s, AV_LOG_ERROR, "Unsupported file version - %"PRId16".%02"PRId16"\n",
               ape->fileversion / 1000, (ape->fileversion % 1000) / 10);
        return -1;
    }

    if (ape->fileversion >= 3980) {
        ape->padding1             = avio_rl16(pb);
        ape->descriptorlength     = avio_rl32(pb);
        ape->headerlength         = avio_rl32(pb);
        ape->seektablelength      = avio_rl32(pb);
        ape->wavheaderlength      = avio_rl32(pb);
        ape->audiodatalength      = avio_rl32(pb);
        ape->audiodatalength_high = avio_rl32(pb);
        ape->wavtaillength        = avio_rl32(pb);
        avio_read(pb, ape->md5, 16);

        /* Skip any unknown bytes at the end of the descriptor.
           This is for future compatibility */
        if (ape->descriptorlength > 52)
            avio_skip(pb, ape->descriptorlength - 52);

        /* Read header data */
        ape->compressiontype      = avio_rl16(pb);
        ape->formatflags          = avio_rl16(pb);
        ape->blocksperframe       = avio_rl32(pb);
        ape->finalframeblocks     = avio_rl32(pb);
        ape->totalframes          = avio_rl32(pb);
        ape->bps                  = avio_rl16(pb);
        ape->channels             = avio_rl16(pb);
        ape->samplerate           = avio_rl32(pb);
    } else {
        ape->descriptorlength = 0;
        ape->headerlength = 32;

        ape->compressiontype      = avio_rl16(pb);
        ape->formatflags          = avio_rl16(pb);
        ape->channels             = avio_rl16(pb);
        ape->samplerate           = avio_rl32(pb);
        ape->wavheaderlength      = avio_rl32(pb);
        ape->wavtaillength        = avio_rl32(pb);
        ape->totalframes          = avio_rl32(pb);
        ape->finalframeblocks     = avio_rl32(pb);

        if (ape->formatflags & MAC_FORMAT_FLAG_HAS_PEAK_LEVEL) {
            avio_skip(pb, 4); /* Skip the peak level */
            ape->headerlength += 4;
        }

        if (ape->formatflags & MAC_FORMAT_FLAG_HAS_SEEK_ELEMENTS) {
            ape->seektablelength = avio_rl32(pb);
            ape->headerlength += 4;
            ape->seektablelength *= sizeof(int32_t);
        } else
            ape->seektablelength = ape->totalframes * sizeof(int32_t);

        if (ape->formatflags & MAC_FORMAT_FLAG_8_BIT)
            ape->bps = 8;
        else if (ape->formatflags & MAC_FORMAT_FLAG_24_BIT)
            ape->bps = 24;
        else
            ape->bps = 16;

        if (ape->fileversion >= 3950)
            ape->blocksperframe = 73728 * 4;
        else if (ape->fileversion >= 3900 || (ape->fileversion >= 3800  && ape->compressiontype >= 4000))
            ape->blocksperframe = 73728;
        else
            ape->blocksperframe = 9216;

        /* Skip any stored wav header */
        if (!(ape->formatflags & MAC_FORMAT_FLAG_CREATE_WAV_HEADER))
            avio_skip(pb, ape->wavheaderlength);
    }

    if(!ape->totalframes){
        av_log(s, AV_LOG_ERROR, "No frames in the file!\n");
        return AVERROR(EINVAL);
    }
    if(ape->totalframes > UINT_MAX / sizeof(APEFrame)){
        av_log(s, AV_LOG_ERROR, "Too many frames: %"PRIu32"\n",
               ape->totalframes);
        return -1;
    }
    if (ape->seektablelength && (ape->seektablelength / sizeof(*ape->seektable)) < ape->totalframes) {
        av_log(s, AV_LOG_ERROR, "Number of seek entries is less than number of frames: %ld vs. %"PRIu32"\n",
               ape->seektablelength / sizeof(*ape->seektable), ape->totalframes);
        return AVERROR_INVALIDDATA;
    }
    ape->frames       = av_malloc(ape->totalframes * sizeof(APEFrame));
    if(!ape->frames)
        return AVERROR(ENOMEM);
    ape->firstframe   = ape->junklength + ape->descriptorlength + ape->headerlength + ape->seektablelength + ape->wavheaderlength;
    ape->currentframe = 0;


    ape->totalsamples = ape->finalframeblocks;
    if (ape->totalframes > 1)
        ape->totalsamples += ape->blocksperframe * (ape->totalframes - 1);

    if (ape->seektablelength > 0) {
        ape->seektable = av_malloc(ape->seektablelength);
        for (i = 0; i < ape->seektablelength / sizeof(uint32_t); i++)
            ape->seektable[i] = avio_rl32(pb);
    }

    ape->frames[0].pos     = ape->firstframe;
    ape->frames[0].nblocks = ape->blocksperframe;
    ape->frames[0].skip    = 0;
    for (i = 1; i < ape->totalframes; i++) {
        ape->frames[i].pos      = ape->seektable[i] + ape->junklength;
        ape->frames[i].nblocks  = ape->blocksperframe;
        ape->frames[i - 1].size = ape->frames[i].pos - ape->frames[i - 1].pos;
        ape->frames[i].skip     = (ape->frames[i].pos - ape->frames[0].pos) & 3;
    }
    //ape->frames[ape->totalframes - 1].size    = ape->finalframeblocks * 4;
    ape->frames[ape->totalframes - 1].nblocks = ape->finalframeblocks;
    file_size = avio_size(pb);
    if (file_size > 0) {
        final_size = file_size - ape->frames[ape->totalframes - 1].pos -
                     ape->wavtaillength;
        final_size -= final_size & 3;
    }
    if (file_size <= 0 || final_size <= 0)
        final_size = ape->finalframeblocks * 8;
    ape->frames[ape->totalframes - 1].size = final_size;
    for (i = 0; i < ape->totalframes; i++) {
        if(ape->frames[i].skip){
            ape->frames[i].pos  -= ape->frames[i].skip;
            ape->frames[i].size += ape->frames[i].skip;
        }
        ape->frames[i].size = (ape->frames[i].size + 3) & ~3;
    }


    ape_dumpinfo(s, ape);

    /* try to read APE tags */
    if (pb->seekable) {
        ff_ape_parse_tag(s);
        avio_seek(pb, 0, SEEK_SET);
    }

    av_log(s, AV_LOG_DEBUG, "Decoding file - v%d.%02d, compression level %"PRIu16"\n",
           ape->fileversion / 1000, (ape->fileversion % 1000) / 10,
           ape->compressiontype);

    /* now we are ready: build format streams */
    st = av_new_stream(s, 0);
    if (!st)
        return -1;

    total_blocks = (ape->totalframes == 0) ? 0 : ((ape->totalframes - 1) * ape->blocksperframe) + ape->finalframeblocks;

    st->codec->codec_type      = AVMEDIA_TYPE_AUDIO;
    st->codec->codec_id        = CODEC_ID_APE;
    st->codec->codec_tag       = MKTAG('A', 'P', 'E', ' ');
    st->codec->channels        = ape->channels;
    st->codec->sample_rate     = ape->samplerate;
    st->codec->bits_per_coded_sample = ape->bps;
    st->codec->frame_size      = MAC_SUBFRAME_SIZE;

    st->nb_frames = ape->totalframes;
    st->start_time = 0;
    st->duration  = total_blocks / MAC_SUBFRAME_SIZE;
    av_set_pts_info(st, 64, MAC_SUBFRAME_SIZE, ape->samplerate);

    st->codec->extradata = av_malloc(APE_EXTRADATA_SIZE);
    st->codec->extradata_size = APE_EXTRADATA_SIZE;
    AV_WL16(st->codec->extradata + 0, ape->fileversion);
    AV_WL16(st->codec->extradata + 2, ape->compressiontype);
    AV_WL16(st->codec->extradata + 4, ape->formatflags);

    pts = 0;
    for (i = 0; i < ape->totalframes; i++) {
        ape->frames[i].pts = pts;
        av_add_index_entry(st, ape->frames[i].pos, ape->frames[i].pts, 0, 0, AVINDEX_KEYFRAME);
        pts += ape->blocksperframe / MAC_SUBFRAME_SIZE;
    }

    return 0;
}

static int ape_read_packet(AVFormatContext * s, AVPacket * pkt)
{
    int ret;
    int nblocks;
    APEContext *ape = s->priv_data;
    uint32_t extra_size = 8;

    if (url_feof(s->pb))
	return AVERROR_EOF;
        //return AVERROR(EIO);
    if (ape->currentframe >= ape->totalframes)
	return AVERROR_EOF;
        //return AVERROR(EIO);
    //find the correct currentframe when seeking
    while(0)
    {
        if(ape->frames[ape->currentframe].pos==s->pb->pos)
            break;
        if(ape->currentframe==0&&ape->frames[ape->currentframe].pos>s->pb->pos)
            break;
        if(ape->currentframe==ape->totalframes-1)
            break;
        if(ape->frames[ape->currentframe].pos<s->pb->pos&&ape->frames[ape->currentframe+1].pos>s->pb->pos)
	{
	    int len=ape->frames[ape->currentframe+1].pos-ape->frames[ape->currentframe].pos;
            if(s->pb->pos>ape->frames[ape->currentframe].pos+len/2)
                ape->currentframe+=1;
            break;
	}
        else if(ape->frames[ape->currentframe+1].pos<s->pb->pos)
            ape->currentframe+=1;
        else
            ape->currentframe-=1;
    }
    avio_seek (s->pb, ape->frames[ape->currentframe].pos, SEEK_SET);

    /* Calculate how many blocks there are in this frame */
    if (ape->currentframe == (ape->totalframes - 1))
        nblocks = ape->finalframeblocks;
    else
        nblocks = ape->blocksperframe;

    if (av_new_packet(pkt,  ape->frames[ape->currentframe].size + extra_size) < 0)
        return AVERROR(ENOMEM);

    AV_WL32(pkt->data    , nblocks);
    AV_WL32(pkt->data + 4, ape->frames[ape->currentframe].skip);
    ret = avio_read(s->pb, pkt->data + extra_size, ape->frames[ape->currentframe].size);

    pkt->pts = ape->frames[ape->currentframe].pts;
    pkt->stream_index = 0;

    /* note: we need to modify the packet size here to handle the last
       packet */
    if(ret < 0 ){	
	 av_log(s, AV_LOG_INFO,"ape read frame fail,skip this frame.frame size %d,ret %d \n",ape->frames[ape->currentframe].size,ret);
	 pkt->size = 0;
    }
    else
    	pkt->size = ret + extra_size;

    ape->currentframe++;

    return 0;
}

static int ape_read_close(AVFormatContext * s)
{
    APEContext *ape = s->priv_data;

    av_freep(&ape->frames);
    av_freep(&ape->seektable);
    return 0;
}

static int ape_read_seek(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
    AVStream *st = s->streams[stream_index];
    APEContext *ape = s->priv_data;
    int index = av_index_search_timestamp(st, timestamp, flags);

    if (index < 0)
        return -1;

    ape->currentframe = index;
    return 0;
}

AVInputFormat ff_ape_demuxer = {
    "ape",
    NULL_IF_CONFIG_SMALL("Monkey's Audio"),
    sizeof(APEContext),
    ape_probe,
    ape_read_header,
    ape_read_packet,
    ape_read_close,
    ape_read_seek,
    .extensions = "ape,apl,mac"
};
