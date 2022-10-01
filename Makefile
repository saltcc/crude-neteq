CC = gcc
CXX = g++ -std=c++11
#CFLAGS = -fno-exceptions -fvisibility=hidden -DNDEBUG -DWEBRTC_POSIX -g
CFLAGS = -DNDEBUG -DWEBRTC_POSIX -g
INC = -I.

APP = plctest
APP_ACCE = accelerate
DSPSRC = $(wildcard common_audio/signal_processing/*.c)
#OBJS = $(patsubst %.c, %.o, $(SRC))
DSPOBJS = $(DSPSRC:%.c=%.o)
DSPLIB = libsignal_processing.a
BASESRC = $(wildcard rtc_base/*.cc)
BASEOBJS = $(BASESRC:%.cc=%.o)
BASELIB = libbase.a
VADSRC = $(wildcard common_audio/vad/*.c)
VADOBJS = $(VADSRC:%.c=%.o)
VADLIB = libwebrtc_vad.a
#DYLIBS = libbase.dylib libvad.dylib libdsp.dylib

SCALESRC = common_audio/signal_processing/dot_product_with_scale.cc
SCALEOBJ = common_audio/signal_processing/dot_product_with_scale.o
DSPSRC += common_audio/third_party/spl_sqrt_floor/spl_sqrt_floor.c

app: $(APP)
app_acce: $(APP_ACCE)
dsp: $(DSPLIB)
base: $(BASELIB)
vad: $(VADLIB)
mute: mutetest
copy: copytest
noise: noisetest
dylib: $(DYLIBS)

#$(APP): test.cc $(DSPLIB) $(BASELIB)
#	$(CXX) $< -o $@ -I. -L. -lsignal_processing -lrtc_base

$(APP): test_neteq.cc $(DSPLIB) $(VADLIB) $(BASELIB)
	$(CXX) $< -o $@ -I. -L. -Lneteq -lplc -lwebrtc_vad -lbase -lsignal_processing -g -lpthread

$(APP_ACCE): test_accelerate.cc $(DSPLIB) $(VADLIB) $(BASELIB)
	$(CXX) $< -o $@ -I. -L. -Lneteq -lplc -lwebrtc_vad -lbase -lsignal_processing -g -lpthread

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

mutetest: test_mute.cc
	$(CXX) $(CFLAGS) $< -o $@

copytest: test_copy.cc
	$(CXX) $(CFLAGS) $< -o $@ $(INC)

noisetest: test_noise.cc
	$(CXX) $(CFLAGS) $< -o $@ $(INC) -L. -Lneteq -lplc -lwebrtc_vad -lbase -lsignal_processing 

#libbase.dylib: $(BASEOBJS)
#	$(CXX) -dynamiclib -undefined suppress -flat_namespace $^ -o $@
#libvad.dylib: $(VADOBJS)
#	$(CXX) -dynamiclib -undefined suppress -flat_namespace $^ -o $@
#libdsp.dylib: $(DSPOBJS) $(SCALEOBJ)
#	$(CXX) -dynamiclib -undefined suppress -flat_namespace $^ -o $@
	
all: $(APP) $(APP_ACCE) $(BASELIB) $(VADLIB)

.PHONY: clean

clean:
	-rm $(DSPOBJS) $(DSPLIB) $(VADOBJS) $(VADLIB) $(BASEOBJS) $(BASELIB) $(APP) $(DYLIBS) $(APP_ACCE) $(SCALEOBJ)
