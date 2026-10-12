// HD texture replacement for textures the game copies out of the ROM into fixed buffers.
// The pack (mods/*.o2r) stores them as alt/<path> RGBA32 resources; the interpreter asks us,
// by raw address + active TLUT, whether one exists.
#include <fast/interpreter.h>
#include <fast/resource/ResourceType.h>
#include <fast/resource/type/Texture.h>
#include <ship/Context.h>
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
extern uint8_t MsgCharImgNormal[];
extern uint8_t MsgCharImgSubtitle[];
extern uint16_t D_802F4560[80][8];

extern unsigned char ui_msg_palettes[16][32];
extern uint8_t ui_msg_bubble_left_png[];
extern uint8_t ui_msg_bubble_mid_png[];
extern uint8_t ui_msg_bubble_right_png[];
extern uint8_t ui_msg_arrow_png[];
extern uint8_t ui_msg_frame_a_1_1_png[], ui_msg_frame_a_1_2_png[], ui_msg_frame_a_1_3_png[], ui_msg_frame_a_1_4_png[];
extern uint8_t ui_msg_frame_a_1_5_png[], ui_msg_frame_a_2_1_png[], ui_msg_frame_a_2_5_png[], ui_msg_frame_a_3_1_png[];
extern uint8_t ui_msg_frame_a_3_5_png[], ui_msg_frame_a_4_1_png[], ui_msg_frame_a_4_5_png[], ui_msg_frame_a_5_1_png[];
extern uint8_t ui_msg_frame_a_5_2_png[], ui_msg_frame_a_5_3_png[], ui_msg_frame_a_5_4_png[], ui_msg_frame_a_5_5_png[];
extern uint8_t ui_msg_frame_b_1_1_png[], ui_msg_frame_b_1_2_png[], ui_msg_frame_b_1_3_png[], ui_msg_frame_b_1_4_png[];
extern uint8_t ui_msg_frame_b_1_5_png[], ui_msg_frame_b_2_1_png[], ui_msg_frame_b_2_5_png[], ui_msg_frame_b_3_1_png[];
extern uint8_t ui_msg_frame_b_3_5_png[], ui_msg_frame_b_4_1_png[], ui_msg_frame_b_4_5_png[], ui_msg_frame_b_5_1_png[];
extern uint8_t ui_msg_frame_b_5_2_png[], ui_msg_frame_b_5_3_png[], ui_msg_frame_b_5_4_png[], ui_msg_frame_b_5_5_png[];
extern uint8_t ui_msg_sign_corner_topleft_png[];
extern uint8_t ui_msg_sign_corner_topright_png[];
extern uint8_t ui_msg_sign_corner_bottomleft_png[];
extern uint8_t ui_msg_sign_corner_bottomright_png[];
extern uint8_t ui_msg_lamppost_corner_bottomright_png[];
extern uint8_t ui_msg_sign_side_top_png[];
extern uint8_t ui_msg_sign_side_left_png[];
extern uint8_t ui_msg_sign_side_right_png[];
extern uint8_t ui_msg_sign_side_bottom_png[];
extern uint8_t ui_msg_sign_fill_png[];
extern uint8_t ui_msg_sign_pal[];
extern uint8_t ui_msg_lamppost_pal[];
extern uint8_t ui_msg_background_png[];
extern uint8_t ui_msg_rewind_arrow_png[];
extern uint8_t ui_msg_rewind_arrow_pal[];
extern uint8_t ui_msg_star_png[];
extern uint8_t speech_bubble_img[];
extern uint8_t speech_bubble_pal[];
extern uint8_t inspect_icon_img[];
extern uint8_t inspect_icon_pal[];
extern uint8_t ispy_icon_img[];
extern uint8_t ispy_icon_1_pal[];
extern uint8_t ispy_icon_2_pal[];
extern uint8_t ispy_icon_3_pal[];
extern uint8_t theater_walls_png[];
extern uint8_t theater_curtains_png[];
extern uint8_t theater_floor_png[];
extern uint8_t theater_floor_pal[];
extern uint8_t D_802E9170[]; // entity shadow, square
extern uint8_t D_802E91F0[]; // entity shadow, circle
extern uint16_t* gSpriteShadingProfile; // SpriteShadingProfile*, flags first
}

struct HdNamedAsset {
    const unsigned char* addr;
    const char* path; // asset path without extension, e.g. "ui/watt" or "ui/watt.disabled"
};

// Must stay outside the anonymous namespace: its extern "C" symbols would otherwise get internal linkage.
#include "hd_ui_table.inc"
#include "hd_entity_table.inc"

struct HdIconRaster {
    uint32_t offset; // within the icon ROM segment
    const char* path;
};

struct HdIconPalette {
    uint32_t offset;
    const char* path; // icon that owns this palette
    bool disabled;
};

#include "hd_icon_table.inc"

namespace {

// Pack glyphs are 16x oversized (256x256 for a 16x16 glyph); keep 4x, plenty for 960x544.
constexpr uint32_t kTargetScale = 4;

constexpr uint32_t kPalBytes = 16; // D_802F4560[pal] stride
constexpr uint32_t kPalCount = 80;
constexpr uint32_t kTlutBytes = 32; // pal16 load

struct HdCharset {
    const uint8_t* raster;
    uint32_t glyphBytes, glyphCount;
    const char* prefix;
    const char* palPrefix; // nullptr: the pack only has palette 0
};

const HdCharset kCharsets[] = {
    { MsgCharImgNormal, 128, 0x5100 / 128, "alt/charset/charset_standard_", "@charset_standard_palette_" },
    { MsgCharImgSubtitle, 72, 0xB88 / 72, "alt/charset/charset_subtitle_", nullptr },
};

// A CI image is only swapped when the active TLUT matches the palette the pack baked in.
struct HdAlt {
    const uint8_t* pal;
    const char* suffix;
};

struct HdImage {
    const uint8_t* addr;
    const char* path;
    const uint8_t* basePal; // nullptr for non-CI images
    HdAlt alts[2];
};

#define MSG_PAL0 ui_msg_palettes[0]
#define SIGN_ALT { { ui_msg_sign_pal, "@sign_corner_topleft.pal" } }
#define FRAME(set, n) { ui_msg_frame_##set##_##n##_png, "alt/ui/msg/frame_" #set "_" #n, MSG_PAL0, {} }

const HdImage kImages[] = {
    { ui_msg_bubble_left_png, "alt/ui/msg/bubble_left", MSG_PAL0, {} },
    { ui_msg_bubble_mid_png, "alt/ui/msg/bubble_mid", MSG_PAL0, {} },
    { ui_msg_bubble_right_png, "alt/ui/msg/bubble_right", MSG_PAL0, {} },
    { ui_msg_arrow_png, "alt/ui/msg/arrow", MSG_PAL0, {} },
    FRAME(a, 1_1), FRAME(a, 1_2), FRAME(a, 1_3), FRAME(a, 1_4), FRAME(a, 1_5), FRAME(a, 2_1), FRAME(a, 2_5),
    FRAME(a, 3_1), FRAME(a, 3_5), FRAME(a, 4_1), FRAME(a, 4_5), FRAME(a, 5_1), FRAME(a, 5_2), FRAME(a, 5_3),
    FRAME(a, 5_4), FRAME(a, 5_5),
    FRAME(b, 1_1), FRAME(b, 1_2), FRAME(b, 1_3), FRAME(b, 1_4), FRAME(b, 1_5), FRAME(b, 2_1), FRAME(b, 2_5),
    FRAME(b, 3_1), FRAME(b, 3_5), FRAME(b, 4_1), FRAME(b, 4_5), FRAME(b, 5_1), FRAME(b, 5_2), FRAME(b, 5_3),
    FRAME(b, 5_4), FRAME(b, 5_5),
    // Plain sign names are baked with whichever palette the extractor saw first (checked by color).
    { ui_msg_sign_corner_topleft_png, "alt/ui/msg/sign_corner_topleft", ui_msg_sign_pal, {} },
    { ui_msg_sign_corner_topright_png, "alt/ui/msg/sign_corner_topright", ui_msg_lamppost_pal, SIGN_ALT },
    { ui_msg_sign_corner_bottomleft_png, "alt/ui/msg/sign_corner_bottomleft", ui_msg_lamppost_pal, SIGN_ALT },
    { ui_msg_sign_corner_bottomright_png, "alt/ui/msg/sign_corner_bottomright", ui_msg_sign_pal, {} },
    { ui_msg_lamppost_corner_bottomright_png, "alt/ui/msg/lamppost_corner_bottomright", ui_msg_lamppost_pal, {} },
    { ui_msg_sign_side_top_png, "alt/ui/msg/sign_side_top", ui_msg_lamppost_pal, SIGN_ALT },
    { ui_msg_sign_side_left_png, "alt/ui/msg/sign_side_left", ui_msg_lamppost_pal, SIGN_ALT },
    { ui_msg_sign_side_right_png, "alt/ui/msg/sign_side_right", ui_msg_lamppost_pal, SIGN_ALT },
    { ui_msg_sign_side_bottom_png, "alt/ui/msg/sign_side_bottom", ui_msg_lamppost_pal, SIGN_ALT },
    { ui_msg_sign_fill_png, "alt/ui/msg/sign_fill", ui_msg_lamppost_pal, SIGN_ALT },
    { ui_msg_background_png, "alt/ui/msg/background", nullptr, {} },
    { ui_msg_rewind_arrow_png, "alt/ui/msg/rewind_arrow", ui_msg_rewind_arrow_pal, {} },
    { ui_msg_star_png, "alt/ui/msg/star", nullptr, {} },
    { speech_bubble_img, "alt/misc/speech_bubble/texture", speech_bubble_pal, {} },
    { inspect_icon_img, "alt/icons/inspect_icon/img", inspect_icon_pal, {} },
    { ispy_icon_img, "alt/icons/ispy_icon/img", ispy_icon_1_pal, { { ispy_icon_2_pal, "@pal2" }, { ispy_icon_3_pal, "@pal3" } } },
    { theater_walls_png, "alt/theater/theater_walls", nullptr, {} },
    { theater_curtains_png, "alt/theater/theater_curtains", nullptr, {} },
    { theater_floor_png, "alt/theater/theater_floor", theater_floor_pal, {} },
    { D_802E9170, "alt/entities/Shadows/Entity_Shadow_TexSquare", nullptr, {} },
    { D_802E91F0, "alt/entities/Shadows/Entity_Shadow_TexCircle", nullptr, {} },
};

#undef FRAME
#undef SIGN_ALT
#undef MSG_PAL0

struct HdEntry {
    std::vector<uint8_t> data;
    uint16_t width = 0, height = 0;
    float hByteScale = 1, vPixelScale = 1;
    bool valid = false;
};

// A replacement is identified by the texture buffer and the TLUT it was baked with.
struct HdKey {
    const void* tex;
    const void* pal;
    bool operator==(const HdKey& o) const {
        return tex == o.tex && pal == o.pal;
    }
};

struct HdKeyHash {
    size_t operator()(const HdKey& k) const {
        return std::hash<const void*>()(k.tex) * 31 + std::hash<const void*>()(k.pal);
    }
};

std::unordered_map<HdKey, HdEntry, HdKeyHash> sCache;
std::unordered_map<uint64_t, HdEntry> sIconCache; // (raster offset << 32) | palette offset
std::unordered_map<const void*, const HdImage*> sImagesByAddr;
std::unordered_map<const void*, const char*> sUiImagesByAddr;
std::unordered_map<const void*, const char*> sUiPalettesByAddr;
std::unordered_map<const void*, const char*> sEntityImagesByAddr;
std::unordered_map<const void*, const char*> sEntityPalettesByAddr;
std::unordered_map<uint32_t, const HdIconRaster*> sIconRasters;
std::unordered_map<uint32_t, const HdIconPalette*> sIconPalettes;

// Icon buffers are heap/cache slots that get reused, so remember a hash of what was loaded.
struct IconBuf {
    uint32_t offset, size, hash;
};
std::unordered_map<const void*, IconBuf> sIconBufs;
uint32_t sLoaded = 0;

// Sprites are keyed as (isPlayer << 31 | sprite << 16 | raster or palette index).
constexpr uint32_t kPlayerBit = 1u << 31;
struct SpriteInfo {
    std::vector<std::array<uint8_t, kTlutBytes>> palettes;
    std::vector<int8_t> defaultPal;                   // per raster
    std::unordered_map<const void*, std::vector<uint16_t>> compRasters; // component cmdList -> rasters it shows
};
// Mirrors of the PORT-built SpriteAnimData (src/101b90_len_8f0.c).
struct GameRaster {
    uint8_t* image;
    uint8_t width, height;
    int8_t palette, quadCacheIndex;
};
struct GameAnimComp {
    uint16_t* cmdList;
    int16_t cmdListSize;
    int16_t offset[3];
};
struct GameSpriteData {
    GameRaster** rasters;
    uint8_t** palettes;
    int32_t maxComponents, colorVariations;
};
std::unordered_map<uint32_t, SpriteInfo> sSprites; // owner -> info
std::unordered_map<const void*, uint32_t> sSpritePalByAddr;
struct CompHdKey {
    const void* cmdList;
    uint32_t owner, overridePal;
    bool operator==(const CompHdKey& o) const {
        return cmdList == o.cmdList && owner == o.owner && overridePal == o.overridePal;
    }
};
struct CompHdKeyHash {
    size_t operator()(const CompHdKey& k) const {
        return std::hash<const void*>()(k.cmdList) * 31 + ((size_t)k.owner ^ k.overridePal);
    }
};
std::unordered_map<CompHdKey, bool, CompHdKeyHash> sCompHasHd; // cleared on sprite load: cmdLists move
std::unordered_map<const void*, const void*> sDrawComp;        // raster image -> cmdList it was last drawn by
uint32_t sFrame = 0;

// Large images (backgrounds, title, logos, portraits...) that the game draws as strips of rows.
struct HdRegion {
    uint32_t size, rowBytes, hash;
    std::string path;
    std::vector<uint8_t> pal; // CI only: the palette the pack's image was colored with
    bool hashed = true;       // static buffers are filled after init; hash them on first use
};
struct RegionSlot {
    HdEntry hd;
    uint32_t lastUse = 0;
};
std::map<const uint8_t*, HdRegion> sRegions;
std::unordered_map<std::string, RegionSlot> sRegionCache;

uint32_t Fnv1a(const void* p, uint32_t size) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < size; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

bool IconAt(const void* p, uint32_t* offset) {
    auto it = sIconBufs.find(p);
    if (it == sIconBufs.end()) {
        return false;
    }
    if (Fnv1a(p, it->second.size) != it->second.hash) {
        sIconBufs.erase(it);
        return false;
    }
    *offset = it->second.offset;
    return true;
}

// Map textures can be several KB and are checked on every draw, so only sample them.
uint32_t SampleHash(const void* p, uint32_t size) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    const uint32_t step = std::max(1u, size / 32);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < size; i += step) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

// VPixelScale is the exact HD/original pixel ratio; shrink to kTargetScale. Packs use odd ratios
// (13x, 5x...), so resample by kTargetScale/VPixelScale when that lands on whole pixels, else by
// the largest whole divisor. A fractional ratio would misplace rows. num == den: nothing to do.
void ShrinkRgba(const uint8_t* src, uint32_t srcW, uint32_t srcH, float vScale, std::vector<uint8_t>& out,
                uint32_t& num, uint32_t& den) {
    const uint32_t vp = (uint32_t)vScale;
    num = 1;
    den = 1;
    if (vp > kTargetScale && (float)vp == vScale && srcW * kTargetScale % vp == 0 &&
        srcH * kTargetScale % vp == 0) {
        num = kTargetScale;
        den = vp;
    } else {
        den = std::max(1u, (uint32_t)(vScale / kTargetScale));
        while (den > 1 && (srcW % den != 0 || srcH % den != 0)) {
            den--;
        }
    }
    if (num == den) {
        return;
    }

    const uint32_t dstW = srcW * num / den;
    const uint32_t dstH = srcH * num / den;
    out.resize(dstW * dstH * 4);
    for (uint32_t y = 0; y < dstH; y++) {
        const uint32_t sy0 = y * srcH / dstH, sy1 = (y + 1) * srcH / dstH;
        for (uint32_t x = 0; x < dstW; x++) {
            const uint32_t sx0 = x * srcW / dstW, sx1 = (x + 1) * srcW / dstW;
            // Alpha-weighted box filter so transparent texels don't darken the glyph outline.
            uint32_t r = 0, g = 0, b = 0, a = 0;
            for (uint32_t sy = sy0; sy < sy1; sy++) {
                const uint8_t* s = src + (sy * srcW + sx0) * 4;
                for (uint32_t sx = sx0; sx < sx1; sx++, s += 4) {
                    r += s[0] * s[3];
                    g += s[1] * s[3];
                    b += s[2] * s[3];
                    a += s[3];
                }
            }
            uint8_t* d = &out[(y * dstW + x) * 4];
            d[0] = a ? r / a : 0;
            d[1] = a ? g / a : 0;
            d[2] = a ? b / a : 0;
            d[3] = a / ((sx1 - sx0) * (sy1 - sy0));
        }
    }
}

bool LoadHd(const std::string& path, HdEntry& e) {
    auto rm = Ship::Context::GetInstance()->GetResourceManager();
    if (!rm->GetArchiveManager()->HasFile(path)) {
        return false;
    }
    auto res = rm->LoadResourceProcess(path, true);
    if (res == nullptr || res->GetInitData()->Type != static_cast<uint32_t>(Fast::ResourceType::Texture)) {
        return false;
    }
    auto tex = std::static_pointer_cast<Fast::Texture>(res);
    if (tex->Type != Fast::TextureType::RGBA32bpp || tex->ImageData == nullptr || tex->Width == 0 ||
        tex->Height == 0 || tex->ImageDataSize < (uint32_t)tex->Width * tex->Height * 4) {
        rm->UnloadResource(path);
        return false;
    }

    uint32_t num, den;
    ShrinkRgba(tex->ImageData, tex->Width, tex->Height, tex->VPixelScale, e.data, num, den);
    if (num == den) {
        e.data.assign(tex->ImageData, tex->ImageData + (size_t)tex->Width * tex->Height * 4);
    }
    e.width = tex->Width * num / den;
    e.height = tex->Height * num / den;
    e.hByteScale = tex->HByteScale * num / den;
    e.vPixelScale = tex->VPixelScale * num / den;
    e.valid = true;

    tex.reset();
    res.reset();
    rm->UnloadResource(path);
    return true;
}

// The path is only built on a cache miss; this runs for every glyph drawn each frame.
template <typename Map, typename Key, typename PathFn, typename LoadFn>
bool Fetch(Map& cache, const Key& key, PathFn makePath, LoadFn load, Fast::RawTexReplacement* out) {
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::string path = makePath();
        it = cache.emplace(key, HdEntry{}).first;
        if (load(path, it->second) && sLoaded++ == 0) {
            fprintf(stderr, "[HD] first replacement: %s -> %ux%u\n", path.c_str(), it->second.width,
                    it->second.height);
        }
    }
    const HdEntry& e = it->second;
    if (!e.valid) {
        return false;
    }
    out->data = e.data.data();
    out->width = e.width;
    out->height = e.height;
    out->h_byte_scale = e.hByteScale;
    out->v_pixel_scale = e.vPixelScale;
    return true;
}

template <typename Map, typename Key, typename PathFn>
bool Fetch(Map& cache, const Key& key, PathFn makePath, Fast::RawTexReplacement* out) {
    return Fetch(cache, key, makePath, LoadHd, out);
}

// The pack only has the glyph/palette pairs its dumper saw. A missing pair is the plain glyph recolored
// by a gray -> RGBA ramp learned from glyphs the pack has both plain and in that palette.
constexpr uint32_t kRampSteps = 32;
constexpr uint32_t kRampDonors = 2;
using CharsetRamp = std::array<std::array<float, 4>, kRampSteps>; // r, g, b, alpha factor
std::map<std::pair<const HdCharset*, uint32_t>, std::optional<CharsetRamp>> sCharsetRamps;

uint32_t RampStep(const uint8_t* px) {
    return (px[0] + px[1] + px[2]) / 3 * kRampSteps / 256;
}

std::optional<CharsetRamp> LearnCharsetRamp(const HdCharset& cs, const std::string& suffix) {
    std::array<std::array<float, 5>, kRampSteps> acc{};
    uint32_t donors = 0;
    for (uint32_t g = 0; g < cs.glyphCount && donors < kRampDonors; g++) {
        const std::string plainPath = cs.prefix + std::to_string(g);
        HdEntry plain, tinted;
        if (!LoadHd(plainPath + suffix, tinted) || !LoadHd(plainPath, plain) || plain.width != tinted.width ||
            plain.height != tinted.height) {
            continue;
        }
        donors++;
        for (size_t i = 0; i < plain.data.size(); i += 4) {
            const uint8_t* p = &plain.data[i];
            const uint8_t* t = &tinted.data[i];
            if (p[3] < 128) {
                continue;
            }
            auto& s = acc[RampStep(p)];
            s[0] += t[0];
            s[1] += t[1];
            s[2] += t[2];
            s[3] += (float)t[3] / p[3];
            s[4] += 1;
        }
    }

    std::vector<uint32_t> known;
    for (uint32_t k = 0; k < kRampSteps; k++) {
        if (acc[k][4] > 0) {
            known.push_back(k);
        }
    }
    if (known.empty()) {
        return std::nullopt;
    }
    CharsetRamp ramp;
    for (uint32_t k = 0; k < kRampSteps; k++) {
        auto hi = std::lower_bound(known.begin(), known.end(), k);
        uint32_t l, h;
        if (hi == known.end()) {
            l = h = known.back();
        } else if (*hi == k || hi == known.begin()) {
            l = h = *hi;
        } else {
            l = *(hi - 1);
            h = *hi;
        }
        const float t = h != l ? (float)(k - l) / (h - l) : 0;
        for (int c = 0; c < 4; c++) {
            ramp[k][c] = (acc[l][c] / acc[l][4]) * (1 - t) + (acc[h][c] / acc[h][4]) * t;
        }
    }
    return ramp;
}

bool RecolorGlyph(const HdCharset& cs, uint32_t glyph, uint32_t pal, HdEntry& e) {
    auto it = sCharsetRamps.find({ &cs, pal });
    if (it == sCharsetRamps.end()) {
        it = sCharsetRamps.emplace(std::make_pair(&cs, pal), LearnCharsetRamp(cs, cs.palPrefix + std::to_string(pal)))
                 .first;
    }
    if (!it->second || !LoadHd(cs.prefix + std::to_string(glyph), e)) {
        return false;
    }
    const CharsetRamp& ramp = *it->second;
    for (size_t i = 0; i < e.data.size(); i += 4) {
        uint8_t* p = &e.data[i];
        const auto& c = ramp[RampStep(p)];
        p[0] = (uint8_t)(c[0] + 0.5f);
        p[1] = (uint8_t)(c[1] + 0.5f);
        p[2] = (uint8_t)(c[2] + 0.5f);
        p[3] = (uint8_t)std::min(255.0f, p[3] * c[3] + 0.5f);
    }
    return true;
}

bool LookupCharset(const HdCharset& cs, const uint8_t* addr, const uint8_t* tlut, Fast::RawTexReplacement* out) {
    uint32_t off = (uint32_t)(addr - cs.raster);
    if (off % cs.glyphBytes != 0) {
        return false;
    }
    const uint8_t* palBase = reinterpret_cast<const uint8_t*>(D_802F4560);
    if (tlut < palBase || tlut >= palBase + kPalCount * kPalBytes || (tlut - palBase) % kPalBytes != 0) {
        return false;
    }
    uint32_t glyph = off / cs.glyphBytes;
    uint32_t pal = (uint32_t)(tlut - palBase) / kPalBytes;
    if (pal != 0 && cs.palPrefix == nullptr) {
        return false;
    }
    return Fetch(
        sCache, HdKey{ addr, tlut },
        [&] {
            // The pack bakes palette 0 into the plain name; other palettes are "@" variants.
            std::string path = cs.prefix + std::to_string(glyph);
            if (pal != 0) {
                path += cs.palPrefix + std::to_string(pal);
            }
            return path;
        },
        [&](const std::string& path, HdEntry& e) {
            return LoadHd(path, e) || (pal != 0 && RecolorGlyph(cs, glyph, pal, e));
        },
        out);
}

bool LookupImage(const HdImage& img, const uint8_t* tlut, Fast::RawTexReplacement* out) {
    const uint8_t* pal = nullptr;
    const char* suffix = "";
    if (img.basePal != nullptr) {
        if (tlut == nullptr) {
            return false;
        }
        if (memcmp(tlut, img.basePal, kTlutBytes) == 0) {
            pal = img.basePal;
        } else {
            for (const HdAlt& alt : img.alts) {
                if (alt.pal != nullptr && memcmp(tlut, alt.pal, kTlutBytes) == 0) {
                    pal = alt.pal;
                    suffix = alt.suffix;
                    break;
                }
            }
            if (pal == nullptr) {
                return false;
            }
        }
    }
    return Fetch(
        sCache, HdKey{ img.addr, pal }, [&] { return std::string(img.path) + suffix; }, out);
}

bool LookupIcon(const void* addr, const void* tlut, Fast::RawTexReplacement* out) {
    uint32_t rasterOff, palOff;
    if (!IconAt(addr, &rasterOff) || tlut == nullptr || !IconAt(tlut, &palOff)) {
        return false;
    }
    auto rit = sIconRasters.find(rasterOff);
    auto pit = sIconPalettes.find(palOff);
    if (rit == sIconRasters.end() || pit == sIconPalettes.end()) {
        return false;
    }
    const HdIconRaster& r = *rit->second;
    const HdIconPalette& p = *pit->second;
    return Fetch(
        sIconCache, ((uint64_t)rasterOff << 32) | palOff,
        [&] {
            // Plain name = the icon's own palette; shared/disabled ones are "@<palette owner>[.disabled].pal".
            std::string path = std::string("alt/") + r.path;
            if (p.disabled || strcmp(p.path, r.path) != 0) {
                const char* slash = strrchr(p.path, '/');
                path += std::string("@") + (slash ? slash + 1 : p.path) + (p.disabled ? ".disabled.pal" : ".pal");
            }
            return path;
        },
        out);
}

bool LookupUi(const void* addr, const char* path, uint32_t fmt, const void* tlut, Fast::RawTexReplacement* out) {
    if (fmt != G_IM_FMT_CI) {
        return Fetch(sCache, HdKey{ addr, nullptr }, [&] { return std::string("alt/") + path; }, out);
    }
    auto pit = sUiPalettesByAddr.find(tlut);
    if (pit == sUiPalettesByAddr.end()) {
        return false;
    }
    const char* palPath = pit->second;
    return Fetch(
        sCache, HdKey{ addr, tlut },
        [&] {
            // Plain name = image's own palette; others are "@<palette file>" variants in the same folder.
            std::string p = std::string("alt/") + path;
            if (strcmp(palPath, path) != 0) {
                const char* slash = strrchr(palPath, '/');
                p += std::string("@") + (slash ? slash + 1 : palPath) + ".pal";
            }
            return p;
        },
        out);
}

bool LookupEntity(const void* addr, const char* path, uint32_t fmt, const void* tlut, Fast::RawTexReplacement* out) {
    if (fmt != G_IM_FMT_CI) {
        return Fetch(sCache, HdKey{ addr, nullptr }, [&] { return std::string("alt/") + path; }, out);
    }
    auto pit = sEntityPalettesByAddr.find(tlut);
    if (pit == sEntityPalettesByAddr.end()) {
        return false;
    }
    const char* palPath = pit->second;
    return Fetch(
        sCache, HdKey{ addr, tlut },
        [&] {
            // Other palettes are "@tlut_<offset>" files; the plain one holds the entity's usual palette.
            const std::string base = std::string("alt/") + path;
            const std::string variant = base + "@" + (strrchr(palPath, '/') + 1);
            auto am = Ship::Context::GetInstance()->GetResourceManager()->GetArchiveManager();
            if (am->HasFile(variant)) {
                return variant;
            }
            const size_t dirLen = strrchr(path, '/') - path + 1;
            return strncmp(path, palPath, dirLen) == 0 ? base : std::string();
        },
        out);
}

std::string SpriteName(uint32_t owner) {
    char buf[32];
    if (owner & kPlayerBit) {
        snprintf(buf, sizeof(buf), "player_sprite_%u", (owner >> 16) & 0x7FFF);
    } else {
        snprintf(buf, sizeof(buf), "npc_sprite_%03u", (owner >> 16) & 0x7FFF);
    }
    return buf;
}

// The plain file holds the palette the pack's dumper saw first: normally the raster's own, but palette 0
// when the raster's own palette has an explicit @ file.
std::string SpriteHdPath(uint32_t owner, uint32_t raster, uint32_t palKey, int def) {
    auto am = Ship::Context::GetInstance()->GetResourceManager()->GetArchiveManager();
    const std::string base = "alt/sprites/" + SpriteName(owner) + "_raster_" + std::to_string(raster);
    const std::string alt = base + "@" + SpriteName(palKey & 0xFFFF0000u) + "_pal_" + std::to_string(palKey & 0xFFFF);
    if (am->HasFile(alt)) {
        return alt;
    }
    if ((palKey & 0xFFFF0000u) != owner || !am->HasFile(base)) {
        return "";
    }
    const int q = palKey & 0xFFFF;
    if (q == def || (q == 0 && def > 0 && am->HasFile(base + "@" + SpriteName(owner) + "_pal_" + std::to_string(def)))) {
        return base;
    }
    return "";
}

// Player back sprites keep their animations in the front sprite before them.
const SpriteInfo* AnimSource(uint32_t owner, const SpriteInfo& info) {
    if (!info.compRasters.empty() || !(owner & kPlayerBit) || (owner & 0x7FFF0000u) == 0) {
        return &info;
    }
    auto it = sSprites.find(owner - 0x10000u);
    return it != sSprites.end() ? &it->second : &info;
}

int DefaultPal(const SpriteInfo& info, uint32_t raster) {
    return raster < info.defaultPal.size() ? std::max<int>(0, info.defaultPal[raster]) : 0;
}

// A color variant the pack only has for some frames would flicker HD/SD within one component; keep those SD.
bool CompHasHd(uint32_t owner, const SpriteInfo& info, const void* cmdList, const std::vector<uint16_t>& rasters,
               uint32_t overridePal) {
    const CompHdKey key{ cmdList, owner, overridePal };
    auto it = sCompHasHd.find(key);
    if (it != sCompHasHd.end()) {
        return it->second;
    }
    bool ok = true;
    for (uint16_t r : rasters) {
        if (r >= info.defaultPal.size()) {
            continue;
        }
        const int def = DefaultPal(info, r);
        const uint32_t pal = def != 0 ? (owner | def) : overridePal;
        if (SpriteHdPath(owner, r, pal, def).empty()) {
            ok = false;
            break;
        }
    }
    sCompHasHd.emplace(key, ok);
    return ok;
}

std::string SpritePath(uint32_t rasterKey, uint32_t palKey) {
    const uint32_t owner = rasterKey & 0xFFFF0000u;
    const uint32_t raster = rasterKey & 0xFFFF;
    auto sit = sSprites.find(owner);
    return SpriteHdPath(owner, raster, palKey, sit != sSprites.end() ? DefaultPal(sit->second, raster) : 0);
}

// The pack only has the frames its dumper saw. Each sprite part (component) runs its own command list; one
// missing some of its frames would flicker HD/SD, so that part stays SD while the others can still be HD.
bool AnimFitsHd(uint32_t rasterKey, uint32_t palKey, const void* img) {
    const uint32_t owner = rasterKey & 0xFFFF0000u;
    auto sit = sSprites.find(owner);
    auto dit = sDrawComp.find(img);
    if (sit == sSprites.end() || dit == sDrawComp.end()) {
        return true;
    }
    const SpriteInfo& info = sit->second;
    const SpriteInfo* animInfo = AnimSource(owner, info);
    auto cit = animInfo->compRasters.find(dit->second);
    if (cit == animInfo->compRasters.end()) {
        return true;
    }
    // A raster on its own non-zero palette ignores the animation's color variant; assume the plain one.
    const int def = DefaultPal(info, rasterKey & 0xFFFF);
    const uint32_t overridePal = (def != 0 && palKey == (owner | def)) ? owner : palKey;
    return CompHasHd(owner, info, dit->second, cit->second, overridePal);
}

// HD file for a sprite raster drawn with this TLUT: the variant the pack colored with it, or "" for SD.
std::string SpriteVariant(uint32_t rasterKey, const void* img, const uint8_t* tlut) {
    // NPCs often draw from copies of their palettes, so match by content first.
    const uint32_t owner = rasterKey & 0xFFFF0000u;
    auto own = sSprites.find(owner);
    if (own != sSprites.end()) {
        const auto& pals = own->second.palettes;
        for (uint32_t q = 0; q < pals.size(); q++) {
            if (memcmp(tlut, pals[q].data(), kTlutBytes) != 0 || !AnimFitsHd(rasterKey, owner | q, img)) {
                continue;
            }
            std::string path = SpritePath(rasterKey, owner | q);
            if (!path.empty()) {
                return path;
            }
        }
    }
    auto pit = sSpritePalByAddr.find(tlut);
    if (pit == sSpritePalByAddr.end() || (pit->second & 0xFFFF0000u) == owner) {
        return "";
    }
    auto fit = sSprites.find(pit->second & 0xFFFF0000u);
    const uint32_t q = pit->second & 0xFFFF;
    if (fit == sSprites.end() || q >= fit->second.palettes.size() ||
        memcmp(tlut, fit->second.palettes[q].data(), kTlutBytes) != 0 || !AnimFitsHd(rasterKey, pit->second, img)) {
        return "";
    }
    return SpritePath(rasterKey, pit->second);
}

// Diagnostic: say once per image and reason why a large image stays SD.
void RegionSd(const std::string& path, const char* why) {
    static std::unordered_set<std::string> sLogged;
    if (sLogged.insert(path + why).second) {
        fprintf(stderr, "[HD] %s stays SD: %s\n", path.c_str(), why);
    }
}

bool LookupRegion(const uint8_t* addr, uint32_t fmt, const uint8_t* tlut, Fast::RawTexReplacement* out) {
    auto it = sRegions.upper_bound(addr);
    if (it == sRegions.begin()) {
        return false;
    }
    --it;
    const HdRegion& r = it->second;
    const uint32_t off = (uint32_t)(addr - it->first);
    if (off >= r.size) {
        return false;
    }
    if (off % r.rowBytes != 0) {
        RegionSd(r.path, "draw starts mid-row");
        return false;
    }
    if (!r.hashed) {
        it->second.hash = SampleHash(it->first, r.size);
        it->second.hashed = true;
    } else if (SampleHash(it->first, r.size) != r.hash) {
        RegionSd(r.path, "buffer changed since it was loaded");
        sRegions.erase(it);
        return false;
    }
    if ((fmt == G_IM_FMT_CI) != !r.pal.empty()) {
        RegionSd(r.path, "drawn in another format");
        return false;
    }
    if (!r.pal.empty() && (tlut == nullptr || memcmp(tlut, r.pal.data(), r.pal.size()) != 0)) {
        RegionSd(r.path, "drawn with another palette");
        return false;
    }

    auto cit = sRegionCache.find(r.path);
    if (cit == sRegionCache.end()) {
        cit = sRegionCache.emplace(r.path, RegionSlot{}).first;
        if (LoadHd(r.path, cit->second.hd) && sLoaded++ == 0) {
            fprintf(stderr, "[HD] first replacement: %s\n", r.path.c_str());
        }
    }
    cit->second.lastUse = sFrame;
    const HdEntry& e = cit->second.hd;
    // Strips start at a row of the image: hand over the HD rows from the matching one.
    const float hdRow = (off / r.rowBytes) * e.vPixelScale;
    if (!e.valid) {
        RegionSd(r.path, "HD file missing or not RGBA32");
        return false;
    }
    if (hdRow != (float)(uint32_t)hdRow || (uint32_t)hdRow >= e.height) {
        RegionSd(r.path, "strip does not land on a whole HD row");
        return false;
    }
    out->data = e.data.data() + (size_t)(uint32_t)hdRow * e.width * 4;
    out->width = e.width;
    out->height = e.height - (uint16_t)hdRow;
    out->h_byte_scale = e.hByteScale;
    out->v_pixel_scale = e.vPixelScale;
    return true;
}

bool PortRawTexLookup(const void* addr, uint32_t fmt, const void* tlut, Fast::RawTexReplacement* out) {
    auto rm = Ship::Context::GetInstance()->GetResourceManager();
    if (rm == nullptr || !rm->IsAltAssetsEnabled()) {
        return false;
    }
    const uint8_t* a = static_cast<const uint8_t*>(addr);
    const uint8_t* t = static_cast<const uint8_t*>(tlut);
    if (!sRegions.empty() && LookupRegion(a, fmt, t, out)) {
        return true;
    }
    for (const HdCharset& cs : kCharsets) {
        if (a >= cs.raster && a < cs.raster + cs.glyphCount * cs.glyphBytes) {
            return LookupCharset(cs, a, t, out);
        }
    }
    auto it = sImagesByAddr.find(addr);
    if (it != sImagesByAddr.end()) {
        return LookupImage(*it->second, t, out);
    }
    auto uit = sUiImagesByAddr.find(addr);
    if (uit != sUiImagesByAddr.end()) {
        return LookupUi(addr, uit->second, fmt, tlut, out);
    }
    auto eit = sEntityImagesByAddr.find(addr);
    if (eit != sEntityImagesByAddr.end()) {
        return LookupEntity(addr, eit->second, fmt, tlut, out);
    }
    if (fmt == G_IM_FMT_CI) {
        return LookupIcon(addr, tlut, out);
    }
    return false;
}

} // namespace

extern "C" void port_hd_icon_loaded(const void* dest, uint32_t iconOffset, uint32_t size) {
    if (dest != nullptr && size > 0) {
        sIconBufs[dest] = { iconOffset, size, Fnv1a(dest, size) };
    }
}

extern "C" void port_hd_sprite_loaded(int32_t isPlayer, int32_t sprite, const void* spriteAnimData) {
    const uint32_t owner = (isPlayer ? kPlayerBit : 0) | ((uint32_t)sprite << 16);
    const auto* sd = static_cast<const GameSpriteData*>(spriteAnimData);
    const void* const end = reinterpret_cast<const void*>(-1);
    SpriteInfo& info = sSprites[owner];
    info = SpriteInfo{};

    for (uint32_t p = 0; sd->palettes[p] != end; p++) {
        info.palettes.emplace_back();
        memcpy(info.palettes.back().data(), sd->palettes[p], kTlutBytes);
        sSpritePalByAddr[sd->palettes[p]] = owner | p;
    }
    for (uint32_t r = 0; sd->rasters[r] != end; r++) {
        info.defaultPal.push_back(sd->rasters[r]->palette);
    }

    auto* const* anims = reinterpret_cast<GameAnimComp** const*>(sd + 1);
    for (uint32_t a = 0; anims[a] != end; a++) {
        for (GameAnimComp* const* c = anims[a]; *c != end; c++) {
            const uint16_t* cmd = (*c)->cmdList;
            if (info.compRasters.count(cmd) != 0) {
                continue;
            }
            std::vector<uint16_t> rasters;
            const int n = (*c)->cmdListSize / 2;
            for (int i = 0; i < n;) {
                const uint16_t v = cmd[i];
                switch (v & 0xF000) {
                    case 0x1000:
                        if ((v & 0xFFF) != 0xFFF) {
                            rasters.push_back(v & 0xFFF);
                        }
                        i += 1;
                        break;
                    case 0x3000:
                        i += (v & 0xF) <= 1 ? 4 : 1;
                        break;
                    case 0x4000:
                        i += 3;
                        break;
                    case 0x5000:
                        i += (v & 0xF) <= 3 ? 2 : 1;
                        break;
                    case 0x7000:
                        i += 2;
                        break;
                    case 0x0000:
                    case 0x2000:
                    case 0x6000:
                    case 0x8000:
                        i += 1;
                        break;
                    default:
                        i = n;
                        break;
                }
            }
            std::sort(rasters.begin(), rasters.end());
            rasters.erase(std::unique(rasters.begin(), rasters.end()), rasters.end());
            info.compRasters.emplace(cmd, std::move(rasters));
        }
    }
    sCompHasHd.clear();
}

extern "C" void port_hd_sprite_draw(const void* img, const void* cmdList) {
    if (img != nullptr) {
        sDrawComp[img] = cmdList;
    }
}

void PortHdShrinkTexture(Fast::Texture* tex) {
    if (tex->Type != Fast::TextureType::RGBA32bpp || tex->ImageData == nullptr ||
        tex->ImageDataSize < (uint32_t)tex->Width * tex->Height * 4) {
        return;
    }
    std::vector<uint8_t> out;
    uint32_t num, den;
    ShrinkRgba(tex->ImageData, tex->Width, tex->Height, tex->VPixelScale, out, num, den);
    if (num == den) {
        return;
    }
    uint8_t* data = new uint8_t[out.size()];
    memcpy(data, out.data(), out.size());
    delete[] tex->ImageData;
    tex->ImageData = data;
    tex->ImageDataSize = out.size();
    tex->Width = tex->Width * num / den;
    tex->Height = tex->Height * num / den;
    tex->HByteScale = tex->HByteScale * num / den;
    tex->VPixelScale = tex->VPixelScale * num / den;
}

std::string PortHdSpriteVariant(const std::string& path, const void* img, const void* tlut,
                                const uint8_t** coverageTlut) {
    unsigned sprite, raster;
    bool isPlayer;
    if (sscanf(path.c_str(), "sprites/npc_sprite_%u_raster_%u", &sprite, &raster) == 2) {
        isPlayer = false;
    } else if (sscanf(path.c_str(), "sprites/player_sprite_%u_raster_%u", &sprite, &raster) == 2) {
        isPlayer = true;
    } else {
        return path;
    }
    if (tlut == nullptr) {
        return "";
    }
    const uint8_t* t = static_cast<const uint8_t*>(tlut);
    const uint32_t key = (isPlayer ? kPlayerBit : 0) | (sprite << 16) | raster;
    std::string variant = SpriteVariant(key, img, t);
    if (variant.empty()) {
        return "";
    }
    // Lit maps also read each sprite through a per-frame shading palette (tile 0, TLUT 1).
    if (gSpriteShadingProfile != nullptr && (*gSpriteShadingProfile & 1)) {
        *coverageTlut = t;
    }
    return variant.substr(4); // drop "alt/": the mod loader layers it back on
}

extern "C" void port_hd_frame() {
    sFrame++;
    // Title, logos and portraits are seen once in a while; drop their HD copies after ~10 s unused.
    if (sFrame % 120 == 0) {
        for (auto it = sRegionCache.begin(); it != sRegionCache.end();) {
            it = it->second.lastUse + 600 < sFrame ? sRegionCache.erase(it) : std::next(it);
        }
    }
}

extern "C" void port_hd_image_loaded(const void* base, uint32_t size, uint32_t rowBytes, const char* path,
                                     const void* pal, uint32_t palBytes) {
    if (base == nullptr || size == 0 || rowBytes == 0) {
        return;
    }
    HdRegion r;
    r.size = size;
    r.rowBytes = rowBytes;
    r.hash = SampleHash(base, size);
    r.path = std::string("alt/") + path;
    if (pal != nullptr) {
        r.pal.assign(static_cast<const uint8_t*>(pal), static_cast<const uint8_t*>(pal) + palBytes);
    }
    static std::unordered_set<std::string> sLogged;
    if (sLogged.insert(r.path).second) {
        fprintf(stderr, "[HD] image registered: %s (%u bytes, %u per row)\n", r.path.c_str(), size, rowBytes);
    }
    sRegions[static_cast<const uint8_t*>(base)] = std::move(r);
}

void PortHdTextures_Init() {
    for (const HdImage& img : kImages) {
        sImagesByAddr[img.addr] = &img;
    }
    for (const HdNamedAsset& a : kUiImages) {
        sUiImagesByAddr[a.addr] = a.path;
    }
    for (const HdNamedAsset& a : kUiPalettes) {
        sUiPalettesByAddr[a.addr] = a.path;
    }
    for (const HdNamedAsset& a : kEntityImages) {
        sEntityImagesByAddr[a.addr] = a.path;
    }
    for (const HdNamedAsset& a : kEntityPalettes) {
        sEntityPalettesByAddr[a.addr] = a.path;
    }
    for (const HdIconRaster& r : kIconRasters) {
        sIconRasters[r.offset] = &r;
    }
    for (const HdIconPalette& p : kIconPalettes) {
        sIconPalettes[p.offset] = &p;
    }
    // Window corners are four IA8 corners stacked in one buffer, each loaded from its own offset.
    const struct {
        const unsigned char* img;
        uint32_t width, height;
        const char* path;
    } kBoxCorners[] = {
        { ui_box_corners1_png, 16, 64, "alt/ui/box/corners1" }, { ui_box_corners4_png, 8, 32, "alt/ui/box/corners4" },
        { ui_box_corners5_png, 16, 32, "alt/ui/box/corners5" }, { ui_box_corners9_png, 16, 64, "alt/ui/box/corners9" },
    };
    for (const auto& c : kBoxCorners) {
        HdRegion r;
        r.size = c.width * c.height;
        r.rowBytes = c.width;
        r.path = c.path;
        r.hashed = false;
        sRegions[c.img] = std::move(r);
    }
    Fast::gRawTexReplacementLookup = PortRawTexLookup;
}
