/*
 * Dynacolor MVC Common Definitions
 * Copyright (c) 2020-2021 Tom Needham
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

#ifndef AVFORMAT_DYNACOLOR_H
#define AVFORMAT_DYNACOLOR_H

#include "avformat.h"

// The data structure of the streaming video:
// [16-byte DynacolorBasicIdx header] + [16-byte DynacolorExtraIdx header] + ['SuperExtraSize' bytes] + [32-byte PES header] + [video body]
// The 'SuperExtraSize' is defined in the 'DynacolorExtraIdx' structure to store the additional information, and usually equals zero.

#define DYNACOLOR_PES_HEADER_SIZE 32
#define DYNACOLOR_EXTRA_SIZE 16
#define DYNACOLOR_SUPEREXTRA_SIZE 108
#define DYNACOLOR_SUPEREXTRA_TITLE_SIZE 12
#define DYNACOLOR_SUPEREXTRA_EXTRA_DATA_SIZE 96

#define DYNACOLOR_H265_FORMAT_PREFIX 0x0B // NOTE: Not Officially supported just trying it out
#define DYNACOLOR_AUDIO_FORMAT_PREFIX 0x0C
#define DYNACOLOR_MPEG4_FORMAT_PREFIX 0x0D
#define DYNACOLOR_JPEG_FORMAT_PREFIX 0x0E
#define DYNACOLOR_H264_FORMAT_PREFIX 0x0F

// Big Endian
// DG600 uses big-endian CPU

// Basic Idx: 16 bytes
typedef struct
{
    unsigned int Header1 : 8;   // [2] "@X" -- X:device type, DG600 is "6" @6
    unsigned int Header2 : 8;   // [2] "@X" -- X:device type, DG600 is "6" @6
    unsigned int reserved : 4;  // 4 bits
    unsigned int Source : 1;    // 1 bit  : (0 - DVR, 1 - IP Dev)
    unsigned int WidthBase : 2; // 0:720 , 1:704, 2:640
    unsigned int Reserved0 : 1; // 1 bit  : (0 - Disable(backward compatibile), 1 - Enable)
                                // this is basically for VSS use since VSS needs to support 96 channels, and
                                // there is originally 5 bits only for channel number in the header.
                                // With these two extra bits, the channel number becomes ((Channel_Ext<<5)|Channel).
    unsigned int Channel_Ext : 2;
    unsigned int StreamType : 2; // 0-NormalStream, 1-VStream, 2-DualStream
    unsigned int QI : 4;         // 4 bits : QI (0~16)

    unsigned int Size; // [4]
    unsigned int Time; // [4]

    unsigned int NTSC_PAL : 1; // 1 bit : 0:NTSC/1:PAL
    unsigned int ImgMode : 1;  // 1 bit : 0:Live/1:Playback
    unsigned int Audio : 1;    // 1 bit : 0:Video/1:Audio
    unsigned int DST : 1;      // 1 bit : DST
    unsigned int Covert : 1;   // 1 bit : Covert
    unsigned int Vloss : 1;    // 1 bit : Video loss
    unsigned int AlarmIn : 1;  // 1 bit : Alarm In
    unsigned int Motion : 1;   // 1 bit : Motion

    unsigned int ExtraEnable : 1; // 1 bit  : 0: no extra         1: have extra
    unsigned int EventEnable : 1; // 1 bit  : 0: Normal status    1: Event duration
    unsigned int PPS : 6;         // 6 bits : Per-channel PPS Idx (0~127 level)

    unsigned int Type : 3;    // 3 bits : type (0~7) 0: none 1: I-pic, 2: P-Pic, 3: B-Pic
    unsigned int Channel : 5; // 5 bits : Channel No (0~31)
    unsigned int Chksum : 8;  // [1]

} DynacolorBasicIdx;

// Extra Idx: 16 bytes

typedef struct
{
    unsigned int DST_Minutes : 8; // 0~7 = 0 ~ 120 minutes
    unsigned int IsDST_S_W : 1;   //   0 = summer time , 1 =winter time
    unsigned int OverSpeed : 1;
    unsigned int SkipPicCnt : 5;

    // use one bit to represent exception alarm
    // can't get exactually exception no here
    unsigned int Exception : 1;
    unsigned int SuperExtraSize : 16;

    unsigned int Serno; // [4]

    // Original tEventInfo16CH was broken 'cause of the byte alignment problem
    // - use the following directly
    // the first bit can descript first channel's evnet status, so that it can totally script 16 channels' status.
    // if IP camera, the first bit also descripts the first IP cmaera's status.
    char AlmIn[2];  // 16 channel, if Source of DynacolorBasicIdx = 0, it means analog camera's status. If Source = 1, it means IP camera's.
    char Vloss[2];  // 16 channel, if Source of DynacolorBasicIdx = 0, it means analog camera's status. If Source = 1, it means IP camera's.
    char Motion[2]; // 16 channel, if Source of DynacolorBasicIdx = 0, it means analog camera's status. If Source = 1, it means IP camera's.

    unsigned char IsDVRRec : 2;

    // For TimeZone setting
    // - 0 means OFF (backward compatibility)
    // - Delta : 30 min
    // - Start from : -12:00
    // - Aka.
    //    1 : -12:00 (-720 min)
    //    2 : -11:30 (-690 min)
    //    3 : -11:00 (-660 min)
    //    ....
    //   25 : +00:00 (+000 min)
    //    ....
    //   41 : +08:00 (+480 min)
    //   42 : +08:30 (+510 min)
    //   43 : +09:00 (+540 min)
    //    ....
    //   51 : +13:00 (+780 min)

    unsigned char TimeZone : 6;
    char Chksum;

} DynacolorExtraIdx;

// Super Extra Index :
//  type = 0
//  remain_size = 108
// structure to store cam title & extra data(GPS/Text)

typedef struct
{

    // the type for DynacolorExtraIdx is fixed to 0
    int type;

    // the remain_size for DynacolorExtraIdx is fixed to : 12 + 96 = 108
    int remain_size;

    // CAM_TIT_LEN
    char title[12];

    //   Merge original account0 ~ account3
    // - GPS & Text support will use the same place
    // - REC_INDEX_EXTRA_TYPE, REC_INDEX_EXTRA_CH, REC_INDEX_EXTRA_LENGTH are
    //   used for some specific info
    // - refer to list.h for detail

    char extra_data[96];

} DynacolorSuperExtraIdx;

typedef struct
{
    DynacolorBasicIdx Basic;
    DynacolorExtraIdx Extra;
    DynacolorSuperExtraIdx SuperExtra;
} DynacolorHeader;

typedef struct
{
    // 4 bytes
    unsigned int start_code0 : 8; // must be 0x00
    unsigned int start_code1 : 8; // must be 0x00
    unsigned int start_code2 : 8; // must be 0x01
    unsigned int format_id : 4;   // JPEG:0xd, MPEG4:0xe, H.264:0xf, Audio:0xc
    unsigned int channel_id : 4;  // channel id from 0 to 15 for CH1 to CH16

    unsigned int unused_0;
    unsigned int unused_1;
    unsigned int unused_2;
    unsigned int unused_3;
    unsigned int unused_4;

    // size of the video body and this PES header (total 3 bytes including the markers)
    unsigned int size_bit7to0 : 8;   // from bit7 to bit0
    unsigned int size_bit10to8 : 3;  // from bit10 to bit8
    unsigned int size_bit14to11 : 4; // from bit14 to bit11
    unsigned int size_bit21to15 : 7; // from bit21 to bit15
    unsigned int size_marker0 : 1;   // must be 0x1
    unsigned int size_marker1 : 1;   // must be 0x1

    // 1 byte for the picture type
    unsigned int picture_type : 8;   // 1: I-slice, 2: P-slice, 3: B-slice
    unsigned int is_interleaved : 1; // 0: even/odd fields are separated horizontally
                                     // 1: even/odd fields are interleaved
    unsigned int field_id : 2;       // 0: odd field, 1: even field, 2/3: undefined
    unsigned int unused_5 : 29;

} DynacolorPesHeader;

typedef struct {
    DynacolorHeader header;
    DynacolorPesHeader pes_header;
} DynacolorContext;

int ff_dyna_get_stream_format(AVFormatContext *ctx, DynacolorHeader *header);

int ff_dyna_read_file_header(AVFormatContext *ctx, AVIOContext *pb, unsigned char *pes_data, DynacolorPesHeader *pes, unsigned int *size,
                               time_t *time, DynacolorHeader *cdata_B, unsigned int *basicIdx_H, unsigned int *basicIdx_L,
                               unsigned char first);
                               
int ff_dyna_write_file_header(AVFormatContext *ctx, AVIOContext *pb,
                                DynacolorHeader *header, unsigned int *size, time_t *time);
                                
int ff_dyna_read_pes_header(AVFormatContext *ctx, uint8_t *pes_data);

int ff_dyna_write_pes_header(AVFormatContext *ctx, AVIOContext *pb,
                                DynacolorPesHeader *pes, unsigned int size);

int ff_dyna_check_pes_packet_header(AVFormatContext *ctx, int format_prefix);

#endif
