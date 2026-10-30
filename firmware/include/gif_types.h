#ifndef GIF_TYPES_H
#define GIF_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// AnimatedGIF -- shared struct for PROGMEM boot & idle animation data
// ---------------------------------------------------------------------------
// Supports both legacy uncompressed frames[N][1024] and RLE-compressed
// rle_data + rle_offsets[N + 1].
#ifndef ANIMATED_GIF_DEFINED
#define ANIMATED_GIF_DEFINED
typedef struct {
  const uint8_t  frame_count;
  const uint16_t width;
  const uint16_t height;
  const uint16_t *delays;
  const uint8_t  (*frames)[1024];
  const uint8_t  *rle_data;
  const uint32_t *rle_offsets;
} AnimatedGIF;
#endif // ANIMATED_GIF_DEFINED

// ---------------------------------------------------------------------------
// .qgif binary format constants
// ---------------------------------------------------------------------------
// Standard uncompressed layout (height == 64):
//   [0]       uint8_t   frame_count
//   [1..2]    uint16_t  width   (LE)
//   [3..4]    uint16_t  height  (LE, 64)
//   [5..]     uint16_t  delays[frame_count]  (LE)
//   [..]      uint8_t   frames[frame_count][QGIF_FRAME_SIZE]
//
// RLE-compressed layout (height == 64 | QGIF_RLE_FLAG):
//   [0]       uint8_t   frame_count
//   [1..2]    uint16_t  width   (LE)
//   [3..4]    uint16_t  height  (LE, 64 | 0x8000)
//   [5..]     uint16_t  delays[frame_count]  (LE)
//   [..]      uint32_t  offsets[frame_count + 1] (LE byte offsets into RLE payload)
//   [..]      uint8_t   rle_payload[...]
//
#define QGIF_HEADER_SIZE   5
#define QGIF_MAX_FRAMES    255
#define QGIF_FRAME_WIDTH   128
#define QGIF_FRAME_HEIGHT  64
#define QGIF_FRAME_SIZE    ((QGIF_FRAME_WIDTH / 8) * QGIF_FRAME_HEIGHT)  // 1024
#define QGIF_RLE_FLAG      0x8000U

// Decode an RLE-compressed frame slice into a 1024-byte frame buffer.
static inline size_t qgifRleDecode(const uint8_t *src, size_t srcLen, uint8_t *dst, size_t dstMax) {
  size_t si = 0, di = 0;
  while (si < srcLen && di < dstMax) {
    uint8_t ctrl = src[si++];
    if (ctrl & 0x80) {
      size_t run = ctrl & 0x7F;
      if (si >= srcLen) break;
      uint8_t val = src[si++];
      while (run-- > 0 && di < dstMax) {
        dst[di++] = val;
      }
    } else {
      size_t lit = ctrl;
      while (lit-- > 0 && si < srcLen && di < dstMax) {
        dst[di++] = src[si++];
      }
    }
  }
  while (di < dstMax) {
    dst[di++] = 0;
  }
  return di;
}

// Decode a PROGMEM AnimatedGIF frame (either RLE or raw) into dst1024.
static inline void gifDecodeProgmemFrame(uint8_t *dst1024, const AnimatedGIF *gif, uint8_t frameIdx) {
  if (!gif || frameIdx >= gif->frame_count) return;
  if (gif->rle_data && gif->rle_offsets) {
    uint32_t start = gif->rle_offsets[frameIdx];
    uint32_t end   = gif->rle_offsets[frameIdx + 1];
    qgifRleDecode(gif->rle_data + start, end - start, dst1024, QGIF_FRAME_SIZE);
  } else if (gif->frames) {
    memcpy(dst1024, gif->frames[frameIdx], QGIF_FRAME_SIZE);
  }
}

#endif // GIF_TYPES_H
