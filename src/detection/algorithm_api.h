#pragma once
#include <stdint.h>

// Versioned C ABI. No Qt/C++ objects, ownership, allocators or exceptions cross
// this boundary. CBOR control/results use the documented v1 schema.
#ifdef __cplusplus
extern "C" {
#endif
typedef int (*IntrovertDetectorEmitV1)(void *, const uint8_t *, uint64_t);
typedef struct IntrovertDetectorHostV1 {
    uint32_t size;
    uint32_t interface_version;
    void *context;
    int (*cancelled)(void *);
    // Sources 0/1 are scoped to the call. Reads stay within their hard windows;
    // rates are mono s16 PCM, 8–96 kHz; no paths or credentials are exposed.
    int64_t (*read_pcm)(void *, uint32_t source, uint32_t rate, int64_t first, uint32_t count, int16_t *output);
    IntrovertDetectorEmitV1 write_result;
} IntrovertDetectorHostV1;
typedef struct IntrovertDetectorApiV1 {
    uint32_t size;
    uint32_t interface_version;
    const char *(*metadata)(void);
    // Calls are serial by default. parallel_matches permits concurrent matches;
    // parallel_extraction permits concurrent extract/validate calls. Opted-in
    // operations must be reentrant with independent host callback contexts.
    int (*invoke)(const char *, const uint8_t *, uint64_t, const IntrovertDetectorHostV1 *);
} IntrovertDetectorApiV1;
typedef const IntrovertDetectorApiV1 *(*IntrovertDetectorEntryV1)(void);
#ifdef __cplusplus
}
#endif
