/**
 * rom_offsets.h - ROM offset lookup and byte-swap utilities for PC port
 *
 * On N64, ROM segment addresses (logos_ROM_START, etc.) are linker-generated
 * and contain actual ROM offsets. On PC, they're zero-length stub arrays.
 * This module provides a runtime lookup: given a stub address, returns
 * the real ROM offset from the US ROM.
 *
 * Also provides byte-swap macros for big-endian ROM data on little-endian PC.
 */

#ifndef ROM_OFFSETS_H
#define ROM_OFFSETS_H

#ifdef PORT

#include "ultra64.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Resolve a linker stub address to its actual ROM offset.
 * Returns the ROM offset, or 0xFFFFFFFF if not found.
 */
u32 resolve_rom_offset(const void* stub_addr);

/**
 * Tell the HD texture provider which icon-segment raster/palette (by offset) now sits in dest.
 */
void port_hd_icon_loaded(const void* dest, u32 iconOffset, u32 size);

/**
 * Sprites: the pack names them alt/sprites/{npc_sprite_%03d|player_sprite_%d}_raster_<n>[@<owner>_pal_<p>].
 * The mod system (port/ModAssets.cpp) tracks raster buffers; loaded() records each sprite's palettes and the
 * rasters every animation part shows. Sprite draws mark their part in the display list (gSPPortSpritePart).
 */
void port_hd_sprite_loaded(s32 isPlayer, s32 sprite, const void* spriteAnimData);
void port_hd_frame(void);

/**
 * A large image (size bytes, rowBytes per row) the game draws in strips; HD file is alt/<path>.
 * CI images pass the palette the pack colored them with; the HD copy is only used while it's active.
 */
void port_hd_image_loaded(const void* base, u32 size, u32 rowBytes, const char* path, const void* pal, u32 palBytes);

/**
 * Byte-swap macros for big-endian ROM data on little-endian PC.
 * N64 ROM data is big-endian. On PC (little-endian), multi-byte fields
 * read from ROM must be byte-swapped.
 */
#if defined(__GNUC__) || defined(__clang__)
#define ROM_BSWAP32(x) __builtin_bswap32(x)
#define ROM_BSWAP16(x) __builtin_bswap16(x)
#else
#define ROM_BSWAP32(x) (((x) >> 24) | (((x) >> 8) & 0xFF00) | (((x) << 8) & 0xFF0000) | ((x) << 24))
#define ROM_BSWAP16(x) (((x) >> 8) | ((x) << 8))
#endif

#ifdef __cplusplus
}
#endif

#endif // PORT
#endif // ROM_OFFSETS_H
