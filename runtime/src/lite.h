/* The compressed copy of the game's archive: GameData/bundle_PC.lite in place of bundle_PC.ipk (2 GB -> ~0.2 GB).
 *
 * Why: 1.7 GB of the archive is uncompressed PCM audio. lite_make.c (the launcher on Android, tools on the PC) writes
 * a copy whose sounds are Opus and whose other uncompressed files are zlib; lite_read.c gives the game the original
 * archive's bytes back at the original offsets (hle_k32_file.c opens it in place of bundle_PC.ipk), so the game
 * never knows. Sounds come back with the same header and the same number of samples (loops stay exact).
 *
 * Layout (little-endian):
 *   0   "RLITE1\0\0"
 *   8   u64 size of the original archive
 *   16  u32 size of the original archive's directory (its first bytes, kept as they are)   u32 number of regions
 *   24  u64 offset of the region table
 *   32  the directory
 *   ... the regions' data, then the table: one LiteRegion per region, sorted, covering the original archive from the
 *       directory's end to its end without gaps
 * A region's data by method: LITE_RAW the bytes; LITE_ZLIB zlib; LITE_ZERO nothing (zeros between files);
 * LITE_OPUS a RIFF WAVE file whose data chunk is Opus:
 *   u32 prefix length, the bytes before the PCM samples; u32 suffix length, the bytes after them;
 *   u32 channels, u32 samples per channel, u32 encoder lookahead (samples to drop at the start), u32 packets;
 *   then each packet: u16 length, the packet (20 ms each, 48 kHz).
 */
#ifndef LITE_H
#define LITE_H
#include <stdint.h>

enum { LITE_RAW = 0, LITE_ZLIB = 1, LITE_OPUS = 2, LITE_ZERO = 3 };
typedef struct { uint64_t orig_off; uint32_t orig_len, method; uint64_t pack_off; uint32_t pack_len, pad; } LiteRegion;
#define LITE_MAGIC "RLITE1\0\0"

/* lite_make.c: progress(done, total) is called from the writing thread; returns 0 or an error text */
const char *lite_make(const char *ipk_path, const char *out_path, int threads, void (*progress)(uint64_t done, uint64_t total));
/* lite_read.c */
typedef struct Lite Lite;
Lite *lite_open(const char *path);
uint64_t lite_size(Lite *l);
int64_t lite_pread(Lite *l, void *buf, uint64_t n, uint64_t off);   /* the original archive's bytes */
void lite_close(Lite *l);
#include <stddef.h>
int lite_path_for(const char *path, char *out, size_t n);
Lite *lite_shared(const char *path);
#endif
