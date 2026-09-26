#ifndef REAWEB_STREAM_H
#define REAWEB_STREAM_H
#include "reaweb_service.h"
#ifdef __cplusplus
extern "C" {
#endif

#define REAWEB_STREAM_ABI 1
typedef uint64_t ReaWeb_StreamHandle;
enum ReaWeb_StreamKind { REAWEB_FRAME = 1, REAWEB_AUDIO, REAWEB_SPECTRUM, REAWEB_METER, REAWEB_WAVEFORM, REAWEB_BINARY, REAWEB_MIDI };
enum ReaWeb_StreamFormat { REAWEB_PIXEL_RGBA8 = 1, REAWEB_PIXEL_BGRA8, REAWEB_FLOAT32, REAWEB_BYTES };
enum ReaWeb_StreamStatus {
  REAWEB_STREAM_NOT_FOUND = 20, REAWEB_STREAM_CLOSED, REAWEB_UNSUPPORTED_FORMAT,
  REAWEB_BUFFER_FULL, REAWEB_PRODUCER_BUSY
};
typedef struct ReaWeb_StreamDesc {
  uint32_t size, abi_version;
  uint32_t format, max_bytes, capacity;
  uint32_t width, height, stride;
  uint32_t channels, sample_rate, block_frames, fft_size;
  double update_rate;
  const char* source;
  /* Optional service owner. Unregistering it closes associated streams. */
  ReaWeb_ServiceHandle owner;
} ReaWeb_StreamDesc;

/* Main-thread creation. Descriptor and source are copied. Resolve each named
   Create<Type>Stream export with REAPER GetFunc using this signature. */
typedef int (*ReaWeb_CreateStreamFn)(const char* name, const ReaWeb_StreamDesc*, ReaWeb_StreamHandle*);
typedef ReaWeb_CreateStreamFn ReaWeb_CreateFrameStreamFn;
typedef ReaWeb_CreateStreamFn ReaWeb_CreateAudioStreamFn;
typedef ReaWeb_CreateStreamFn ReaWeb_CreateSpectrumStreamFn;
typedef ReaWeb_CreateStreamFn ReaWeb_CreateMeterStreamFn;
typedef ReaWeb_CreateStreamFn ReaWeb_CreateWaveformStreamFn;
typedef ReaWeb_CreateStreamFn ReaWeb_CreateBinaryStreamFn;
typedef ReaWeb_CreateStreamFn ReaWeb_CreateMIDIStreamFn;

/* Single producer per stream. Copies into preallocated bounded storage without
   allocations, locks, host calls or consumer waits. Concurrent publication
   returns PRODUCER_BUSY. Timestamp is seconds in the producer's clock.
   Audio/MIDI drop the incoming block on BUFFER_FULL. Other kinds keep latest.
   Stop/join producers before closing or unloading the provider. */
typedef int (*ReaWeb_PublishStreamFn)(ReaWeb_StreamHandle, const void*, uint32_t bytes, uint64_t sequence, double timestamp);
typedef ReaWeb_PublishStreamFn ReaWeb_PublishFrameFn;
typedef ReaWeb_PublishStreamFn ReaWeb_PublishAudioFn;
typedef ReaWeb_PublishStreamFn ReaWeb_PublishSpectrumFn;
typedef ReaWeb_PublishStreamFn ReaWeb_PublishMeterFn;
typedef ReaWeb_PublishStreamFn ReaWeb_PublishWaveformFn;
typedef ReaWeb_PublishStreamFn ReaWeb_PublishBinaryFn;
typedef ReaWeb_PublishStreamFn ReaWeb_PublishMIDIFn;
/* Main thread. Idempotent for a closed handle, never closes a new stream with
   the same name. Consumer detach does not close the producer. */
typedef int (*ReaWeb_CloseStreamFn)(ReaWeb_StreamHandle);

#ifdef __cplusplus
}
#endif
#endif
