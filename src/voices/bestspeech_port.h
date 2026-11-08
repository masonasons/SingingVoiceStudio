/* bestspeech_port.h -- BeSTspeech's stages and its lattice, for
 * src/voices/bestspeech.cpp, which knows nothing of the engine's insides. */
#ifndef SVS_BESTSPEECH_PORT_H
#define SVS_BESTSPEECH_PORT_H

#include <stddef.h>
#include <stdint.h>

#include "bst.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the room a stream is given, which the rule pass may grow it into */
#define BST_SVS_STREAM 0x200

/* Runs a phrase's stream of `len` bytes, its header at `hdr`, through the
   rule pass, the accents, the pair scan, the contour and the frame
   generator, in `voice`. Writes the sixteen-byte frames, and for each the
   number of segments taken when it was made (so frame i belongs to segment
   frame_seg[i] - 1); and for each segment the sound it is the second half of,
   or -1 for a command. Returns the number of frames, or -1. */
int svs_bst_frames(bst *h, const char *build, int voice, uint8_t *stream, int len, int hdr,
                   uint8_t *frames, int max_frames, int *frame_seg, int *seg_sound, int max_seg,
                   int *nseg);

/* The lattice over a build's tables, frame by frame. */
typedef struct svs_bst_lattice svs_bst_lattice;
svs_bst_lattice *svs_bst_lattice_new(bst *h);
void svs_bst_lattice_free(svs_bst_lattice *l);
/* Plays one frame, writing its samples; 0 for an end marker. */
size_t svs_bst_lattice_frame(svs_bst_lattice *l, const uint8_t f[16], int16_t *out, size_t max);

#ifdef __cplusplus
}
#endif

#endif
