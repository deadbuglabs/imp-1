/* imp_sprites.h — 1-bit mascot poses (bare red imp, #f80c00).
 * Bitmaps are generated from files/static/mascot-1bit/ — see
 * tools_gen/gen_assets.py. 1 = imp pixel (drawn IMP_COLOUR_IMP). */
#ifndef IMP_SPRITES_H
#define IMP_SPRITES_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { const char *id; const uint8_t *bits; uint16_t w, h; } imp_sprite_t;
const imp_sprite_t *imp_sprite_find(const char *pose_id);
int imp_sprite_count(void);
const imp_sprite_t *imp_sprite_get(int i);
#ifdef __cplusplus
}
#endif
#endif
