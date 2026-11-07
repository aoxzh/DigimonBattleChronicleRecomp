#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct AAudioStream AAudioStream;
typedef struct AAudioStreamBuilder AAudioStreamBuilder;

enum {
    AAUDIO_OK = 0,
    AAUDIO_FORMAT_PCM_I16 = 1,
    AAUDIO_CHANNEL_STEREO = 12,
    AAUDIO_CALLBACK_RESULT_CONTINUE = 0,
};
typedef int aaudio_data_callback_result_t;
typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(
    AAudioStream *, void *, void *, int32_t);

int AAudio_createStreamBuilder(AAudioStreamBuilder **);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *, int);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *, int);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *, int);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder *, AAudioStream_dataCallback, void *);
int AAudioStreamBuilder_openStream(AAudioStreamBuilder *, AAudioStream **);
void AAudioStreamBuilder_delete(AAudioStreamBuilder *);
int AAudioStream_requestStart(AAudioStream *);
int AAudioStream_requestStop(AAudioStream *);
int AAudioStream_requestPause(AAudioStream *);
int AAudioStream_close(AAudioStream *);
#ifdef __cplusplus
}
#endif
