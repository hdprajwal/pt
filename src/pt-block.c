#include "pt-block.h"

#define Q_UL 0x1
#define Q_UR 0x2
#define Q_LL 0x4
#define Q_LR 0x8

/* Quadrant fills for U+2596..U+259F indexed by cp - 0x2596. The ten glyphs
 * have no ordering the geometry can derive, so the sets are tabulated. */
static const guint8 quadrants[] = {
  Q_LL,                      /* U+2596 */
  Q_LR,                      /* U+2597 */
  Q_UL,                      /* U+2598 */
  Q_UL | Q_LL | Q_LR,        /* U+2599 */
  Q_UL | Q_LR,               /* U+259A */
  Q_UL | Q_UR | Q_LL,        /* U+259B */
  Q_UL | Q_UR | Q_LR,        /* U+259C */
  Q_UR,                      /* U+259D */
  Q_UR | Q_LL,               /* U+259E */
  Q_UR | Q_LL | Q_LR,        /* U+259F */
};

int pt_block_glyph_rects(guint32 cp, PtBlockRect out[4]) {
  if (cp < 0x2580 || cp > 0x259f) return 0;

  /* lower eighths ▁▂▃▄▅▆▇█, growing upward from the cell floor */
  if (cp >= 0x2581 && cp <= 0x2588) {
    float h = (float)(cp - 0x2580) / 8.0f;
    out[0] = (PtBlockRect){ 0.0f, 1.0f - h, 1.0f, h, 1.0f };
    return 1;
  }
  /* left eighths ▉▊▋▌▍▎▏, shrinking from 7/8 down to 1/8 */
  if (cp >= 0x2589 && cp <= 0x258f) {
    float w = (float)(0x2590 - cp) / 8.0f;
    out[0] = (PtBlockRect){ 0.0f, 0.0f, w, 1.0f, 1.0f };
    return 1;
  }
  if (cp >= 0x2596) {
    guint8 mask = quadrants[cp - 0x2596];
    int n = 0;
    for (int i = 0; i < 4; i++) {
      if ((mask & (1u << i)) == 0) continue;
      out[n++] = (PtBlockRect){ (i & 1) ? 0.5f : 0.0f, (i & 2) ? 0.5f : 0.0f,
                                0.5f, 0.5f, 1.0f };
    }
    return n;
  }

  switch (cp) {
    case 0x2580: out[0] = (PtBlockRect){ 0.0f, 0.0f, 1.0f, 0.5f, 1.0f }; return 1;
    case 0x2590: out[0] = (PtBlockRect){ 0.5f, 0.0f, 0.5f, 1.0f, 1.0f }; return 1;
    case 0x2591: out[0] = (PtBlockRect){ 0.0f, 0.0f, 1.0f, 1.0f, 0.25f }; return 1;
    case 0x2592: out[0] = (PtBlockRect){ 0.0f, 0.0f, 1.0f, 1.0f, 0.5f }; return 1;
    case 0x2593: out[0] = (PtBlockRect){ 0.0f, 0.0f, 1.0f, 1.0f, 0.75f }; return 1;
    case 0x2594: out[0] = (PtBlockRect){ 0.0f, 0.0f, 1.0f, 0.125f, 1.0f }; return 1;
    case 0x2595: out[0] = (PtBlockRect){ 0.875f, 0.0f, 0.125f, 1.0f, 1.0f }; return 1;
    default: return 0;
  }
}
