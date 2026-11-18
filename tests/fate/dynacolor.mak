DYNACOLOR_SAMPLE = $(TARGET_SAMPLES)/dynacolor/dynacolor-camera-sample-600k

FATE_DYNACOLOR-$(call FRAMECRC, DYNACOLOR, MPEG4, EXTRACT_EXTRADATA_BSF) += fate-dynacolor-demux
fate-dynacolor-demux: CMD = framecrc -i $(DYNACOLOR_SAMPLE) -c copy -copyinkf

FATE_DYNACOLOR-$(call DEMDEC, DYNACOLOR, MPEG4) += fate-dynacolor-seek
fate-dynacolor-seek: libavformat/tests/seek$(EXESUF)
fate-dynacolor-seek: CMD = run libavformat/tests/seek$(EXESUF) $(DYNACOLOR_SAMPLE) -duration 3 -frames 2

FATE_SAMPLES_FFMPEG += $(FATE_DYNACOLOR-yes)
fate-dynacolor: $(FATE_DYNACOLOR-yes)
