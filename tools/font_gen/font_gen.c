#include <stdio.h>
#include "font.h"

/*
 * Emits third_party/common/font.c: a 256-entry 8x8 menu font indexed by
 * Unicode code point U+0000 - U+00FF.
 *
 *   0x00 - 0x1F : blank (control range)
 *   0x20 - 0x7E : font_bescii[cp - 0x20]
 *   0x7F - 0x9F : .notdef box glyph  (DEL + C1 range; the box is also drawn
 *                                     by ui_draw_char for any code point
 *                                     >= U+0100 and for malformed UTF-8)
 *   0xA0 - 0xFF : font_bescii[cp - 0x20]  (Latin-1 Supplement)
 *
 * font_bescii comes from font.h (rasterised from Bescii Mono, CC0 1.0
 * public domain). Glyph bytes are row-major, MSB-first (bit 0x80 =
 * leftmost pixel), matching ui_draw_char(), so they are emitted verbatim.
 */

static const unsigned char box[8] =
    { 0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF, 0x00 };
static const unsigned char blank[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

static void emit(const unsigned char g[8], const char *comment) {
    printf("    {0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x}, // %s\n",
        g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], comment);
}

int main(int argc, char *argv[]) {
    char cmt[64];

    printf("char font8x8_basic[256][8] = {\n");
    for (int i = 0; i < 256; i++) {
        if (i <= 0x1F) {
            sprintf(cmt, "0x%02X", i);
            emit(blank, cmt);
        } else if (i <= 0x7E) {
            sprintf(cmt, "0x%02X %c", i, (i >= 33 && i != 92) ? i : ' ');
            emit(font_bescii[i - 0x20], cmt);
        } else if (i <= 0x9F) {
            sprintf(cmt, "0x%02X box", i);
            emit(box, cmt);
        } else {
            sprintf(cmt, "0x%02X U+%04X", i, i);
            emit(font_bescii[i - 0x20], cmt);
        }
    }
    printf("};\n");
    return 0;
}
