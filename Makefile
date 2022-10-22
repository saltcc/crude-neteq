CC = gcc
CXX = g++ -std=c++11
CFLAGS = -fno-exceptions -fvisibility=hidden -DNDEBUG -DWEBRTC_POSIX -g
#CFLAGS = -DNDEBUG -DWEBRTC_POSIX -g
INC = -I.

.PHONY: libs
#libs:neteq dsp base vad
libs:dsp base vad

DSPSRC = $(wildcard common_audio/signal_processing/*.c)
DSPSRC += common_audio/third_party/spl_sqrt_floor/spl_sqrt_floor.c
DSPOBJS = $(DSPSRC:%.c=%.o)
DSPLIB = libsignal_processing.a

BASESRC = $(wildcard rtc_base/*.cc)
BASEOBJS = $(BASESRC:%.cc=%.o)
BASELIB = libbase.a

VADSRC = $(wildcard common_audio/vad/*.c)
VADOBJS = $(VADSRC:%.c=%.o)
VADLIB = libwebrtc_vad.a

SCALESRC = common_audio/signal_processing/dot_product_with_scale.cc
SCALEOBJ = common_audio/signal_processing/dot_product_with_scale.o

dsp: $(DSPLIB)
base: $(BASELIB)
vad: $(VADLIB)

APP_PLC=plctest
APP_ACCE=accetest
APP_MUTE=mutetest
APP_COPY=copytest
APP_NOISE=noisetest

plc:  $(APP_PLC)
acce: $(APP_ACCE)
#mute: $(APP_MUTE)
#copy: $(APP_COPY)
noise:$(APP_NOISE)

$(DSPLIB): $(DSPOBJS) $(SCALEOBJ)
	ar cr $@ $^

$(BASELIB): $(BASEOBJS)
	ar cr $@ $^

$(VADLIB): $(VADOBJS)
	ar cr $@ $^

$(DSPOBJS): %.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@ $(INC)

$(SCALEOBJ): $(SCALESRC)
	$(CXX) $(CFLAGS) -c $< -o $@ $(INC)

$(BASEOBJS): %.o: %.cc
	$(CXX) $(CFLAGS) -c $< -o $@ $(INC)

$(VADOBJS): %.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@ $(INC)


plctest: test_neteq.cc $(DSPLIB) $(VADLIB) $(BASELIB)
	$(CXX) $< -o $@ -I. -L. -Lneteq -Lcodec/interface -lplc -lwebrtc_vad -lbase -lsignal_processing -g -lpthread -lg711
accetest: test_accelerate.cc $(DSPLIB) $(VADLIB) $(BASELIB)
	$(CXX) $< -o $@ -I. -L. -Lneteq -lplc -lwebrtc_vad -lbase -lsignal_processing -g -lpthread
#mutetest: test_mute.cc
#	$(CXX) $(CFLAGS) $< -o $@ -I. -L. -Lneteq -lplc -lwebrtc_vad -lbase -lsignal_processing -g -lpthread
#copytest: test_copy.cc
#	$(CXX) $(CFLAGS) $< -o $@ $(INC) -I. -L. -Lneteq -lplc -lwebrtc_vad -lbase -lsignal_processing -g -lpthread
noisetest: test_noise.cc
	$(CXX) $(CFLAGS) $< -o $@ $(INC) -L. -Lneteq -lplc -lwebrtc_vad -lbase -lsignal_processing -lpthread

#NETEQ=
#neteq:$NETEQ
#$NETEQ:
#	$(MAKE) -C ./neteq/

app:$(APP_PLC) $(APP_ACCE) $(APP_NOISE)
	
.PHONY: clean

clean:
	-rm $(DSPOBJS) $(DSPLIB) $(VADOBJS) $(VADLIB) $(BASEOBJS) $(BASELIB) $(DYLIBS) $(SCALEOBJ) \
	$(APP_PLC) $(APP_ACCE) $(APP_NOISE)
