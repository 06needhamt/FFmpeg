DYNACOLOR_SAMPLE = $(TARGET_SAMPLES)/dynacolor/dynacolor-camera-sample-600k

FATE_DYNACOLOR-$(call FRAMECRC, DYNACOLOR, MPEG4, EXTRACT_EXTRADATA_BSF) += fate-dynacolor-demux
fate-dynacolor-demux: CMD = framecrc -i $(DYNACOLOR_SAMPLE) -c copy -copyinkf

FATE_DYNACOLOR-$(call DEMDEC, DYNACOLOR, MPEG4) += fate-dynacolor-seek
fate-dynacolor-seek: libavformat/tests/seek$(EXESUF)
fate-dynacolor-seek: CMD = run libavformat/tests/seek$(EXESUF) $(DYNACOLOR_SAMPLE) -duration 3 -frames 2

# The packets demuxed from the remuxed file must match fate-dynacolor-demux,
# except for the corrupt flag of the truncated last packet of the sample.
FATE_DYNACOLOR-$(call REMUX, DYNACOLOR, MPEG4_DECODER EXTRACT_EXTRADATA_BSF) += fate-dynacolor-remux
fate-dynacolor-remux: CMD = stream_remux dynacolor $(DYNACOLOR_SAMPLE) "" dynacolor "-map 0 -map_metadata 0 -copyinkf" "" "-c copy -copyinkf"

# H.264 in MP4: the muxer converts the stream to Annex B with in-band SPS/PPS.
FATE_DYNACOLOR-$(call REMUX, DYNACOLOR, MOV_DEMUXER H264_DECODER H264_PARSER EXTRACT_EXTRADATA_BSF) += fate-dynacolor-h264
fate-dynacolor-h264: CMD = stream_remux mov $(TARGET_SAMPLES)/h264/h264_3bf_nopyramid_nobsrestriction.mp4 "" dynacolor "-map 0:v" "" "-c copy"

# Annex B H.264 with an AUD but no in-band SPS/PPS: the muxer inserts them
# from extradata after the AUD, which must stay first.
FATE_DYNACOLOR-$(call REMUX, DYNACOLOR, MOV_DEMUXER H264_DECODER H264_PARSER EXTRACT_EXTRADATA_BSF H264_MP4TOANNEXB_BSF H264_METADATA_BSF FILTER_UNITS_BSF) += fate-dynacolor-h264-aud
fate-dynacolor-h264-aud: CMD = stream_remux mov $(TARGET_SAMPLES)/h264/h264_3bf_nopyramid_nobsrestriction.mp4 "" dynacolor "-map 0:v -bsf:v h264_mp4toannexb,h264_metadata=aud=insert,filter_units=remove_types=7-8" "" "-c copy"

# MPEG-4 with B-frames and the VOL header only in extradata: the muxer
# repeats it on keyframes and writes B-pictures as such.
FATE_DYNACOLOR_FFMPEG-$(call TRANSCODE, MPEG4, DYNACOLOR, LAVFI_INDEV TESTSRC2_FILTER WRAPPED_AVFRAME_DECODER EXTRACT_EXTRADATA_BSF) += fate-dynacolor-mpeg4
fate-dynacolor-mpeg4: CMD = transcode lavfi "testsrc2=size=176x144:rate=25:duration=3" dynacolor "-c:v mpeg4 -g 25 -bf 2 -flags +global_header -metadata creation_time=2009-01-21T14:21:08Z" "-c copy"

# Two channels with keyframes at the same times. After each seek, the
# keyframes of both channels keep the timestamps of linear reading; the other
# records of a channel are estimated until its next indexed keyframe.
tests/data/fate/dynacolor-channels.dyna: TAG = GEN
tests/data/fate/dynacolor-channels.dyna: ffmpeg$(PROGSSUF)$(EXESUF) | tests/data/fate
	$(M)$(TARGET_EXEC) $(TARGET_PATH)/$< -nostdin -f lavfi -i testsrc2=s=176x144:r=25:d=3 \
	-f lavfi -i testsrc2=s=128x96:r=25:d=3 -map 0 -map 1 -c:v mpeg4 -g 10 -threads 1 \
	-dct fastint -idct simple -flags +bitexact -fflags +bitexact -y $(TARGET_PATH)/$@ 2>/dev/null

FATE_DYNACOLOR_FFMPEG-$(call ENCDEC, MPEG4, DYNACOLOR, LAVFI_INDEV TESTSRC2_FILTER WRAPPED_AVFRAME_DECODER) += fate-dynacolor-seek-channels
fate-dynacolor-seek-channels: libavformat/tests/seek$(EXESUF) tests/data/fate/dynacolor-channels.dyna
fate-dynacolor-seek-channels: CMD = run libavformat/tests/seek$(EXESUF) $(TARGET_PATH)/tests/data/fate/dynacolor-channels.dyna -duration 3 -frames 4

FATE_SAMPLES_FFMPEG += $(FATE_DYNACOLOR-yes)
FATE_FFMPEG += $(FATE_DYNACOLOR_FFMPEG-yes)
fate-dynacolor: $(FATE_DYNACOLOR-yes) $(FATE_DYNACOLOR_FFMPEG-yes)
