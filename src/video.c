/* video.c - GBA Video Player.
 *
 * Sections:
 *   A. Binary data (INCBIN from graphics/video/)
 *   B. Types and local macros
 *   C. Heap-allocated context
 *   D. Decoder state (BSS)
 *   E. BitReader
 *   F. YUV->RGB555 codebook conversion
 *   G. ADPCM decoder
 *   H. Sound helpers
 *   I. Video decoder - codebook loading
 *   J. Video decoder render - I/P frame
 *   K. Player task
 */

#include "global.h"
#include "bg.h"
#include "video.h"
#include "task.h"
#include "gpu_regs.h"
#include "m4a.h"
#include "malloc.h"
#include "main.h"

/* ---------------------------------------------------------------------------
 * A. Binary data
 * --------------------------------------------------------------------------- */

static const u8  sVideoData[]         = INCBIN_U8( "graphics/video/video_data.bin");
static const u32 sFrameOffsets[]      = INCBIN_U32("graphics/video/frame_offsets.bin");
static const u8  sClipLut[]           = INCBIN_U8( "graphics/video/clip_lookup_table.bin");
static const u16 sBigBlockOffsets[]   = INCBIN_U16("graphics/video/big_block_offsets.bin");
static const u16 sZoneBlockOffsets[]  = INCBIN_U16("graphics/video/zone_block_offsets.bin");
static const u16 sZoneMotionOffsets[] = INCBIN_U16("graphics/video/zone_motion_offsets.bin");
#if VIDEO_AUDIO_FORMAT != 2
static const u8  sAudioData[]         = INCBIN_U8( "graphics/video/audio_data.bin");
#ifdef VIDEO_FRAME_AUDIO_COUNT
static const u32 sFrameAudioOffsets[] = INCBIN_U32("graphics/video/frame_audio_offsets.bin");
#endif
#endif

/* ---------------------------------------------------------------------------
 * B. Types and local macros
 * --------------------------------------------------------------------------- */

/* TIMER_CASCADE is not defined in pret decompilation projects — define locally */
#define VIDEO_TIMER_CASCADE 0x04

/* YUV 2x2 block - 6 bytes packed (YUV420: 4Y + 1Cb + 1Cr) */
typedef struct {
    u8 y[2][2];
    s8 cb;
    s8 cr;
} __attribute__((packed)) VidYuvBlock;

/* RGB555 2x2 block - named fields only*/
typedef struct {
    u16 row0[2];
    u16 row1[2];
} __attribute__((packed)) VidRgb555Block;

/* ---------------------------------------------------------------------------
 * C. Heap-allocated context
 *
 * Size breakdown (ADPCM, codebook=256, 240x160, SPF=192):
 *   codebookRaw : 256*6+4  =  1540 B
 *   rgb555Buf   : 2*256*8  =  4096 B
 *   frameBuf    : 240*160*2= 76800 B
 *   mixBuf      : 2*192    =   384 B  (ADPCM only)
 *   --------------------------------
 *   Total ADPCM :            82820 B  (~80.9 KB)
 *   Total PCM   :            82436 B  (~80.5 KB)
 * --------------------------------------------------------------------------- */

typedef struct {
    u8             codebookRaw[VIDEO_CODEBOOK_SIZE * 6 + 4];
    VidRgb555Block rgb555Buf[2][VIDEO_CODEBOOK_SIZE];
    u16 ALIGNED(4) frameBuf[DISPLAY_WIDTH * DISPLAY_HEIGHT];
#if VIDEO_AUDIO_FORMAT == 1
    s8             mixBuf[2][VIDEO_ADPCM_SPF];
    /* ADPCM predictor state */
    u32            playBuf;
    u32            adpcmOffset;
    int            lastSample;
    int            lastIndex;
#endif
    /* Frame timing and decoder state */
    u32            vblAcc;
    s32            frame;
    s32            rgb555Idx;
    s32            lastCheckFrame;
    s32            nextIFrame;
    /* Codebook pointers and flags */
    VidYuvBlock   *codebook;
    VidRgb555Block *rgb555Codebook;
    bool8          codebookLoaded;
    bool8          rgb555Loaded;
} VideoPlayerCtx;

EWRAM_DATA static VideoPlayerCtx *sCtx = NULL;

/* ---------------------------------------------------------------------------
 * D. Decoder state
 * --------------------------------------------------------------------------- */

EWRAM_DATA static VidYuvBlock    *sCodebook       = NULL;
EWRAM_DATA static bool8           sCodebookLoaded = FALSE;
EWRAM_DATA static VidRgb555Block *sRgb555Codebook = NULL;
EWRAM_DATA static bool8           sRgb555Loaded   = FALSE;

#if VIDEO_AUDIO_FORMAT == 1
EWRAM_DATA static u32 sPlayBuf     = 0;
EWRAM_DATA static u32 sAdpcmOffset = 0;
EWRAM_DATA static int sLastSample  = 0;
EWRAM_DATA static int sLastIndex   = 0;
#endif

/* ---------------------------------------------------------------------------
 * E. BitReader
 * --------------------------------------------------------------------------- */

typedef struct {
    const u8 **srcPtr;
    const u8  *innerSrc;
    u32        bitBuf;
    u8         bitsLeft;
    u8         bitLen;
    u8         bitMask;
} BitReader;

static void BitReader_Fill(BitReader *r) {
    while (r->bitsLeft <= 24) {
        r->bitBuf  |= ((u32)(*r->innerSrc)) << r->bitsLeft;
        r->innerSrc++;
        r->bitsLeft += 8;
    }
}

static void BitReader_Init(BitReader *r, const u8 **src, s32 bits) {
    r->srcPtr   = src;
    r->innerSrc = *src;
    r->bitBuf   = 0;
    r->bitsLeft = 0;
    r->bitLen   = (u8)bits;
    r->bitMask  = (u8)((1 << bits) - 1);
    if (bits < 8)
        BitReader_Fill(r);
}

static u8 BitReader_Read(BitReader *r) {
    u8 result;
    if (r->bitLen == 8)
        return *r->innerSrc++;
    if (r->bitsLeft < r->bitLen)
        BitReader_Fill(r);
    result       = (u8)(r->bitBuf & r->bitMask);
    r->bitBuf  >>= r->bitLen;
    r->bitsLeft -= r->bitLen;
    return result;
}

static void BitReader_Finish(BitReader *r) {
    if (r->bitsLeft > 0)
        r->innerSrc -= (r->bitsLeft >> 3);
    *r->srcPtr = r->innerSrc;
}

/* ---------------------------------------------------------------------------
 * F. YUV->RGB555 codebook conversion (ROM)
 * --------------------------------------------------------------------------- */

static void ConvertYuvToRgb555(const VidYuvBlock *yuv, VidRgb555Block *rgb, s32 size) {
    const u8 *lut = sClipLut + 1024;
    s32 i, y, x;
    for (i = 0; i < size; i++) {
        s8  cb = yuv[i].cb;
        s8  cr = yuv[i].cr;
        s32 dR =  (cr << 1);
        s32 dG =  (cb >> 1) + cr;
        s32 dB =  (cb << 1);
        for (y = 0; y < 2; y++) {
            for (x = 0; x < 2; x++) {
                u8  yv  = yuv[i].y[y][x];
                u16 r   = lut[yv + dR];
                u16 g   = lut[yv - dG];
                u16 b   = lut[yv + dB];
                u16 px  = r | (g << 5) | (b << 10);
                if (y == 0) rgb[i].row0[x] = px;
                else        rgb[i].row1[x] = px;
            }
        }
    }
}

static void CopyCodebook(u8 *dstRaw, const u8 *src, VidYuvBlock **outPtr, s32 size) {
    u8  *dst;
    s32  remain;
    s32  tail;

    dst     = dstRaw + 4;
    *outPtr = (VidYuvBlock *)dst;
    remain  = size * 6;

    if ((u32)src & 1) {
        u8 d = *src++;
        dst[-1] = dst[-3] = d;
        *outPtr = (VidYuvBlock *)((u32)*outPtr - 1);
        remain--;
    }
    if ((u32)src & 2) {
        dst[-2] = *src++;
        dst[-1] = *src++;
        *outPtr = (VidYuvBlock *)((u32)*outPtr - 2);
        remain -= 2;
    }
    DmaCopy32(3, src, dst, (u32)(remain & ~3));
    tail = remain & 3;
    src += remain - tail;
    dst += remain - tail;
    while (tail--) *dst++ = *src++;
}

/* ---------------------------------------------------------------------------
 * G. ADPCM decoder
 * --------------------------------------------------------------------------- */

#if VIDEO_AUDIO_FORMAT == 1

static const s8 sIma9StepIndices[16] = {
    -1, -1, -1, -1, 2, 4, 7, 12,
    -1, -1, -1, -1, 2, 4, 7, 12
};

static const u16 sImaStepTable[89] = {
        7,    8,    9,   10,   11,   12,   13,   14,   16,   17,
       19,   21,   23,   25,   28,   31,   34,   37,   41,   45,
       50,   55,   60,   66,   73,   80,   88,   97,  107,  118,
      130,  143,  157,  173,  190,  209,  230,  253,  279,  307,
      337,  371,  408,  449,  494,  544,  598,  658,  724,  796,
      876,  963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
     2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
     5894, 6484, 7132, 7845, 8630, 9493,10442,11487,12635,13899,
    15289,16818,18500,20350,22385,24623,27086,29794,32767
};

static int Ima9Rescale(int step, unsigned int code) {
    int diff = step >> 3;
    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    if ((code & 7) == 7) diff += step >> 1;
    if (code & 8) diff = -diff;
    return diff;
}

static void AdpcmDecodeBlock(s8 *dst) {
    int          last_sample = sLastSample;
    int          index       = sLastIndex;
    unsigned int by          = 0;
    u32          written     = 0;
    u32          audio_len   = (u32)sizeof(sAudioData);
    int          step, diff;
    unsigned int code;

    if (sAdpcmOffset >= audio_len) {
        for (; written < (u32)VIDEO_ADPCM_SPF; written++)
            dst[written] = 0;
        return;
    }

    while (written < (u32)VIDEO_ADPCM_SPF) {
        if (index < 0)  index = 0;
        if (index > 88) index = 88;
        step = sImaStepTable[index];

        if (written & 1) {
            code = by >> 4;
        } else {
            if (sAdpcmOffset >= audio_len) {
                for (; written < (u32)VIDEO_ADPCM_SPF; written++)
                    dst[written] = 0;
                break;
            }
            by   = sAudioData[sAdpcmOffset++];
            code = by & 0x0F;
        }

        diff = Ima9Rescale(step, code);
        index += sIma9StepIndices[code & 0x07];

        last_sample += diff;
        if (last_sample < -32768) last_sample = -32768;
        if (last_sample >  32767) last_sample =  32767;

        dst[written++] = (s8)(last_sample >> 8);
    }

    sLastSample = last_sample;
    sLastIndex  = index;
}

/* Timer 1 ISR - cascade from Timer 0.
 * Swap DMA to freshly decoded buffer, then decode the finished buffer. */
static void AdpcmTimer1Isr(void) {
    sPlayBuf ^= 1;

    REG_DMA1CNT_H = 0;
    asm volatile("eor r0, r0; eor r0, r0" ::: "r0");
    REG_DMA1SAD   = (u32)sCtx->mixBuf[sPlayBuf];
    REG_DMA1DAD   = (u32)&REG_FIFO_A;
    REG_DMA1CNT_L = 1;
    REG_DMA1CNT_H = DMA_DEST_FIXED | DMA_SRC_INC | DMA_REPEAT |
                    DMA_32BIT | DMA_START_SPECIAL | DMA_ENABLE;

    AdpcmDecodeBlock(sCtx->mixBuf[sPlayBuf ^ 1]);

    REG_IF       = INTR_FLAG_TIMER1;
    INTR_CHECK  |= INTR_FLAG_TIMER1;
}

static void AdpcmSeek(u32 offset) {
    u32 warmup_bytes = (u32)(VIDEO_ADPCM_SPF / 2 * 16);
    u32 i;

    sLastSample  = 0;
    sLastIndex   = 0;
    sPlayBuf     = 0;
    sAdpcmOffset = (offset >= warmup_bytes) ? (offset - warmup_bytes) : 0;
    sAdpcmOffset &= ~1u;

    for (i = 0; i < 16; i++)
        AdpcmDecodeBlock(sCtx->mixBuf[0]);

    if (sAdpcmOffset < offset)
        sAdpcmOffset = offset & ~1u;

    AdpcmDecodeBlock(sCtx->mixBuf[0]);
    AdpcmDecodeBlock(sCtx->mixBuf[1]);
    sPlayBuf = 0;
}

static void SoundResumeAdpcm(void) {
    REG_SOUNDCNT_H = SOUND_A_MIX_FULL | SOUND_A_RIGHT_OUTPUT |
                     SOUND_A_LEFT_OUTPUT | SOUND_A_FIFO_RESET;

    REG_TM1CNT_H = 0;
    REG_TM0CNT_H = 0;
    REG_DMA1CNT_H = 0;
    REG_DMA1SAD   = (u32)sCtx->mixBuf[0];
    REG_DMA1DAD   = (u32)&REG_FIFO_A;
    REG_DMA1CNT_L = 1;
    REG_DMA1CNT_H = DMA_DEST_FIXED | DMA_SRC_INC | DMA_REPEAT |
                    DMA_32BIT | DMA_START_SPECIAL | DMA_ENABLE;
    REG_TM1CNT_L = (u16)(0x10000 - VIDEO_ADPCM_SPF);
    REG_TM0CNT_L = (u16)(0x10000 - VIDEO_ADPCM_TIMER_RELOAD);
    REG_TM0CNT_H = TIMER_ENABLE;
    REG_TM1CNT_H = VIDEO_TIMER_CASCADE | TIMER_INTR_ENABLE | TIMER_ENABLE;
}

#endif /* VIDEO_AUDIO_FORMAT == 1 */

/* ---------------------------------------------------------------------------
 * H. Sound helpers
 * --------------------------------------------------------------------------- */

static void SoundStop(void) {
    REG_DMA1CNT_H = 0;
    REG_TM1CNT_H  = 0;
    REG_TM0CNT_H  = 0;
}

static void VideoSoundInit(void) {
    REG_SOUNDCNT_X = SOUND_MASTER_ENABLE;
    REG_SOUNDCNT_H = SOUND_A_MIX_FULL | SOUND_A_RIGHT_OUTPUT |
                     SOUND_A_LEFT_OUTPUT | SOUND_A_FIFO_RESET;
#if VIDEO_AUDIO_FORMAT == 1
#if VIDEO_SAMPLE_RATE <= 10923
    REG_SOUNDBIAS = 0x200 | (0 << 14);
#elif VIDEO_SAMPLE_RATE <= 21845
    REG_SOUNDBIAS = 0x200 | (1 << 14);
#elif VIDEO_SAMPLE_RATE <= 43690
    REG_SOUNDBIAS = 0x200 | (2 << 14);
#else
    REG_SOUNDBIAS = 0x200 | (3 << 14);
#endif
#endif
}

#if VIDEO_AUDIO_FORMAT == 0
static void SoundPlayPcm(const u8 *data) {
    SoundStop();
    REG_TM0CNT_L = (u16)(0x10000 - (16777216 / VIDEO_SAMPLE_RATE));
    REG_TM0CNT_H = TIMER_ENABLE;
    DmaSet(1, data, &REG_FIFO_A,
        (DMA_ENABLE | DMA_START_SPECIAL | DMA_32BIT |
         DMA_SRC_INC | DMA_DEST_FIXED | DMA_REPEAT) << 16);
}
#endif

/* ---------------------------------------------------------------------------
 * I. Video decoder - codebook loading (ROM)
 * --------------------------------------------------------------------------- */

static void VideoDecoder_ResetCodebook(void) {
    sCodebookLoaded        = FALSE;
    sRgb555Loaded          = FALSE;
    sCtx->lastCheckFrame   = -1;
    sCtx->nextIFrame       = -1;
}

static void VideoDecoder_Init(void) {
    sCtx->rgb555Idx = 1;
    VideoDecoder_ResetCodebook();
    sRgb555Codebook = sCtx->rgb555Buf[sCtx->rgb555Idx];
}

static void VideoDecoder_LoadAndConvert(const u8 *src) {
    if (!sCodebookLoaded)
        CopyCodebook(sCtx->codebookRaw, src, &sCodebook, VIDEO_CODEBOOK_SIZE);
    if (!sRgb555Loaded)
        ConvertYuvToRgb555(sCodebook, sCtx->rgb555Buf[sCtx->rgb555Idx ^ 1], VIDEO_CODEBOOK_SIZE);
}

static void VideoDecoder_PreloadCodebook(const u8 *src) {
    if (*src != VIDEO_FRAME_TYPE_I || sRgb555Loaded) {
        VBlankIntrWait();
        return;
    }
    if (!sCodebookLoaded) {
        CopyCodebook(sCtx->codebookRaw, src + 1, &sCodebook, VIDEO_CODEBOOK_SIZE);
        sCodebookLoaded = TRUE;
        return;
    }
    if (!sRgb555Loaded) {
        ConvertYuvToRgb555(sCodebook, sCtx->rgb555Buf[sCtx->rgb555Idx ^ 1], VIDEO_CODEBOOK_SIZE);
        sRgb555Loaded = TRUE;
    }
}

static void VideoDecoder_FindNextIFrame(s32 startFrame) {
    const s32 kMaxSearch = 30;
    s32 i;
    if (sCtx->lastCheckFrame == -1)
        sCtx->lastCheckFrame = startFrame;
    for (i = 0; i < kMaxSearch; i++) {
        if (sVideoData[sFrameOffsets[sCtx->lastCheckFrame]] == VIDEO_FRAME_TYPE_I) {
            sCtx->nextIFrame     = sCtx->lastCheckFrame;
            sCtx->lastCheckFrame = -1;
            return;
        }
        sCtx->lastCheckFrame++;
    }
}

/* ---------------------------------------------------------------------------
 * J. Video decoder render - I-frame and P-frame
 * --------------------------------------------------------------------------- */

static void RenderBlock(const VidRgb555Block *cb, u16 *dst) {
    dst[0]              = cb->row0[0];
    dst[1]              = cb->row0[1];
    dst[DISPLAY_WIDTH]  = cb->row1[0];
    dst[DISPLAY_WIDTH+1]= cb->row1[1];
}

static void RenderColorBlock(const VidRgb555Block *cb, u16 *dst) {
    u16 c0 = cb->row0[0], c1 = cb->row0[1];
    u16 c2 = cb->row1[0], c3 = cb->row1[1];
    u32 p0 = (u32)c0 | ((u32)c0 << 16);
    u32 p1 = (u32)c1 | ((u32)c1 << 16);
    u32 p2 = (u32)c2 | ((u32)c2 << 16);
    u32 p3 = (u32)c3 | ((u32)c3 << 16);
    *(u32 *)(dst)     = p0;  *(u32 *)(dst + 2) = p1;  dst += DISPLAY_WIDTH;
    *(u32 *)(dst)     = p0;  *(u32 *)(dst + 2) = p1;  dst += DISPLAY_WIDTH;
    *(u32 *)(dst)     = p2;  *(u32 *)(dst + 2) = p3;  dst += DISPLAY_WIDTH;
    *(u32 *)(dst)     = p2;  *(u32 *)(dst + 2) = p3;
}

static void RenderTextureBlock(const VidRgb555Block *cb, const u8 idx[4], u16 *dst) {
    RenderBlock(&cb[idx[0]], dst);
    RenderBlock(&cb[idx[1]], dst + 2);
    RenderBlock(&cb[idx[2]], dst + DISPLAY_WIDTH * 2);
    RenderBlock(&cb[idx[3]], dst + DISPLAY_WIDTH * 2 + 2);
}

static const u16 kSubOffsets[4] = {
    0, 2, DISPLAY_WIDTH * 2, DISPLAY_WIDTH * 2 + 2
};

static void DecodePartial4x4Block(u8 *validBitmap, BitReader *reader,
                                   u16 *dst, const VidRgb555Block *cb) {
    u8 i;
    for (i = 0; i < 4; i++) {
        if (*validBitmap & 1) {
            u8 idx = BitReader_Read(reader);
            RenderBlock(&cb[idx], dst + kSubOffsets[i]);
        }
        *validBitmap >>= 1;
    }
}

static void DecodeSegment(u8 bitLen, u16 segIdx,
                           const u8 **src, u16 *zoneDst,
                           const VidRgb555Block *cb) {
    const VidRgb555Block *segCb;
    u8          numBlocks;
    const u8   *bmpPtr;
    BitReader   reader;
    const u8   *bp;
    bool8       isOdd;

    segCb     = cb + ((u32)segIdx << bitLen);
    numBlocks = *(*src)++;
    bmpPtr    = *src;

    *src += (numBlocks >> 1) * 3;
    if (numBlocks & 1) *src += 2;

    BitReader_Init(&reader, src, bitLen);

    bp     = bmpPtr;
    isOdd  = (numBlocks & 1) != FALSE;
    numBlocks >>= 1;

    while (numBlocks--) {
        u8   vbm  = *bp++;
        u8   zi   = *bp++;
        u16 *dst1 = zoneDst + sZoneBlockOffsets[zi];
        DecodePartial4x4Block(&vbm, &reader, dst1, segCb);
        zi = *bp++;
        {
            u16 *dst2 = zoneDst + sZoneBlockOffsets[zi];
            DecodePartial4x4Block(&vbm, &reader, dst2, segCb);
        }
    }
    if (isOdd) {
        u8   vbm  = *bp++;
        u8   zi   = *bp++;
        u16 *dst1 = zoneDst + sZoneBlockOffsets[zi];
        DecodePartial4x4Block(&vbm, &reader, dst1, segCb);
    }
    BitReader_Finish(&reader);
}

static void ApplyMotionCompensation(const u8 **src, u16 *dst) {
    u8   zoneBitmap = *(*src)++;
    u16 *zSrc = (u16 *)VRAM;
    u16 *zDst = dst;

    while (zoneBitmap) {
        if (zoneBitmap & 1) {
            u8 stripCount = *(*src)++;
            u8 i;
            for (i = 0; i < stripCount; i++) {
                u8  blockIdx  = (*src)[0];
                u8  encodedMv = (*src)[1];
                u8  contCount = (*src)[2];
                s32 dx, dy, dstOff, srcOff, stripW, y;
                *src += 3;

                dx     = (encodedMv & 0x0F) - VIDEO_MOTION_RANGE;
                dy     = ((encodedMv >> 4) & 0x0F) - VIDEO_MOTION_RANGE;
                dstOff = sZoneMotionOffsets[blockIdx];
                srcOff = dstOff + dy * DISPLAY_WIDTH + dx;
                stripW = VIDEO_MOTION_BLOCK_SIZE * contCount;

                for (y = 0; y < VIDEO_MOTION_BLOCK_SIZE; y++) {
                    DmaCopy32(3, &zSrc[srcOff], &zDst[dstOff],
                              (u32)(stripW * 2));
                    dstOff += DISPLAY_WIDTH;
                    srcOff += DISPLAY_WIDTH;
                }
            }
        }
        zoneBitmap >>= 1;
        zSrc += VIDEO_MOTION_BLOCK_SIZE * VIDEO_MOTION_BLOCK_SIZE * 240;
        zDst += VIDEO_MOTION_BLOCK_SIZE * VIDEO_MOTION_BLOCK_SIZE * 240;
    }
}

static void DecodeIFrame(const u8 *src, u16 *dst) {
    const u16 kTotalBlocks =
        (DISPLAY_WIDTH  / (VIDEO_BLOCK_W * 2)) *
        (DISPLAY_HEIGHT / (VIDEO_BLOCK_H * 2));
    u16 idx;

    if (!sRgb555Loaded)
        VideoDecoder_LoadAndConvert(src);
    VideoDecoder_ResetCodebook();

    src                    += VIDEO_CODEBOOK_SIZE * 6;
    sCtx->rgb555Idx        ^= 1;
    sRgb555Codebook         = sCtx->rgb555Buf[sCtx->rgb555Idx];

    for (idx = 0; idx < kTotalBlocks; idx++) {
        u16 *bigDst = dst + sBigBlockOffsets[idx];
        u8   first  = *src++;
        if (first == VIDEO_COLOR_BLOCK_MARKER) {
            RenderColorBlock(&sRgb555Codebook[*src++], bigDst);
        } else {
            u8 qi[4];
            qi[0] = first;
            qi[1] = *src++;
            qi[2] = *src++;
            qi[3] = *src++;
            RenderTextureBlock(sRgb555Codebook, qi, bigDst);
        }
    }
}

static void DecodePFrame(const u8 *src, u16 *dst) {
    u16 detailBitmap;
    u16 colorBitmap;
    u8  zoneIdx;
    u16 bmp;

    ApplyMotionCompensation(&src, dst);

    detailBitmap = src[0] | ((u16)src[1] << 8);
    colorBitmap  = src[2] | ((u16)src[3] << 8);
    src += 4;

    zoneIdx = 0;
    bmp     = detailBitmap;
    while (bmp) {
        if (bmp & 1) {
            u16  zoneBase = (u16)(zoneIdx * VIDEO_ZONE_HEIGHT * DISPLAY_WIDTH);
            u16 *zDst     = dst + zoneBase;
            u16  smallBmp = src[0] | ((u16)src[1] << 8);
            u16  seg;
            u8   medBmp;
            u8   mseg;
            src += 2;
            for (seg = 0; seg < 16; seg++)
                if (smallBmp & (1u << seg))
                    DecodeSegment(4, seg, &src, zDst, sRgb555Codebook);
            medBmp = *src++;
            for (mseg = 0; mseg < 4; mseg++)
                if (medBmp & (1u << mseg))
                    DecodeSegment(6, mseg, &src, zDst, sRgb555Codebook);
            DecodeSegment(8, 0, &src, zDst, sRgb555Codebook);
        }
        bmp >>= 1;
        zoneIdx++;
    }

    zoneIdx = 0;
    bmp     = colorBitmap;
    while (bmp) {
        if (bmp & 1) {
            u8   count    = *src++;
            u16  zoneBase = (u16)(zoneIdx * VIDEO_ZONE_HEIGHT * DISPLAY_WIDTH);
            u16 *zDst     = dst + zoneBase;
            u8   i;
            for (i = 0; i < count; i++) {
                u8 zi = *src++;
                u8 ci = *src++;
                RenderColorBlock(&sRgb555Codebook[ci], zDst + sZoneBlockOffsets[zi]);
            }
        }
        bmp >>= 1;
        zoneIdx++;
    }
}

static void DecodeFrame(const u8 *frameData, u16 *dst) {
    u8 type = *frameData++;
    if (type == VIDEO_FRAME_TYPE_I)
        DecodeIFrame(frameData, dst);
    else
        DecodePFrame(frameData, dst);
}

/* ---------------------------------------------------------------------------
 * K. Player task
 * --------------------------------------------------------------------------- */

#define tState        data[0]
#define tDone         data[1]
#define tSavedDispcnt data[2]
#define tShouldCopy   data[3]
#define tForceSync    data[4]

#define VIDEO_LCD_FPS 597275

#if VIDEO_AUDIO_FORMAT == 1
static IntrFunc sSavedTimer1Isr;
#endif

static void Task_VideoPlayer(u8 taskId);

static void VideoPlayerVBlankCb(void) {
    u8 taskId;
    if (!FuncIsActiveTask(Task_VideoPlayer))
        return;
    sCtx->vblAcc += VIDEO_FPS_RAW;
    if (sCtx->vblAcc >= VIDEO_LCD_FPS) {
        sCtx->vblAcc -= VIDEO_LCD_FPS;
        taskId = FindTaskIdByFunc(Task_VideoPlayer);
        gTasks[taskId].tShouldCopy = TRUE;
    }
}

u8 VideoPlayer_Start(void) {
    return CreateTask(Task_VideoPlayer, 0);
}

bool8 VideoPlayer_IsDone(void) {
    return FindTaskIdByFunc(Task_VideoPlayer) == TASK_NONE;
}

void VideoPlayer_ForceStop(void) {
    u8 taskId = FindTaskIdByFunc(Task_VideoPlayer);
    if (taskId != TASK_NONE)
        gTasks[taskId].tState = 2;
}

static void VideoPlayer_InitGpu(void)
{
    DmaClearLarge16(3, (void *)VRAM, VRAM_SIZE, 0x1000);
    DmaClear32(3, (void *)OAM, OAM_SIZE);
    DmaClear16(3, (void *)PLTT, PLTT_SIZE);

    SetGpuReg(REG_OFFSET_DISPCNT, 0);

    ResetBgsAndClearDma3BusyFlags(0);

    SetBgAffine(2, 0, 0, 0, 0, 256, 256, 0);
    SetGpuReg(REG_OFFSET_BG2HOFS, 0);
    SetGpuReg(REG_OFFSET_BG2VOFS, 0);
    SetGpuReg(REG_OFFSET_BG2X_L, 0);
    SetGpuReg(REG_OFFSET_BG2X_H, 0);
    SetGpuReg(REG_OFFSET_BG2Y_L, 0);
    SetGpuReg(REG_OFFSET_BG2Y_H, 0);
    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_MODE_3 | DISPCNT_BG2_ON);
}

static void VideoPlayer_Init(u8 taskId) {
    struct Task *t = &gTasks[taskId];

    sCtx = AllocZeroed(sizeof(VideoPlayerCtx));
    if (sCtx == NULL) {
        gTasks[taskId].tDone = TRUE;
        DestroyTask(taskId);
        return;
    }

    t->tSavedDispcnt = REG_DISPCNT;
    VideoPlayer_InitGpu();

    m4aSoundVSyncOff();
    REG_DMA1CNT_H = 0;
    REG_DMA2CNT_H = 0;
    REG_SOUNDCNT_X = 0;
    REG_SOUNDCNT_H = 0;

    VideoDecoder_Init();
    CpuFastFill(0, sCtx->frameBuf, DISPLAY_WIDTH * DISPLAY_HEIGHT * 2);
    CpuFastCopy(sCtx->frameBuf, (void *)VRAM, DISPLAY_WIDTH * DISPLAY_HEIGHT * 2);
    VideoSoundInit();

#if VIDEO_AUDIO_FORMAT == 1
    sSavedTimer1Isr = gIntrTable[6];
    gIntrTable[6]   = AdpcmTimer1Isr;
    EnableInterrupts(INTR_FLAG_TIMER1);
#endif

    SetVBlankCallback(VideoPlayerVBlankCb);

    sCtx->frame  = 0;
    sCtx->vblAcc = 0;
    t->tShouldCopy = FALSE;
    t->tForceSync  = TRUE;
    t->tState      = 1;
}

static void VideoPlayer_Run(u8 taskId) {
    struct Task *t = &gTasks[taskId];

    DecodeFrame(sVideoData + sFrameOffsets[sCtx->frame], sCtx->frameBuf);

    while (!t->tShouldCopy) {
        if (sRgb555Loaded) {
            VBlankIntrWait();
        } else {
            if (sCtx->nextIFrame == -1) {
                VideoDecoder_FindNextIFrame(sCtx->frame + 1);
                if (!t->tShouldCopy)
                    VBlankIntrWait();
            } else {
                VideoDecoder_PreloadCodebook(sVideoData + sFrameOffsets[sCtx->nextIFrame]);
            }
        }
    }
    t->tShouldCopy = FALSE;

    CpuFastCopy(sCtx->frameBuf, (void *)VRAM, DISPLAY_WIDTH * DISPLAY_HEIGHT * 2);

#ifdef VIDEO_FRAME_AUDIO_COUNT
    if (t->tForceSync) {
#if VIDEO_AUDIO_FORMAT == 0
        SoundPlayPcm(sAudioData + sFrameAudioOffsets[sCtx->frame]);
#elif VIDEO_AUDIO_FORMAT == 1
        AdpcmSeek(sFrameAudioOffsets[sCtx->frame]);
        VBlankIntrWait();
        SoundResumeAdpcm();
#endif
        t->tForceSync = FALSE;
    }
#endif

    sCtx->frame++;
    if (sCtx->frame >= VIDEO_FRAME_COUNT) {
        t->tState = 2;
        return;
    }

#if VIDEO_SKIP_KEY != 0
    if (gMain.newKeys & VIDEO_SKIP_KEY)
        t->tState = 2;
#endif
}

static void VideoPlayer_Cleanup(u8 taskId) {
    SoundStop();

#if VIDEO_AUDIO_FORMAT == 1
    gIntrTable[6] = sSavedTimer1Isr;
    DisableInterrupts(INTR_FLAG_TIMER1);
#endif

    SetVBlankCallback(NULL);
    REG_SOUNDCNT_X = 0;
    REG_SOUNDCNT_H = 0;
    REG_SOUNDCNT_L = 0;
    VBlankIntrWait();
    REG_SOUNDBIAS  = 0x200;
    m4aSoundInit();
    m4aSoundVSyncOn();
    SetGpuReg(REG_OFFSET_DISPCNT, (u16)gTasks[taskId].tSavedDispcnt);

    FREE_AND_SET_NULL(sCtx);

    gTasks[taskId].tDone = TRUE;
    DestroyTask(taskId);
}

static void Task_VideoPlayer(u8 taskId) {
    switch (gTasks[taskId].tState) {
    case 0:  VideoPlayer_Init(taskId);    break;
    case 1:  VideoPlayer_Run(taskId);     break;
    default: VideoPlayer_Cleanup(taskId); break;
    }
}
