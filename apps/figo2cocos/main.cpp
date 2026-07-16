// figo2cocos — convert a figo design (.fig / canvas.json / REST JSON) into
// Cocos Creator 3.x assets: one .prefab per top-level frame + a shared textures/
// folder of deduplicated PNG sprites (each with a .meta sprite-frame). The
// design's own ThorVG rasterizer bakes the sprites, so the textures are
// pixel-identical to the figo runtime.
//
// A .prefab is a JSON array of serialized engine objects (cc.Prefab, cc.Node,
// cc.UITransform, cc.Sprite, cc.Label, …) wired by {"__id__": <arrayIndex>}
// references — the format ported from psd2prefab (Cocos Creator 3.4.2+).
//
// Node mapping:
//   TEXT                                   -> cc.Node + cc.Label (system font)
//   plain solid rect / container bg        -> cc.Node + cc.Sprite (1x1 white, tinted)
//   ellipse / vector / gradient / image /
//     stroke / effect / rounded panel      -> cc.Node + cc.Sprite (baked PNG; 9-slice)
//   container                              -> cc.Node holding a bg sprite + children
//   anything empty                         -> cc.Node (passthrough)
//
// Coordinate flip: every node uses anchor (0,1) (top-left), _lpos = (relX, -relY)
// and a cc.UITransform with the figo box size — a clean Figma(Y-down) -> Cocos
// (Y-up) mapping that composes correctly down the tree.
//
// Scope (v1): self-contained prefab per frame (no nested-prefab extraction),
// system fonts, axis-aligned (no rotation) — matching psd2prefab's output shape.

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
#include <iterator>
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
using nlohmann::json;

// PNG encoding is shared across the three exporters (Paeth + real deflate).
#include "../anim_tracks.h"
#include "../exporter_png.h"
using figopng::writePng;

// ===================== helpers ==============================================

static std::string sanitizeName(const std::string& name, const char* fallback) {
    std::string out;
    for (unsigned char c : name) {
        if (c < 0x20 || c == '.' || c == '/' || c == ':' || c == '@' || c == '%' ||
            c == '$' || c == '"' || c == '\\')
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

// Filesystem-safe stem (alnum/-/_), capped — for texture/prefab filenames.
// A name with no ASCII residue (e.g. pure CJK) would collapse to underscores
// and collide with every sibling like it — fall back to fallback+name-hash.
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

// Round to 0.01 and emit an INTEGER json number when integral — Creator's own
// serializer (JS) writes 92, not 92.0; matching it keeps the editor's
// eventual re-save byte-identical instead of churning every number.
static json rnd(float v) {
    double r = std::round(v * 100.0) / 100.0;
    if (r == 0.0) return json(0);  // normalize -0
    if (r == std::floor(r) && std::fabs(r) < 1e9) return json((int64_t)r);
    return json(r);
}

// A figo Color [0,1] -> Cocos cc.Color {r,g,b,a} 0..255 ints, folding the
// paint's separate opacity into alpha.
static int u8(float v) { return std::max(0, std::min(255, (int)std::lround(v * 255.0f))); }
static json ccColor(const figo::Color& c) {
    return json{{"__type__", "cc.Color"}, {"r", u8(c.r)}, {"g", u8(c.g)}, {"b", u8(c.b)}, {"a", u8(c.a)}};
}
static json ccPaintColor(const figo::Paint& p) {
    figo::Color c = p.color;
    c.a *= p.opacity;
    return ccColor(c);
}
static json ccSize(float w, float h) {
    return json{{"__type__", "cc.Size"}, {"width", rnd(w)}, {"height", rnd(h)}};
}
static json ccVec2(float x, float y) {
    return json{{"__type__", "cc.Vec2"}, {"x", rnd(x)}, {"y", rnd(y)}};
}
static json ccVec3(float x, float y, float z) {
    return json{{"__type__", "cc.Vec3"}, {"x", rnd(x)}, {"y", rnd(y)}, {"z", rnd(z)}};
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

// ===================== fonts ================================================
// Bundle real .ttf/.otf files (--fonts DIR) so cc.Label uses the design's
// typography (cc.TTFFont asset) instead of the system font. Matching ported
// from figo2godot/figo2unity.

struct FontEntry {
    std::string familyNorm;  // lowercased, spaces removed
    std::string family;      // as parsed
    int weight = 400;
    bool italic = false;
    std::string file;  // basename under fonts/
    std::string uuid;  // 36-char asset uuid
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
static uint32_t be32v(const std::vector<uint8_t>& b, size_t o) {
    return o + 3 < b.size() ? (uint32_t)((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) : 0;
}

// Parse a .ttf/.otf for family (name id 1), weight (OS/2 usWeightClass) and
// italic (OS/2 fsSelection bit0). Falls back to the filename when absent.
static bool parseFontMeta(const fs::path& path, std::string& family, int& weight, bool& italic) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 12) return false;
    size_t base = 0;
    if (b[0] == 't' && b[1] == 't' && b[2] == 'c' && b[3] == 'f') base = be32v(b, 12);  // .ttc -> font 0
    uint16_t numTables = be16(b, base + 4);
    size_t rec = base + 12;
    size_t nameOff = 0, os2Off = 0;
    for (uint16_t i = 0; i < numTables; ++i, rec += 16) {
        if (rec + 16 > b.size()) break;
        std::string tag((const char*)&b[rec], 4);
        uint32_t off = be32v(b, rec + 8);
        if (tag == "name") nameOff = off;
        else if (tag == "OS/2") os2Off = off;
    }
    weight = 400;
    italic = false;
    family.clear();
    if (os2Off) {
        weight = be16(b, os2Off + 4);
        italic = (be16(b, os2Off + 62) & 0x01) != 0;
        if (weight <= 0) weight = 400;
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

// ===================== prefab-extraction helpers ============================

static bool colorNe(const figo::Color& a, const figo::Color& b) {
    return std::fabs(a.r - b.r) + std::fabs(a.g - b.g) + std::fabs(a.b - b.b) +
               std::fabs(a.a - b.a) > 0.002f;
}
static int descendants(const Node& n) {
    int c = 0;
    for (const auto& k : n.children) c += 1 + descendants(*k);
    return c;
}
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

// ===================== Cocos UUIDs ==========================================

// 36-char hyphenated v4-shaped uuid from a 64-bit seed (deterministic, so the
// same content yields the same uuid across runs and dedups cleanly).
static std::string makeUuid(uint64_t seed) {
    std::mt19937_64 rng(seed ? seed : 0x9E3779B97F4A7C15ull);
    uint8_t b[16];
    for (int i = 0; i < 16; i += 8) {
        uint64_t v = rng();
        for (int k = 0; k < 8; ++k) b[i + k] = (uint8_t)(v >> (k * 8));
    }
    b[6] = (b[6] & 0x0f) | 0x40;  // version 4
    b[8] = (b[8] & 0x3f) | 0x80;  // variant
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        s.push_back(hex[b[i] >> 4]);
        s.push_back(hex[b[i] & 0xf]);
    }
    return s;
}

// Compress a 36-char hyphenated uuid to Cocos' 22-char fileId form (2 head hex
// chars kept, the remaining 30 packed 3-hex -> 2-base64). Ported from
// psd2prefab's UuidUtils.compressHex (reserved head = 2).
static std::string compressUuid(const std::string& uuid) {
    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string hexStr;
    for (char c : uuid)
        if (c != '-') hexStr.push_back(c);
    if (hexStr.size() != 32) return uuid;
    auto hv = [&](size_t i) { char c = hexStr[i]; return c <= '9' ? c - '0' : (std::tolower(c) - 'a' + 10); };
    std::string out;
    out.push_back(hexStr[0]);
    out.push_back(hexStr[1]);
    for (size_t i = 2; i + 2 < 32; i += 3) {
        int h1 = hv(i), h2 = hv(i + 1), h3 = hv(i + 2);
        out.push_back(B64[(h1 << 2) | (h2 >> 2)]);
        out.push_back(B64[((h2 & 3) << 4) | h3]);
    }
    return out;
}

// FNV-1a 64 over a string (stable uuid seeds from names/content).
static uint64_t fnv1a(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// ===================== meta templates =======================================
// Byte-for-byte the form Creator 3.8.8's importers write. Emitting an OLDER
// importer version makes the editor run its meta migration on first import and
// write every file back ("prefabs keep getting auto-modified"); matching the
// current version leaves the files untouched.

static const char* kSpriteFrameMeta = R"META({
  "ver": "1.0.27",
  "importer": "image",
  "imported": true,
  "uuid": "$TEX",
  "files": [
    ".json",
    ".png"
  ],
  "subMetas": {
    "6c48a": {
      "importer": "texture",
      "uuid": "$TEX@6c48a",
      "displayName": "$NAME",
      "id": "6c48a",
      "name": "texture",
      "userData": {
        "wrapModeS": "clamp-to-edge",
        "wrapModeT": "clamp-to-edge",
        "imageUuidOrDatabaseUri": "$TEX",
        "minfilter": "$FILTER",
        "magfilter": "$FILTER",
        "mipfilter": "none",
        "anisotropy": 0,
        "isUuid": true,
        "visible": false
      },
      "ver": "1.0.22",
      "imported": true,
      "files": [
        ".json"
      ],
      "subMetas": {}
    },
    "f9941": {
      "importer": "sprite-frame",
      "uuid": "$TEX@f9941",
      "displayName": "$NAME",
      "id": "f9941",
      "name": "spriteFrame",
      "userData": {
        "trimType": "auto",
        "trimThreshold": 1,
        "rotated": false,
        "offsetX": 0,
        "offsetY": 0,
        "trimX": 0,
        "trimY": 0,
        "width": $W,
        "height": $H,
        "rawWidth": $W,
        "rawHeight": $H,
        "borderTop": $BT,
        "borderBottom": $BB,
        "borderLeft": $BL,
        "borderRight": $BR,
        "packable": true,
        "isUuid": true,
        "imageUuidOrDatabaseUri": "$TEX@6c48a",
        "atlasUuid": "",
        "pixelsToUnit": 100,
        "pivotX": 0.5,
        "pivotY": 0.5,
        "meshType": 0,
        "vertices": {
          "rawPosition": [
            -$HW,
            -$HH,
            0,
            $HW,
            -$HH,
            0,
            -$HW,
            $HH,
            0,
            $HW,
            $HH,
            0
          ],
          "indexes": [
            0,
            1,
            2,
            2,
            1,
            3
          ],
          "uv": [
            0,
            $H,
            $W,
            $H,
            0,
            0,
            $W,
            0
          ],
          "nuv": [
            0,
            0,
            1,
            0,
            0,
            1,
            1,
            1
          ],
          "minPos": [
            -$HW,
            -$HH,
            0
          ],
          "maxPos": [
            $HW,
            $HH,
            0
          ]
        }
      },
      "ver": "1.0.12",
      "imported": true,
      "files": [
        ".json"
      ],
      "subMetas": {}
    }
  },
  "userData": {
    "type": "sprite-frame",
    "hasAlpha": true,
    "redirect": "$TEX@6c48a",
    "fixAlphaTransparencyArtifacts": false
  }
}
)META";

static const char* kPrefabMeta = R"META({
  "ver": "1.1.50",
  "importer": "prefab",
  "imported": true,
  "uuid": "$UUID",
  "files": [
    ".json"
  ],
  "subMetas": {},
  "userData": {
    "syncNodeName": "$NAME"
  }
}
)META";

static const char* kDirMeta = R"META({
  "ver": "1.2.0",
  "importer": "directory",
  "imported": true,
  "uuid": "$UUID",
  "files": [],
  "subMetas": {},
  "userData": {}
}
)META";

static const char* kTtfMeta = R"META({
  "ver": "1.0.1",
  "importer": "ttf-font",
  "imported": true,
  "uuid": "$UUID",
  "files": [
    ".json",
    "$FILE"
  ],
  "subMetas": {},
  "userData": {}
})META";

static const char* kTsMeta = R"META({
  "ver": "4.0.24",
  "importer": "typescript",
  "imported": true,
  "uuid": "$UUID",
  "files": [],
  "subMetas": {},
  "userData": {}
}
)META";

// Runtime replayer for captured CSS animations (see apps/anim_tracks.h for
// key preprocessing). Keys arrive pre-linearized on an absolute seconds
// timeline in CSS conventions; this converts to Cocos (y-up, ccw+ angle) and
// emulates the transform-origin pivot via position compensation — the node's
// anchorPoint stays (0,1), so children are unaffected.
static const char* kFigoAnimTs = R"TS(import { _decorator, CCBoolean, CCFloat, Component, UIOpacity, UITransform, Vec3 } from 'cc';
const { ccclass, property } = _decorator;

/** figo export: replays a captured CSS animation on this node.
 *  Keys are flat arrays on an absolute [0,dur] seconds timeline, already
 *  linearized by the exporter (easing sampled into sub-keys):
 *    alphaK  [t,a ...]        opacity 0..1
 *    scaleK  [t,sx,sy ...]
 *    posK    [t,dx,dy ...]    CSS px delta, y down
 *    rotK    [t,deg ...]      CSS degrees, clockwise positive
 *  Rotation/scale pivot at (pivotX,pivotY) — fractions of the node box, CSS
 *  y-down — emulated by moving the node so the pivot point stays fixed. */
@ccclass('FigoAnim')
export class FigoAnim extends Component {
    @property(CCFloat) dur = 0;
    @property(CCBoolean) loop = false;
    @property(CCBoolean) stepMode = false;
    @property(CCFloat) pivotX = 0.5;
    @property(CCFloat) pivotY = 0.5;
    @property({ type: [CCFloat] }) alphaK: number[] = [];
    @property({ type: [CCFloat] }) scaleK: number[] = [];
    @property({ type: [CCFloat] }) posK: number[] = [];
    @property({ type: [CCFloat] }) rotK: number[] = [];

    private _t = 0;
    private _rest = new Vec3();
    private _hasRest = false;

    protected onLoad(): void {
        this._rest.set(this.node.position);
        this._hasRest = true;
        this.applyAt(0);
    }

    protected update(dt: number): void {
        if (this.dur <= 0) return;
        this._t += dt;
        let t = this._t;
        if (this.loop) t %= this.dur;
        else if (t > this.dur) t = this.dur;
        this.applyAt(t);
    }

    private samp(k: number[], stride: number, t: number, out: number[]): boolean {
        const n = Math.floor(k.length / stride);
        if (n < 2) return false;
        let i = 1;
        while (i < n - 1 && k[i * stride] < t) i++;
        const t0 = k[(i - 1) * stride], t1 = k[i * stride];
        let u = t1 > t0 ? (t - t0) / (t1 - t0) : 1;
        if (u < 0) u = 0; else if (u > 1) u = 1;
        if (this.stepMode) u = u < 1 ? 0 : 1;
        for (let c = 1; c < stride; c++)
            out[c - 1] = k[(i - 1) * stride + c] * (1 - u) + k[i * stride + c] * u;
        return true;
    }

    private applyAt(t: number): void {
        const v: number[] = [0, 0];
        if (this.samp(this.alphaK, 2, t, v)) {
            let op = this.getComponent(UIOpacity);
            if (!op) op = this.addComponent(UIOpacity);
            op.opacity = Math.max(0, Math.min(255, Math.round(v[0] * 255)));
        }
        let sx = 1, sy = 1, rot = 0, dx = 0, dy = 0;
        let hasS = false, hasR = false, hasP = false;
        if (this.samp(this.scaleK, 3, t, v)) { sx = v[0]; sy = v[1]; hasS = true; }
        if (this.samp(this.rotK, 2, t, v)) { rot = v[0]; hasR = true; }
        if (this.samp(this.posK, 3, t, v)) { dx = v[0]; dy = v[1]; hasP = true; }
        if (!(hasS || hasR || hasP)) return;
        if (!this._hasRest) { this._rest.set(this.node.position); this._hasRest = true; }
        const ut = this.getComponent(UITransform);
        const w = ut ? ut.contentSize.width : 0;
        const h = ut ? ut.contentSize.height : 0;
        const ax = ut ? ut.anchorPoint.x : 0;
        const ay = ut ? ut.anchorPoint.y : 1;
        const px = (this.pivotX - ax) * w;            // pivot rel anchor, y-up px
        const py = ((1 - this.pivotY) - ay) * h;
        const rad = -rot * Math.PI / 180;             // CSS cw+ -> Cocos ccw+
        const cs = Math.cos(rad), sn = Math.sin(rad);
        const mx = cs * sx * px - sn * sy * py;
        const my = sn * sx * px + cs * sy * py;
        this.node.setPosition(this._rest.x + dx + px - mx,
                              this._rest.y - dy + py - my, this._rest.z);
        if (hasS) this.node.setScale(sx, sy, 1);
        if (hasR) this.node.angle = -rot;
    }
}
)TS";

// <dir>.meta sits next to the directory; emitting it ourselves (deterministic
// uuid) keeps the editor from creating one on first import.
static void writeDirMeta(const fs::path& dir);

static std::string subst(std::string tpl, const std::vector<std::pair<std::string, std::string>>& kv) {
    for (const auto& [k, v] : kv) {
        size_t p = 0;
        while ((p = tpl.find(k, p)) != std::string::npos) { tpl.replace(p, k.size(), v); p += v.size(); }
    }
    return tpl;
}

static void writeDirMeta(const fs::path& dir) {
    std::ofstream f(dir.string() + ".meta", std::ios::binary);
    f << subst(kDirMeta, {{"$UUID", makeUuid(fnv1a("dir/" + dir.filename().string()))}});
}

// ===================== converter ============================================

struct Converter {
    figo::FigmaUI* ui = nullptr;
    fs::path outDir, texDir;
    int superScale = 2;
    uint32_t curW = 0, curH = 0;

    struct Sprite {
        std::string file;  // basename, e.g. "icon_ab12cd34.png"
        std::string uuid;  // 36-char texture uuid (sprite-frame is uuid@f9941)
        int w = 0, h = 0;
        bool nine = false;
        int ml = 0, mr = 0, mt = 0, mb = 0;  // 9-slice borders (logical px)
    };
    std::map<uint64_t, Sprite> sprites;  // content hash -> sprite (global dedup)
    std::map<std::string, uint64_t> usedSpriteNames;  // filename -> full hash (8-hex truncation collision detector)
    std::string whiteUuid;               // shared 1x1 white sprite-frame uuid

    // Bundled fonts (--fonts DIR); populated by main.
    std::vector<FontEntry> fonts;
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

    // ---- prefab extraction (--prefabs), same identification as figo2godot ----
    bool prefabs = false;
    bool prefabAnon = false;
    bool inComponent = false;
    std::set<std::string> noPrefab;
    std::set<std::string> prefabPin;  // compTypes ALWAYS extracted (hand-picked captures)
    std::set<std::string> usedComps;

    // fileIds of the objects emitted for one node of a component prefab, keyed
    // by path from the component root ("" = root). Instance propertyOverrides
    // target these via cc.TargetInfo.localID.
    struct CompNodeIds { std::string node, ut, label, sprite; };
    struct Comp {
        std::string name;
        std::string uuid;             // components/<name>.prefab asset uuid
        const Node* canon = nullptr;
        Node* frame = nullptr;
        int count = 0;
        bool extracted = false;
        std::vector<std::pair<const Node*, Node*>> insts;
        std::map<std::string, CompNodeIds> ids;
    };
    std::map<std::string, Comp> compBySig;
    std::vector<std::unique_ptr<Comp>> variants;
    std::map<const Node*, Comp*> nodeComp;

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
    std::string groupKey(const Node& n) const {
        const int wb = (int)std::lround(n.width) / 64, hb = (int)std::lround(n.height) / 64;
        if (!n.compType.empty())
            return n.compType + "|" + std::to_string(wb) + "x" + std::to_string(hb);
        return "|" + std::to_string(wb) + "x" + std::to_string(hb) + "|" + sig(n, true);
    }
    std::string slotKey(const Node& n) const {
        return (n.compType.empty() ? "" : "c:" + n.compType + "|") + sig(n, true, 0);
    }
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
    void scanComponents(Node* frame) {
        std::function<void(Node&)> rec2 = [&](Node& n) {
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
                rec2(cn);
            }
        };
        rec2(*frame);
    }
    const Comp* extractedComponent(const Node& n) {
        if (!prefabs || inComponent || n.children.empty() || n.type == NodeType::Text) return nullptr;
        auto it = nodeComp.find(&n);
        return (it != nodeComp.end() && it->second->extracted) ? it->second : nullptr;
    }

    // While emitting a component prefab, emit() records each node's fileIds here.
    std::map<std::string, CompNodeIds>* rec = nullptr;

    // Instance stub node indices of the file being emitted. The engine only
    // expands nested prefab instances listed in the ROOT PrefabInfo's
    // nestedPrefabInstanceRoots — without it every instance stays an empty
    // stub at load ("only one PlayerTile shows": the sole inline one).
    std::vector<int> instanceStubs;

    // per-prefab object array + a unique-fileId counter
    json arr = json::array();
    uint64_t fileIdSeed = 0;
    int push(json obj) {
        int idx = (int)arr.size();
        arr.push_back(std::move(obj));
        return idx;
    }
    std::string nextFileId() { return compressUuid(makeUuid(fileIdSeed++)); }

    // ---- 9-slice shrink (identical to figo2godot) ----
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
    // unique content. Mirrors figo2godot::bake.
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
            sp.uuid = makeUuid(h);
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
        // Half extents as JS-style numbers (46 / 45.5, never 45.50).
        auto half = [](int v) {
            return (v % 2 == 0) ? std::to_string(v / 2) : std::to_string(v / 2) + ".5";
        };
        // $HW/$HH substituted before $W/$H (shared prefix).
        std::string meta = subst(kSpriteFrameMeta, {
            {"$TEX", sp.uuid}, {"$NAME", sp.file.substr(0, sp.file.find('.'))},
            {"$HW", half(sp.w)}, {"$HH", half(sp.h)},
            {"$W", std::to_string(sp.w)}, {"$H", std::to_string(sp.h)},
            {"$BT", std::to_string(sp.mt)}, {"$BB", std::to_string(sp.mb)},
            {"$BL", std::to_string(sp.ml)}, {"$BR", std::to_string(sp.mr)},
            // 九宫格贴图中段仅 1px,线性过滤放大会把边界行晕染成大片渐变
            // (面板顶部高亮边变"发光");边角 1:1、中段纯色,nearest 像素精确。
            {"$FILTER", sp.nine ? "nearest" : "linear"},
        });
        std::ofstream f(texDir / (sp.file + ".meta"), std::ios::binary);
        f << meta;
    }

    // The shared 1x1 white sprite-frame that all solid quads tint.
    void ensureWhite() {
        if (!whiteUuid.empty()) return;
        uint32_t px = 0xFFFFFFFFu;
        writePng(texDir / "white.png", &px, 1, 1);
        Sprite sp;
        sp.file = "white.png";
        sp.uuid = makeUuid(fnv1a("figo2cocos/white"));
        sp.w = sp.h = 1;
        writeSpriteMeta(sp);
        whiteUuid = sp.uuid;
    }

    // ---- component builders (Cocos 3.4.2+ field sets, from psd2prefab) ----
    json compBase(const char* type, int nodeIdx) {
        return json{
            {"__type__", type}, {"_name", ""}, {"_objFlags", 0},
            {"_enabled", true}, {"node", {{"__id__", nodeIdx}}}, {"_id", ""},
            {"__prefab", nullptr},  // patched to its CompPrefabInfo by addComp
        };
    }
    // Push a component, attach it to nodeIdx, and give it a CompPrefabInfo.
    // Returns the comp's fileId (cc.TargetInfo.localID targets it).
    std::string addComp(int nodeIdx, json comp) {
        int compIdx = push(std::move(comp));
        arr[nodeIdx]["_components"].push_back({{"__id__", compIdx}});
        std::string fid = nextFileId();
        int cpiIdx = push(json{{"__type__", "cc.CompPrefabInfo"}, {"fileId", fid}});
        arr[compIdx]["__prefab"] = {{"__id__", cpiIdx}};
        return fid;
    }

    json uiTransform(int nodeIdx, float w, float h) {
        json c = compBase("cc.UITransform", nodeIdx);
        c["_contentSize"] = ccSize(w, h);
        c["_anchorPoint"] = ccVec2(0.0f, 1.0f);  // top-left
        return c;
    }
    json spriteComp(int nodeIdx, const std::string& frameUuid, int type, int sizeMode, const json& color) {
        json c = compBase("cc.Sprite", nodeIdx);
        c["_srcBlendFactor"] = 2;
        c["_dstBlendFactor"] = 4;
        c["_spriteFrame"] = {{"__uuid__", frameUuid + "@f9941"}, {"__expectedType__", "cc.SpriteFrame"}};
        c["_type"] = type;          // 0 simple, 1 sliced
        c["_sizeMode"] = sizeMode;  // 0 custom, 1 trimmed
        c["_fillType"] = 0;
        c["_fillCenter"] = ccVec2(0, 0);
        c["_fillStart"] = 0;
        c["_fillRange"] = 0;
        c["_isTrimmedMode"] = true;
        c["_atlas"] = nullptr;
        c["_visFlags"] = 0;
        c["_customMaterial"] = nullptr;
        c["_color"] = color;
        c["_useGrayscale"] = false;
        return c;
    }

    // 单行固定框标签改为贴字自适应(overflow NONE)而非 CLAMP:设计框是浏览器
    // 对文本 run 的紧贴测量,引擎回退字体往往更宽,CLAMP 会截断("80%" 丢尾)。
    // 对齐由调用方把锚点移到对齐边来保住(见 Text 分支)。多行仍走 CLAMP 包裹。
    static bool labelHugs(const Node& n) {
        const float lineH = n.textStyle.lineHeightPx > 0 ? n.textStyle.lineHeightPx
                                                         : n.textStyle.fontSize * 1.2f;
        const bool multiline = lineH > 0 && n.height > lineH * 1.8f;
        const std::string& ar = n.textStyle.autoResize;
        return ar != "HEIGHT" && !multiline;  // WIDTH_AND_HEIGHT 本就 hug
    }

    std::string addLabel(int nodeIdx, const Node& n) {
        json c = compBase("cc.Label", nodeIdx);
        c["_srcBlendFactor"] = 2;
        c["_dstBlendFactor"] = 4;
        c["_string"] = n.characters;
        c["_fontSize"] = (int)(n.textStyle.fontSize + 0.5f);
        const float lineH = n.textStyle.lineHeightPx > 0 ? n.textStyle.lineHeightPx : n.textStyle.fontSize * 1.2f;
        c["_lineHeight"] = rnd(lineH);
        // Map figo autoResize -> Cocos Label overflow. NONE (0) makes the Label
        // shrink the node down to the text, which DISCARDS the authored box and
        // breaks alignment — a centered title collapses to the box's top-left
        // corner (anchor 0,1), so text no longer sits where the design put it.
        // Keep the box so _horizontalAlign/_verticalAlign place text as designed:
        //   WIDTH_AND_HEIGHT (hug both) -> NONE          (node == text)
        //   HEIGHT (fixed width, grow)  -> RESIZE_HEIGHT (keep width, wrap+grow)
        //   NONE / TRUNCATE (fixed box) -> CLAMP         (keep box, align, clip)
        const bool multiline = lineH > 0 && n.height > lineH * 1.8f;
        const std::string& ar = n.textStyle.autoResize;
        int overflow;
        bool wrap;
        if (ar == "WIDTH_AND_HEIGHT") { overflow = 0; wrap = false; }
        else if (ar == "HEIGHT")      { overflow = 3; wrap = true; }
        else if (labelHugs(n))        { overflow = 0; wrap = false; }  // 单行:贴字防截断
        else                          { overflow = 1; wrap = multiline; }
        c["_enableWrapText"] = wrap;
        // Bundled real font (--fonts): reference the cc.TTFFont asset; weight/
        // slant baked into the matched file aren't synthesized again below.
        const FontEntry* fe = matchFont(n.textStyle.fontFamily, n.textStyle.fontWeight,
                                        n.textStyle.italic);
        if (fe) {
            c["_font"] = json{{"__uuid__", fe->uuid}, {"__expectedType__", "cc.TTFFont"}};
            c["_isSystemFontUsed"] = false;
        } else {
            c["_isSystemFontUsed"] = true;
        }
        c["_spacingX"] = rnd(n.textStyle.letterSpacing);  // px, same unit as Figma
        c["_underlineHeight"] = 0;
        c["_visFlags"] = 0;
        c["_customMaterial"] = nullptr;
        figo::Color col{0, 0, 0, 1};
        if (const auto* f = solidFill(n)) { col = f->color; col.a *= f->opacity; }
        c["_color"] = ccColor(col);
        c["_overflow"] = overflow;
        c["_cacheMode"] = 0;
        int ha = 1;  // CENTER
        switch (n.textStyle.alignH) {
            case figo::TextStyle::AlignH::Left: ha = 0; break;
            case figo::TextStyle::AlignH::Right: ha = 2; break;
            case figo::TextStyle::AlignH::Justified: ha = 0; break;
            default: ha = 1; break;
        }
        int va = 1;  // CENTER
        switch (n.textStyle.alignV) {
            case figo::TextStyle::AlignV::Top: va = 0; break;
            case figo::TextStyle::AlignV::Bottom: va = 2; break;
            default: va = 1; break;
        }
        c["_horizontalAlign"] = ha;
        c["_verticalAlign"] = va;
        c["_actualFontSize"] = (int)(n.textStyle.fontSize + 0.5f);
        c["_isItalic"] = n.textStyle.italic && (!fe || !fe->italic);
        c["_isBold"] = n.textStyle.fontWeight >= 600 && (!fe || fe->weight < 600);
        c["_isUnderline"] = false;
        return addComp(nodeIdx, std::move(c));
    }

    // ---- CSS-animation replay: FigoAnim.ts runtime component ---------------
    // Cocos 3.x AnimationClip JSON serialization is intricate (tracks/curve
    // object graphs); a tiny runtime script with pre-linearized keys (easing
    // sampled to linear segments by figoanim::build) is deterministic and
    // byte-stable. The script is written once to scripts/FigoAnim.ts; the
    // component references it by compressed uuid, keys stay in CSS conventions
    // (pos y-down px, rot clockwise degrees) and the script converts.
    bool usedAnim = false;
    std::string animScriptUuid() { return makeUuid(fnv1a("script/FigoAnim")); }

    void attachAnim(const Node& n, int idx, float bx, float by, float bw, float bh) {
        if (!n.anim) return;
        const figoanim::Tracks tk = figoanim::build(*n.anim);
        if (!tk.any()) return;
        usedAnim = true;
        const float lx0 = n.relativeTransform.m02, ly0 = n.relativeTransform.m12;
        json c = compBase(compressUuid(animScriptUuid()).c_str(), idx);
        c["dur"] = rnd(tk.length);
        c["loop"] = tk.loop;
        c["stepMode"] = tk.step;
        // Pivot as a fraction of the EMITTED rect (authored on the logical box;
        // a baked leaf's rect is the padded bake bbox).
        c["pivotX"] = rnd(bw > 0 ? ((lx0 - bx) + tk.pivotX * n.width) / bw : 0.5f);
        c["pivotY"] = rnd(bh > 0 ? ((ly0 - by) + tk.pivotY * n.height) / bh : 0.5f);
        json ak = json::array(), sk = json::array(), pk = json::array(), rk = json::array();
        for (const auto& k : tk.alpha) { ak.push_back(rnd(k.first)); ak.push_back(rnd(k.second)); }
        for (const auto& k : tk.scale) { sk.push_back(rnd(k[0])); sk.push_back(rnd(k[1])); sk.push_back(rnd(k[2])); }
        for (const auto& k : tk.pos)   { pk.push_back(rnd(k[0])); pk.push_back(rnd(k[1])); pk.push_back(rnd(k[2])); }
        for (const auto& k : tk.rot)   { rk.push_back(rnd(k.first)); rk.push_back(rnd(k.second)); }
        c["alphaK"] = std::move(ak);
        c["scaleK"] = std::move(sk);
        c["posK"] = std::move(pk);
        c["rotK"] = std::move(rk);
        addComp(idx, std::move(c));
    }

    // fileId of the UITransform created by the most recent newNode() call
    // (captured for instance _contentSize overrides).
    std::string lastUtFileId;

    // Screen roots get a CENTERED anchor (0.5,0.5) so a prefab dropped into a
    // Canvas lands centered (a child's _lpos is relative to the parent's
    // anchor point — with the old (0,1) root anchor the whole UI grew
    // right-down from the Canvas center). Direct children of the root then
    // need their coordinates shifted from top-left-relative to
    // center-relative by (-w/2, +h/2). Component prefab roots keep (0,1) —
    // instance placement math depends on it.
    float rootShiftX = 0, rootShiftY = 0;

    // Build a cc.Node skeleton (no components/children yet) at local (lx, ly)
    // top-left relative to parent top-left, with the given content size.
    int newNode(const std::string& name, int parentIdx, float lx, float ly, float w, float h, const Node* src) {
        const float sx = (parentIdx == 1 && !inComponent) ? rootShiftX : 0.0f;
        const float sy = (parentIdx == 1 && !inComponent) ? rootShiftY : 0.0f;
        lx += sx;
        ly -= sy;  // ly is Y-down; the shift is in Cocos Y-up
        json node = {
            {"__type__", "cc.Node"}, {"_name", name}, {"_objFlags", 0},
            {"_parent", parentIdx < 0 ? json(nullptr) : json{{"__id__", parentIdx}}},
            {"_children", json::array()}, {"_active", true},
            {"_components", json::array()}, {"_prefab", nullptr}, {"_id", ""},
            {"_lpos", ccVec3(lx, -ly, 0)}, {"_lrot", ccVec3(0, 0, 0)},
            {"_lscale", ccVec3(1, 1, 1)}, {"_euler", ccVec3(0, 0, 0)},
            {"_layer", 33554432},  // UI_2D
        };
        if (src && !src->visible) node["_active"] = false;
        int idx = push(std::move(node));
        lastUtFileId = addComp(idx, uiTransform(idx, w, h));  // every UI node carries a UITransform
        if (src && src->opacity < 0.999f) {
            json op = compBase("cc.UIOpacity", idx);
            op["_opacity"] = u8(src->opacity);
            addComp(idx, std::move(op));
        }
        return idx;
    }

    std::string finishNode(int idx) {
        // Each node gets a PrefabInfo pointing back at the prefab root (idx 1).
        std::string fid = nextFileId();
        int piIdx = push(json{
            {"__type__", "cc.PrefabInfo"}, {"root", {{"__id__", 1}}},
            {"asset", {{"__id__", 0}}}, {"fileId", fid}, {"sync", false},
        });
        arr[idx]["_prefab"] = {{"__id__", piIdx}};
        return fid;
    }

    // Emit node `n` under parentIdx; returns its array index. pax/pay = parent's
    // absolute top-left (for placing baked sprites in parent-local coords).
    // path = node path from the component root when recording (rec != null).
    int emit(Node& n, int parentIdx, const std::string& name, float pax, float pay,
             const std::string& path = "", bool inScroll = false) {
        const bool isRoot = parentIdx < 0;
        const float lx = isRoot ? 0.0f : n.relativeTransform.m02;
        const float ly = isRoot ? 0.0f : n.relativeTransform.m12;

        // Prefab reuse: a repeated component becomes a nested-prefab instance
        // (cc.PrefabInstance + propertyOverrides), its subtree not re-emitted.
        if (const Comp* comp = extractedComponent(n)) {
            usedComps.insert(comp->name);
            return emitPrefabInstance(*const_cast<Comp*>(comp), n, parentIdx, name, lx, ly);
        }

        CompNodeIds recIds;
        // Emitted rect (baked leaves overwrite with painted bounds) — the CSS
        // animation's pivot must be re-expressed as a fraction of it.
        float abx = lx, aby = ly, abw = n.width, abh = n.height;
        auto fin = [&](int idx) {
            attachAnim(n, idx, abx, aby, abw, abh);
            recIds.node = finishNode(idx);
            if (rec) (*rec)[path] = recIds;
            return idx;
        };

        // Figma constraints -> cc.Widget (ALWAYS mode), so the layout responds
        // when the parent resizes. Min/Min needs no Widget (the top-left anchor
        // scheme already pins there). Insets come from the ACTUAL emitted rect
        // (baked leaves sit at painted bounds), so nothing moves at design size.
        // Skipped for component-prefab internals (override math assumes plain
        // _lpos) and direct children of a scroll __content (their runtime
        // parent is the content extent, not the design box).
        auto addWidget = [&](int idx, float x, float y, float w, float h) {
            using C = figo::Constraint;
            if (isRoot || rec || inScroll || !n.parent) return;
            if (n.constraintH == C::Min && n.constraintV == C::Min) return;
            const float pw = n.parent->width, ph = n.parent->height;
            if (pw <= 0 || ph <= 0) return;
            enum { TOP = 1, MID = 2, BOT = 4, LEFT = 8, CENTER = 16, RIGHT = 32 };
            int flags = 0;
            json wgt = compBase("cc.Widget", idx);
            wgt["_target"] = nullptr;
            wgt["_left"] = 0; wgt["_right"] = 0; wgt["_top"] = 0; wgt["_bottom"] = 0;
            wgt["_horizontalCenter"] = 0; wgt["_verticalCenter"] = 0;
            wgt["_isAbsLeft"] = true; wgt["_isAbsRight"] = true;
            wgt["_isAbsTop"] = true; wgt["_isAbsBottom"] = true;
            wgt["_isAbsHorizontalCenter"] = true; wgt["_isAbsVerticalCenter"] = true;
            switch (n.constraintH) {
                case C::Max:     flags |= RIGHT; wgt["_right"] = rnd(pw - x - w); break;
                case C::Center:  flags |= CENTER; wgt["_horizontalCenter"] = rnd(x + w / 2 - pw / 2); break;
                case C::Stretch: flags |= LEFT | RIGHT; wgt["_left"] = rnd(x); wgt["_right"] = rnd(pw - x - w); break;
                case C::Scale:   flags |= LEFT | RIGHT; wgt["_left"] = rnd(x / pw); wgt["_right"] = rnd((pw - x - w) / pw);
                                 wgt["_isAbsLeft"] = false; wgt["_isAbsRight"] = false; break;
                default: break;  // Min: left pin comes free with the (0,1) anchor
            }
            switch (n.constraintV) {
                case C::Max:     flags |= BOT; wgt["_bottom"] = rnd(ph - y - h); break;
                case C::Center:  flags |= MID; wgt["_verticalCenter"] = rnd(-(y + h / 2 - ph / 2)); break;
                case C::Stretch: flags |= TOP | BOT; wgt["_top"] = rnd(y); wgt["_bottom"] = rnd(ph - y - h); break;
                case C::Scale:   flags |= TOP | BOT; wgt["_top"] = rnd(y / ph); wgt["_bottom"] = rnd((ph - y - h) / ph);
                                 wgt["_isAbsTop"] = false; wgt["_isAbsBottom"] = false; break;
                default: break;  // Min: top pin comes free with the (0,1) anchor
            }
            if (!flags) return;
            wgt["_alignFlags"] = flags;
            wgt["_originalWidth"] = rnd(w);
            wgt["_originalHeight"] = rnd(h);
            wgt["_alignMode"] = 2;  // ALWAYS
            wgt["_lockFlags"] = 0;
            addComp(idx, std::move(wgt));
        };

        // Place baked leaves at their painted bounds; everything else at its box.
        auto emitBakedLeaf = [&](const Baked& b) -> int {
            abx = b.x - pax; aby = b.y - pay; abw = (float)b.w; abh = (float)b.h;
            int idx = newNode(name, parentIdx, b.x - pax, b.y - pay, (float)b.w, (float)b.h, &n);
            recIds.ut = lastUtFileId;
            const Sprite& sp = sprites[b.hash];
            recIds.sprite = addComp(idx, spriteComp(idx, sp.uuid, b.nine ? 1 : 0, b.nine ? 0 : 1, ccColor({1, 1, 1, 1})));
            addWidget(idx, b.x - pax, b.y - pay, (float)b.w, (float)b.h);
            return fin(idx);
        };

        if (n.type == NodeType::Text) {
            int idx = newNode(name, parentIdx, lx, ly, n.width, n.height, &n);
            recIds.ut = lastUtFileId;
            // 贴字自适应(overflow NONE)的标签按引擎自己的字体度量重排尺寸;把
            // 锚点移到对齐边,文字从对齐边反向生长,授权的对齐关系不被破坏
            // (右对齐钉右缘、居中钉中心;默认 (0,1) 只对左对齐成立)。
            if (labelHugs(n) || n.textStyle.autoResize == "WIDTH_AND_HEIGHT") {
                float ax = 0.0f;
                if (n.textStyle.alignH == figo::TextStyle::AlignH::Center) ax = 0.5f;
                else if (n.textStyle.alignH == figo::TextStyle::AlignH::Right) ax = 1.0f;
                if (ax > 0.0f) {
                    int ut = arr[idx]["_components"][0]["__id__"].get<int>();
                    arr[ut]["_anchorPoint"] = ccVec2(ax, 1.0f);
                    arr[idx]["_lpos"]["x"] = rnd(arr[idx]["_lpos"]["x"].get<float>() + ax * n.width);
                }
            }
            if (const auto* f = solidFill(n)) bindToken(path, "text", f->colorVar);
            recIds.label = addLabel(idx, n);
            addWidget(idx, lx, ly, n.width, n.height);
            return fin(idx);
        }

        if (!isRoot && isVectorIcon(n)) {
            Baked b = bake(n, superScale, /*flatten=*/true);
            if (b.ok) return emitBakedLeaf(b);
            int idx = newNode(name, parentIdx, lx, ly, n.width, n.height, &n);
            recIds.ut = lastUtFileId;
            addWidget(idx, lx, ly, n.width, n.height);
            return fin(idx);  // no recurse into the (failed) flatten
        }

        const bool isContainer = !n.children.empty();
        if (!isContainer && needsBake(n)) {
            Baked b;
            if (nineCandidate(n)) b = bake(n, 1, false, true, true);
            if (!b.ok) b = bake(n, superScale);
            if (b.ok) return emitBakedLeaf(b);
            int idx = newNode(name, parentIdx, lx, ly, n.width, n.height, &n);
            recIds.ut = lastUtFileId;
            addWidget(idx, lx, ly, n.width, n.height);
            return fin(idx);
        }

        // Scroll analysis (mirrors figo2godot): a frame becomes a ScrollView
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

        // Container or solid leaf: a node at its own box, optional bg + children.
        int idx = newNode(name, parentIdx, lx, ly, n.width, n.height, &n);
        recIds.ut = lastUtFileId;
        addWidget(idx, lx, ly, n.width, n.height);

        // A scroll node hosts cc.Mask, which can't share a node with cc.Sprite —
        // its background moves to a full-size __bg child (outside __content, so
        // it stays put while the content scrolls).
        bool hasBgChild = false;
        auto bgChild = [&](const std::string& uuid, int type, int sizeMode, const json& col,
                           float bx, float by, float bw, float bh) {
            int bg = newNode("__bg", idx, bx, by, bw, bh, nullptr);
            CompNodeIds bgIds;
            bgIds.ut = lastUtFileId;
            bgIds.sprite = addComp(bg, spriteComp(bg, uuid, type, sizeMode, col));
            bgIds.node = finishNode(bg);
            if (rec) (*rec)[path.empty() ? "__bg" : path + "/__bg"] = bgIds;
            arr[idx]["_children"].push_back({{"__id__", bg}});
            hasBgChild = true;
        };
        if (needsBake(n)) {
            // Complex background: a baked sprite child filling (or overhanging) the box.
            Baked b;
            if (nineCandidate(n)) b = bake(n, 1, false, true, true);
            if (!b.ok) b = bake(n, superScale);
            if (b.ok) {
                const Sprite& sp = sprites[b.hash];
                if (b.nine && !scrollFrame) {
                    // Stretchable bg: sprite on the node itself, sliced to the box.
                    recIds.sprite = addComp(idx, spriteComp(idx, sp.uuid, 1, 0, ccColor({1, 1, 1, 1})));
                } else if (b.nine) {
                    bgChild(sp.uuid, 1, 0, ccColor({1, 1, 1, 1}), 0, 0, n.width, n.height);
                } else {
                    // Fixed bg (may overhang via glow): a sized __bg child.
                    bgChild(sp.uuid, 0, 1, ccColor({1, 1, 1, 1}),
                            b.x - n.absoluteTransform.m02, b.y - n.absoluteTransform.m12,
                            (float)b.w, (float)b.h);
                }
            }
        } else if (const auto* f = solidFill(n)) {
            ensureWhite();
            if (scrollFrame) {
                bindToken(path.empty() ? "__bg" : path + "/__bg", "fill", f->colorVar);
                bgChild(whiteUuid, 0, 0, ccPaintColor(*f), 0, 0, n.width, n.height);
            } else {
                bindToken(path, "fill", f->colorVar);
                recIds.sprite = addComp(idx, spriteComp(idx, whiteUuid, 0, 0, ccPaintColor(*f)));
            }
        }

        // Auto-layout (conservative v1): a pure stack container gets cc.Layout
        // so content added/removed in Cocos reflows. cc.Layout can only pack
        // from the start edge and manages EVERY active child, so skip when the
        // design packs Center/Max/SpaceBetween, wraps, scrolls, mixes in
        // absolutely-placed children, has a __bg child (would be packed too),
        // or any child carries a visible effect (its baked sprite sits at
        // painted bounds, which the layout would re-pack incorrectly).
        bool inLayout = false;
        {
            using ALAlign = figo::AutoLayout::Align;
            const auto& al = n.autoLayout;
            bool layoutOk = isContainer && al.enabled() && !al.wrap && !scrollFrame &&
                            !hasBgChild && al.primaryAlign == ALAlign::Min;
            if (layoutOk)
                for (auto& c : n.children)
                    if (c->layoutAbsolute || hasVisibleEffect(*c)) { layoutOk = false; break; }
            if (layoutOk) {
                const bool horizontal = al.mode == figo::AutoLayout::Mode::Horizontal;
                json lay = compBase("cc.Layout", idx);
                lay["_resizeMode"] = al.primarySizing == figo::AutoLayout::Sizing::Hug ? 1 : 0;
                lay["_layoutType"] = horizontal ? 1 : 2;
                lay["_cellSize"] = json{{"__type__", "cc.Size"}, {"width", 40}, {"height", 40}};
                lay["_startAxis"] = 0;
                lay["_paddingLeft"] = rnd(al.paddingLeft);
                lay["_paddingRight"] = rnd(al.paddingRight);
                lay["_paddingTop"] = rnd(al.paddingTop);
                lay["_paddingBottom"] = rnd(al.paddingBottom);
                lay["_spacingX"] = rnd(horizontal ? al.itemSpacing : 0.0f);
                lay["_spacingY"] = rnd(horizontal ? 0.0f : al.itemSpacing);
                lay["_verticalDirection"] = 1;    // top-to-bottom
                lay["_horizontalDirection"] = 0;  // left-to-right
                lay["_constraint"] = 0;
                lay["_constraintNum"] = 2;
                lay["_affectedByScale"] = false;
                lay["_isAlign"] = false;
                addComp(idx, std::move(lay));
                inLayout = true;
            }
        }

        // Scroll frame -> cc.Mask (rect clip) + cc.ScrollView on the node, with
        // a __content child holding the absolutely-placed children; the content
        // size is what tells the ScrollView how far to scroll. scrollFixed
        // children stay direct children of the node (drawn above the content).
        int contentIdx = idx;
        if (scrollFrame) {
            json mask = compBase("cc.Mask", idx);
            mask["_type"] = 0;  // rect
            mask["_inverted"] = false;
            mask["_segments"] = 64;
            mask["_alphaThreshold"] = 0.1;
            addComp(idx, std::move(mask));

            contentIdx = newNode("__content", idx,
                                 0, 0,
                                 scrollH ? scrollCW : n.width,
                                 scrollV ? scrollCH : n.height, nullptr);
            finishNode(contentIdx);
            arr[idx]["_children"].push_back({{"__id__", contentIdx}});

            json sv = compBase("cc.ScrollView", idx);
            sv["bounceDuration"] = 1;
            sv["brake"] = 0.5;
            sv["elastic"] = true;
            sv["inertia"] = true;
            sv["horizontal"] = scrollH;
            sv["vertical"] = scrollV;
            sv["cancelInnerEvents"] = true;
            sv["scrollEvents"] = json::array();
            sv["_content"] = {{"__id__", contentIdx}};
            sv["_horizontalScrollBar"] = nullptr;
            sv["_verticalScrollBar"] = nullptr;
            addComp(idx, std::move(sv));
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
            const int host = (scrollFrame && child->scrollFixed) ? idx : contentIdx;
            // Widgets are suppressed under a scroll __content (wrong parent
            // reference) and inside a cc.Layout (the layout owns placement).
            int childIdx = emit(*child, host, uniq, n.absoluteTransform.m02, n.absoluteTransform.m12,
                                path.empty() ? uniq : path + "/" + uniq,
                                (scrollFrame && !child->scrollFixed) || inLayout);
            arr[host]["_children"].push_back({{"__id__", childIdx}});
        }
        return fin(idx);
    }

    // ---- nested-prefab instancing (cc.PrefabInstance) ----

    // Emit `n` as an instance of comp's components/<name>.prefab. The host gets
    // a minimal stub cc.Node whose PrefabInfo carries the asset uuid + a
    // cc.PrefabInstance holding propertyOverrides (targeted via the fileIds
    // recorded when the component prefab was emitted) and mountedChildren for
    // instance-only nodes. Format verified against Creator 3.8 output.
    int emitPrefabInstance(Comp& comp, Node& n, int parentIdx, const std::string& name,
                           float lx, float ly) {
        const int stubIdx = push(json{
            {"__type__", "cc.Node"}, {"_objFlags", 0},
            {"_parent", parentIdx < 0 ? json(nullptr) : json{{"__id__", parentIdx}}},
            {"_prefab", nullptr}, {"__editorExtras__", json::object()},
        });
        const int instIdx = push(json{
            {"__type__", "cc.PrefabInstance"}, {"fileId", nextFileId()},
            {"prefabRootNode", {{"__id__", 1}}},
            {"mountedChildren", json::array()}, {"mountedComponents", json::array()},
            {"propertyOverrides", json::array()}, {"removedComponents", json::array()},
        });
        // The instance node's fileId must be UNIQUE within the host prefab —
        // it's the node's identity there. (An editor-authored single instance
        // happens to reuse the source root's fileId, but ten instances of one
        // component sharing a fileId collapse to a single surviving node on
        // import — the "only one PlayerTile shows" bug.)
        const int piIdx = push(json{
            {"__type__", "cc.PrefabInfo"}, {"root", {{"__id__", stubIdx}}},
            {"asset", {{"__uuid__", comp.uuid}, {"__expectedType__", "cc.Prefab"}}},
            {"fileId", nextFileId()}, {"instance", {{"__id__", instIdx}}},
            {"targetOverrides", nullptr},
        });
        arr[stubIdx]["_prefab"] = {{"__id__", piIdx}};
        instanceStubs.push_back(stubIdx);

        std::map<std::string, int> targetIdx;  // localID -> cc.TargetInfo idx
        auto targetFor = [&](const std::string& localId) {
            auto it = targetIdx.find(localId);
            if (it != targetIdx.end()) return it->second;
            int ti = push(json{{"__type__", "cc.TargetInfo"}, {"localID", json::array({localId})}});
            targetIdx[localId] = ti;
            return ti;
        };
        auto pushOverride = [&](const std::string& localId, const char* prop, json value) {
            int ov = push(json{
                {"__type__", "CCPropertyOverrideInfo"},
                {"targetInfo", {{"__id__", targetFor(localId)}}},
                {"propertyPath", json::array({prop})}, {"value", std::move(value)},
            });
            arr[instIdx]["propertyOverrides"].push_back({{"__id__", ov}});
        };
        std::map<std::string, std::vector<int>> mounted;  // comp path -> host node idxs

        // Root placement + identity (root shift: see rootShiftX declaration).
        const CompNodeIds& root = comp.ids[""];
        const float sx = (parentIdx == 1 && !inComponent) ? rootShiftX : 0.0f;
        const float sy = (parentIdx == 1 && !inComponent) ? rootShiftY : 0.0f;
        pushOverride(root.node, "_name", name);
        pushOverride(root.node, "_lpos", ccVec3(lx + sx, -ly + sy, 0));
        if (!n.visible) pushOverride(root.node, "_active", false);
        if (std::lround(n.width) != std::lround(comp.canon->width) ||
            std::lround(n.height) != std::lround(comp.canon->height))
            pushOverride(root.ut, "_contentSize", ccSize(n.width, n.height));

        walkOverrides(*comp.canon, n, "", comp, stubIdx, pushOverride, mounted);

        for (auto& [path, nodes] : mounted) {
            int ti = targetFor(comp.ids[path].node);
            json mc = json{{"__type__", "cc.MountedChildrenInfo"},
                           {"targetInfo", {{"__id__", ti}}}, {"nodes", json::array()}};
            for (int idx : nodes) mc["nodes"].push_back({{"__id__", idx}});
            arr[instIdx]["mountedChildren"].push_back({{"__id__", push(std::move(mc))}});
        }
        return stubIdx;
    }

    // Per-instance overrides vs the canon: children matched by slot identity
    // (nearest-position tiebreak); a canon-only slot is hidden (_active), a
    // matched slot gets _lpos/_contentSize/_string/_color/_spriteFrame
    // overrides, an instance-only slot is MOUNTED as a fresh node subtree.
    void walkOverrides(const Node& canon, Node& inst, const std::string& path, Comp& comp,
                       int stubIdx,
                       const std::function<void(const std::string&, const char*, json)>& pushOverride,
                       std::map<std::string, std::vector<int>>& mounted) {
        auto childPath = [&](const std::string& uniq) {
            return path.empty() ? uniq : path + "/" + uniq;
        };
        auto spriteRef = [&](uint64_t hash) {
            return json{{"__uuid__", sprites[hash].uuid + "@f9941"}, {"__expectedType__", "cc.SpriteFrame"}};
        };
        auto pushRect = [&](const CompNodeIds& ids, float x, float y, float w, float h) {
            pushOverride(ids.node, "_lpos", ccVec3(x, -y, 0));
            if (!ids.ut.empty()) pushOverride(ids.ut, "_contentSize", ccSize(w, h));
        };

        // Container solid bg: white sprite tinted on the node itself.
        if (!canon.children.empty())
            if (const auto* cb = solidFill(canon)) if (const auto* ib = solidFill(inst))
                if (colorNe(cb->color, ib->color) && !comp.ids[path].sprite.empty())
                    pushOverride(comp.ids[path].sprite, "_color", ccPaintColor(*ib));

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
                if (cc.visible && !ids.node.empty()) pushOverride(ids.node, "_active", false);
                continue;
            }
            Node& ic = *icp;
            const bool posDiff =
                std::lround(cc.width) != std::lround(ic.width) ||
                std::lround(cc.height) != std::lround(ic.height) ||
                std::lround(cc.relativeTransform.m02) != std::lround(ic.relativeTransform.m02) ||
                std::lround(cc.relativeTransform.m12) != std::lround(ic.relativeTransform.m12);
            if (cc.type == NodeType::Text && !ids.label.empty()) {
                const auto *fc = solidFill(cc), *fi = solidFill(ic);
                if (posDiff)
                    pushRect(ids, ic.relativeTransform.m02, ic.relativeTransform.m12, ic.width, ic.height);
                if (cc.characters != ic.characters)
                    pushOverride(ids.label, "_string", ic.characters);
                if (fc && fi && colorNe(fc->color, fi->color))
                    pushOverride(ids.label, "_color", ccPaintColor(*fi));
            } else if (needsBake(cc)) {
                Baked bc = bake(cc, superScale), bi = bake(ic, superScale);
                if (bi.ok && (bc.hash != bi.hash || posDiff)) {
                    const float pax = ic.parent ? ic.parent->absoluteTransform.m02 : 0.f;
                    const float pay = ic.parent ? ic.parent->absoluteTransform.m12 : 0.f;
                    auto bgIt = comp.ids.find(cp + "/__bg");
                    if (cc.children.empty() && !ids.sprite.empty()) {
                        pushRect(ids, bi.x - pax, bi.y - pay, (float)bi.w, (float)bi.h);
                        if (bc.hash != bi.hash) pushOverride(ids.sprite, "_spriteFrame", spriteRef(bi.hash));
                    } else if (bgIt != comp.ids.end()) {
                        if (posDiff)
                            pushRect(ids, ic.relativeTransform.m02, ic.relativeTransform.m12, ic.width, ic.height);
                        pushRect(bgIt->second, bi.x - ic.absoluteTransform.m02,
                                 bi.y - ic.absoluteTransform.m12, (float)bi.w, (float)bi.h);
                        if (bc.hash != bi.hash) pushOverride(bgIt->second.sprite, "_spriteFrame", spriteRef(bi.hash));
                    } else if (!ids.sprite.empty()) {
                        if (posDiff)
                            pushRect(ids, ic.relativeTransform.m02, ic.relativeTransform.m12, ic.width, ic.height);
                        if (bc.hash != bi.hash) pushOverride(ids.sprite, "_spriteFrame", spriteRef(bi.hash));
                    }
                }
                if (!cc.children.empty())
                    walkOverrides(cc, ic, cp, comp, stubIdx, pushOverride, mounted);
            } else {
                const std::string ci = imageRefOf(cc), ii = imageRefOf(ic);
                const bool imgDiff = !ii.empty() && ii != ci;
                const auto *lc = solidFill(cc), *li = solidFill(ic);
                const bool leafColDiff = cc.children.empty() && lc && li && colorNe(lc->color, li->color);
                if (posDiff && !ids.node.empty())
                    pushRect(ids, ic.relativeTransform.m02, ic.relativeTransform.m12, ic.width, ic.height);
                if (imgDiff && !ids.sprite.empty()) {
                    Baked b = bake(ic, superScale);
                    if (b.ok) pushOverride(ids.sprite, "_spriteFrame", spriteRef(b.hash));
                }
                if (leafColDiff && !ids.sprite.empty())
                    pushOverride(ids.sprite, "_color", ccPaintColor(*li));
                walkOverrides(cc, ic, cp, comp, stubIdx, pushOverride, mounted);
            }
        }

        // Instance-only slots: mounted as fresh node subtrees (the engine
        // reparents them onto the instantiated target at load).
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
            int addedIdx = emit(extra, stubIdx, uniq,
                                inst.absoluteTransform.m02, inst.absoluteTransform.m12);
            mounted[path].push_back(addedIdx);
        }
    }

    // Emit a component's reusable prefab (components/<name>.prefab), recording
    // every node's fileIds into comp.ids for instance overrides.
    void emitComponentPrefab(Comp& comp) {
        curW = (uint32_t)std::ceil(std::max(1.0f, comp.frame->width));
        curH = (uint32_t)std::ceil(std::max(1.0f, comp.frame->height));
        ui->setViewport(curW, curH);
        ui->selectFrame(comp.frame->name);
        ui->render();

        arr = json::array();
        fileIdSeed = fnv1a("comp/" + comp.name) | 1;
        // _name must match the asset filename — the editor's name-sync rewrites
        // the file on import otherwise.
        push(json{
            {"__type__", "cc.Prefab"}, {"_name", comp.name}, {"_objFlags", 0},
            {"_native", ""}, {"data", {{"__id__", 1}}},
            {"optimizationPolicy", 0}, {"asyncLoadAssets", false}, {"persistent", false},
        });
        inComponent = true;
        rec = &comp.ids;
        instanceStubs.clear();  // components never nest instances (inComponent)
        emit(*const_cast<Node*>(comp.canon), -1, comp.name, 0.0f, 0.0f, "");
        rec = nullptr;
        inComponent = false;

        fs::create_directories(outDir / "components");
        writeDirMeta(outDir / "components");
        std::ofstream pf(outDir / "components" / (comp.name + ".prefab"), std::ios::binary);
        pf << arr.dump(2);
        std::ofstream mf(outDir / "components" / (comp.name + ".prefab.meta"), std::ios::binary);
        mf << subst(kPrefabMeta, {{"$UUID", comp.uuid}, {"$NAME", comp.name}});
    }

    static constexpr float kNineMinArea = 40000.0f;  // ~200x200 logical
    bool nineCandidate(const Node& n) {
        if (!isRectish(n.type)) return false;
        if (hasVisibleEffect(n)) return false;
        if (n.width * n.height < kNineMinArea) return false;
        return true;
    }

    std::map<std::string, int> frameStemCount;  // filename collision guard

    // Theme-token bindings collected during emit and written to
    // figo_tokens.json — prefab colors are resolved literals, so this is the
    // hook engine-side reskinning needs to re-bind them.
    json tokenBindings = json::array();
    std::string curFrame;
    void bindToken(const std::string& path, const char* channel, const std::string& var) {
        if (var.empty() || inComponent) return;  // component paths are frame-relative
        tokenBindings.push_back({{"frame", curFrame},
                                 {"node", path.empty() ? "." : path},
                                 {"channel", channel},
                                 {"var", var}});
    }

    void convertFrame(Node* frame) {
        curW = (uint32_t)std::ceil(std::max(1.0f, frame->width));
        curH = (uint32_t)std::ceil(std::max(1.0f, frame->height));
        ui->setViewport(curW, curH);
        if (!ui->selectFrame(frame->name)) {
            std::fprintf(stderr, "  WARN: selectFrame(%s) failed\n", frame->name.c_str());
            return;
        }
        ui->render();  // computes absoluteTransform

        curFrame = frame->name;
        arr = json::array();
        // Frames whose names sanitize to the same stem would overwrite each
        // other's .prefab AND collide on uuid seeds; suffix repeats. The first
        // occurrence keeps today's stem/uuid so existing references survive.
        std::string uniq = frame->name;
        std::string stem = fileStem(frame->name, "Frame");
        if (int k = ++frameStemCount[stem]; k > 1) {
            uniq += "#" + std::to_string(k);
            stem += "_" + std::to_string(k);
        }
        fileIdSeed = fnv1a(uniq) | 1;

        // idx 0 = cc.Prefab (data -> root node at idx 1). _name must match the
        // asset filename (editor name-sync rewrites the file otherwise).
        push(json{
            {"__type__", "cc.Prefab"}, {"_name", stem}, {"_objFlags", 0},
            {"_native", ""}, {"data", {{"__id__", 1}}},
            {"optimizationPolicy", 0}, {"asyncLoadAssets", false}, {"persistent", false},
        });
        rootShiftX = -frame->width / 2.0f;
        rootShiftY = frame->height / 2.0f;
        instanceStubs.clear();
        emit(*frame, -1, stem, 0.0f, 0.0f);  // root node pushed at idx 1
        rootShiftX = rootShiftY = 0;
        // Register every nested-prefab instance on the root PrefabInfo so the
        // engine expands them at load.
        if (!instanceStubs.empty()) {
            json roots = json::array();
            for (int idx : instanceStubs) roots.push_back({{"__id__", idx}});
            arr[arr[1]["_prefab"]["__id__"].get<int>()]["nestedPrefabInstanceRoots"] = std::move(roots);
        }
        // Center the screen root's own anchor so the prefab lands centered in
        // a Canvas (children were emitted center-relative via rootShift).
        const int rootUt = arr[1]["_components"][0]["__id__"].get<int>();
        arr[rootUt]["_anchorPoint"] = ccVec2(0.5f, 0.5f);

        std::ofstream pf(outDir / (stem + ".prefab"), std::ios::binary);
        pf << arr.dump(2);
        std::ofstream mf(outDir / (stem + ".prefab.meta"), std::ios::binary);
        mf << subst(kPrefabMeta, {{"$UUID", makeUuid(fnv1a("prefab/" + uniq))}, {"$NAME", stem}});
        std::printf("  %s -> %s.prefab (%zu objects)\n", frame->name.c_str(), stem.c_str(), arr.size());
    }
};

// Index a directory of .ttf/.otf into a FontEntry list, copying each into the
// output's fonts/ dir with a ttf-font .meta (deterministic uuid).
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
        if (!parseFontMeta(de.path(), fe.family, fe.weight, fe.italic)) continue;
        fe.familyNorm = normFamily(fe.family);
        fe.file = de.path().filename().string();
        fe.uuid = makeUuid(fnv1a("font/" + fe.file));
        fs::copy_file(de.path(), outFonts / fe.file, fs::copy_options::overwrite_existing, ec);
        std::ofstream mf(outFonts / (fe.file + ".meta"), std::ios::binary);
        mf << subst(kTtfMeta, {{"$UUID", fe.uuid}, {"$FILE", fe.file}});
        out.push_back(std::move(fe));
    }
    if (!out.empty()) writeDirMeta(outFonts);
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: figo2cocos <input.fig|canvas.json> [outDir] [--frame NAME] "
                    "[--fonts DIR] [--scale N] [--prefabs] [--prefab-anon] [--no-prefab T1,T2] "
                    "[--prefab-pin T1,T2]\n");
        return 2;
    }
    const std::string input = argv[1];
    fs::path outDir = "cocos_out";
    std::string onlyFrame;
    fs::path fontsDir;
    int scale = 2;
    bool prefabs = false;
    bool prefabAnon = false;
    std::set<std::string> noPrefabArg;
    std::set<std::string> prefabPinArg;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frame" && i + 1 < argc) onlyFrame = argv[++i];
        else if (a == "--fonts" && i + 1 < argc) fontsDir = argv[++i];
        else if (a == "--scale" && i + 1 < argc) scale = std::max(1, atoi(argv[++i]));
        else if (a == "--prefabs") prefabs = true;
        else if (a == "--prefab-anon") { prefabs = true; prefabAnon = true; }
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
    writeDirMeta(texDir);

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
    cv.prefabAnon = prefabAnon;
    cv.noPrefab = noPrefabArg;
    cv.prefabPin = prefabPinArg;

    // Prefab pre-pass: find repeated components, cluster state variants, emit
    // each cluster's components/<name>.prefab FIRST (instances must know the
    // fileIds inside the source prefab), then the frame loop instances them.
    if (prefabs) {
        for (Node* frame : frames) {
            if (!onlyFrame.empty() && frame->name != onlyFrame) continue;
            cv.scanComponents(frame);
        }
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
            std::string base = fileStem(comp.name, "Comp");
            std::string uniq = base;
            int k = 2;
            while (usedNames.count(uniq)) uniq = base + "_" + std::to_string(k++);
            usedNames.insert(uniq);
            comp.name = uniq;
            comp.uuid = makeUuid(fnv1a("comp/" + uniq));
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
        json tj;
        tj["modes"] = vt.modes;
        tj["activeMode"] = vt.activeMode;
        json vars = json::object();
        for (const auto& v : vt.vars) {
            json mv = json::object();
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

    // CSS-animation replay runtime: written once when any node carries anim
    // keys. The prefab components reference it by this exact uuid — the .meta
    // must ship with the .ts or the references break on import.
    if (cv.usedAnim) {
        fs::create_directories(outDir / "scripts", ec);
        writeDirMeta(outDir / "scripts");
        std::ofstream sf(outDir / "scripts" / "FigoAnim.ts", std::ios::binary);
        sf << kFigoAnimTs;
        std::ofstream smf(outDir / "scripts" / "FigoAnim.ts.meta", std::ios::binary);
        smf << subst(kTsMeta, {{"$UUID", cv.animScriptUuid()}});
        std::printf("anim: FigoAnim.ts runtime written (scripts/)\n");
    }

    // Prune orphan component prefabs (extracted but never instanced by a frame).
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
