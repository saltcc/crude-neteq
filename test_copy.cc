
#include <stdio.h>
#include <string.h>

#if (!defined(WEBRTC_HAS_NEON)) && !defined(MIPS32_LE)
/* Initialize function pointers to the generic C version. */
static void InitPointersToC(void) {
  WebRtcSpl_MaxAbsValueW16 = WebRtcSpl_MaxAbsValueW16C;
  WebRtcSpl_MaxAbsValueW32 = WebRtcSpl_MaxAbsValueW32C;
  WebRtcSpl_MaxValueW16 = WebRtcSpl_MaxValueW16C;
  WebRtcSpl_MaxValueW32 = WebRtcSpl_MaxValueW32C;
  WebRtcSpl_MinValueW16 = WebRtcSpl_MinValueW16C;
  WebRtcSpl_MinValueW32 = WebRtcSpl_MinValueW32C;
  WebRtcSpl_CrossCorrelation = WebRtcSpl_CrossCorrelationC;
  WebRtcSpl_DownsampleFast = WebRtcSpl_DownsampleFastC;
  WebRtcSpl_ScaleAndAddVectorsWithRound =
      WebRtcSpl_ScaleAndAddVectorsWithRoundC;
}
#endif

#if defined(WEBRTC_HAS_NEON)
/* Initialize function pointers to the Neon version. */
static void InitPointersToNeon(void) {
  WebRtcSpl_MaxAbsValueW16 = WebRtcSpl_MaxAbsValueW16Neon;
  WebRtcSpl_MaxAbsValueW32 = WebRtcSpl_MaxAbsValueW32Neon;
  WebRtcSpl_MaxValueW16 = WebRtcSpl_MaxValueW16Neon;
  WebRtcSpl_MaxValueW32 = WebRtcSpl_MaxValueW32Neon;
  WebRtcSpl_MinValueW16 = WebRtcSpl_MinValueW16Neon;
  WebRtcSpl_MinValueW32 = WebRtcSpl_MinValueW32Neon;
  WebRtcSpl_CrossCorrelation = WebRtcSpl_CrossCorrelationNeon;
  WebRtcSpl_DownsampleFast = WebRtcSpl_DownsampleFastNeon;
  WebRtcSpl_ScaleAndAddVectorsWithRound =
      WebRtcSpl_ScaleAndAddVectorsWithRoundC;
}
#endif

#if defined(MIPS32_LE)
/* Initialize function pointers to the MIPS version. */
static void InitPointersToMIPS(void) {
  WebRtcSpl_MaxAbsValueW16 = WebRtcSpl_MaxAbsValueW16_mips;
  WebRtcSpl_MaxValueW16 = WebRtcSpl_MaxValueW16_mips;
  WebRtcSpl_MaxValueW32 = WebRtcSpl_MaxValueW32_mips;
  WebRtcSpl_MinValueW16 = WebRtcSpl_MinValueW16_mips;
  WebRtcSpl_MinValueW32 = WebRtcSpl_MinValueW32_mips;
  WebRtcSpl_CrossCorrelation = WebRtcSpl_CrossCorrelation_mips;
  WebRtcSpl_DownsampleFast = WebRtcSpl_DownsampleFast_mips;
#if defined(MIPS_DSP_R1_LE)
  WebRtcSpl_MaxAbsValueW32 = WebRtcSpl_MaxAbsValueW32_mips;
  WebRtcSpl_ScaleAndAddVectorsWithRound =
      WebRtcSpl_ScaleAndAddVectorsWithRound_mips;
#else
  WebRtcSpl_MaxAbsValueW32 = WebRtcSpl_MaxAbsValueW32C;
  WebRtcSpl_ScaleAndAddVectorsWithRound =
      WebRtcSpl_ScaleAndAddVectorsWithRoundC;
#endif
}
#endif

static void InitFunctionPointers(void) {
#if defined(WEBRTC_HAS_NEON)
  InitPointersToNeon();
#elif defined(MIPS32_LE)
  InitPointersToMIPS();
#else
  InitPointersToC();
#endif  /* WEBRTC_HAS_NEON */
}
#if defined(WEBRTC_POSIX)
#include <pthread.h>

static void once(void (*func)(void)) {
  static pthread_once_t lock = PTHREAD_ONCE_INIT;
  pthread_once(&lock, func);
}

#elif defined(_WIN32)
#include <windows.h>

static void once(void (*func)(void)) {
  /* Didn't use InitializeCriticalSection() since there's no race-free context
   * in which to execute it.
   *
   * TODO(kma): Change to different implementation (e.g.
   * InterlockedCompareExchangePointer) to avoid issues similar to
   * http://code.google.com/p/webm/issues/detail?id=467.
   */
  static CRITICAL_SECTION lock = {(void *)((size_t)-1), -1, 0, 0, 0, 0};
  static int done = 0;

  EnterCriticalSection(&lock);
  if (!done) {
    func();
    done = 1;
  }
  LeaveCriticalSection(&lock);
}

/* There's no fallback version as an #else block here to ensure thread safety.
 * In case of neither pthread for WEBRTC_POSIX nor _WIN32 is present, build
 * system should pick it up.
 */
#endif  /* WEBRTC_POSIX */



int main(){
	printf("hello! copy!\n");

	//test plc
	int fs_hz = 48000;
	size_t channels = 1;

	int fs_mult_ = fs_hz / 8000;
	int samples_10ms = static_cast<size_t>(10 * 8 * fs_mult_);

	FILE *pcm = fopen("48.pcm", "rb");
	if(pcm == NULL){
		printf("open pcm file failed!\n");
		return 0;
	}
	FILE *outfile = fopen("copy.pcm", "wb");

	int16_t buf[480];
	int16_t lastbuf[480];
	int read;
	int count = 0;
	while(!feof(pcm)){
		read = fread(buf, sizeof(int16_t), samples_10ms, pcm);
		if(read != samples_10ms){
			printf("read: %d\n", read);
			break;
		}
		count++;
		int lost = (count % 10 == 0);
		if(!lost){
			fwrite(buf, sizeof(int16_t), samples_10ms, outfile);
			memcpy(lastbuf, buf, sizeof(int16_t) * samples_10ms);
		}else{
			fwrite(lastbuf, sizeof(int16_t), samples_10ms, outfile);
		}
	}
	
	fflush(outfile);
	fclose(outfile);
	fclose(pcm);

	printf("leaving application!\n");
	return 0;
}
