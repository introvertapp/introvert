#pragma once
#include "algorithm_api.h"

#ifdef __cplusplus
extern "C" {
#endif
// Additive session interface. The original algorithm v1 entry remains supported.
// Only borrowed bytes, fixed-width values and callbacks cross the boundary.
typedef struct IntrovertDetectorAudioReadV1 {
    const uint8_t *episode;
    uint64_t episode_size;
    uint32_t segment_type, sample_rate;
    double window_start, window_end, start, end;
    int64_t first;
    uint32_t count;
} IntrovertDetectorAudioReadV1;
typedef struct IntrovertDetectorServicesV1 {
    uint32_t size, version;
    void *context;
    int (*cancelled)(void *);
    // Reentrant bounded requests. Replies use the existing CBOR envelope.
    int (*request)(void *, const char *, const uint8_t *, uint64_t, void *, IntrovertDetectorEmitV1);
    int64_t (*read_pcm)(void *, const IntrovertDetectorAudioReadV1 *, int16_t *, void *, IntrovertDetectorEmitV1);
    int (*publish)(void *, const uint8_t *, uint64_t);
} IntrovertDetectorServicesV1;
typedef struct IntrovertDetectorSessionApiV1 {
    uint32_t size, version;
    int (*run)(const uint8_t *, uint64_t, const IntrovertDetectorServicesV1 *);
} IntrovertDetectorSessionApiV1;
typedef const IntrovertDetectorSessionApiV1 *(*IntrovertDetectorSessionEntryV1)(void);
#ifdef __cplusplus
}
#endif
