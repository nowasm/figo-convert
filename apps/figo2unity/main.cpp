// figo2unity 鈥?convert a figo design (.fig / canvas.json / REST JSON) into
// Unity UGUI assets: one .prefab (YAML) per top-level frame + a shared
// textures/ folder of deduplicated PNG sprites (each with a TextureImporter
// .meta). The design's own ThorVG rasterizer bakes the sprites, so the
// textures are pixel-identical to the figo runtime.
//
// A .prefab is a sequence of YAML documents ("--- !u!<classId> &<fileID>"),
// one per serialized engine object (GameObject, RectTransform, CanvasRenderer,
// MonoBehaviour for UI.Image / UI.Text), wired by {fileID: N} references.
// Drop the output folder anywhere under a Unity project's Assets/ (keep the
// .meta files!) and drag the prefab into a Canvas.
//
// Node mapping:
//   TEXT                                   -> GameObject + UI.Text (built-in font)
//   plain solid rect / container bg        -> GameObject + UI.Image (no sprite = tinted quad)
//   ellipse / vector / gradient / image /
//     stroke / effect / rounded panel      -> GameObject + UI.Image (baked PNG; 9-slice)
//   container                              -> GameObject holding a bg image + children
//   anything empty                         -> GameObject (passthrough)
//
// Coordinate flip: every RectTransform anchors AND pivots at (0,1) (top-left),
// anchoredPosition = (relX, -relY), sizeDelta = the figo box 鈥?a clean
// Figma(Y-down) -> Unity UGUI(Y-up) mapping that composes correctly down the
// tree.
//
// Scope (v1): self-contained prefab per frame (no nested-prefab extraction),
// built-in font, axis-aligned (no rotation) 鈥?matching figo2cocos' shape.

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <figo/figo.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using figo::Node;
using figo::NodeType;

// PNG encoding is shared across the three exporters (Paeth + real deflate).
#include "../anim_tracks.h"
#include "../exporter_png.h"
using figopng::writePng;

// ===================== helpers ==============================================

static std::string sanitizeName(const std::string& name, const char* fallback) {
    std::string out;
    for (unsigned char c : name) {
        if (c < 0x20 || c == '/' || c == '\\')
            out.push_back('_');
        else
            out.push_back(static_cast<char>(c));
    }
    size_t bgn = out.find_first_not_of(' ');
    size_t end = out.find_last_not_of(' ');
    if (bgn == std::string::npos) return fallback;
    out = out.substr(bgn, end - bgn + 1);
    return out.empty() ? std::string(fallback) : out;
}

// Filesystem-safe stem (alnum/-/_), capped 鈥?for texture/prefab filenames.
// A name with no ASCII residue (e.g. pure CJK) would collapse to underscores
// and collide with every sibling like it 鈥?fall back to fallback+name-hash.
static std::string fileStem(const std::string& name, const char* fallback) {
    std::string stem;
    for (char c : sanitizeName(name, fallback))
        stem.push_back((std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_');
    if (stem.find_first_not_of("_-") == std::string::npos) {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : name) { h ^= c; h *= 1099511628211ull; }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s_%08x", fallback, (unsigned)(h & 0xffffffffu));
        return buf;
    }
    if (stem.size() > 40) stem.resize(40);
    return stem;
}

// Float -> compact YAML scalar, rounded to 0.01 (Unity writes plain decimals).
static std::string f2s(float v) {
    double r = std::round(v * 100.0) / 100.0;
    if (r == 0.0) return "0";  // normalize -0
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", r);
    return buf;
}

// Animation scalar -> YAML: full range (times in seconds, quaternion
// components, slopes — NO clamping), 1e-5 precision.
static std::string a2s(float v) {
    double r = std::round((double)v * 100000.0) / 100000.0;
    if (r == 0.0) return "0";  // normalize -0
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", r);
    return buf;
}

// Color channel [0,1] -> YAML (Unity colors are floats).
static std::string c2s(float v) {
    double r = std::round(std::max(0.0f, std::min(1.0f, v)) * 1000.0) / 1000.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", r);
    return buf;
}

// Double-quoted YAML string with the escapes Unity's parser understands.
static std::string yamlStr(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\x%02x", c); out += b; }
                else out.push_back(static_cast<char>(c));
        }
    }
    out += "\"";
    return out;
}

static bool isRectish(NodeType t) {
    switch (t) {
        case NodeType::Rectangle:
        case NodeType::Frame:
        case NodeType::Group:
        case NodeType::Section:
        case NodeType::Component:
        case NodeType::ComponentSet:
        case NodeType::Instance:
        case NodeType::Canvas:
            return true;
        default:
            return false;
    }
}

static const figo::Paint* solidFill(const Node& n) {
    for (const auto& p : n.fills)
        if (p.visible && p.type == figo::PaintType::Solid) return &p;
    return nullptr;
}
static bool hasVisibleStroke(const Node& n) {
    for (const auto& p : n.strokes)
        if (p.visible) return true;
    return false;
}
static bool hasVisibleEffect(const Node& n) {
    for (const auto& e : n.effects)
        if (e.visible) return true;
    return false;
}
static bool nonSolidVisibleFill(const Node& n) {
    for (const auto& p : n.fills)
        if (p.visible && p.type != figo::PaintType::Solid) return true;
    return false;
}
static float cornerR(const Node& n) {
    if (n.cornerRadius > 0) return n.cornerRadius;
    if (n.rectangleCornerRadii) {
        const auto& r = *n.rectangleCornerRadii;
        return std::max({r[0], r[1], r[2], r[3]});
    }
    return 0;
}

static bool subtreeHasText(const Node& n) {
    if (n.type == NodeType::Text) return true;
    for (const auto& c : n.children)
        if (subtreeHasText(*c)) return true;
    return false;
}
static bool subtreeHasVector(const Node& n) {
    switch (n.type) {
        case NodeType::Vector:
        case NodeType::BooleanOperation:
        case NodeType::Star:
        case NodeType::RegularPolygon:
        case NodeType::Line:
            return true;
        default:
            break;
    }
    for (const auto& c : n.children)
        if (subtreeHasVector(*c)) return true;
    return false;
}

// A multi-part vector icon (or boolean op) is best rasterized as one flat image.
static bool isVectorIcon(const Node& n) {
    if (n.type == NodeType::BooleanOperation) return true;
    if (n.children.empty()) return false;
    const float maxDim = std::max(n.width, n.height);
    if (maxDim > 128.0f) return false;
    if (subtreeHasText(n)) return false;
    return subtreeHasVector(n);
}

// A node whose appearance a flat tinted quad can't reproduce -> must be baked.
static bool needsBake(const Node& n) {
    if (!n.visible) return false;
    switch (n.type) {
        case NodeType::Ellipse:
        case NodeType::Vector:
        case NodeType::Line:
        case NodeType::Star:
        case NodeType::RegularPolygon:
        case NodeType::BooleanOperation:
            return true;
        default:
            break;
    }
    if (nonSolidVisibleFill(n)) return true;
    if (!n.fillGeometry.empty()) return true;
    if (hasVisibleStroke(n)) return true;
    if (hasVisibleEffect(n)) return true;
    if (cornerR(n) > 1.0f) return true;
    return false;
}

// ===================== prefab-extraction helpers ============================
// Ported from figo2godot: engine-agnostic identification of repeated
// components (cards/buttons/rows) worth extracting as reusable prefabs.

static bool colorNe(const figo::Color& a, const figo::Color& b) {
    return std::fabs(a.r - b.r) + std::fabs(a.g - b.g) + std::fabs(a.b - b.b) +
               std::fabs(a.a - b.a) > 0.002f;
}

// Count descendants (cheap gate against extracting trivial sub-groups).
static int descendants(const Node& n) {
    int c = 0;
    for (const auto& k : n.children) c += 1 + descendants(*k);
    return c;
}

// A generic/auto-generated container name (div_12, span3, css-1abc, node5,
// wrapper, "") carries no semantic meaning 鈥?not worth using as a prefab name.
static bool genericName(const std::string& s) {
    if (s.empty()) return true;
    std::string l;
    for (char c : s) l.push_back((char)std::tolower((unsigned char)c));
    static const char* pre[] = {"div", "span", "css", "node", "wrapper", "container",
                                "box", "group", "item", "frame", "rectangle", "ellipse",
                                "vector", "text", "line", "image", "instance", "component"};
    for (const char* p : pre) {
        size_t n = std::strlen(p);
        if (l.rfind(p, 0) != 0) continue;
        bool g = true;  // prefix followed only by digits / space / - / _ -> generic
        for (size_t i = n; i < l.size(); ++i) {
            char c = l[i];
            if (!(std::isdigit((unsigned char)c) || c == ' ' || c == '-' || c == '_')) { g = false; break; }
        }
        if (g) return true;
    }
    return false;
}
// First non-empty text in a subtree (for naming an anonymous container).
static std::string firstText(const Node& n) {
    if (n.type == NodeType::Text && !n.characters.empty()) return n.characters;
    for (const auto& c : n.children) {
        std::string s = firstText(*c);
        if (!s.empty()) return s;
    }
    return "";
}
// A readable name for an anonymous container: its own name if meaningful, else
// a short prefix (ASCII word or CJK run) of the first text in its subtree,
// else "Group".
static std::string genName(const Node& n) {
    if (!genericName(n.name)) return n.name;
    const std::string t = firstText(n);
    std::string out;
    size_t i = 0, cps = 0;
    while (i < t.size() && cps < 8) {
        const unsigned char c = (unsigned char)t[i];
        if (c < 0x80) {
            if (std::isalnum(c)) { out.push_back((char)c); ++i; ++cps; }
            else if (out.empty()) { ++i; }  // skip leading punctuation/space
            else break;                     // word boundary
        } else {  // multi-byte UTF-8 (CJK etc.) — keep whole codepoints
            const size_t len = (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3
                             : (c >> 3) == 0x1E ? 4 : 1;
            if (i + len > t.size()) break;
            out.append(t, i, len);
            i += len; ++cps;
        }
    }
    return out.empty() ? std::string("Group") : out;
}

// ===================== Unity GUIDs ==========================================

// 32-hex-char GUID from a 64-bit seed (deterministic, so the same content
// yields the same guid across runs and dedups cleanly).
static std::string makeGuid(uint64_t seed) {
    std::mt19937_64 rng(seed ? seed : 0x9E3779B97F4A7C15ull);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 4; ++i) {
        uint64_t v = rng();
        for (int k = 0; k < 8; ++k) {
            s.push_back(hex[(v >> (k * 8 + 4)) & 0xf]);
            s.push_back(hex[(v >> (k * 8)) & 0xf]);
        }
        if (s.size() >= 32) break;
    }
    s.resize(32);
    return s;
}

// FNV-1a 64 over a string (stable guid seeds from names/content).
static uint64_t fnv1a(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// ===================== fonts ================================================
// Bundle real .ttf/.otf files (--fonts DIR) so UI.Text uses the design's
// typography instead of the built-in font. Matching ported from figo2godot;
// each font also yields a line-height factor from its hhea/head tables so
// lineSpacing maps the design's pixel line height correctly.

struct FontEntry {
    std::string familyNorm;   // lowercased, spaces removed
    std::string family;       // as parsed (for the importer's fontNames)
    int weight = 400;
    bool italic = false;
    std::string file;         // basename under fonts/
    std::string guid;         // 32-hex meta guid
    float lineFactor = 1.2f;  // (hhea ascent - descent + lineGap) / unitsPerEm
};

static std::string normFamily(const std::string& s) {
    std::string out;
    for (char c : s)
        if (c != ' ') out.push_back((char)std::tolower((unsigned char)c));
    return out;
}

static uint16_t be16(const std::vector<uint8_t>& b, size_t o) {
    return o + 1 < b.size() ? (uint16_t)((b[o] << 8) | b[o + 1]) : 0;
}
static uint32_t be32(const std::vector<uint8_t>& b, size_t o) {
    return o + 3 < b.size() ? (uint32_t)((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) : 0;
}

// Parse a .ttf/.otf for family (name id 1), weight (OS/2 usWeightClass),
// italic (OS/2 fsSelection bit0) and the natural line-height factor
// (hhea metrics / head unitsPerEm). Falls back to the filename when absent.
static bool parseFontMeta(const fs::path& path, std::string& family, int& weight, bool& italic,
                          float& lineFactor) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12) return false;
    size_t base = 0;
    if (b[0] == 't' && b[1] == 't' && b[2] == 'c' && b[3] == 'f') base = be32(b, 12);  // .ttc -> font 0
    uint16_t numTables = be16(b, base + 4);
    size_t rec = base + 12;
    size_t nameOff = 0, os2Off = 0, hheaOff = 0, headOff = 0;
    for (uint16_t i = 0; i < numTables; ++i, rec += 16) {
        if (rec + 16 > b.size()) break;
        std::string tag((const char*)&b[rec], 4);
        uint32_t off = be32(b, rec + 8);
        if (tag == "name") nameOff = off;
        else if (tag == "OS/2") os2Off = off;
        else if (tag == "hhea") hheaOff = off;
        else if (tag == "head") headOff = off;
    }
    weight = 400;
    italic = false;
    family.clear();
    lineFactor = 1.2f;
    if (os2Off) {
        weight = be16(b, os2Off + 4);
        italic = (be16(b, os2Off + 62) & 0x01) != 0;
        if (weight <= 0) weight = 400;
    }
    if (hheaOff && headOff) {
        int16_t asc = (int16_t)be16(b, hheaOff + 4);
        int16_t desc = (int16_t)be16(b, hheaOff + 6);
        int16_t gap = (int16_t)be16(b, hheaOff + 8);
        uint16_t upm = be16(b, headOff + 18);
        if (upm && asc > 0) lineFactor = (float)(asc - desc + gap) / (float)upm;
    }
    if (nameOff) {
        uint16_t count = be16(b, nameOff + 2);
        size_t strBase = nameOff + be16(b, nameOff + 4);
        std::string best;
        for (uint16_t i = 0; i < count; ++i) {
            size_t r = nameOff + 6 + i * 12;
            uint16_t plat = be16(b, r), nameID = be16(b, r + 6);
            uint16_t len = be16(b, r + 8), so = be16(b, r + 10);
            if (nameID != 1) continue;  // family
            if (strBase + so + len > b.size()) continue;  // truncated record
            std::string s;
            if (plat == 3 || plat == 0) {  // UTF-16BE -> UTF-8 (CJK names survive)
                for (uint32_t k = 0; k + 1 < len; k += 2) {
                    uint32_t cp = ((uint32_t)b[strBase + so + k] << 8) | b[strBase + so + k + 1];
                    if (cp >= 0xD800 && cp <= 0xDBFF && k + 3 < len) {  // surrogate pair
                        const uint32_t lo =
                            ((uint32_t)b[strBase + so + k + 2] << 8) | b[strBase + so + k + 3];
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            k += 2;
                        }
                    }
                    if (cp < 0x80) {
                        s.push_back((char)cp);
                    } else if (cp < 0x800) {
                        s.push_back((char)(0xC0 | (cp >> 6)));
                        s.push_back((char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        s.push_back((char)(0xE0 | (cp >> 12)));
                        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        s.push_back((char)(0x80 | (cp & 0x3F)));
                    } else {
                        s.push_back((char)(0xF0 | (cp >> 18)));
                        s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                        s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                        s.push_back((char)(0x80 | (cp & 0x3F)));
                    }
                }
            } else {  // Mac ASCII
                for (uint16_t k = 0; k < len; ++k) s.push_back((char)b[strBase + so + k]);
            }
            if (plat == 3) best = s;
            else if (best.empty()) best = s;
        }
        family = best;
    }
    if (family.empty()) family = path.stem().string();
    return true;
}

// Built-in engine references (stable across Unity versions):
//   UI.Image script guid, UI.Text script guid (UnityEngine.UI assembly), and
//   the built-in font (Arial.ttf, LegacyRuntime.ttf since 2022.2 鈥?same ref).
static const char* kImageScript = "fe87c0e1cc204ed48ad3b37840f39efc";
static const char* kTextScript = "5f7201a12d95ffc409449d95f23cf332";
static const char* kScrollRectScript = "1aa08ab6e0800fa44ae55d278d1423e3";
static const char* kRectMask2DScript = "3312d7739989d2b4e91e6319e9a96d76";
static const char* kHLayoutScript = "30649d3a9faa99c48a7b1166b86bf2a0";
static const char* kVLayoutScript = "59f8146938fff824cb5fd77236b75775";
static const char* kLayoutElementScript = "306cc8c2b49d7114eaa3623786fc2126";
// TextMeshProUGUI + the default "LiberationSans SDF" font asset from the TMP
// essential resources (the target project must have them imported).
static const char* kTmpTextScript = "f4688fdb7df04437aeb418b961361dc5";
static const char* kTmpDefaultFontAsset = "8f586378b4e144a9851e7b34d9b748ee";
static const char* kBuiltinFont = "0000000000000000e000000000000000";

// ===================== meta templates =======================================

static const char* kTextureMeta = R"META(fileFormatVersion: 2
guid: $GUID
TextureImporter:
  internalIDToNameTable: []
  externalObjects: {}
  serializedVersion: 12
  mipmaps:
    mipMapMode: 0
    enableMipMap: 0
    sRGBTexture: 1
    linearTexture: 0
    fadeOut: 0
    borderMipMap: 0
    mipMapsPreserveCoverage: 0
    alphaTestReferenceValue: 0.5
    mipMapFadeDistanceStart: 1
    mipMapFadeDistanceEnd: 3
  bumpmap:
    convertToNormalMap: 0
    externalNormalMap: 0
    heightScale: 0.25
    normalMapFilter: 0
  isReadable: 0
  streamingMipmaps: 0
  streamingMipmapsPriority: 0
  vTOnly: 0
  ignoreMasterTextureLimit: 0
  grayScaleToAlpha: 0
  generateCubemap: 6
  cubemapConvolution: 0
  seamlessCubemap: 0
  textureFormat: 1
  maxTextureSize: 2048
  textureSettings:
    serializedVersion: 2
    filterMode: $FILTER
    aniso: 1
    mipBias: 0
    wrapU: 1
    wrapV: 1
    wrapW: 1
  nPOTScale: 0
  lightmap: 0
  compressionQuality: 50
  spriteMode: 1
  spriteExtrude: 1
  spriteMeshType: 1
  alignment: 0
  spritePivot: {x: 0.5, y: 0.5}
  spritePixelsToUnits: 100
  spriteBorder: {x: $BL, y: $BB, z: $BR, w: $BT}
  spriteGenerateFallbackPhysicsShape: 0
  alphaUsage: 1
  alphaIsTransparency: 1
  spriteTessellationDetail: -1
  textureType: 8
  textureShape: 1
  singleChannelComponent: 0
  flipbookRows: 1
  flipbookColumns: 1
  maxTextureSizeSet: 0
  compressionQualitySet: 0
  textureFormatSet: 0
  ignorePngGamma: 0
  applyGammaDecoding: 0
  platformSettings:
  - serializedVersion: 3
    buildTarget: DefaultTexturePlatform
    maxTextureSize: 2048
    resizeAlgorithm: 0
    textureFormat: -1
    textureCompression: 1
    compressionQuality: 50
    crunchedCompression: 0
    allowsAlphaSplitting: 0
    overridden: 0
    androidETC2FallbackOverride: 0
    forceMaximumCompressionQuality_BC6H_BC7: 0
  spriteSheet:
    serializedVersion: 2
    sprites: []
    outline: []
    physicsShape: []
    bones: []
    spriteID: 5e97eb03825dee720800000000000000
    internalID: 0
    vertices: []
    indices:
    edges: []
    weights: []
    secondaryTextures: []
    nameFileIdTable: {}
  pSDRemoveMatte: 0
  pSDShowRemoveMatteOption: 0
  userData:
  assetBundleName:
  assetBundleVariant:
)META";

static const char* kFontMeta = R"META(fileFormatVersion: 2
guid: $GUID
TrueTypeFontImporter:
  externalObjects: {}
  serializedVersion: 4
  fontSize: 16
  forceTextureCase: -2
  characterSpacing: 0
  characterPadding: 1
  includeFontData: 1
  fontNames:
  - $FAMILY
  fallbackFontReferences: []
  customCharacters:
  fontRenderingMode: 0
  ascentCalculationMode: 1
  useLegacyBoundsCalculation: 0
  shouldRoundAdvanceValue: 1
  userData:
  assetBundleName:
  assetBundleVariant:
)META";

static const char* kPrefabMeta = R"META(fileFormatVersion: 2
guid: $GUID
PrefabImporter:
  externalObjects: {}
  userData:
  assetBundleName:
  assetBundleVariant:
)META";

// UI/Default clone with LINEAR-space alpha compensation for glyph coverage.
// In a Linear project UGUI blends glyph AA coverage in linear space, so small
// dark text reads washed-out/thin ("text not black enough"). Remapping the
// coverage — dark text 1-(1-a)^2.2, bright text a^2.2, luminance-lerped —
// makes the linear blend land on the gamma-blend result the design was
// authored against (exact for dark-on-any and bright-on-dark). In a Gamma
// project UNITY_COLORSPACE_GAMMA disables the remap entirely.
static const char* kTextShader = R"SHDR(Shader "Figo/UIText"
{
    Properties
    {
        [PerRendererData] _MainTex ("Sprite Texture", 2D) = "white" {}
        _Color ("Tint", Color) = (1,1,1,1)
        _StencilComp ("Stencil Comparison", Float) = 8
        _Stencil ("Stencil ID", Float) = 0
        _StencilOp ("Stencil Operation", Float) = 0
        _StencilWriteMask ("Stencil Write Mask", Float) = 255
        _StencilReadMask ("Stencil Read Mask", Float) = 255
        _ColorMask ("Color Mask", Float) = 15
        [Toggle(UNITY_UI_ALPHACLIP)] _UseUIAlphaClip ("Use Alpha Clip", Float) = 0
    }
    SubShader
    {
        Tags
        {
            "Queue"="Transparent" "IgnoreProjector"="True" "RenderType"="Transparent"
            "PreviewType"="Plane" "CanUseSpriteAtlas"="True"
        }
        Stencil
        {
            Ref [_Stencil]
            Comp [_StencilComp]
            Pass [_StencilOp]
            ReadMask [_StencilReadMask]
            WriteMask [_StencilWriteMask]
        }
        Cull Off
        Lighting Off
        ZWrite Off
        ZTest [unity_GUIZTestMode]
        Blend SrcAlpha OneMinusSrcAlpha
        ColorMask [_ColorMask]
        Pass
        {
            Name "Default"
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 2.0
            #include "UnityCG.cginc"
            #include "UnityUI.cginc"
            #pragma multi_compile_local _ UNITY_UI_CLIP_RECT
            #pragma multi_compile_local _ UNITY_UI_ALPHACLIP

            struct appdata_t
            {
                float4 vertex   : POSITION;
                float4 color    : COLOR;
                float2 texcoord : TEXCOORD0;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };
            struct v2f
            {
                float4 vertex   : SV_POSITION;
                fixed4 color    : COLOR;
                float2 texcoord : TEXCOORD0;
                float4 worldPosition : TEXCOORD1;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            sampler2D _MainTex;
            fixed4 _Color;
            fixed4 _TextureSampleAdd;
            float4 _ClipRect;
            float4 _MainTex_ST;
            float _UIVertexColorAlwaysGammaSpace;

            v2f vert(appdata_t v)
            {
                v2f OUT;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(OUT);
                OUT.worldPosition = v.vertex;
                OUT.vertex = UnityObjectToClipPos(OUT.worldPosition);
                OUT.texcoord = TRANSFORM_TEX(v.texcoord, _MainTex);
                // Canvas.vertexColorAlwaysGammaSpace passes gamma-encoded
                // vertex colors; convert like the built-in UI/Default does.
                #ifndef UNITY_COLORSPACE_GAMMA
                if (_UIVertexColorAlwaysGammaSpace > 0)
                    v.color.rgb = GammaToLinearSpace(v.color.rgb);
                #endif
                OUT.color = v.color * _Color;
                return OUT;
            }

            fixed4 frag(v2f IN) : SV_Target
            {
                half4 color = (tex2D(_MainTex, IN.texcoord) + _TextureSampleAdd) * IN.color;
                #ifndef UNITY_COLORSPACE_GAMMA
                half lum = pow(saturate(dot(saturate(color.rgb), half3(0.299, 0.587, 0.114))), 0.4545);
                half aDark = 1.0 - pow(saturate(1.0 - color.a), 2.2);
                half aBright = pow(saturate(color.a), 2.2);
                color.a = lerp(aDark, aBright, lum);
                #endif
                #ifdef UNITY_UI_CLIP_RECT
                color.a *= UnityGet2DClipping(IN.worldPosition.xy, _ClipRect);
                #endif
                #ifdef UNITY_UI_ALPHACLIP
                clip (color.a - 0.001);
                #endif
                return color;
            }
            ENDCG
        }
    }
}
)SHDR";

static const char* kShaderMeta = R"META(fileFormatVersion: 2
guid: $GUID
ShaderImporter:
  externalObjects: {}
  defaultTextures: []
  nonModifiableTextures: []
  preprocessorOverride: 0
  userData:
  assetBundleName:
  assetBundleVariant:
)META";

static const char* kTextMaterial = R"MAT(%YAML 1.1
%TAG !u! tag:unity3d.com,2011:
--- !u!21 &2100000
Material:
  serializedVersion: 8
  m_ObjectHideFlags: 0
  m_CorrespondingSourceObject: {fileID: 0}
  m_PrefabInstance: {fileID: 0}
  m_PrefabAsset: {fileID: 0}
  m_Name: FigoText
  m_Shader: {fileID: 4800000, guid: $SHADER, type: 3}
  m_Parent: {fileID: 0}
  m_ModifiedSerializedProperties: 0
  m_ValidKeywords: []
  m_InvalidKeywords: []
  m_LightmapFlags: 4
  m_EnableInstancingVariants: 0
  m_DoubleSidedGI: 0
  m_CustomRenderQueue: -1
  stringTagMap: {}
  disabledShaderPasses: []
  m_LockedProperties:
  m_SavedProperties:
    serializedVersion: 3
    m_TexEnvs:
    - _MainTex:
        m_Texture: {fileID: 0}
        m_Scale: {x: 1, y: 1}
        m_Offset: {x: 0, y: 0}
    m_Ints: []
    m_Floats:
    - _ColorMask: 15
    - _Stencil: 0
    - _StencilComp: 8
    - _StencilOp: 0
    - _StencilReadMask: 255
    - _StencilWriteMask: 255
    - _UseUIAlphaClip: 0
    m_Colors:
    - _Color: {r: 1, g: 1, b: 1, a: 1}
  m_BuildTextureStacks: []
)MAT";

static const char* kMatMeta = R"META(fileFormatVersion: 2
guid: $GUID
NativeFormatImporter:
  externalObjects: {}
  mainObjectFileID: 2100000
  userData:
  assetBundleName:
  assetBundleVariant:
)META";

static std::string subst(std::string tpl, const std::vector<std::pair<std::string, std::string>>& kv) {
    for (const auto& [k, v] : kv) {
        size_t p = 0;
        while ((p = tpl.find(k, p)) != std::string::npos) { tpl.replace(p, k.size(), v); p += v.size(); }
    }
    return tpl;
}

// ===================== converter ============================================

struct Converter {
    figo::FigmaUI* ui = nullptr;
    fs::path outDir, texDir;
    int superScale = 2;
    uint32_t curW = 0, curH = 0;

    struct Sprite {
        std::string file;  // basename, e.g. "icon_ab12cd34.png"
        std::string guid;  // 32-hex texture guid (sprite sub-asset = fileID 21300000)
        int w = 0, h = 0;
        bool nine = false;
        int ml = 0, mr = 0, mt = 0, mb = 0;  // 9-slice borders (logical px)
    };
    std::map<uint64_t, Sprite> sprites;  // content hash -> sprite (global dedup)
    std::map<std::string, uint64_t> usedSpriteNames;  // filename -> full hash (8-hex truncation collision detector)

    // Bundled fonts (--fonts DIR); populated by main.
    std::vector<FontEntry> fonts;

    // Best font file for a (family, weight, italic): exact family first, then
    // nearest weight in the family; else a Unicode-broad Noto Sans fallback so
    // CJK/symbols keep consistent metrics (same policy as figo2godot).
    const FontEntry* bestInFamily(const std::string& fam, int weight, bool italic) {
        const FontEntry* best = nullptr;
        int bestScore = 1 << 30;
        for (const auto& fe : fonts) {
            if (fe.familyNorm != fam) continue;
            int score = std::abs(fe.weight - weight) + (fe.italic == italic ? 0 : 1000);
            if (score < bestScore) { bestScore = score; best = &fe; }
        }
        return best;
    }
    const FontEntry* matchFont(const std::string& family, int weight, bool italic) {
        if (const FontEntry* fe = bestInFamily(normFamily(family), weight, italic)) return fe;
        const FontEntry* fb = nullptr;
        int fbScore = 1 << 30;
        for (const auto& fe : fonts) {
            if (fe.familyNorm.rfind("notosans", 0) != 0) continue;
            int score = std::abs(fe.weight - weight) + (fe.italic == italic ? 0 : 1000);
            if (score < fbScore) { fbScore = score; fb = &fe; }
        }
        return fb;
    }

    // ---- prefab extraction (--prefabs), ported from figo2godot ----
    bool prefabs = false;
    bool prefabAnon = false;   // --prefab-anon: also extract repeated ANONYMOUS containers
    bool tmp = false;          // --tmp: TextMeshProUGUI instead of legacy UI.Text
    bool linearComp = false;   // --linear: 预补偿半透明 alpha,近似 Linear 色彩空间下的 sRGB 观感

    // --linear:Linear 色彩空间工程的 UGUI 在线性空间做半透明混合——亮色辉光叠
    // 深底比 sRGB 设计稿亮 50%+,暗色遮罩叠亮底则偏浅。精确补偿依赖真实背景,
    // 无通解;按源像素亮度假定一个典型背景亮度 D(亮源多叠深底 D=0.1、暗源多
    // 叠亮底 D=0.9,中段平滑过渡),再解出使线性混合结果等于 sRGB 设计观感的
    // alpha:a' = (T^γ - D^γ) / (S^γ - D^γ),T = (1-a)·D + a·S。假定底上精确,
    // 一般深色 UI 偏差在几个灰阶内。只动 alpha 不动色值。
    static float compAlpha(float a, float lumR, float lumG, float lumB) {
        if (a <= 0.f || a >= 1.f) return a;
        const float g = 2.2f;
        const float S = 0.2126f * lumR + 0.7152f * lumG + 0.0722f * lumB;
        float t = (S - 0.3f) / 0.4f;
        t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        t = t * t * (3.f - 2.f * t);                       // smoothstep(0.3, 0.7, S)
        const float D = 0.9f + (0.1f - 0.9f) * t;          // 假定背景亮度
        const float T = (1.f - a) * D + a * S;             // sRGB 设计观感(亮度)
        const float Tl = std::pow(T, g), Dl = std::pow(D, g), Sl = std::pow(S, g);
        if (std::fabs(Sl - Dl) < 1e-5f) return a;          // 源≈假定底,无从解,保持
        const float v = (Tl - Dl) / (Sl - Dl);
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }
    static void linearAlphaComp(std::vector<uint32_t>& px, int w) {
        // 补偿曲线在低 alpha 端很陡(1..45 压进 0..14),直接四舍五入会把渐变
        // 量化成可见台阶、微弱外圈(1..4)整段归零形成生硬截止边。用 Bayer 8x8
        // 有序抖动量化:平均覆盖率不变,台阶打散为平滑过渡,外圈尾巴以稀疏
        // 像素保留。
        static const int B[8][8] = {
            { 0, 32,  8, 40,  2, 34, 10, 42}, {48, 16, 56, 24, 50, 18, 58, 26},
            {12, 44,  4, 36, 14, 46,  6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
            { 3, 35, 11, 43,  1, 33,  9, 41}, {51, 19, 59, 27, 49, 17, 57, 25},
            {15, 47,  7, 39, 13, 45,  5, 37}, {63, 31, 55, 23, 61, 29, 53, 21}};
        for (size_t i = 0; i < px.size(); ++i) {
            uint32_t& p = px[i];
            const uint32_t a = (p >> 24) & 0xff;
            if (a == 0 || a == 255) continue;
            const float av = 255.f * compAlpha(a / 255.f, (p & 0xff) / 255.f,
                                               ((p >> 8) & 0xff) / 255.f, ((p >> 16) & 0xff) / 255.f);
            const int x = (int)(i % (size_t)w), y = (int)(i / (size_t)w);
            const float thr = (B[y & 7][x & 7] + 0.5f) / 64.f;
            int q = (int)std::floor(av + thr);
            q = q < 0 ? 0 : (q > 255 ? 255 : q);
            p = (p & 0x00ffffffu) | ((uint32_t)q << 24);
        }
    }
    bool inComponent = false;  // true while emitting a component prefab (no nesting-instances)
    std::set<std::string> noPrefab;   // compTypes never extracted (--no-prefab)
    std::set<std::string> prefabPin;  // compTypes ALWAYS extracted (hand-picked captures)
    std::set<std::string> usedComps;  // comp names actually instanced by a frame

    // fileIDs of the objects emitted for one node of a component prefab, keyed
    // by its path from the component root ("" = root, "Row/Title", "鈥?__bg").
    // Instance overrides target these ids inside the source prefab.
    struct CompNodeIds { int64_t go = 0, rt = 0, text = 0, img = 0; };
    struct Comp {
        std::string name;             // unique component name / file stem
        std::string guid;             // components/<name>.prefab meta guid
        const Node* canon = nullptr;  // canonical (superset) instance
        Node* frame = nullptr;        // canon's top-level frame (for render context)
        int count = 0;
        bool extracted = false;
        std::vector<std::pair<const Node*, Node*>> insts;  // (instance node, its frame)
        std::map<std::string, CompNodeIds> ids;            // filled by emitComponentPrefab
    };
    std::map<std::string, Comp> compBySig;  // groupKey -> all its instances
    std::vector<std::unique_ptr<Comp>> variants;  // structural clusters (stable addresses)
    std::map<const Node*, Comp*> nodeComp;        // instance node -> its cluster

    // Structural signature (see figo2godot for the full rationale): groups
    // visually-identical components; struct_ drops text/solid-color values and
    // image refs (those become per-instance overrides); maxDepth==0 keeps only
    // the immediate children's bare types (slot identity across state variants).
    std::string sig(const Node& n, bool struct_ = false, int maxDepth = -1) const {
        std::string s = std::to_string((int)n.type);
        if (n.type == NodeType::Text) {
            s += struct_ ? "T" : ("T" + std::to_string((int)(n.textStyle.fontSize + 0.5f)) + n.textStyle.fontFamily);
        } else {
            bool hasImg = false;
            for (const auto& p : n.fills) if (p.visible && p.type == figo::PaintType::Image) { hasImg = true; break; }
            if (!struct_) s += "#" + std::to_string((int)std::lround(n.width)) + "x" +
                               std::to_string((int)std::lround(n.height));
            else if (hasImg) s += "#" + std::to_string((int)std::lround(n.width) / 16) + "x" +
                                  std::to_string((int)std::lround(n.height) / 16);
            for (const auto& p : n.fills) {
                if (!p.visible) continue;
                if (p.type == figo::PaintType::Solid) {
                    if (struct_) s += "s";
                    else { char c[10]; std::snprintf(c, sizeof(c), "s%02x%02x%02x",
                        (int)(p.color.r * 255), (int)(p.color.g * 255), (int)(p.color.b * 255)); s += c; }
                } else if (p.type == figo::PaintType::Image) s += struct_ ? "i" : "i" + p.imageRef;
                else s += "g";
            }
            if (!n.strokes.empty()) s += "k";
            if (cornerR(n) > 0) s += "r";
            if (hasVisibleEffect(n)) s += "e";
        }
        s += "{";
        for (const auto& c : n.children) {
            if (!struct_ && c->type != NodeType::Text)
                s += "@" + std::to_string((int)std::lround(c->relativeTransform.m02)) + "," +
                     std::to_string((int)std::lround(c->relativeTransform.m12));
            if (maxDepth == 0)
                s += std::to_string((int)c->type) + (c->compType.empty() ? "" : "c" + c->compType) + ";";
            else
                s += sig(*c, struct_, maxDepth < 0 ? -1 : maxDepth - 1) + ";";
        }
        s += "}";
        return s;
    }
    // Grouping key: source components group by compType + coarse size bucket
    // (state variants collapse into one prefab); anonymous containers keep the
    // structural sig in the key.
    std::string groupKey(const Node& n) const {
        const int wb = (int)std::lround(n.width) / 64, hb = (int)std::lround(n.height) / 64;
        if (!n.compType.empty())
            return n.compType + "|" + std::to_string(wb) + "x" + std::to_string(hb);
        return "|" + std::to_string(wb) + "x" + std::to_string(hb) + "|" + sig(n, true);
    }
    // Slot identity for matching a child across a component's state variants.
    std::string slotKey(const Node& n) const {
        return (n.compType.empty() ? "" : "c:" + n.compType + "|") + sig(n, true, 0);
    }
    // Match-quality gate: a variant reusing <60% of the canon's children (or of
    // its own) renders better inlined than forced onto the prefab.
    bool poorFit(const Node& canon, const Node& inst) const {
        const Node* c = &canon;
        const Node* i = &inst;
        while (c->children.size() == 1 && i->children.size() == 1) {
            c = c->children[0].get();
            i = i->children[0].get();
        }
        const size_t cc = c->children.size(), ic = i->children.size();
        if (cc == 0) return false;
        std::vector<char> usedI(ic, 0);
        size_t matched = 0;
        for (const auto& cch : c->children) {
            const std::string key = slotKey(*cch);
            for (size_t j = 0; j < ic; ++j)
                if (!usedI[j] && slotKey(*i->children[j]) == key) { usedI[j] = 1; ++matched; break; }
        }
        return matched * 5 < cc * 3 || (ic > 0 && matched * 5 < ic * 3);
    }
    static std::string imageRefOf(const Node& n) {
        for (const auto& p : n.fills)
            if (p.visible && p.type == figo::PaintType::Image) return p.imageRef;
        return "";
    }
    // Tally repeated component candidates across a frame's tree.
    void scanComponents(Node* frame) {
        std::function<void(Node&)> rec = [&](Node& n) {
            for (auto& c : n.children) {
                Node& cn = *c;
                const bool isComp = !cn.compType.empty();
                const bool anon = prefabAnon && cn.compType.empty();
                const bool pinned = isComp && prefabPin.count(cn.compType);
                if ((isComp || anon) && !cn.children.empty() && cn.type != NodeType::Text &&
                    (pinned || (!isVectorIcon(cn) && cn.width > 4 && cn.height > 4 && descendants(cn) >= 3))) {
                    auto& comp = compBySig[groupKey(cn)];
                    comp.count++;
                    comp.insts.push_back({ &cn, frame });
                    if (comp.name.empty()) comp.name = isComp ? cn.compType : genName(cn);
                }
                rec(cn);
            }
        };
        rec(*frame);
    }
    const Comp* extractedComponent(const Node& n) {
        if (!prefabs || inComponent || n.children.empty() || n.type == NodeType::Text) return nullptr;
        auto it = nodeComp.find(&n);
        return (it != nodeComp.end() && it->second->extracted) ? it->second : nullptr;
    }

    // While emitting a component prefab, emit() records each node's fileIDs
    // here (keyed by path) so instances can target them in m_Modifications.
    std::map<std::string, CompNodeIds>* rec = nullptr;

    // per-prefab YAML document list + a deterministic fileID generator.
    // fileIDs must look RANDOM (splitmix64 over a per-prefab seed), not
    // sequential: Unity's PrefabImporter derives the imported object ids of a
    // nested-prefab instance by MIXING the source fileID with the
    // PrefabInstance fileID — small sequential ids in both files collide there
    // ("Assertion failed: fidA != fidB" on import). Seeded per prefab, so
    // reruns stay stable.
    std::string yaml;
    uint64_t idState = 0;
    int objCount = 0;
    int64_t newId() {
        ++objCount;
        uint64_t z = (idState += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        int64_t v = (int64_t)(z & 0x7fffffffffffffffull);
        return v ? v : 1;
    }
    void doc(int classId, int64_t fileId, const std::string& body) {
        yaml += "--- !u!" + std::to_string(classId) + " &" + std::to_string(fileId) + "\n";
        yaml += body;
    }

    // ---- 9-slice shrink (identical to figo2godot/figo2cocos) ----
    static bool nineShrink(std::vector<uint32_t>& px, int& w, int& h, int scale,
                           int& ml, int& mr, int& mt, int& mb) {
        if (w < 2 * scale + 1 && h < 2 * scale + 1) return false;
        auto colEq = [&](int a, int b) {
            for (int y = 0; y < h; ++y) if (px[(size_t)y * w + a] != px[(size_t)y * w + b]) return false;
            return true;
        };
        auto rowEq = [&](int a, int b) {
            for (int x = 0; x < w; ++x) if (px[(size_t)a * w + x] != px[(size_t)b * w + x]) return false;
            return true;
        };
        int cs = 0, ce = 0, s = 0;
        for (int x = 1; x < w; ++x) { if (colEq(x, x - 1)) { if (x - s > ce - cs) { cs = s; ce = x; } } else s = x; }
        int rs = 0, re = 0; s = 0;
        for (int y = 1; y < h; ++y) { if (rowEq(y, y - 1)) { if (y - s > re - rs) { rs = s; re = y; } } else s = y; }
        const bool cw9 = ce - cs >= scale, ch9 = re - rs >= scale;
        if (!cw9 && !ch9) return false;
        ml = cs / scale; mr = (w - 1 - ce) / scale; mt = rs / scale; mb = (h - 1 - re) / scale;
        const int nw = ml + 1 + mr, nh = mt + 1 + mb;
        std::vector<uint32_t> out((size_t)nw * nh);
        auto srcX = [&](int ox) { return ox < ml ? ox * scale : (ox == ml ? cs : ce + 1 + (ox - ml - 1) * scale); };
        auto srcY = [&](int oy) { return oy < mt ? oy * scale : (oy == mt ? rs : re + 1 + (oy - mt - 1) * scale); };
        const int blk = scale;
        for (int oy = 0; oy < nh; ++oy) {
            const int sy = srcY(oy), bh = (oy == mt) ? 1 : blk;
            for (int ox = 0; ox < nw; ++ox) {
                const int sx = srcX(ox), bw2 = (ox == ml) ? 1 : blk;
                uint32_t r = 0, g = 0, b = 0, a = 0; int cnt = 0;
                for (int yy = 0; yy < bh && sy + yy < h; ++yy)
                    for (int xx = 0; xx < bw2 && sx + xx < w; ++xx) {
                        uint32_t p = px[(size_t)(sy + yy) * w + sx + xx];
                        r += p & 0xff; g += (p >> 8) & 0xff; b += (p >> 16) & 0xff; a += (p >> 24) & 0xff; ++cnt;
                    }
                if (!cnt) cnt = 1;
                out[(size_t)oy * nw + ox] = (r / cnt) | ((g / cnt) << 8) | ((b / cnt) << 16) | ((a / cnt) << 24);
            }
        }
        px.swap(out); w = nw; h = nh;
        return true;
    }

    struct Baked {
        bool ok = false;
        uint64_t hash = 0;
        float x = 0, y = 0;  // frame-absolute top-left of painted region (logical)
        int w = 0, h = 0;
        bool nine = false;
        int ml = 0, mr = 0, mt = 0, mb = 0;
    };

    // Bake a node to a deduped PNG; writes textures/<name>.png(+.meta) once per
    // unique content. Mirrors figo2cocos::bake.
    Baked bake(const Node& n, int scale, bool flatten = false, bool tryNine = false, bool nineOnly = false) {
        Baked out;
        ui->setViewport(curW * scale, curH * scale);
        auto clone = figo::cloneNode(n, nullptr);
        if (!flatten) clone->children.clear();
        clone->opacity = 1.0f;
        clone->runtimeOpacity = -1.0f;
        clone->runtimeVisible = -1;
        clone->relativeTransform = n.absoluteTransform;

        std::vector<uint32_t> buf;
        uint32_t bw = 0, bh = 0;
        std::vector<Node*> one{clone.get()};
        if (!ui->renderer().renderOverlay(one, 0.0f, buf, bw, bh)) return out;

        int x0 = bw, y0 = bh, x1 = -1, y1 = -1;
        for (uint32_t y = 0; y < bh; ++y)
            for (uint32_t x = 0; x < bw; ++x)
                if ((buf[(size_t)y * bw + x] >> 24) & 0xff) {
                    if ((int)x < x0) x0 = x;
                    if ((int)x > x1) x1 = x;
                    if ((int)y < y0) y0 = y;
                    if ((int)y > y1) y1 = y;
                }
        if (x1 < x0 || y1 < y0) return out;
        int cw = x1 - x0 + 1, ch = y1 - y0 + 1;

        std::vector<uint32_t> crop((size_t)cw * ch);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                crop[(size_t)y * cw + x] = buf[(size_t)(y0 + y) * bw + x0 + x];

        if (tryNine && nineShrink(crop, cw, ch, scale, out.ml, out.mr, out.mt, out.mb)) out.nine = true;
        else if (nineOnly) return out;

        if (linearComp) linearAlphaComp(crop, cw);  // 哈希前做,dedup 对补偿后内容成立

        uint64_t h = 1469598103934665603ull;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(crop.data());
        for (size_t i = 0; i < crop.size() * 4; ++i) { h ^= bytes[i]; h *= 1099511628211ull; }

        auto it = sprites.find(h);
        if (it == sprites.end()) {
            // Short 8-hex suffix for readability; dedup key is the full 64-bit
            // hash, so DETECT truncation collisions — the colliding file falls
            // back to the full-hash name and the user is told.
            char suffix[24];
            std::snprintf(suffix, sizeof(suffix), "_%08x", (unsigned)(h & 0xffffffffu));
            std::string name = fileStem(n.name, "sprite") + suffix + ".png";
            if (auto un = usedSpriteNames.find(name); un != usedSpriteNames.end() && un->second != h) {
                std::fprintf(stderr,
                    "  WARN: sprite filename collision on %s (%016llx vs %016llx) — widening to full hash\n",
                    name.c_str(), (unsigned long long)un->second, (unsigned long long)h);
                std::snprintf(suffix, sizeof(suffix), "_%016llx", (unsigned long long)h);
                name = fileStem(n.name, "sprite") + suffix + ".png";
            }
            usedSpriteNames[name] = h;
            if (!writePng(texDir / name, crop.data(), cw, ch)) return out;
            Sprite sp;
            sp.file = name;
            sp.guid = makeGuid(h);
            sp.w = cw;
            sp.h = ch;
            sp.nine = out.nine;
            sp.ml = out.ml; sp.mr = out.mr; sp.mt = out.mt; sp.mb = out.mb;
            writeSpriteMeta(sp);
            it = sprites.emplace(h, sp).first;
        }

        const float sc = (float)scale;
        out.x = x0 / sc;
        out.y = y0 / sc;
        out.w = (int)std::lround(cw / sc);
        out.h = (int)std::lround(ch / sc);
        out.ok = true;
        out.hash = h;
        return out;
    }

    void writeSpriteMeta(const Sprite& sp) {
        std::string meta = subst(kTextureMeta, {
            {"$GUID", sp.guid},
            {"$BL", std::to_string(sp.ml)}, {"$BR", std::to_string(sp.mr)},
            {"$BT", std::to_string(sp.mt)}, {"$BB", std::to_string(sp.mb)},
            // 九宫格贴图中段仅 1px,双线性过滤放大会把边界行晕染成大片渐变
            // (面板顶部高亮边变"发光");边角 1:1、中段纯色,Point 像素精确。
            {"$FILTER", sp.nine ? "0" : "1"},  // 0 Point / 1 Bilinear
        });
        std::ofstream f(texDir / (sp.file + ".meta"), std::ios::binary);
        f << meta;
    }

    // ---- YAML document builders ----

    static std::string ref(int64_t id) { return "{fileID: " + std::to_string(id) + "}"; }

    // Shared header lines for every serialized object.
    static std::string objHead() {
        return
            "  m_ObjectHideFlags: 0\n"
            "  m_CorrespondingSourceObject: {fileID: 0}\n"
            "  m_PrefabInstance: {fileID: 0}\n"
            "  m_PrefabAsset: {fileID: 0}\n";
    }

    void emitGameObject(int64_t goId, const std::string& name, const std::vector<int64_t>& comps, bool active) {
        std::string b = "GameObject:\n" + objHead();
        b += "  serializedVersion: 6\n  m_Component:\n";
        for (int64_t c : comps) b += "  - component: " + ref(c) + "\n";
        b += "  m_Layer: 5\n";  // UI
        b += "  m_Name: " + yamlStr(name) + "\n";
        b += "  m_TagString: Untagged\n  m_Icon: {fileID: 0}\n  m_NavMeshLayer: 0\n"
             "  m_StaticEditorFlags: 0\n  m_IsActive: " + std::string(active ? "1" : "0") + "\n";
        doc(1, goId, b);
    }

    // Fully-specified RectTransform. Pivot defaults to (0,1) — the top-left
    // scheme every emitter assumes; only animated nodes override it (rotation/
    // scale spin about the pivot), with anchoredPosition compensated by the
    // caller so the rect doesn't move.
    void emitRectTransformRaw(int64_t rtId, int64_t goId, int64_t parentRt,
                              float aMinX, float aMinY, float aMaxX, float aMaxY,
                              float apX, float apY, float sdX, float sdY,
                              const std::vector<int64_t>& children,
                              float pivX = 0.0f, float pivY = 1.0f) {
        std::string b = "RectTransform:\n" + objHead();
        b += "  m_GameObject: " + ref(goId) + "\n";
        b += "  m_LocalRotation: {x: 0, y: 0, z: 0, w: 1}\n"
             "  m_LocalPosition: {x: 0, y: 0, z: 0}\n"
             "  m_LocalScale: {x: 1, y: 1, z: 1}\n"
             "  m_ConstrainProportionsScale: 0\n";
        b += "  m_Children:\n";
        if (children.empty()) b = b.substr(0, b.size() - 1) + " []\n";
        else for (int64_t c : children) b += "  - " + ref(c) + "\n";
        b += "  m_Father: " + (parentRt ? ref(parentRt) : std::string("{fileID: 0}")) + "\n";
        b += "  m_LocalEulerAnglesHint: {x: 0, y: 0, z: 0}\n";
        b += "  m_AnchorMin: {x: " + f2s(aMinX) + ", y: " + f2s(aMinY) + "}\n";
        b += "  m_AnchorMax: {x: " + f2s(aMaxX) + ", y: " + f2s(aMaxY) + "}\n";
        b += "  m_AnchoredPosition: {x: " + f2s(apX) + ", y: " + f2s(apY) + "}\n";
        b += "  m_SizeDelta: {x: " + f2s(sdX) + ", y: " + f2s(sdY) + "}\n";
        b += "  m_Pivot: {x: " + f2s(pivX) + ", y: " + f2s(pivY) + "}\n";
        doc(224, rtId, b);
    }

    // RectTransform pinned to the parent's top-left: anchor (0,1), pivot (0,1),
    // anchoredPosition (lx, -ly), sizeDelta (w, h).
    void emitRectTransform(int64_t rtId, int64_t goId, int64_t parentRt,
                           float lx, float ly, float w, float h,
                           const std::vector<int64_t>& children) {
        emitRectTransformRaw(rtId, goId, parentRt, 0, 1, 0, 1, lx, -ly, w, h, children);
    }

    // Figma constraints -> UGUI anchors. Design-size result is pixel-identical
    // to the top-left scheme; the anchors only matter when the parent resizes.
    // X axis (pivot.x = 0): anchoredPosition.x = left edge relative to anchor.
    // Y axis (pivot.y = 1, Unity Y-up): anchoredPosition.y = top edge rel anchor.
    void emitRectTransformC(int64_t rtId, int64_t goId, int64_t parentRt,
                            float lx, float ly, float w, float h, float pw, float ph,
                            figo::Constraint ch, figo::Constraint cv,
                            const std::vector<int64_t>& children) {
        using C = figo::Constraint;
        float aMinX = 0, aMaxX = 0, apX = lx, sdX = w;
        if (pw > 0) switch (ch) {
            case C::Max:     aMinX = aMaxX = 1; apX = lx - pw; break;
            case C::Center:  aMinX = aMaxX = 0.5f; apX = lx - pw / 2; break;
            case C::Stretch: aMinX = 0; aMaxX = 1; apX = lx; sdX = w - pw; break;
            case C::Scale:   aMinX = lx / pw; aMaxX = (lx + w) / pw; apX = 0; sdX = 0; break;
            default: break;  // Min: top-left scheme
        }
        float aMinY = 1, aMaxY = 1, apY = -ly, sdY = h;
        if (ph > 0) switch (cv) {
            case C::Max:     aMinY = aMaxY = 0; apY = ph - ly; break;
            case C::Center:  aMinY = aMaxY = 0.5f; apY = ph / 2 - ly; break;
            case C::Stretch: aMinY = 0; aMaxY = 1; apY = -ly; sdY = h - ph; break;
            case C::Scale:   aMinY = (ph - ly - h) / ph; aMaxY = (ph - ly) / ph; apY = 0; sdY = 0; break;
            default: break;  // Min: pin to the parent's top
        }
        emitRectTransformRaw(rtId, goId, parentRt, aMinX, aMinY, aMaxX, aMaxY,
                             apX, apY, sdX, sdY, children);
    }

    void emitCanvasRenderer(int64_t crId, int64_t goId) {
        std::string b = "CanvasRenderer:\n" + objHead();
        b += "  m_GameObject: " + ref(goId) + "\n  m_CullTransparentMesh: 1\n";
        doc(222, crId, b);
    }

    void emitCanvasGroup(int64_t cgId, int64_t goId, float alpha) {
        std::string b = "CanvasGroup:\n" + objHead();
        b += "  m_GameObject: " + ref(goId) + "\n  m_Enabled: 1\n";
        b += "  m_Alpha: " + c2s(alpha) + "\n";
        b += "  m_Interactable: 1\n  m_BlocksRaycasts: 1\n  m_IgnoreParentGroups: 0\n";
        doc(225, cgId, b);
    }

    // ---- CSS-animation replay: legacy AnimationClip asset + Animation comp ----
    // One .anim per animated node (anims/<frame>_<node>_<n>.anim), legacy so
    // it plays without an Animator controller; Animation autoplays it. Tracks:
    // CanvasGroup.m_Alpha (opacity), localScale, localEulerAngles z (CSS deg
    // negated — Unity z+ is CCW), RectTransform anchoredPosition floats
    // (baseX+dx, baseY-dy). Keys are pre-linearized by figoanim::build; linear
    // tangents are the segment slopes, steps() easing = Infinity tangents.
    int animSeq = 0;

    std::string writeAnimClip(const figoanim::Tracks& tk, const std::string& nodeName,
                              float baseX, float baseY) {
        const std::string file = fileStem(curFrame, "Frame") + "_" +
                                 fileStem(nodeName, "node") + "_" + std::to_string(++animSeq);
        const std::string guid = makeGuid(fnv1a("anim/" + file));
        const char* INF = "Infinity";

        auto slope1 = [&](const std::vector<std::pair<float, float>>& ks, size_t i, bool in) -> std::string {
            if (tk.step) return INF;
            const size_t a = in ? i - 1 : i, b = in ? i : i + 1;
            if ((in && i == 0) || (!in && i + 1 >= ks.size())) return "0";
            const float dt = ks[b].first - ks[a].first;
            return dt <= 1e-6f ? "0" : a2s((ks[b].second - ks[a].second) / dt);
        };
        auto floatCurve = [&](const std::vector<std::pair<float, float>>& ks,
                              const char* attribute, int classID) {
            std::string s = "  - serializedVersion: 2\n    curve:\n      serializedVersion: 2\n      m_Curve:\n";
            for (size_t i = 0; i < ks.size(); ++i) {
                s += "      - serializedVersion: 3\n";
                s += "        time: " + a2s(ks[i].first) + "\n";
                s += "        value: " + a2s(ks[i].second) + "\n";
                s += "        inSlope: " + slope1(ks, i, true) + "\n";
                s += "        outSlope: " + slope1(ks, i, false) + "\n";
                s += "        tangentMode: 0\n        weightedMode: 0\n"
                     "        inWeight: 0.33333334\n        outWeight: 0.33333334\n";
            }
            s += "      m_PreInfinity: 2\n      m_PostInfinity: 2\n      m_RotationOrder: 4\n";
            s += std::string("    attribute: ") + attribute + "\n    path: \n";
            s += "    classID: " + std::to_string(classID) + "\n    script: {fileID: 0}\n    flags: 0\n";
            return s;
        };
        std::string rotc, scale, floats;
        if (!tk.rot.empty()) {
            // Legacy clips ignore m_EulerCurves at runtime — the z-spin must be
            // baked into quaternion m_RotationCurves. Quaternion interpolation
            // takes the short way round, so subdivide each segment to <=45°
            // steps (a 0→360 loop with only 2 keys wouldn't move at all).
            std::vector<std::pair<float, float>> rk;  // t, unity z deg (= -cssDeg)
            for (size_t i = 0; i + 1 < tk.rot.size(); ++i) {
                const float t0 = tk.rot[i].first, t1 = tk.rot[i + 1].first;
                const float a0 = -tk.rot[i].second, a1 = -tk.rot[i + 1].second;
                const int steps = std::max(1, (int)std::ceil(std::fabs(a1 - a0) / 45.0f));
                for (int s = 0; s < steps; ++s) {
                    const float u = (float)s / (float)steps;
                    rk.push_back({t0 + (t1 - t0) * u, a0 + (a1 - a0) * u});
                }
            }
            rk.push_back({tk.rot.back().first, -tk.rot.back().second});
            auto qz = [](float deg) { return std::sin(deg * 3.14159265f / 360.0f); };
            auto qw = [](float deg) { return std::cos(deg * 3.14159265f / 360.0f); };
            auto sl = [&](size_t i, bool in, float (*q)(float)) -> std::string {
                if (tk.step) return INF;
                const size_t a = in ? i - 1 : i, b = in ? i : i + 1;
                if ((in && i == 0) || (!in && i + 1 >= rk.size())) return "0";
                const float dt = rk[b].first - rk[a].first;
                return dt <= 1e-6f ? "0" : a2s((q(rk[b].second) - q(rk[a].second)) / dt);
            };
            auto qzf = +[](float deg) { return std::sin(deg * 3.14159265f / 360.0f); };
            auto qwf = +[](float deg) { return std::cos(deg * 3.14159265f / 360.0f); };
            rotc = "  - curve:\n      serializedVersion: 2\n      m_Curve:\n";
            for (size_t i = 0; i < rk.size(); ++i) {
                rotc += "      - serializedVersion: 3\n";
                rotc += "        time: " + a2s(rk[i].first) + "\n";
                rotc += "        value: {x: 0, y: 0, z: " + a2s(qz(rk[i].second)) + ", w: " + a2s(qw(rk[i].second)) + "}\n";
                rotc += "        inSlope: {x: 0, y: 0, z: " + sl(i, true, qzf) + ", w: " + sl(i, true, qwf) + "}\n";
                rotc += "        outSlope: {x: 0, y: 0, z: " + sl(i, false, qzf) + ", w: " + sl(i, false, qwf) + "}\n";
                rotc += "        tangentMode: 0\n        weightedMode: 0\n"
                        "        inWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334, w: 0.33333334}\n"
                        "        outWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334, w: 0.33333334}\n";
            }
            rotc += "      m_PreInfinity: 2\n      m_PostInfinity: 2\n      m_RotationOrder: 4\n    path: \n";
        }
        if (!tk.scale.empty()) {
            std::vector<std::array<float, 3>> ks;
            for (auto& k : tk.scale) ks.push_back({k[0], k[1], k[2]});
            // scale z must stay 1 (vecCurve writes z: 0) — build directly too.
            auto sl = [&](size_t i, int c, bool in) -> std::string {
                if (tk.step) return INF;
                const size_t a = in ? i - 1 : i, b = in ? i : i + 1;
                if ((in && i == 0) || (!in && i + 1 >= ks.size())) return "0";
                const float dt = ks[b][0] - ks[a][0];
                return dt <= 1e-6f ? "0" : a2s((ks[b][c] - ks[a][c]) / dt);
            };
            scale = "  - curve:\n      serializedVersion: 2\n      m_Curve:\n";
            for (size_t i = 0; i < ks.size(); ++i) {
                scale += "      - serializedVersion: 3\n";
                scale += "        time: " + a2s(ks[i][0]) + "\n";
                scale += "        value: {x: " + a2s(ks[i][1]) + ", y: " + a2s(ks[i][2]) + ", z: 1}\n";
                scale += "        inSlope: {x: " + sl(i, 1, true) + ", y: " + sl(i, 2, true) + ", z: 0}\n";
                scale += "        outSlope: {x: " + sl(i, 1, false) + ", y: " + sl(i, 2, false) + ", z: 0}\n";
                scale += "        tangentMode: 0\n        weightedMode: 0\n"
                         "        inWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334}\n"
                         "        outWeight: {x: 0.33333334, y: 0.33333334, z: 0.33333334}\n";
            }
            scale += "      m_PreInfinity: 2\n      m_PostInfinity: 2\n      m_RotationOrder: 4\n    path: \n";
        }
        if (!tk.alpha.empty()) floats += floatCurve(tk.alpha, "m_Alpha", 225);
        if (!tk.pos.empty()) {
            std::vector<std::pair<float, float>> px, py;
            for (auto& k : tk.pos) { px.push_back({k[0], baseX + k[1]}); py.push_back({k[0], baseY - k[2]}); }
            floats += floatCurve(px, "m_AnchoredPosition.x", 224);
            floats += floatCurve(py, "m_AnchoredPosition.y", 224);
        }

        std::string y = "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n--- !u!74 &7400000\nAnimationClip:\n";
        y += "  serializedVersion: 7\n";
        y += objHead();
        y += "  m_Name: " + file + "\n";
        y += "  m_Legacy: 1\n  m_Compressed: 0\n  m_UseHighQualityCurve: 1\n";
        y += rotc.empty() ? "  m_RotationCurves: []\n" : "  m_RotationCurves:\n" + rotc;
        y += "  m_CompressedRotationCurves: []\n  m_EulerCurves: []\n";
        y += "  m_PositionCurves: []\n";
        y += scale.empty() ? "  m_ScaleCurves: []\n" : "  m_ScaleCurves:\n" + scale;
        y += floats.empty() ? "  m_FloatCurves: []\n" : "  m_FloatCurves:\n" + floats;
        y += "  m_PPtrCurves: []\n  m_SampleRate: 60\n";
        y += "  m_WrapMode: " + std::string(tk.loop ? "2" : "8") + "\n";  // 2=Loop, 8=ClampForever (hold end pose)
        y += "  m_Bounds:\n    m_Center: {x: 0, y: 0, z: 0}\n    m_Extent: {x: 0, y: 0, z: 0}\n";
        y += "  m_ClipBindingConstant:\n    genericBindings: []\n    pptrCurveMapping: []\n";
        y += "  m_AnimationClipSettings:\n    serializedVersion: 2\n"
             "    m_AdditiveReferencePoseClip: {fileID: 0}\n    m_AdditiveReferencePoseTime: 0\n"
             "    m_StartTime: 0\n    m_StopTime: " + a2s(tk.length) + "\n"
             "    m_OrientationOffsetY: 0\n    m_Level: 0\n    m_CycleOffset: 0\n"
             "    m_HasAdditiveReferencePose: 0\n    m_LoopTime: " + std::string(tk.loop ? "1" : "0") + "\n"
             "    m_LoopBlend: 0\n    m_LoopBlendOrientation: 0\n    m_LoopBlendPositionY: 0\n"
             "    m_LoopBlendPositionXZ: 0\n    m_KeepOriginalOrientation: 0\n"
             "    m_KeepOriginalPositionY: 1\n    m_KeepOriginalPositionXZ: 0\n"
             "    m_HeightFromFeet: 0\n    m_Mirror: 0\n";
        y += "  m_EditorCurves: []\n  m_EulerEditorCurves: []\n";
        y += "  m_HasGenericRootTransform: 0\n  m_HasMotionFloatCurves: 0\n  m_Events: []\n";

        std::error_code ec;
        fs::create_directories(outDir / "anims", ec);
        std::ofstream af(outDir / "anims" / (file + ".anim"), std::ios::binary);
        af << y;
        std::ofstream mf(outDir / "anims" / (file + ".anim.meta"), std::ios::binary);
        mf << "fileFormatVersion: 2\nguid: " << guid
           << "\nNativeFormatImporter:\n  externalObjects: {}\n  mainObjectFileID: 7400000\n"
              "  userData: \n  assetBundleName: \n  assetBundleVariant: \n";
        return guid;
    }

    void emitAnimationComp(int64_t id, int64_t goId, const std::string& clipGuid) {
        std::string b = "Animation:\n  serializedVersion: 4\n" + objHead();
        b += "  m_GameObject: " + ref(goId) + "\n  m_Enabled: 1\n";
        b += "  m_Animation: {fileID: 7400000, guid: " + clipGuid + ", type: 2}\n";
        b += "  m_Animations:\n  - {fileID: 7400000, guid: " + clipGuid + ", type: 2}\n";
        b += "  m_WrapMode: 0\n  m_PlayAutomatically: 1\n  m_AnimatePhysics: 0\n"
             "  m_UpdateMode: 0\n  m_CullingType: 0\n";
        doc(111, id, b);
    }

    static std::string monoHead(int64_t goId, const char* scriptGuid) {
        return std::string("MonoBehaviour:\n") + objHead() +
            "  m_GameObject: " + ref(goId) + "\n"
            "  m_Enabled: 1\n  m_EditorHideFlags: 0\n"
            "  m_Script: {fileID: 11500000, guid: " + scriptGuid + ", type: 3}\n"
            "  m_Name: \n  m_EditorClassIdentifier: \n";
    }

    // Horizontal/VerticalLayoutGroup that only ARRANGES children (control /
    // force-expand all off): children keep their own RectTransform sizes, the
    // group re-packs positions with the design's padding/spacing — pixel-equal
    // at design time, reflows when content is added/removed in Unity.
    void emitLayoutGroup(int64_t id, int64_t goId, const figo::AutoLayout& al) {
        const bool horizontal = al.mode == figo::AutoLayout::Mode::Horizontal;
        std::string b = monoHead(goId, horizontal ? kHLayoutScript : kVLayoutScript);
        b += "  m_Padding:\n";
        b += "    m_Left: " + f2s(al.paddingLeft) + "\n";
        b += "    m_Right: " + f2s(al.paddingRight) + "\n";
        b += "    m_Top: " + f2s(al.paddingTop) + "\n";
        b += "    m_Bottom: " + f2s(al.paddingBottom) + "\n";
        // 3x3 alignment grid (UpperLeft=0 .. LowerRight=8): the main axis takes
        // primaryAlign, the cross axis counterAlign.
        auto a3 = [](figo::AutoLayout::Align a) {
            return a == figo::AutoLayout::Align::Center ? 1
                 : a == figo::AutoLayout::Align::Max ? 2 : 0;
        };
        const int row = horizontal ? a3(al.counterAlign) : a3(al.primaryAlign);
        const int col = horizontal ? a3(al.primaryAlign) : a3(al.counterAlign);
        b += "  m_ChildAlignment: " + std::to_string(row * 3 + col) + "\n";
        b += "  m_ReverseArrangement: 0\n";
        b += "  m_Spacing: " + f2s(al.itemSpacing) + "\n";
        b += "  m_ChildForceExpandWidth: 0\n  m_ChildForceExpandHeight: 0\n";
        b += "  m_ChildControlWidth: 0\n  m_ChildControlHeight: 0\n";
        b += "  m_ChildScaleWidth: 0\n  m_ChildScaleHeight: 0\n";
        doc(114, id, b);
    }

    // LayoutElement that opts a decoration child (__bg) out of the parent's
    // LayoutGroup flow.
    void emitLayoutIgnore(int64_t id, int64_t goId) {
        std::string b = monoHead(goId, kLayoutElementScript);
        b += "  m_IgnoreLayout: 1\n"
             "  m_MinWidth: -1\n  m_MinHeight: -1\n"
             "  m_PreferredWidth: -1\n  m_PreferredHeight: -1\n"
             "  m_FlexibleWidth: -1\n  m_FlexibleHeight: -1\n"
             "  m_LayoutPriority: 1\n";
        doc(114, id, b);
    }

    void emitRectMask2D(int64_t id, int64_t goId) {
        std::string b = monoHead(goId, kRectMask2DScript);
        b += "  m_Padding: {x: 0, y: 0, z: 0, w: 0}\n  m_Softness: {x: 0, y: 0}\n";
        doc(114, id, b);
    }

    void emitScrollRect(int64_t id, int64_t goId, int64_t contentRt, bool h, bool v) {
        std::string b = monoHead(goId, kScrollRectScript);
        b += "  m_Content: " + ref(contentRt) + "\n";
        b += "  m_Horizontal: " + std::string(h ? "1" : "0") + "\n";
        b += "  m_Vertical: " + std::string(v ? "1" : "0") + "\n";
        b += "  m_MovementType: 1\n  m_Elasticity: 0.1\n  m_Inertia: 1\n"
             "  m_DecelerationRate: 0.135\n  m_ScrollSensitivity: 1\n"
             "  m_Viewport: {fileID: 0}\n"  // null -> ScrollRect uses its own rect
             "  m_HorizontalScrollbar: {fileID: 0}\n  m_VerticalScrollbar: {fileID: 0}\n"
             "  m_HorizontalScrollbarVisibility: 0\n  m_VerticalScrollbarVisibility: 0\n"
             "  m_HorizontalScrollbarSpacing: 0\n  m_VerticalScrollbarSpacing: 0\n"
             "  m_OnValueChanged:\n    m_PersistentCalls:\n      m_Calls: []\n";
        doc(114, id, b);
    }

    // Text material (Figo/UIText, linear-space alpha compensation); empty
    // until main() emits the shader+material pair.
    std::string textMatGuid;

    // Theme-token bindings collected during emit and written to
    // figo_tokens.json — prefab colors are resolved literals, so this is the
    // hook engine-side reskinning needs to re-bind them.
    nlohmann::json tokenBindings = nlohmann::json::array();
    std::string curFrame;
    void bindToken(const std::string& path, const char* channel, const std::string& var) {
        if (var.empty() || inComponent) return;  // component paths are frame-relative
        tokenBindings.push_back({{"frame", curFrame},
                                 {"node", path.empty() ? "." : path},
                                 {"channel", channel},
                                 {"var", var}});
    }

    static std::string graphicBody(const figo::Color& col, const std::string& matRef = "{fileID: 0}") {
        return
            "  m_Material: " + matRef + "\n"
            "  m_Color: {r: " + c2s(col.r) + ", g: " + c2s(col.g) + ", b: " + c2s(col.b) +
            ", a: " + c2s(col.a) + "}\n"
            // Design exports carry no interactivity — raycastTarget defaults
            // OFF so decorative graphics don't eat clicks or raycast time;
            // enable it per-node in Unity on the few actually-clickable ones.
            "  m_RaycastTarget: 0\n"
            "  m_RaycastPadding: {x: 0, y: 0, z: 0, w: 0}\n"
            "  m_Maskable: 1\n"
            "  m_OnCullStateChanged:\n"
            "    m_PersistentCalls:\n"
            "      m_Calls: []\n";
    }

    // UI.Image. spriteGuid empty = no sprite (solid tinted quad).
    // type: 0 simple, 1 sliced.
    void emitImage(int64_t imgId, int64_t goId, const std::string& spriteGuid, int type, const figo::Color& col) {
        figo::Color c2 = col;
        // 纯色半透明图形(遮罩/暗化层)的 alpha 同样按 --linear 补偿;贴图 tint
        // 通常不透明,不受影响。文字已有 FigoUIText shader 补偿,不在此路径。
        if (linearComp && c2.a < 0.999f) c2.a = compAlpha(c2.a, c2.r, c2.g, c2.b);
        std::string b = monoHead(goId, kImageScript) + graphicBody(c2);
        b += "  m_Sprite: " + (spriteGuid.empty()
                 ? std::string("{fileID: 0}")
                 : "{fileID: 21300000, guid: " + spriteGuid + ", type: 3}") + "\n";
        b += "  m_Type: " + std::to_string(type) + "\n";
        b += "  m_PreserveAspect: 0\n  m_FillCenter: 1\n  m_FillMethod: 4\n"
             "  m_FillAmount: 1\n  m_FillClockwise: 1\n  m_FillOrigin: 0\n"
             "  m_UseSpriteMesh: 0\n  m_PixelsPerUnitMultiplier: 1\n";
        doc(114, imgId, b);
    }

    // Text-free composite of the current frame (scale 1), captured once per
    // frame for sampling the true pixels under a text box.
    std::vector<uint32_t> frameBg;
    uint32_t frameBgW = 0, frameBgH = 0;

    void captureTextFreeBg(Node* frame) {
        // Render a CLONE (like bake does) — renderOverlay on the live tree
        // clobbers runtime state and breaks every later bake.
        auto clone = figo::cloneNode(*frame, nullptr);
        clone->opacity = 1.0f;
        clone->runtimeOpacity = -1.0f;
        clone->runtimeVisible = -1;
        clone->relativeTransform = frame->absoluteTransform;
        std::function<void(Node&)> rec2 = [&](Node& n) {
            if (n.type == NodeType::Text) n.visible = false;
            for (auto& c : n.children) rec2(*c);
        };
        rec2(*clone);
        frameBg.clear();
        frameBgW = frameBgH = 0;
        ui->setViewport(curW, curH);
        std::vector<Node*> one{clone.get()};
        ui->renderer().renderOverlay(one, 0.0f, frameBg, frameBgW, frameBgH);
    }

    // Average opaque color of the text-free composite under the node's box.
    bool sampledBg(const Node& n, figo::Color& out) {
        if (frameBg.empty() || frameBgW == 0) return false;
        const int x0 = std::max(0, (int)n.absoluteTransform.m02);
        const int y0 = std::max(0, (int)n.absoluteTransform.m12);
        const int x1 = std::min((int)frameBgW, (int)(n.absoluteTransform.m02 + n.width));
        const int y1 = std::min((int)frameBgH, (int)(n.absoluteTransform.m12 + n.height));
        if (x1 <= x0 || y1 <= y0) return false;
        double r = 0, g = 0, b = 0, a = 0;
        long cnt = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                uint32_t v = frameBg[(size_t)y * frameBgW + x];
                r += v & 0xff; g += (v >> 8) & 0xff; b += (v >> 16) & 0xff; a += (v >> 24) & 0xff;
                ++cnt;
            }
        if (!cnt || a / cnt < 230) return false;  // bg not solidly painted
        out = {(float)(r / cnt / 255.0), (float)(g / cnt / 255.0), (float)(b / cnt / 255.0), 1.0f};
        return true;
    }

    // Effective opaque solid color underneath a node: nearest ancestor solid
    // fills composited until one is opaque. False when an image/gradient bg
    // intervenes before an opaque solid is reached.
    static bool effectiveSolidBg(const Node& n, figo::Color& out) {
        std::vector<figo::Color> layers;
        for (const Node* p = n.parent; p; p = p->parent) {
            if (nonSolidVisibleFill(*p)) return false;  // unknown pixels below
            if (const auto* f = solidFill(*p)) {
                figo::Color c = f->color;
                c.a *= f->opacity;
                layers.push_back(c);
                if (c.a >= 0.99f) {
                    figo::Color bg = c;
                    for (auto it = layers.rbegin() + 1; it != layers.rend(); ++it) {
                        bg.r = it->r * it->a + bg.r * (1 - it->a);
                        bg.g = it->g * it->a + bg.g * (1 - it->a);
                        bg.b = it->b * it->a + bg.b * (1 - it->a);
                    }
                    bg.a = 1;
                    out = bg;
                    return true;
                }
            }
        }
        return false;
    }

    // Pre-composite a semi-transparent text color against the pixels actually
    // underneath it (ancestor solid chain, else the sampled text-free frame
    // composite) and emit an OPAQUE color. Alpha blending happens in the
    // project's working color space, so a design-authored black@60% washes out
    // visibly in a LINEAR-space project ("text looks faded"); an opaque glyph
    // interior renders identically in Gamma and Linear.
    void opaqueTextColor(const Node& n, figo::Color& col) {
        if (col.a >= 0.999f) return;
        figo::Color bg;
        if (!effectiveSolidBg(n, bg) && !sampledBg(n, bg)) return;
        col.r = col.r * col.a + bg.r * (1 - col.a);
        col.g = col.g * col.a + bg.g * (1 - col.a);
        col.b = col.b * col.a + bg.b * (1 - col.a);
        col.a = 1;
    }

    // extraAlpha folds the NODE opacity into the color (instead of a
    // CanvasGroup) so opaqueTextColor can pre-composite muted text — a 60%
    // CanvasGroup washes out in Linear projects exactly like a 60% fill.
    void emitText(int64_t txtId, int64_t goId, const Node& n, float extraAlpha = 1.0f) {
        figo::Color col{0, 0, 0, 1};
        if (const auto* f = solidFill(n)) { col = f->color; col.a *= f->opacity; }
        col.a *= extraAlpha;
        opaqueTextColor(n, col);
        const std::string matRef = textMatGuid.empty()
            ? std::string("{fileID: 0}")
            : "{fileID: 2100000, guid: " + textMatGuid + ", type: 2}";
        std::string b = monoHead(goId, kTextScript) + graphicBody(col, matRef);

        const int fontSize = (int)(n.textStyle.fontSize + 0.5f);
        // Unity lineSpacing multiplies the font's own line height 鈥?divide the
        // design's pixel line height by the font's real em factor (parsed from
        // the bundled file's hhea/head; ~1.15 em for the built-in font).
        const FontEntry* fe = matchFont(n.textStyle.fontFamily, n.textStyle.fontWeight,
                                        n.textStyle.italic);
        const float emFactor = fe ? fe->lineFactor : 1.15f;
        float lineSpacing = 1.0f;
        if (n.textStyle.lineHeightPx > 0 && fontSize > 0)
            lineSpacing = n.textStyle.lineHeightPx / (fontSize * emFactor);

        // Alignment: 3x3 grid, 0=UpperLeft .. 8=LowerRight.
        int col3 = 1;  // center
        switch (n.textStyle.alignH) {
            case figo::TextStyle::AlignH::Left: col3 = 0; break;
            case figo::TextStyle::AlignH::Right: col3 = 2; break;
            case figo::TextStyle::AlignH::Justified: col3 = 0; break;
            default: col3 = 1; break;
        }
        int row3 = 1;  // middle
        switch (n.textStyle.alignV) {
            case figo::TextStyle::AlignV::Top: row3 = 0; break;
            case figo::TextStyle::AlignV::Bottom: row3 = 2; break;
            default: row3 = 1; break;
        }
        // Overflow (h: 0 wrap / 1 overflow; v: 0 truncate / 1 overflow):
        //   WIDTH_AND_HEIGHT (hug both)   -> overflow both (box == text)
        //   HEIGHT (fixed width, grow)    -> wrap + overflow (grow downward)
        //   NONE/TRUNCATE fixed multiline -> wrap + truncate (clip, don't overlap
        //                                    the neighbors below)
        //   NONE/TRUNCATE fixed one-liner -> no wrap + overflow: keep it a single
        //                                    line spilling right, like figo's
        //                                    truncation. Truncate here would drop
        //                                    the whole line when Unity's font is
        //                                    a hair taller than the authored box.
        const float lineH = n.textStyle.lineHeightPx > 0 ? n.textStyle.lineHeightPx
                                                         : n.textStyle.fontSize * 1.2f;
        const bool multiline = lineH > 0 && n.height > lineH * 1.8f;
        const std::string& ar = n.textStyle.autoResize;
        int hOverflow, vOverflow;
        if (ar == "WIDTH_AND_HEIGHT") { hOverflow = 1; vOverflow = 1; }
        else if (ar == "HEIGHT")      { hOverflow = 0; vOverflow = 1; }
        else if (multiline)           { hOverflow = 0; vOverflow = 0; }
        else                          { hOverflow = 1; vOverflow = 1; }

        // With a bundled file the weight/slant are baked into the font itself 鈥?        // only synthesize what the matched file lacks (avoids double-bolding).
        int fontStyle = 0;
        if (n.textStyle.fontWeight >= 600 && (!fe || fe->weight < 600)) fontStyle |= 1;
        if (n.textStyle.italic && (!fe || !fe->italic)) fontStyle |= 2;

        b += "  m_FontData:\n";
        b += fe ? "    m_Font: {fileID: 12800000, guid: " + fe->guid + ", type: 3}\n"
                : "    m_Font: {fileID: 10102, guid: " + std::string(kBuiltinFont) + ", type: 0}\n";
        b += "    m_FontSize: " + std::to_string(fontSize) + "\n";
        b += "    m_FontStyle: " + std::to_string(fontStyle) + "\n";
        b += "    m_BestFit: 0\n    m_MinSize: 0\n    m_MaxSize: " +
             std::to_string(std::max(300, fontSize + 1)) + "\n";
        b += "    m_Alignment: " + std::to_string(row3 * 3 + col3) + "\n";
        b += "    m_AlignByGeometry: 0\n    m_RichText: 0\n";
        b += "    m_HorizontalOverflow: " + std::to_string(hOverflow) + "\n";
        b += "    m_VerticalOverflow: " + std::to_string(vOverflow) + "\n";
        b += "    m_LineSpacing: " + c2s(lineSpacing) + "\n";
        b += "  m_Text: " + yamlStr(n.characters) + "\n";
        doc(114, txtId, b);
    }

    // --tmp: TextMeshProUGUI instead of legacy UI.Text. All texts reference the
    // default LiberationSans SDF font asset (SDF atlases can't be generated
    // offline) — swap the font asset in Unity for custom/CJK faces. Emits both
    // the pre-3.2 (m_enableWordWrapping) and current (m_TextWrappingMode) wrap
    // field names; the deserializer ignores whichever it doesn't know.
    void emitTmpText(int64_t txtId, int64_t goId, const Node& n, float extraAlpha = 1.0f) {
        figo::Color col{0, 0, 0, 1};
        if (const auto* f = solidFill(n)) { col = f->color; col.a *= f->opacity; }
        col.a *= extraAlpha;
        opaqueTextColor(n, col);
        std::string b = monoHead(goId, kTmpTextScript) + graphicBody(col);

        const float fontSize = n.textStyle.fontSize;
        int hAlign = 2;  // HorizontalAlignmentOptions: Left=1 Center=2 Right=4 Justified=8
        switch (n.textStyle.alignH) {
            case figo::TextStyle::AlignH::Left: hAlign = 1; break;
            case figo::TextStyle::AlignH::Right: hAlign = 4; break;
            case figo::TextStyle::AlignH::Justified: hAlign = 8; break;
            default: hAlign = 2; break;
        }
        int vAlign = 512;  // VerticalAlignmentOptions: Top=256 Middle=512 Bottom=1024
        switch (n.textStyle.alignV) {
            case figo::TextStyle::AlignV::Top: vAlign = 256; break;
            case figo::TextStyle::AlignV::Bottom: vAlign = 1024; break;
            default: vAlign = 512; break;
        }
        // Wrap/overflow (see emitText for the autoResize semantics). TMP can do
        // a real per-character ellipsis, so fixed multiline boxes upgrade from
        // "truncate" to Ellipsis; fixed one-liners stay Overflow (Ellipsis
        // could blank the line when the box is a hair shorter than the font).
        const float lineH = n.textStyle.lineHeightPx > 0 ? n.textStyle.lineHeightPx
                                                         : n.textStyle.fontSize * 1.2f;
        const bool multiline = lineH > 0 && n.height > lineH * 1.8f;
        const std::string& ar = n.textStyle.autoResize;
        int wrap, overflow;  // overflow: TextOverflowModes Overflow=0 Ellipsis=1
        if (ar == "WIDTH_AND_HEIGHT") { wrap = 0; overflow = 0; }
        else if (ar == "HEIGHT")      { wrap = 1; overflow = 0; }
        else if (multiline)           { wrap = 1; overflow = 1; }
        else                          { wrap = 0; overflow = 0; }

        int fontStyle = 0;  // FontStyles: Bold=1 Italic=2 (synthesized by TMP)
        if (n.textStyle.fontWeight >= 600) fontStyle |= 1;
        if (n.textStyle.italic) fontStyle |= 2;
        // TMP character spacing is in em/100.
        const float charSpacing = (n.textStyle.letterSpacing != 0 && fontSize > 0)
            ? n.textStyle.letterSpacing / fontSize * 100.0f : 0.0f;

        auto u8c = [](float v) {
            return (uint32_t)std::lround(std::max(0.0f, std::min(1.0f, v)) * 255.0f);
        };
        const uint32_t rgba = (u8c(col.a) << 24) | (u8c(col.b) << 16) |
                              (u8c(col.g) << 8) | u8c(col.r);
        b += "  m_text: " + yamlStr(n.characters) + "\n";
        b += "  m_isRightToLeft: 0\n";
        b += "  m_fontAsset: {fileID: 11400000, guid: " + std::string(kTmpDefaultFontAsset) +
             ", type: 2}\n";
        b += "  m_sharedMaterial: {fileID: 0}\n";  // TMP falls back to the font asset's material
        b += "  m_fontSharedMaterials: []\n  m_fontMaterial: {fileID: 0}\n  m_fontMaterials: []\n";
        b += "  m_fontColor32:\n    serializedVersion: 2\n    rgba: " + std::to_string(rgba) + "\n";
        b += "  m_fontColor: {r: " + c2s(col.r) + ", g: " + c2s(col.g) +
             ", b: " + c2s(col.b) + ", a: " + c2s(col.a) + "}\n";
        b += "  m_enableVertexGradient: 0\n";
        b += "  m_fontSize: " + f2s(fontSize) + "\n";
        b += "  m_fontSizeBase: " + f2s(fontSize) + "\n";
        b += "  m_fontWeight: 400\n";
        b += "  m_enableAutoSizing: 0\n";
        b += "  m_fontStyle: " + std::to_string(fontStyle) + "\n";
        b += "  m_HorizontalAlignment: " + std::to_string(hAlign) + "\n";
        b += "  m_VerticalAlignment: " + std::to_string(vAlign) + "\n";
        b += "  m_textAlignment: 65535\n";  // deprecated field: "use H/V alignment"
        b += "  m_characterSpacing: " + f2s(charSpacing) + "\n";
        b += "  m_wordSpacing: 0\n  m_lineSpacing: 0\n  m_lineSpacingMax: 0\n"
             "  m_paragraphSpacing: 0\n";
        b += "  m_enableWordWrapping: " + std::to_string(wrap) + "\n";
        b += "  m_TextWrappingMode: " + std::to_string(wrap) + "\n";
        b += "  m_overflowMode: " + std::to_string(overflow) + "\n";
        b += "  m_richText: 0\n";
        doc(114, txtId, b);
    }

    // ---- node emission ----

    // Emit node `n` under parentRt; returns its RectTransform fileID. pax/pay =
    // parent's absolute top-left (for placing baked sprites in parent-local
    // coords). path = node path from the component root when recording.
    int64_t emit(Node& n, int64_t parentRt, const std::string& name, float pax, float pay,
                 const std::string& path = "", bool inScroll = false) {
        const bool isRoot = parentRt == 0;
        const float lx = isRoot ? 0.0f : n.relativeTransform.m02;
        const float ly = isRoot ? 0.0f : n.relativeTransform.m12;

        // Prefab reuse: a repeated component becomes a PrefabInstance of its
        // components/<name>.prefab, with per-instance property overrides.
        if (const Comp* comp = extractedComponent(n)) {
            usedComps.insert(comp->name);
            return emitPrefabInstance(*const_cast<Comp*>(comp), n, parentRt, name, lx, ly);
        }

        const int64_t goId = newId();
        const int64_t rtId = newId();
        std::vector<int64_t> comps{rtId};
        std::vector<int64_t> childRts;
        CompNodeIds recIds;
        recIds.go = goId;
        recIds.rt = rtId;

        float bx = lx, by = ly, bw = n.width, bh = n.height;

        auto addGraphicComps = [&](std::function<void(int64_t)> emitGraphic) -> int64_t {
            int64_t crId = newId();
            emitCanvasRenderer(crId, goId);
            comps.push_back(crId);
            int64_t gId = newId();
            emitGraphic(gId);
            comps.push_back(gId);
            return gId;
        };
        figoanim::Tracks anim;
        if (n.anim) anim = figoanim::build(*n.anim);
        const bool hasAnim = n.anim && anim.any();

        bool cgAdded = false;
        auto addOpacity = [&]() {
            if (n.opacity < 0.999f) {
                int64_t cgId = newId();
                emitCanvasGroup(cgId, goId, n.opacity);
                comps.push_back(cgId);
                cgAdded = true;
            }
        };
        auto finish = [&](bool active) {
            // CSS-animation replay: pivot the rect at the animation's
            // transform-origin (expressed on the LOGICAL box lx/ly/n.size,
            // while the emitted rect bx/by/bw/bh may be the padded bake bbox),
            // compensate anchoredPosition, then attach the clip + Animation.
            float pivX = 0, pivY = 1, apX = bx, apY = -by;
            const bool pivoted = hasAnim && anim.pivoted();
            if (pivoted && bw > 0 && bh > 0) {
                const float px = (lx - bx) + anim.pivotX * n.width;
                const float py = (ly - by) + anim.pivotY * n.height;
                pivX = px / bw;
                pivY = 1 - py / bh;
                apX = bx + px;
                apY = -(by + py);
            }
            if (hasAnim) {
                if (!anim.alpha.empty() && !cgAdded) {
                    int64_t cgId = newId();
                    emitCanvasGroup(cgId, goId, n.opacity < 0.999f ? n.opacity : 1.0f);
                    comps.push_back(cgId);
                    cgAdded = true;
                }
                const std::string clipGuid = writeAnimClip(anim, name, apX, apY);
                int64_t aId = newId();
                emitAnimationComp(aId, goId, clipGuid);
                comps.push_back(aId);
            }
            emitGameObject(goId, name, comps, active);
            // Figma constraints -> anchors, except: the root (drag-in size),
            // component-prefab internals (instance overrides assume the
            // top-left scheme), and direct children of a scroll __content
            // (whose actual parent is the content extent, not the design box).
            // Animated nodes stay on the plain top-left scheme — the clip's
            // anchoredPosition/pivot values assume it; constraint anchors
            // would compose incorrectly.
            const bool useConstraints = !isRoot && !rec && !inScroll && n.parent && !hasAnim &&
                (n.constraintH != figo::Constraint::Min ||
                 n.constraintV != figo::Constraint::Min);
            if (useConstraints)
                emitRectTransformC(rtId, goId, parentRt, bx, by, bw, bh,
                                   n.parent->width, n.parent->height,
                                   n.constraintH, n.constraintV, childRts);
            else
                emitRectTransformRaw(rtId, goId, parentRt, 0, 1, 0, 1,
                                     apX, apY, bw, bh, childRts, pivX, pivY);
            if (rec) (*rec)[path] = recIds;
            return rtId;
        };

        // Place baked leaves at their painted bounds; everything else at its box.
        auto emitBakedLeaf = [&](const Baked& b) -> int64_t {
            const Sprite& sp = sprites[b.hash];
            bx = b.x - pax; by = b.y - pay; bw = (float)b.w; bh = (float)b.h;
            recIds.img = addGraphicComps([&](int64_t id) { emitImage(id, goId, sp.guid, b.nine ? 1 : 0, {1, 1, 1, 1}); });
            addOpacity();
            return finish(n.visible);
        };

        if (n.type == NodeType::Text) {
            // Gradient/image-filled text can't be a tinted UI.Text 鈥?bake it so
            // the pixels match the design (a solid fill still wins if present).
            if (!solidFill(n) && nonSolidVisibleFill(n)) {
                Baked b = bake(n, superScale);
                if (b.ok) return emitBakedLeaf(b);
            }
            // Node opacity folds into the text color (no CanvasGroup) so the
            // muted look survives Linear-space rendering.
            if (const auto* f = solidFill(n)) bindToken(path, "text", f->colorVar);
            recIds.text = addGraphicComps([&](int64_t id) {
                if (tmp) emitTmpText(id, goId, n, n.opacity);
                else emitText(id, goId, n, n.opacity);
            });
            return finish(n.visible);
        }

        if (!isRoot && isVectorIcon(n)) {
            Baked b = bake(n, superScale, /*flatten=*/true);
            if (b.ok) return emitBakedLeaf(b);
            addOpacity();
            return finish(n.visible);  // no recurse into the (failed) flatten
        }

        const bool isContainer = !n.children.empty();
        if (!isContainer && needsBake(n)) {
            Baked b;
            if (nineCandidate(n)) b = bake(n, 1, false, true, true);
            if (!b.ok) b = bake(n, superScale);
            if (b.ok) return emitBakedLeaf(b);
            addOpacity();
            return finish(n.visible);
        }

        // Scroll analysis (mirrors figo2godot): a frame becomes a ScrollRect
        // only when marked scrollable AND its (non-fixed) content actually
        // exceeds its box on that axis.
        using SD = figo::ScrollDirection;
        float scrollCW = 0.0f, scrollCH = 0.0f;
        if (isContainer && n.scrolls())
            for (auto& c : n.children) {
                if (c->scrollFixed) continue;
                scrollCW = std::max(scrollCW, c->relativeTransform.m02 + c->width);
                scrollCH = std::max(scrollCH, c->relativeTransform.m12 + c->height);
            }
        const bool scrollV = isContainer && n.scrolls() &&
            (n.scrollDirection == SD::Vertical || n.scrollDirection == SD::Both) &&
            scrollCH > n.height + 1.0f;
        const bool scrollH = isContainer && n.scrolls() &&
            (n.scrollDirection == SD::Horizontal || n.scrollDirection == SD::Both) &&
            scrollCW > n.width + 1.0f;
        const bool scrollFrame = scrollV || scrollH;

        // Auto-layout (conservative v1): a pure stack becomes a Horizontal/
        // VerticalLayoutGroup so content added/removed in Unity reflows.
        // Skipped when the design mixes in absolutely-placed children, wraps,
        // scrolls, uses SpaceBetween/Baseline packing (no LayoutGroup
        // equivalent), or any child carries a visible effect (its baked sprite
        // sits at painted bounds, which the group would re-pack incorrectly).
        using ALAlign = figo::AutoLayout::Align;
        const auto& al = n.autoLayout;
        bool layoutGroup = isContainer && al.enabled() && !al.wrap && !scrollFrame &&
                           al.primaryAlign != ALAlign::SpaceBetween &&
                           al.primaryAlign != ALAlign::Baseline &&
                           al.counterAlign != ALAlign::Baseline;
        if (layoutGroup)
            for (auto& c : n.children)
                if (c->layoutAbsolute || hasVisibleEffect(*c)) { layoutGroup = false; break; }

        // Container or solid leaf: a node at its own box, optional bg + children.
        if (needsBake(n)) {
            // Complex background: baked sprite on the node itself (sliced) or a
            // sized __bg child (fixed, may overhang via glow).
            Baked b;
            if (nineCandidate(n)) b = bake(n, 1, false, true, true);
            if (!b.ok) b = bake(n, superScale);
            if (b.ok) {
                const Sprite& sp = sprites[b.hash];
                if (b.nine) {
                    recIds.img = addGraphicComps([&](int64_t id) { emitImage(id, goId, sp.guid, 1, {1, 1, 1, 1}); });
                } else {
                    const float cx = b.x - n.absoluteTransform.m02, cy = b.y - n.absoluteTransform.m12;
                    const int64_t bgGo = newId(), bgRt = newId(), bgCr = newId(), bgImg = newId();
                    emitCanvasRenderer(bgCr, bgGo);
                    emitImage(bgImg, bgGo, sp.guid, 0, {1, 1, 1, 1});
                    std::vector<int64_t> bgComps{bgRt, bgCr, bgImg};
                    if (layoutGroup) {  // keep the bg out of the layout flow
                        const int64_t leId = newId();
                        emitLayoutIgnore(leId, bgGo);
                        bgComps.push_back(leId);
                    }
                    emitGameObject(bgGo, "__bg", bgComps, true);
                    emitRectTransform(bgRt, bgGo, rtId, cx, cy, (float)b.w, (float)b.h, {});
                    childRts.push_back(bgRt);
                    if (rec) {
                        CompNodeIds bg;
                        bg.go = bgGo; bg.rt = bgRt; bg.img = bgImg;
                        (*rec)[path.empty() ? "__bg" : path + "/__bg"] = bg;
                    }
                }
            }
        } else if (const auto* f = solidFill(n)) {
            figo::Color c = f->color;
            c.a *= f->opacity;
            bindToken(path, "fill", f->colorVar);
            recIds.img = addGraphicComps([&](int64_t id) { emitImage(id, goId, "", 0, c); });
        }
        addOpacity();
        if (layoutGroup) {
            const int64_t lgId = newId();
            emitLayoutGroup(lgId, goId, al);
            comps.push_back(lgId);
        }

        // Scroll frame -> ScrollRect + RectMask2D on the node, with a __content
        // child holding the absolutely-placed children; the content rect's size
        // is what tells the ScrollRect how far to scroll. The node's own bg
        // Image stays put (it isn't part of the content), and scrollFixed
        // children stay direct children of the node (drawn above the content).
        int64_t childHostRt = rtId;
        int64_t contentGo = 0, contentRt = 0;
        std::vector<int64_t> contentChildRts;
        if (scrollFrame) {
            int64_t maskId = newId();
            emitRectMask2D(maskId, goId);
            comps.push_back(maskId);
            contentGo = newId();
            contentRt = newId();
            int64_t srId = newId();
            emitScrollRect(srId, goId, contentRt, scrollH, scrollV);
            comps.push_back(srId);
            childRts.push_back(contentRt);
            childHostRt = contentRt;
        }

        // Children, names unique among siblings.
        std::map<std::string, int> used;
        used["__bg"] = 1;
        used["__content"] = 1;
        int ci = 0;
        for (auto& child : n.children) {
            std::string fb = "Node" + std::to_string(ci++);
            std::string base = sanitizeName(child->name, fb.c_str());
            std::string uniq = base;
            int k = 2;
            while (used.count(uniq)) uniq = base + "_" + std::to_string(k++);
            used[uniq] = 1;
            const bool fixedChild = scrollFrame && child->scrollFixed;
            // Constraint anchors are suppressed under a scroll __content (wrong
            // parent reference) and inside a LayoutGroup (the group owns
            // child placement).
            int64_t rt = emit(*child, fixedChild ? rtId : childHostRt, uniq,
                              n.absoluteTransform.m02, n.absoluteTransform.m12,
                              path.empty() ? uniq : path + "/" + uniq,
                              (scrollFrame && !fixedChild) || layoutGroup);
            (fixedChild || !scrollFrame ? childRts : contentChildRts).push_back(rt);
        }
        if (scrollFrame) {
            emitGameObject(contentGo, "__content", {contentRt}, true);
            emitRectTransform(contentRt, contentGo, rtId, 0, 0,
                              scrollH ? scrollCW : n.width,
                              scrollV ? scrollCH : n.height, contentChildRts);
        }
        return finish(n.visible);
    }

    // ---- PrefabInstance emission (nested-prefab reuse) ----

    struct Mod {
        int64_t target;
        std::string prop, value;
        std::string objRef = "{fileID: 0}";
    };
    struct Added { int64_t srcRt, addedRt; };

    // Emit `n` as a PrefabInstance of comp, with property overrides computed
    // against the canon. Returns the stripped root RectTransform fileID (what
    // the parent's m_Children must reference).
    int64_t emitPrefabInstance(Comp& comp, Node& n, int64_t parentRt, const std::string& name,
                               float lx, float ly) {
        const int64_t instId = newId();
        std::vector<Mod> mods;
        std::map<std::string, int64_t> stripped;  // comp path -> stripped RT fileID in host
        std::vector<Added> adds;
        std::string addedDocs;  // GO trees added under stripped transforms

        auto strippedFor = [&](const std::string& path) -> int64_t {
            auto it = stripped.find(path);
            if (it != stripped.end()) return it->second;
            const int64_t id = newId();
            std::string b = "RectTransform:\n";
            b += "  m_CorrespondingSourceObject: {fileID: " + std::to_string(comp.ids[path].rt) +
                 ", guid: " + comp.guid + ", type: 3}\n";
            b += "  m_PrefabInstance: {fileID: " + std::to_string(instId) + "}\n";
            b += "  m_PrefabAsset: {fileID: 0}\n";
            yaml += "--- !u!224 &" + std::to_string(id) + " stripped\n" + b;
            stripped[path] = id;
            return id;
        };

        // Root placement + identity.
        const CompNodeIds& root = comp.ids[""];
        mods.push_back({root.go, "m_Name", yamlStr(name)});
        mods.push_back({root.rt, "m_AnchoredPosition.x", f2s(lx)});
        mods.push_back({root.rt, "m_AnchoredPosition.y", f2s(-ly)});
        if (std::lround(n.width) != std::lround(comp.canon->width))
            mods.push_back({root.rt, "m_SizeDelta.x", f2s(n.width)});
        if (std::lround(n.height) != std::lround(comp.canon->height))
            mods.push_back({root.rt, "m_SizeDelta.y", f2s(n.height)});
        if (!n.visible) mods.push_back({root.go, "m_IsActive", "0"});

        walkOverrides(*comp.canon, n, "", comp, mods, adds, strippedFor, addedDocs);

        std::string b = "PrefabInstance:\n";
        b += "  m_ObjectHideFlags: 0\n  serializedVersion: 2\n  m_Modification:\n";
        b += "    serializedVersion: 3\n";
        b += "    m_TransformParent: " + ref(parentRt) + "\n";
        b += "    m_Modifications:\n";
        for (const Mod& m : mods) {
            b += "    - target: {fileID: " + std::to_string(m.target) + ", guid: " + comp.guid + ", type: 3}\n";
            b += "      propertyPath: " + m.prop + "\n";
            b += "      value: " + m.value + "\n";
            b += "      objectReference: " + m.objRef + "\n";
        }
        b += "    m_RemovedComponents: []\n    m_RemovedGameObjects: []\n";
        b += "    m_AddedGameObjects:\n";
        if (adds.empty()) b = b.substr(0, b.size() - 1) + " []\n";
        else for (const Added& a : adds) {
            b += "    - targetCorrespondingSourceObject: {fileID: " + std::to_string(a.srcRt) +
                 ", guid: " + comp.guid + ", type: 3}\n";
            b += "      insertIndex: -1\n";
            b += "      addedObject: {fileID: " + std::to_string(a.addedRt) + "}\n";
        }
        b += "    m_AddedComponents: []\n";
        b += "  m_SourcePrefab: {fileID: 100100000, guid: " + comp.guid + ", type: 3}\n";
        doc(1001, instId, b);
        yaml += addedDocs;  // added subtrees were buffered while the instance doc was open
        return strippedFor("");
    }

    // Per-instance overrides vs the canon: children matched by slot identity
    // (nearest-position tiebreak); a canon-only slot is hidden, a matched slot
    // gets offset/size/text/color/sprite overrides, an instance-only slot is
    // ADDED as a fresh GO subtree under the stripped transform.
    void walkOverrides(const Node& canon, Node& inst, const std::string& path, Comp& comp,
                       std::vector<Mod>& mods, std::vector<Added>& adds,
                       const std::function<int64_t(const std::string&)>& strippedFor,
                       std::string& addedDocs) {
        auto childPath = [&](const std::string& uniq) {
            return path.empty() ? uniq : path + "/" + uniq;
        };
        auto pushColor = [&](int64_t target, const figo::Color& c) {
            mods.push_back({target, "m_Color.r", c2s(c.r)});
            mods.push_back({target, "m_Color.g", c2s(c.g)});
            mods.push_back({target, "m_Color.b", c2s(c.b)});
            mods.push_back({target, "m_Color.a", c2s(c.a)});
        };
        auto pushRect = [&](int64_t rt, float x, float y, float w, float h) {
            mods.push_back({rt, "m_AnchoredPosition.x", f2s(x)});
            mods.push_back({rt, "m_AnchoredPosition.y", f2s(-y)});
            mods.push_back({rt, "m_SizeDelta.x", f2s(w)});
            mods.push_back({rt, "m_SizeDelta.y", f2s(h)});
        };
        auto spriteRef = [&](uint64_t hash) {
            return "{fileID: 21300000, guid: " + sprites[hash].guid + ", type: 3}";
        };

        // Container solid bg: Image on the node itself 鈥?override its color.
        if (!canon.children.empty())
            if (const auto* cb = solidFill(canon)) if (const auto* ib = solidFill(inst))
                if (colorNe(cb->color, ib->color) && comp.ids[path].img) {
                    figo::Color c = ib->color;
                    c.a *= ib->opacity;
                    pushColor(comp.ids[path].img, c);
                }

        std::map<std::string, int> used;
        used["__bg"] = 1;
        std::vector<char> instUsed(inst.children.size(), 0);
        auto matchInst = [&](const Node& cc) -> Node* {
            const std::string key = slotKey(cc);
            int best = -1;
            float bestD = 0;
            for (size_t j = 0; j < inst.children.size(); ++j) {
                if (instUsed[j] || slotKey(*inst.children[j]) != key) continue;
                const Node& c = *inst.children[j];
                const float d = std::fabs(c.relativeTransform.m02 - cc.relativeTransform.m02) +
                                std::fabs(c.relativeTransform.m12 - cc.relativeTransform.m12);
                if (best < 0 || d < bestD) { best = (int)j; bestD = d; }
            }
            if (best < 0) return nullptr;
            instUsed[best] = 1;
            return inst.children[(size_t)best].get();
        };

        for (size_t i = 0; i < canon.children.size(); ++i) {
            const Node& cc = *canon.children[i];
            std::string fb = "Node" + std::to_string(i);
            // MUST mirror emit()'s child naming —
            // comp.ids paths were recorded with those names; a mismatch makes
            // every override on this child silently miss.
            std::string base = sanitizeName(cc.name, fb.c_str());
            std::string uniq = base;
            int k = 2;
            while (used.count(uniq)) uniq = base + "_" + std::to_string(k++);
            used[uniq] = 1;
            const std::string cp = childPath(uniq);
            const CompNodeIds& ids = comp.ids[cp];
            Node* icp = matchInst(cc);
            if (!icp) {
                if (cc.visible && ids.go) mods.push_back({ids.go, "m_IsActive", "0"});
                continue;
            }
            Node& ic = *icp;
            const bool posDiff =
                std::lround(cc.width) != std::lround(ic.width) ||
                std::lround(cc.height) != std::lround(ic.height) ||
                std::lround(cc.relativeTransform.m02) != std::lround(ic.relativeTransform.m02) ||
                std::lround(cc.relativeTransform.m12) != std::lround(ic.relativeTransform.m12);
            if (cc.type == NodeType::Text && ids.text) {
                const auto *fc = solidFill(cc), *fi = solidFill(ic);
                if (posDiff)
                    pushRect(ids.rt, ic.relativeTransform.m02, ic.relativeTransform.m12, ic.width, ic.height);
                if (cc.characters != ic.characters)
                    mods.push_back({ids.text, tmp ? "m_text" : "m_Text", yamlStr(ic.characters)});
                if (fc && fi && (colorNe(fc->color, fi->color) ||
                                 std::fabs(cc.opacity - ic.opacity) > 0.002f)) {
                    figo::Color c = fi->color;
                    c.a *= fi->opacity * ic.opacity;  // node opacity folded like emitText
                    opaqueTextColor(ic, c);
                    if (tmp) {  // TMP reads m_fontColor, not the Graphic m_Color
                        mods.push_back({ids.text, "m_fontColor.r", c2s(c.r)});
                        mods.push_back({ids.text, "m_fontColor.g", c2s(c.g)});
                        mods.push_back({ids.text, "m_fontColor.b", c2s(c.b)});
                        mods.push_back({ids.text, "m_fontColor.a", c2s(c.a)});
                    } else {
                        pushColor(ids.text, c);
                    }
                }
            } else if (needsBake(cc)) {
                // Canon emitted this at its baked-sprite bounds (leaf) or with a
                // baked bg (container) 鈥?re-bake the instance and override the
                // sprite + rect wherever the canon put its Image.
                Baked bc = bake(cc, superScale), bi = bake(ic, superScale);
                if (bi.ok && (bc.hash != bi.hash || posDiff)) {
                    const float pax = ic.parent ? ic.parent->absoluteTransform.m02 : 0.f;
                    const float pay = ic.parent ? ic.parent->absoluteTransform.m12 : 0.f;
                    auto bgIt = comp.ids.find(cp + "/__bg");
                    if (cc.children.empty() && ids.img) {
                        // baked leaf: Image on the node at painted bounds
                        pushRect(ids.rt, bi.x - pax, bi.y - pay, (float)bi.w, (float)bi.h);
                        if (bc.hash != bi.hash) mods.push_back({ids.img, "m_Sprite", "", spriteRef(bi.hash)});
                    } else if (bgIt != comp.ids.end()) {
                        // container with a fixed __bg child
                        if (posDiff)
                            pushRect(ids.rt, ic.relativeTransform.m02, ic.relativeTransform.m12, ic.width, ic.height);
                        pushRect(bgIt->second.rt, bi.x - ic.absoluteTransform.m02,
                                 bi.y - ic.absoluteTransform.m12, (float)bi.w, (float)bi.h);
                        if (bc.hash != bi.hash) mods.push_back({bgIt->second.img, "m_Sprite", "", spriteRef(bi.hash)});
                    } else if (ids.img) {
                        // container with a sliced bg Image on the node itself
                        if (posDiff)
                            pushRect(ids.rt, ic.relativeTransform.m02, ic.relativeTransform.m12, ic.width, ic.height);
                        if (bc.hash != bi.hash) mods.push_back({ids.img, "m_Sprite", "", spriteRef(bi.hash)});
                    }
                }
                if (!cc.children.empty())
                    walkOverrides(cc, ic, cp, comp, mods, adds, strippedFor, addedDocs);
            } else {
                const std::string ci = imageRefOf(cc), ii = imageRefOf(ic);
                const bool imgDiff = !ii.empty() && ii != ci;
                const auto *lc = solidFill(cc), *li = solidFill(ic);
                const bool leafColDiff = cc.children.empty() && lc && li && colorNe(lc->color, li->color);
                if (posDiff && ids.rt)
                    pushRect(ids.rt, ic.relativeTransform.m02, ic.relativeTransform.m12, ic.width, ic.height);
                if (imgDiff && ids.img) {
                    Baked b = bake(ic, superScale);
                    if (b.ok) mods.push_back({ids.img, "m_Sprite", "", spriteRef(b.hash)});
                }
                if (leafColDiff && ids.img) {
                    figo::Color c = li->color;
                    c.a *= li->opacity;
                    pushColor(ids.img, c);
                }
                walkOverrides(cc, ic, cp, comp, mods, adds, strippedFor, addedDocs);
            }
        }

        // Instance-only slots: added as fresh GO subtrees under this level's
        // stripped transform (declared in m_AddedGameObjects).
        for (size_t j = 0; j < inst.children.size(); ++j) {
            if (instUsed[j]) continue;
            Node& extra = *inst.children[j];
            if (!extra.visible) continue;
            std::string fb = "Add" + std::to_string(j);
            std::string base = sanitizeName(extra.name, fb.c_str());
            std::string uniq = base;
            int k = 2;
            while (used.count(uniq)) uniq = base + "_" + std::to_string(k++);
            used[uniq] = 1;
            const int64_t parentStripped = strippedFor(path);
            // Buffer the added subtree's docs so they don't interleave with the
            // PrefabInstance document being assembled by our caller.
            std::string save;
            save.swap(yaml);
            const int64_t addedRt = emit(extra, parentStripped, uniq,
                                         inst.absoluteTransform.m02, inst.absoluteTransform.m12);
            addedDocs += yaml;
            yaml.swap(save);
            adds.push_back({comp.ids[path].rt, addedRt});
        }
    }

    // Emit a component's reusable prefab (components/<name>.prefab), recording
    // every node's fileIDs into comp.ids for instance overrides.
    void emitComponentPrefab(Comp& comp) {
        curW = (uint32_t)std::ceil(std::max(1.0f, comp.frame->width));
        curH = (uint32_t)std::ceil(std::max(1.0f, comp.frame->height));
        ui->setViewport(curW, curH);
        ui->selectFrame(comp.frame->name);
        ui->render();
        captureTextFreeBg(comp.frame);

        yaml = "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n";
        idState = fnv1a("comp/" + comp.name);
        objCount = 0;
        inComponent = true;
        rec = &comp.ids;
        emit(*const_cast<Node*>(comp.canon), 0, comp.name, 0.0f, 0.0f, "");
        rec = nullptr;
        inComponent = false;

        fs::create_directories(outDir / "components");
        std::ofstream pf(outDir / "components" / (comp.name + ".prefab"), std::ios::binary);
        pf << yaml;
        std::ofstream mf(outDir / "components" / (comp.name + ".prefab.meta"), std::ios::binary);
        mf << subst(kPrefabMeta, {{"$GUID", comp.guid}});
    }

    static constexpr float kNineMinArea = 40000.0f;  // ~200x200 logical
    bool nineCandidate(const Node& n) {
        if (!isRectish(n.type)) return false;
        if (hasVisibleEffect(n)) return false;
        if (n.width * n.height < kNineMinArea) return false;
        return true;
    }

    std::map<std::string, int> frameStemCount;  // filename collision guard

    void convertFrame(Node* frame) {
        curW = (uint32_t)std::ceil(std::max(1.0f, frame->width));
        curH = (uint32_t)std::ceil(std::max(1.0f, frame->height));
        ui->setViewport(curW, curH);
        if (!ui->selectFrame(frame->name)) {
            std::fprintf(stderr, "  WARN: selectFrame(%s) failed\n", frame->name.c_str());
            return;
        }
        ui->render();  // computes absoluteTransform
        captureTextFreeBg(frame);
        curFrame = frame->name;

        yaml = "%YAML 1.1\n%TAG !u! tag:unity3d.com,2011:\n";
        // Frames whose names sanitize to the same stem would overwrite each
        // other's .prefab AND collide on guid seeds; suffix repeats. The first
        // occurrence keeps today's stem/guid so existing references survive.
        std::string uniq = frame->name;
        std::string stem = fileStem(frame->name, "Frame");
        if (int k = ++frameStemCount[stem]; k > 1) {
            uniq += "#" + std::to_string(k);
            stem += "_" + std::to_string(k);
        }
        idState = fnv1a("frame/" + uniq);  // deterministic per-prefab fileIDs
        objCount = 0;

        emit(*frame, 0, stem, 0.0f, 0.0f);

        std::ofstream pf(outDir / (stem + ".prefab"), std::ios::binary);
        pf << yaml;
        std::ofstream mf(outDir / (stem + ".prefab.meta"), std::ios::binary);
        mf << subst(kPrefabMeta, {{"$GUID", makeGuid(fnv1a("prefab/" + uniq))}});
        std::printf("  %s -> %s.prefab (%d objects)\n", frame->name.c_str(), stem.c_str(), objCount);
    }
};

// Index a directory of .ttf/.otf into a FontEntry list, copying each into the
// output's fonts/ dir with a TrueTypeFontImporter .meta (deterministic guid).
static std::vector<FontEntry> loadFonts(const fs::path& fontsSrc, const fs::path& outFonts) {
    std::vector<FontEntry> out;
    if (fontsSrc.empty() || !fs::exists(fontsSrc)) return out;
    std::error_code ec;
    fs::create_directories(outFonts, ec);
    for (const auto& de : fs::recursive_directory_iterator(fontsSrc, ec)) {
        if (!de.is_regular_file()) continue;
        std::string ext = de.path().extension().string();
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".ttf" && ext != ".otf") continue;
        FontEntry fe;
        if (!parseFontMeta(de.path(), fe.family, fe.weight, fe.italic, fe.lineFactor)) continue;
        fe.familyNorm = normFamily(fe.family);
        fe.file = de.path().filename().string();
        fe.guid = makeGuid(fnv1a("font/" + fe.file));
        fs::copy_file(de.path(), outFonts / fe.file, fs::copy_options::overwrite_existing, ec);
        std::ofstream mf(outFonts / (fe.file + ".meta"), std::ios::binary);
        mf << subst(kFontMeta, {{"$GUID", fe.guid}, {"$FAMILY", fe.family}});
        out.push_back(std::move(fe));
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: figo2unity <input.fig|canvas.json> [outDir] [--frame NAME] "
                    "[--fonts DIR] [--scale N] [--prefabs] [--prefab-anon] [--no-prefab T1,T2] "
                    "[--prefab-pin T1,T2] [--tmp] [--linear]\n");
        return 2;
    }
    const std::string input = argv[1];
    fs::path outDir = "unity_out";
    std::string onlyFrame;
    fs::path fontsDir;
    int scale = 2;
    bool prefabs = false;
    bool prefabAnon = false;
    bool tmpText = false;
    bool linearFlag = false;
    std::set<std::string> noPrefabArg;
    std::set<std::string> prefabPinArg;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frame" && i + 1 < argc) onlyFrame = argv[++i];
        else if (a == "--fonts" && i + 1 < argc) fontsDir = argv[++i];
        else if (a == "--scale" && i + 1 < argc) scale = std::max(1, atoi(argv[++i]));
        else if (a == "--prefabs") prefabs = true;
        else if (a == "--prefab-anon") { prefabs = true; prefabAnon = true; }
        else if (a == "--tmp") tmpText = true;
        else if (a == "--linear") linearFlag = true;
        else if (a == "--no-prefab" && i + 1 < argc) {
            std::string list = argv[++i];
            size_t p = 0;
            while (p < list.size()) {
                size_t q = list.find(',', p);
                if (q == std::string::npos) q = list.size();
                if (q > p) noPrefabArg.insert(list.substr(p, q - p));
                p = q + 1;
            }
        }
        else if (a == "--prefab-pin" && i + 1 < argc) {  // hand-picked comps: always extract
            std::string list = argv[++i];
            size_t p = 0;
            while (p < list.size()) {
                size_t q = list.find(',', p);
                if (q == std::string::npos) q = list.size();
                if (q > p) prefabPinArg.insert(list.substr(p, q - p));
                p = q + 1;
            }
            prefabs = true;
        }
        else if (!a.empty() && a[0] != '-') outDir = a;
    }

    std::printf("loading %s\n", input.c_str());
    std::unique_ptr<figo::FigmaUI> ui;
    try {
        ui = figo::FigmaUI::fromFile(input);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "FAIL: load: %s\n", ex.what());
        return 1;
    }

    std::error_code ec;
    fs::create_directories(outDir, ec);
    fs::path texDir = outDir / "textures";
    fs::create_directories(texDir, ec);

    auto frames = ui->document().topLevelFrames();
    if (frames.empty()) {
        std::fprintf(stderr, "FAIL: no top-level frames\n");
        return 1;
    }

    Converter cv;
    cv.ui = ui.get();
    cv.outDir = outDir;
    cv.texDir = texDir;
    cv.superScale = scale;
    cv.fonts = loadFonts(fontsDir, outDir / "fonts");
    if (!fontsDir.empty())
        std::printf("bundled %zu font file(s) from %s\n", cv.fonts.size(), fontsDir.string().c_str());

    // Text material: Figo/UIText shader compensates glyph-coverage blending in
    // Linear-color-space projects (no-op in Gamma).
    {
        const std::string shaderGuid = makeGuid(fnv1a("figo/uitext.shader"));
        cv.textMatGuid = makeGuid(fnv1a("figo/uitext.mat"));
        std::ofstream sf(outDir / "FigoUIText.shader", std::ios::binary);
        sf << kTextShader;
        std::ofstream sm(outDir / "FigoUIText.shader.meta", std::ios::binary);
        sm << subst(kShaderMeta, {{"$GUID", shaderGuid}});
        std::ofstream mf2(outDir / "FigoText.mat", std::ios::binary);
        mf2 << subst(kTextMaterial, {{"$SHADER", shaderGuid}});
        std::ofstream mm(outDir / "FigoText.mat.meta", std::ios::binary);
        mm << subst(kMatMeta, {{"$GUID", cv.textMatGuid}});
    }
    cv.prefabAnon = prefabAnon;
    cv.noPrefab = noPrefabArg;
    cv.prefabPin = prefabPinArg;
    cv.tmp = tmpText;
    cv.linearComp = linearFlag;

    // Prefab pre-pass: find repeated components, cluster state variants, emit
    // each cluster's components/<name>.prefab FIRST (instances must know the
    // fileIDs inside the source prefab), then the frame loop instances them.
    if (prefabs) {
        for (Node* frame : frames) {
            if (!onlyFrame.empty() && frame->name != onlyFrame) continue;
            cv.scanComponents(frame);
        }
        // Partition each group into structural clusters, richest instance first
        // so each cluster's canon is the SUPERSET of its members.
        for (auto& kv : cv.compBySig) {
            Converter::Comp& group = kv.second;
            if (group.insts.empty()) continue;
            std::sort(group.insts.begin(), group.insts.end(),
                      [](const auto& a, const auto& b) {
                          return descendants(*a.first) > descendants(*b.first);
                      });
            std::vector<Converter::Comp*> clusters;
            for (auto& inst : group.insts) {
                Converter::Comp* fit = nullptr;
                for (Converter::Comp* c : clusters)
                    if (!cv.poorFit(*c->canon, *inst.first)) { fit = c; break; }
                if (!fit) {
                    cv.variants.push_back(std::make_unique<Converter::Comp>());
                    fit = cv.variants.back().get();
                    fit->canon = inst.first;
                    fit->frame = inst.second;
                    fit->name = group.name;
                    clusters.push_back(fit);
                }
                fit->count++;
                cv.nodeComp[inst.first] = fit;
            }
        }
        // Extract every cluster repeated >=2x (skip screen-sized containers and
        // user-suppressed wrapper types); give each a unique prefab name + guid.
        std::set<std::string> usedNames;
        int compCount = 0;
        for (auto& up : cv.variants) {
            Converter::Comp& comp = *up;
            const bool pinned = cv.prefabPin.count(comp.canon->compType) != 0;
            if (comp.count < 2 && !pinned) continue;
            if (!pinned &&
                comp.canon->width >= comp.frame->width * 0.9f &&
                comp.canon->height >= comp.frame->height * 0.9f) continue;
            if (cv.noPrefab.count(comp.canon->compType)) continue;
            std::string base;
            for (char c : sanitizeName(comp.name, "Comp"))
                base.push_back((std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_');
            if (base.empty()) base = "Comp";
            std::string uniq = base;
            int k = 2;
            while (usedNames.count(uniq)) uniq = base + "_" + std::to_string(k++);
            usedNames.insert(uniq);
            comp.name = uniq;
            comp.guid = makeGuid(fnv1a("comp/" + uniq));
            comp.extracted = true;
            cv.emitComponentPrefab(comp);
            ++compCount;
        }
        cv.prefabs = true;
        std::printf("prefabs: %d component(s) extracted\n", compCount);
    }

    int n = 0;
    for (Node* frame : frames) {
        if (!onlyFrame.empty() && frame->name != onlyFrame) continue;
        cv.convertFrame(frame);
        ++n;
    }

    // Keep design-token names alongside the export: the prefab colors are
    // resolved literals, so figo_tokens.json (variable table + node bindings)
    // is what an engine-side reskin script needs to re-bind them.
    const auto& vt = ui->document().variables;
    if (!vt.empty()) {
        nlohmann::json tj;
        tj["modes"] = vt.modes;
        tj["activeMode"] = vt.activeMode;
        nlohmann::json vars = nlohmann::json::object();
        for (const auto& v : vt.vars) {
            nlohmann::json mv = nlohmann::json::object();
            for (size_t m = 0; m < v.values.size() && m < vt.modes.size(); ++m)
                mv[vt.modes[m]] = figo::colorToHex(v.values[m]);
            vars[v.name] = mv;
        }
        tj["variables"] = vars;
        tj["bindings"] = cv.tokenBindings;
        std::ofstream tf(outDir / "figo_tokens.json", std::ios::binary);
        tf << tj.dump(2) << "\n";
        std::printf("tokens: %zu variable(s), %zu binding(s) -> figo_tokens.json\n",
                    vt.vars.size(), cv.tokenBindings.size());
    }

    // Prune orphan component prefabs (extracted but never instanced by a frame
    // — e.g. used only nested inside another component, where it was inlined).
    if (prefabs) {
        int pruned = 0;
        for (auto& up : cv.variants) {
            if (!up->extracted || cv.usedComps.count(up->name)) continue;
            fs::remove(outDir / "components" / (up->name + ".prefab"), ec);
            fs::remove(outDir / "components" / (up->name + ".prefab.meta"), ec);
            ++pruned;
        }
        if (pruned) std::printf("prefabs: %d orphan component(s) pruned\n", pruned);
    }

    std::printf("RESULT: OK, %d prefab(s), %zu unique sprite(s)\n", n, cv.sprites.size());
    return 0;
}
