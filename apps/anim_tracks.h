#pragma once
// Shared CSS-animation preprocessing for the Unity/Cocos exporters: turn a
// figo::NodeAnim (normalized keyframes + easing + delay/iter, captured by
// web2canvas) into per-property LINEAR tracks on an absolute [0,length]
// seconds timeline. Mirrors figo2godot's emitAnim preprocessing:
//   - cubic-bezier easings are SAMPLED into sub-keys per keyframe segment —
//     linear interpolation between samples reproduces any curve, including
//     overshoot bounces (y outside [0,1]);
//   - a FINITE animation's delay becomes a pre-delay hold (CSS backwards
//     fill); infinite loops had their delay phase-baked by web2canvas;
//   - steps() marks the whole track discrete (hold value, jump at next key).
// Values stay in CSS conventions (pos y down, rot clockwise-positive degrees);
// each engine emitter applies its own sign flips.
#include <figo/document.h>

#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace figoanim {

struct Tracks {
    std::vector<std::pair<float, float>> alpha;   // t, a           [0..1]
    std::vector<std::pair<float, float>> rot;     // t, deg         (CSS cw+)
    std::vector<std::array<float, 3>> scale;      // t, sx, sy
    std::vector<std::array<float, 3>> pos;        // t, dx, dy      (CSS px)
    float length = 0;
    bool loop = false;      // iter == 0 (infinite)
    bool step = false;      // steps() easing -> discrete sampling
    float pivotX = 0.5f, pivotY = 0.5f;  // fraction of the LOGICAL node box
    bool any() const { return !(alpha.empty() && rot.empty() && scale.empty() && pos.empty()); }
    bool pivoted() const { return !rot.empty() || !scale.empty(); }
};

// CSS timing function -> cubic-bezier control points; false for linear/steps.
inline bool cssBezier(const std::string& e, float b[4]) {
    if (e == "ease")        { b[0]=0.25f; b[1]=0.1f;  b[2]=0.25f; b[3]=1; return true; }
    if (e == "ease-in")     { b[0]=0.42f; b[1]=0;     b[2]=1;     b[3]=1; return true; }
    if (e == "ease-out")    { b[0]=0;     b[1]=0;     b[2]=0.58f; b[3]=1; return true; }
    if (e == "ease-in-out") { b[0]=0.42f; b[1]=0;     b[2]=0.58f; b[3]=1; return true; }
    if (e.rfind("cubic-bezier(", 0) == 0 &&
        std::sscanf(e.c_str(), "cubic-bezier(%f ,%f ,%f ,%f", &b[0], &b[1], &b[2], &b[3]) == 4)
        return true;
    return false;
}

// Eased fraction y at time fraction x (bisection on the monotonic x(s)).
inline float bezY(const float b[4], float x) {
    float lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        const float s = 0.5f * (lo + hi);
        const float xs = 3*(1-s)*(1-s)*s*b[0] + 3*(1-s)*s*s*b[2] + s*s*s;
        (xs < x ? lo : hi) = s;
    }
    const float s = 0.5f * (lo + hi);
    return 3*(1-s)*(1-s)*s*b[1] + 3*(1-s)*s*s*b[3] + s*s*s;
}

inline Tracks build(const figo::NodeAnim& a) {
    Tracks out;
    out.loop = a.iter == 0;
    out.step = a.ease.find("step") != std::string::npos;
    out.pivotX = a.pivotX;
    out.pivotY = a.pivotY;
    const float dur = a.dur > 0 ? a.dur : 1.0f;
    const float delay = (a.iter != 0 && a.delay > 0) ? a.delay : 0.0f;
    out.length = dur + delay;
    float bz[4];
    const bool haveBez = !out.step && cssBezier(a.ease, bz);

    auto track = [&](const std::function<bool(const figo::AnimKey&)>& has,
                     const std::function<std::vector<float>(const figo::AnimKey&)>& val,
                     const std::function<void(float, const std::vector<float>&)>& push) {
        std::vector<const figo::AnimKey*> src;
        for (const auto& k : a.keys) if (has(k)) src.push_back(&k);
        if (src.size() < 2) return;
        std::vector<std::pair<float, std::vector<float>>> ks;
        if (delay > 0) ks.emplace_back(0.0f, val(*src[0]));  // pre-delay hold
        for (const figo::AnimKey* k : src) ks.emplace_back(delay + k->t * dur, val(*k));
        if (haveBez) {
            // Replay the easing per keyframe SEGMENT by sampling into
            // sub-keys; flat and near-instant segments stay unsampled.
            std::vector<std::pair<float, std::vector<float>>> ex;
            for (size_t i = 0; i + 1 < ks.size(); ++i) {
                ex.push_back(ks[i]);
                const auto& A = ks[i];
                const auto& B = ks[i + 1];
                bool flat = true;
                for (size_t c = 0; c < A.second.size() && flat; ++c)
                    flat = std::fabs(A.second[c] - B.second[c]) <= 1e-4f;
                if (flat || B.first - A.first < 0.04f) continue;
                const int SUB = 12;
                for (int u = 1; u < SUB; ++u) {
                    const float x = float(u) / SUB;
                    const float y = bezY(bz, x);
                    std::vector<float> v(A.second.size());
                    for (size_t c = 0; c < v.size(); ++c)
                        v[c] = A.second[c] + (B.second[c] - A.second[c]) * y;
                    ex.emplace_back(A.first + (B.first - A.first) * x, std::move(v));
                }
            }
            ex.push_back(ks.back());
            ks.swap(ex);
        }
        for (auto& kv : ks) push(kv.first, kv.second);
    };

    track([](const figo::AnimKey& k) { return k.hasOpacity; },
          [](const figo::AnimKey& k) { return std::vector<float>{k.opacity}; },
          [&](float t, const std::vector<float>& v) { out.alpha.push_back({t, v[0]}); });
    track([](const figo::AnimKey& k) { return k.hasRot; },
          [](const figo::AnimKey& k) { return std::vector<float>{k.rot}; },
          [&](float t, const std::vector<float>& v) { out.rot.push_back({t, v[0]}); });
    track([](const figo::AnimKey& k) { return k.hasScale; },
          [](const figo::AnimKey& k) { return std::vector<float>{k.sx, k.sy}; },
          [&](float t, const std::vector<float>& v) { out.scale.push_back({t, v[0], v[1]}); });
    track([](const figo::AnimKey& k) { return k.hasPos; },
          [](const figo::AnimKey& k) { return std::vector<float>{k.dx, k.dy}; },
          [&](float t, const std::vector<float>& v) { out.pos.push_back({t, v[0], v[1]}); });
    return out;
}

}  // namespace figoanim
