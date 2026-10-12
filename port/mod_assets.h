#pragma once
/*
 * .o2r mod support: PaperBoat-format text, font and texture mods placed in
 * ux0:data/papership/mods/. Anything a mod does not provide comes from the ROM.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generated table entry (port/mod_asset_table.c): ROM offset -> PaperBoat resource path. */
typedef struct PortModAsset {
    uint32_t rom;
    uint32_t size;
    uint8_t kind; /* PORT_MOD_KIND_* */
    const char* path;
} PortModAsset;

#define PORT_MOD_KIND_TEXTURE 0
#define PORT_MOD_KIND_BLOB 1

extern const PortModAsset gPortModAssets[];
extern const unsigned int gPortModAssetCount;

/* "Intro_0001" etc. for a message ID, or NULL (port/mod_msg_names.c, generated). */
const char* port_mod_msg_suffix(unsigned int msgID);

/* Fill dest (cap bytes) with a mod's text for msgID. Returns 1 if a mod supplied it. */
int Port_ModLoadMessage(unsigned int msgID, void* dest, unsigned int cap);

/* Called after ROM bytes [rom, rom+size) land at dest: registers textures for replacement
 * and applies font overrides. */
void Port_ModOnRomRead(uint32_t rom, void* dest, uint32_t size);

/* Map textures come from compressed archives, so they are registered by name; suffix is "", "_mm1" or "_aux". */
void Port_ModSetTexArchive(const char* archiveName);
void Port_ModRegisterMapTexture(const void* raster, uint32_t size, const char* name, const char* suffix);

/* Sprite rasters and backgrounds: register a loaded image under its PaperBoat path. */
void Port_ModRegisterPath(const void* data, uint32_t size, const char* path);
/* Same, but draws may also start at any whole row inside the image (rowBytes per row). */
void Port_ModRegisterImage(const void* data, uint32_t size, uint32_t rowBytes, const char* path);

#ifdef __cplusplus
}

#include <memory>
#include <string>
namespace Fast {
class Texture;
}
/* Replacement texture for a raw texture pointer drawn with the given TLUT, or nullptr. Used by the
 * Fast3D SETTIMG handler. coverageTlut is set when the texture is also read through the sprite shading
 * palette. */
std::shared_ptr<Fast::Texture> Port_ModTextureFor(const void* addr, const void* tlut, uint32_t* hdOffset,
                                                  uint32_t* hdRowsSkipped, const uint8_t** coverageTlut);
/* port/hd_textures.cpp: the sprite file to draw for a registered sprite path under this TLUT
 * (palette variant, "" to keep it SD); other paths come back unchanged. */
std::string PortHdSpriteVariant(const std::string& path, const void* img, const void* tlut,
                                const uint8_t** coverageTlut);
/* port/hd_textures.cpp: shrink an RGBA32 HD texture to 4x the original, keeping a whole scale ratio. */
void PortHdShrinkTexture(Fast::Texture* tex);
#endif
