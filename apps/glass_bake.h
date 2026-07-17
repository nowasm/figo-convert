// Shared backdrop-blur ("glass") bake for the engine exporters (figo2godot /
// figo2cocos / figo2unity).
//
// Figma's BACKGROUND_BLUR frosts whatever is painted BELOW the node, but the
// exporters bake nodes in isolation via renderOverlay — no backdrop to sample,
// so glass panels lose the frosting entirely (a 60% dark pill over a dark card
// is invisible and the card's content shows through un-blurred). This helper
// bakes the real backdrop in: render the frame with the node and everything
// painted above it hidden, blur by the effect radius (the runtime's formula),
// mask to the node's exact anti-aliased shape, then composite the node's own
// chrome (fill/stroke) on top. The backdrop is frozen into the pixels — exact
// for a static scene, stale if the content underneath moves at runtime.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <figo/figo.h>

namespace figoglass {

// Copy of renderer.cpp's separable box blur so the baked glass matches the
// figo runtime pixel-for-pixel (3 passes ≈ Gaussian, straight alpha).
inline void blurAxis(std::vector<uint32_t>& px, int w, int h, int hw, bool vertical) {
    if (hw < 1) return;
    std::vector<uint32_t> src = px;
    const int len = vertical ? h : w, lines = vertical ? w : h;
    const int stridePos = vertical ? w : 1, strideLine = vertical ? 1 : w;
    const float norm = 1.0f / static_cast<float>(2 * hw + 1);
    for (int line = 0; line < lines; ++line) {
        const uint32_t* in = src.data() + static_cast<size_t>(line) * strideLine;
        uint32_t* out = px.data() + static_cast<size_t>(line) * strideLine;
        int sum[4] = {0, 0, 0, 0};
        auto at = [&](int i) {
            return in[static_cast<size_t>(std::clamp(i, 0, len - 1)) * stridePos];
        };
        for (int i = -hw; i <= hw; ++i) {
            const uint32_t c = at(i);
            sum[0] += c & 0xFF;
            sum[1] += (c >> 8) & 0xFF;
            sum[2] += (c >> 16) & 0xFF;
            sum[3] += (c >> 24) & 0xFF;
        }
        for (int i = 0; i < len; ++i) {
            out[static_cast<size_t>(i) * stridePos] =
                (static_cast<uint32_t>(sum[3] * norm + 0.5f) << 24) |
                (static_cast<uint32_t>(sum[2] * norm + 0.5f) << 16) |
                (static_cast<uint32_t>(sum[1] * norm + 0.5f) << 8) |
                static_cast<uint32_t>(sum[0] * norm + 0.5f);
            const uint32_t add = at(i + hw + 1), sub = at(i - hw);
            sum[0] += static_cast<int>(add & 0xFF) - static_cast<int>(sub & 0xFF);
            sum[1] += static_cast<int>((add >> 8) & 0xFF) - static_cast<int>((sub >> 8) & 0xFF);
            sum[2] += static_cast<int>((add >> 16) & 0xFF) - static_cast<int>((sub >> 16) & 0xFF);
            sum[3] += static_cast<int>((add >> 24) & 0xFF) - static_cast<int>((sub >> 24) & 0xFF);
        }
    }
}

inline void blur(std::vector<uint32_t>& px, int w, int h, float sigma) {
    const int hw = std::max(1, static_cast<int>(sigma * 0.6f + 0.5f));
    for (int i = 0; i < 3; ++i) {
        blurAxis(px, w, h, hw, false);
        blurAxis(px, w, h, hw, true);
    }
}

inline float backdropBlurRadius(const figo::Node& n) {
    for (const auto& fx : n.effects)
        if (fx.type == figo::Effect::Type::BackgroundBlur && fx.visible && fx.radius > 0)
            return fx.radius;
    return 0.0f;
}

// Composite the glass pixels for `n` into a frame-sized buffer (viewport must
// be curW*scale x curH*scale, matching the exporters' bake()). Returns false
// when any render pass fails; callers fall back to the plain isolated bake.
inline bool bakeGlassPixels(figo::FigmaUI& ui, figo::Node& n, float radius, uint32_t curW,
                            uint32_t curH, int scale, std::vector<uint32_t>& buf,
                            uint32_t& bw, uint32_t& bh) {
    using figo::Node;
    // A node fully outside the frame has no backdrop to sample (and its render
    // passes would be empty) — let the caller fall back to the plain bake,
    // which rebases off-frame nodes into the buffer.
    {
        const float ax = n.absoluteTransform.m02, ay = n.absoluteTransform.m12;
        if (ax >= static_cast<float>(curW) || ay >= static_cast<float>(curH) ||
            ax + n.width <= 0.0f || ay + n.height <= 0.0f)
            return false;
    }
    Node* root = &n;
    while (root->parent) root = root->parent;
    // Child-index path root → n, to find n inside the clone.
    std::vector<size_t> path;
    for (const Node* p = &n; p->parent;) {
        const auto& sib = p->parent->children;
        size_t idx = 0;
        while (idx < sib.size() && sib[idx].get() != p) ++idx;
        if (idx == sib.size()) return false;
        path.push_back(idx);
        p = p->parent;
    }
    std::reverse(path.begin(), path.end());

    ui.setViewport(curW * scale, curH * scale);

    auto primeClone = [](Node& cl, const Node& src) {
        cl.opacity = 1.0f;
        cl.runtimeOpacity = -1.0f;
        cl.runtimeVisible = -1;
        cl.relativeTransform = src.absoluteTransform;
    };

    // Pass 1: content strictly below the node — hide it and every later
    // (painted-above) sibling along its ancestor chain, then blur like the
    // runtime capture (sigma = radius/2, scaled to buffer pixels).
    auto rootClone = figo::cloneNode(*root, nullptr);
    primeClone(*rootClone, *root);
    Node* c = rootClone.get();
    for (size_t d = 0; d < path.size(); ++d) {
        for (size_t s = path[d] + 1; s < c->children.size(); ++s)
            c->children[s]->visible = false;
        c = c->children[path[d]].get();
    }
    c->visible = false;

    std::vector<uint32_t> below, chrome, mask;
    uint32_t w2 = 0, h2 = 0;
    {
        std::vector<Node*> one{rootClone.get()};
        if (!ui.renderer().renderOverlay(one, 0.0f, below, bw, bh)) return false;
    }
    blur(below, static_cast<int>(bw), static_cast<int>(bh),
         std::fmax(radius * 0.5f * scale, 0.5f));

    // Pass 2: the node's own chrome (fill/stroke/corners, no children).
    auto nodeClone = figo::cloneNode(n, nullptr);
    nodeClone->children.clear();
    primeClone(*nodeClone, n);
    {
        std::vector<Node*> one{nodeClone.get()};
        if (!ui.renderer().renderOverlay(one, 0.0f, chrome, w2, h2) || w2 != bw || h2 != bh)
            return false;
    }

    // Pass 3: opaque shape mask — the exact anti-aliased glass coverage.
    auto maskClone = figo::cloneNode(n, nullptr);
    maskClone->children.clear();
    primeClone(*maskClone, n);
    maskClone->effects.clear();
    maskClone->strokes.clear();
    figo::Paint solid;
    solid.type = figo::PaintType::Solid;
    solid.color = {0, 0, 0, 1};
    maskClone->fills.assign(1, solid);
    {
        std::vector<Node*> one{maskClone.get()};
        if (!ui.renderer().renderOverlay(one, 0.0f, mask, w2, h2) || w2 != bw || h2 != bh)
            return false;
    }

    // glass = blurred-below ⊙ mask alpha; chrome composited over (straight
    // alpha "over").
    buf.assign(below.size(), 0);
    for (size_t i = 0; i < buf.size(); ++i) {
        const uint32_t ma = mask[i] >> 24;
        const uint32_t ch = chrome[i];
        if (!ma && !(ch >> 24)) continue;
        const uint32_t g = below[i];
        const float ga = static_cast<float>((g >> 24) & 0xff) * ma / (255.0f * 255.0f);
        const float ca = static_cast<float>((ch >> 24) & 0xff) / 255.0f;
        const float outA = ca + ga * (1.0f - ca);
        if (outA <= 0.0f) continue;
        auto chan = [&](int shift) {
            const float cc = static_cast<float>((ch >> shift) & 0xff);
            const float gc = static_cast<float>((g >> shift) & 0xff);
            const float v = (cc * ca + gc * ga * (1.0f - ca)) / outA;
            return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
        };
        buf[i] = (static_cast<uint32_t>(std::lround(outA * 255.0f)) << 24) |
                 (chan(16) << 16) | (chan(8) << 8) | chan(0);
    }
    return true;
}

}  // namespace figoglass
