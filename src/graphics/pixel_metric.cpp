// Coverage, texture coordinates and sampling follow the GX as Dolphin's software renderer models it
// (VideoBackends/Software Rasterizer.cpp, TransformUnit.cpp, Clipper.cpp, TextureSampler.cpp, Tev.cpp);
// the combiner arithmetic follows its hardware-verified VideoCommon/PixelShaderGen.cpp. Register
// layouts are the SDK's GX encoders' (GXTev.c, GXTransform.c, GXPixel.c).
#include "graphics/pixel_metric.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace sms::graphics {
namespace {

int sign11(std::uint32_t v) {
    const int x = int(v & 0x7ff);
    return (x & 0x400) ? x - 0x800 : x;
}

float xf_float(const std::uint32_t* xf, std::size_t address) {
    float f;
    std::memcpy(&f, &xf[address], sizeof f);
    return f;
}

struct Rgba {
    int r = 0, g = 0, b = 0, a = 0;
};

struct Stage {
    std::uint32_t color = 0, alpha = 0;
    int texmap = 0, texcoord = 0;
    bool texture = false;
    int kcsel = 0, kasel = 0;
};

// Each stage's texture lookup reads texture index stage_texture[stage] (-1: none) of the draw's
// distinct (map, coordinate) pairs.
struct Tev {
    int stages = 0;
    std::array<Stage, 16> stage{};
    std::array<Rgba, 4> reg{};
    std::array<Rgba, 4> konst{};
    std::uint32_t alpha_test = 0;
};

constexpr int konst_fraction[8] = {255, 223, 191, 159, 128, 96, 64, 32};
constexpr int bias_add[4] = {0, 128, -128, 0};
constexpr int scale_left[4] = {0, 1, 2, 0};
constexpr int scale_right[4] = {0, 0, 0, 1};

bool konst_color(const Tev& tev, int sel, Rgba& out) {
    if (sel < 8) {
        out.r = out.g = out.b = konst_fraction[sel];
        return true;
    }
    if (sel < 12) return false;
    const Rgba& k = tev.konst[std::size_t(sel & 3)];
    if (sel < 16) {
        out.r = k.r; out.g = k.g; out.b = k.b;
    } else {
        const int v = sel < 20 ? k.r : sel < 24 ? k.g : sel < 28 ? k.b : k.a;
        out.r = out.g = out.b = v;
    }
    return true;
}

bool konst_alpha(const Tev& tev, int sel, int& out) {
    if (sel < 8) { out = konst_fraction[sel]; return true; }
    if (sel < 16) return false;
    const Rgba& k = tev.konst[std::size_t(sel & 3)];
    out = sel < 20 ? k.r : sel < 24 ? k.g : sel < 28 ? k.b : k.a;
    return true;
}

bool alpha_compare(int alpha, int ref, unsigned function) {
    switch (function) {
    case 0: return false;
    case 1: return alpha < ref;
    case 2: return alpha == ref;
    case 3: return alpha <= ref;
    case 4: return alpha > ref;
    case 5: return alpha != ref;
    case 6: return alpha >= ref;
    default: return true;
    }
}

bool alpha_test(std::uint32_t test, int alpha) {
    const bool a = alpha_compare(alpha, int(test & 0xff), (test >> 16) & 7);
    const bool b = alpha_compare(alpha, int((test >> 8) & 0xff), (test >> 19) & 7);
    switch ((test >> 22) & 3) {
    case 0: return a && b;
    case 1: return a || b;
    case 2: return a != b;
    default: return a == b;
    }
}

// A regular combiner: d + bias +/- lerp(a, b, c), scaled. c is widened to 0..256 and the lerp is
// rounded before dividing by 256 (Dolphin PixelShaderGen.cpp WriteTevRegular, hardware-verified).
int tev_regular(int a, int b, int c, int d, unsigned bias, unsigned sub, unsigned scale) {
    const int lerp = ((a << 8) + (b - a) * (c + (c >> 7))) << scale_left[scale];
    const int term = (lerp + (scale == 3 ? 0 : sub ? 127 : 128)) >> 8;
    return (((d + bias_add[bias]) << scale_left[scale]) + (sub ? -term : term)) >> scale_right[scale];
}

// The output alpha after the last stage, with stage s reading texel[s] when its lookup is enabled.
int tev_alpha(const Tev& tev, const int* texel) {
    std::array<Rgba, 4> reg = tev.reg;
    Rgba tex;
    for (int s = 0; s < tev.stages; ++s) {
        const Stage& st = tev.stage[std::size_t(s)];
        if (st.texture) tex.r = tex.g = tex.b = tex.a = texel[s];
        Rgba k;
        int ka = 0;
        const std::uint32_t cc = st.color, ac = st.alpha;
        if ((cc & 0xf) == 14 || ((cc >> 4) & 0xf) == 14 || ((cc >> 8) & 0xf) == 14 || ((cc >> 12) & 0xf) == 14)
            konst_color(tev, st.kcsel, k);
        if (((ac >> 4) & 7) == 6 || ((ac >> 7) & 7) == 6 || ((ac >> 10) & 7) == 6 || ((ac >> 13) & 7) == 6)
            konst_alpha(tev, st.kasel, ka);
        const auto color_input = [&](unsigned sel) -> Rgba {
            Rgba v;
            switch (sel) {
            case 0: v = reg[0]; break;
            case 1: v.r = v.g = v.b = reg[0].a; break;
            case 2: v = reg[1]; break;
            case 3: v.r = v.g = v.b = reg[1].a; break;
            case 4: v = reg[2]; break;
            case 5: v.r = v.g = v.b = reg[2].a; break;
            case 6: v = reg[3]; break;
            case 7: v.r = v.g = v.b = reg[3].a; break;
            case 8: v = tex; break;
            case 9: v.r = v.g = v.b = tex.a; break;
            case 12: v.r = v.g = v.b = 255; break;
            case 13: v.r = v.g = v.b = 128; break;
            case 14: v = k; break;
            default: break;  // 15 zero; rasterized colour (10, 11) is refused before
            }
            return v;
        };
        const auto alpha_input = [&](unsigned sel) -> int {
            switch (sel) {
            case 0: return reg[0].a;
            case 1: return reg[1].a;
            case 2: return reg[2].a;
            case 3: return reg[3].a;
            case 4: return tex.a;
            case 6: return ka;
            default: return 0;
            }
        };
        // Both combiners read the registers as they were before the stage.
        const Rgba ca = color_input((cc >> 12) & 0xf), cb = color_input((cc >> 8) & 0xf),
                   cc_ = color_input((cc >> 4) & 0xf), cd = color_input(cc & 0xf);
        const int aa = alpha_input((ac >> 13) & 7) & 0xff, ab = alpha_input((ac >> 10) & 7) & 0xff,
                  ac_ = alpha_input((ac >> 7) & 7) & 0xff, ad = alpha_input((ac >> 4) & 7);
        const int in_a[3] = {ca.r & 0xff, ca.g & 0xff, ca.b & 0xff};
        const int in_b[3] = {cb.r & 0xff, cb.g & 0xff, cb.b & 0xff};
        const int in_c[3] = {cc_.r & 0xff, cc_.g & 0xff, cc_.b & 0xff};
        const int in_d[3] = {cd.r, cd.g, cd.b};
        int out[3];
        const unsigned cbias = (cc >> 16) & 3, cop = (cc >> 18) & 1, cscale = (cc >> 20) & 3;
        if (cbias != 3) {
            for (int i = 0; i < 3; ++i) out[i] = tev_regular(in_a[i], in_b[i], in_c[i], in_d[i], cbias, cop, cscale);
        } else {
            const std::uint32_t a24 = std::uint32_t(in_a[2] << 16 | in_a[1] << 8 | in_a[0]);
            const std::uint32_t b24 = std::uint32_t(in_b[2] << 16 | in_b[1] << 8 | in_b[0]);
            for (int i = 0; i < 3; ++i) {
                std::uint32_t a, b;
                switch (cscale) {
                case 0: a = a24 & 0xff; b = b24 & 0xff; break;
                case 1: a = a24 & 0xffff; b = b24 & 0xffff; break;
                case 2: a = a24; b = b24; break;
                default: a = std::uint32_t(in_a[i]); b = std::uint32_t(in_b[i]); break;
                }
                out[i] = in_d[i] + ((cop ? a == b : a > b) ? in_c[i] : 0);
            }
        }
        int alpha;
        const unsigned abias = (ac >> 16) & 3, aop = (ac >> 18) & 1, ascale = (ac >> 20) & 3;
        if (abias != 3) {
            alpha = tev_regular(aa, ab, ac_, ad, abias, aop, ascale);
        } else {
            std::uint32_t a, b;
            const std::uint32_t a24 = std::uint32_t(in_a[2] << 16 | in_a[1] << 8 | in_a[0]);
            const std::uint32_t b24 = std::uint32_t(in_b[2] << 16 | in_b[1] << 8 | in_b[0]);
            switch (ascale) {
            case 0: a = a24 & 0xff; b = b24 & 0xff; break;
            case 1: a = a24 & 0xffff; b = b24 & 0xffff; break;
            case 2: a = a24; b = b24; break;
            default: a = std::uint32_t(aa); b = std::uint32_t(ab); break;
            }
            alpha = ad + ((aop ? a == b : a > b) ? ac_ : 0);
        }
        const auto clamp = [](int v, bool to8) { return to8 ? std::clamp(v, 0, 255) : std::clamp(v, -1024, 1023); };
        Rgba& cdst = reg[(cc >> 22) & 3];
        cdst.r = clamp(out[0], (cc >> 19) & 1);
        cdst.g = clamp(out[1], (cc >> 19) & 1);
        cdst.b = clamp(out[2], (cc >> 19) & 1);
        reg[(ac >> 22) & 3].a = clamp(alpha, (ac >> 19) & 1);
    }
    // The last stage's destination reaches the pixel engine as its low eight bits.
    return reg[(tev.stage[std::size_t(tev.stages - 1)].alpha >> 22) & 3].a & 0xff;
}

struct Lookup {
    int map = 0;
    int width = 0, height = 0;
    unsigned wrap_s = 0, wrap_t = 0;
    int texcoord = 0;
    const MetricTexture* texture = nullptr;
};

int wrap(int coord, unsigned mode, int size) {
    switch (mode) {
    case 0: return std::clamp(coord, 0, size - 1);
    case 1: return coord & (size - 1);
    default:
        if (coord & size) coord = ~coord;
        return coord & (size - 1);
    }
}

// Dolphin's rounding of screen coordinates to 28.4 fixed point (Rasterizer.cpp iround).
int iround(float x) {
    const int t = int(x);
    return (x - float(t)) >= 0.5f ? t + 1 : t;
}

struct Projected {
    float x = 0, y = 0;   // EFB pixels
    float w = 1;
    std::array<std::array<float, 3>, 8> tex{};
    int z_side = 0;       // -1 beyond the far plane, 1 in front of the near plane, 0 inside
};

// Dolphin's attribute plane (Rasterizer.cpp Slope): f0 at v0, sampled at pixel + 0.495.
struct Slope {
    float f0 = 0, dfdx = 0, dfdy = 0, x0 = 0, y0 = 0;
    float at(int x, int y) const { return f0 + dfdx * (float(x) + 0.495f - x0) + dfdy * (float(y) + 0.495f - y0); }
};

Slope slope(float f0, float f1, float f2, const Projected& v0, const Projected& v1, const Projected& v2) {
    const float dx10 = v1.x - v0.x, dx20 = v2.x - v0.x, dy10 = v1.y - v0.y, dy20 = v2.y - v0.y;
    const float d20 = f2 - f0, d10 = f1 - f0;
    const float a = d20 * dy10 - d10 * dy20, b = dx20 * d10 - dx10 * d20, c = dx20 * dy10 - dx10 * dy20;
    return {f0, a / c, b / c, v0.x, v0.y};
}

}  // namespace

unsigned primitive_triangles(std::uint8_t primitive, unsigned vertices) {
    switch (primitive) {
    case 0x80: case 0x88: return vertices / 4 * 2;
    case 0x90: return vertices / 3;
    case 0x98: case 0xa0: return vertices >= 3 ? vertices - 2 : 0;
    default: return 0;
    }
}

void decode_i8(const std::uint8_t* tiled, int width, int height, std::uint8_t* rows) {
    const int tiles_wide = width / 8;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            rows[std::size_t(y) * std::size_t(width) + std::size_t(x)] =
                tiled[std::size_t((y / 4) * tiles_wide + x / 8) * 32 + std::size_t((y & 3) * 8 + (x & 7))];
}

struct MetricDraw::Plan {
    const char* unsupported = nullptr;
    bool nothing_passes = false, empty_scissor = false;
    const std::uint32_t* xf = nullptr;
    Tev tev;
    std::array<Lookup, 16> lookups{};
    int lookup_count = 0;
    std::array<int, 16> stage_lookup{};
    bool single = false;
    std::array<bool, 256> pass_by_texel{};
    int scissor_left = 0, scissor_top = 0, scissor_right = 0, scissor_bottom = 0;
    float projection[6] = {};
    bool orthographic = false, dual = false;
    float vp_wd = 0, vp_ht = 0, vp_x = 0, vp_y = 0;
    std::array<bool, 8> used_texcoord{};
    std::array<std::uint32_t, 8> texgen{}, post{};
    std::array<float, 8> scale_s{}, scale_t{};
};

MetricDraw::MetricDraw(const MetricRegisters& r, MetricTextures& textures, MetricTables* tables)
    : plan_(std::make_unique<Plan>()) {
    // A lambda, so each refusal can return.
    [&] {
        Plan& p = *plan_;
        const auto refuse = [&](const char* why) { p.unsupported = why; };
        const std::uint32_t* bp = r.bp;
        const std::uint32_t* xf = r.xf;
        p.xf = xf;

        const std::uint32_t genmode = bp[0x00];
        const int texgens = int(genmode & 0xf);
        if ((genmode >> 16) & 7) return refuse("indirect texture stages");
        if (((genmode >> 14) & 3) != 0) return refuse("face culling");
        if (bp[0x40] & 1) return refuse("depth test");

        Tev& tev = p.tev;
        tev.stages = int((genmode >> 10) & 0xf) + 1;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t ra = r.tev[0][std::size_t(2 * i)], bg = r.tev[0][std::size_t(2 * i + 1)];
            tev.reg[std::size_t(i)] = {sign11(ra), sign11(bg >> 12), sign11(bg), sign11(ra >> 12)};
            const std::uint32_t kra = r.tev[1][std::size_t(2 * i)], kbg = r.tev[1][std::size_t(2 * i + 1)];
            tev.konst[std::size_t(i)] = {int(kra & 0xff), int((kbg >> 12) & 0xff), int(kbg & 0xff), int((kra >> 12) & 0xff)};
        }
        tev.alpha_test = bp[0xf3];

        // Distinct texture lookups; stage_lookup[s] indexes them, -1 for a stage without one.
        for (int s = 0; s < tev.stages; ++s) {
            Stage& st = tev.stage[std::size_t(s)];
            st.color = bp[0xc0 + 2 * s];
            st.alpha = bp[0xc1 + 2 * s];
            const std::uint32_t order = bp[0x28 + s / 2] >> ((s & 1) ? 12 : 0);
            st.texmap = int(order & 7);
            st.texcoord = int((order >> 3) & 7);
            st.texture = (order >> 6) & 1;
            const std::uint32_t ksel = bp[0xf6 + s / 2] >> ((s & 1) ? 14 : 4);
            st.kcsel = int(ksel & 0x1f);
            st.kasel = int((ksel >> 5) & 0x1f);
            if (bp[0x10 + s] & 0x1fffff) return refuse("indirect texturing in a TEV stage");
            const std::uint32_t cc = st.color, ac = st.alpha;
            for (unsigned shift = 0; shift < 16; shift += 4) {
                const unsigned sel = (cc >> shift) & 0xf;
                if (sel == 10 || sel == 11) return refuse("rasterized colour in a TEV stage");
                Rgba unused;
                if (sel == 14 && !konst_color(tev, st.kcsel, unused)) return refuse("reserved konst colour selection");
            }
            for (unsigned shift = 4; shift < 16; shift += 3) {
                const unsigned sel = (ac >> shift) & 7;
                if (sel == 5) return refuse("rasterized alpha in a TEV stage");
                int unused;
                if (sel == 6 && !konst_alpha(tev, st.kasel, unused)) return refuse("reserved konst alpha selection");
            }
            p.stage_lookup[std::size_t(s)] = -1;
            if (!st.texture) continue;
            if (texgens == 0) continue;  // no coordinates: the lookup reads black
            const int texcoord = st.texcoord < texgens ? st.texcoord : 0;
            const int map = st.texmap;
            int found = -1;
            for (int l = 0; l < p.lookup_count; ++l)
                if (p.lookups[std::size_t(l)].texcoord == texcoord && p.lookups[std::size_t(l)].map == map) found = l;
            if (found < 0) {
                Lookup& lk = p.lookups[std::size_t(p.lookup_count)];
                lk.map = map;
                const unsigned slot = unsigned(map & 3);
                const std::uint32_t mode0 = bp[(map < 4 ? 0x80 : 0xa0) + slot];
                const std::uint32_t image0 = bp[(map < 4 ? 0x88 : 0xa8) + slot];
                const std::uint32_t image3 = bp[(map < 4 ? 0x94 : 0xb4) + slot];
                if (((image0 >> 20) & 0xf) != 1) return refuse("texture format other than I8");
                if ((mode0 >> 4) & 1 || (mode0 >> 7) & 1 || (mode0 >> 5) & 3) return refuse("filtered or mipmapped texture");
                lk.wrap_s = mode0 & 3;
                lk.wrap_t = (mode0 >> 2) & 3;
                if (lk.wrap_s == 3 || lk.wrap_t == 3) return refuse("reserved texture wrap mode");
                lk.width = int(image0 & 0x3ff) + 1;
                lk.height = int((image0 >> 10) & 0x3ff) + 1;
                lk.texcoord = texcoord;
                const std::uint32_t address = (image3 & 0xffffff) << 5;
                lk.texture = textures.texture(address, lk.width, lk.height);
                if (!lk.texture || lk.texture->width != lk.width || lk.texture->height != lk.height)
                    return refuse("texture texels unavailable");
                found = p.lookup_count++;
            }
            p.stage_lookup[std::size_t(s)] = found;
        }

        // With one lookup, which stages share, the alpha test depends on its texel only.
        p.single = p.lookup_count <= 1;
        if (p.single) {
            // The table depends on the stages, registers and alpha test only; draws repeat them.
            std::uint64_t h = std::uint64_t(tev.stages) << 32 | tev.alpha_test;
            const auto add = [&h](std::uint64_t v) { h = (h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2))) * 0xff51afd7ed558ccdull; };
            for (int s = 0; s < tev.stages; ++s) {
                const Stage& st = tev.stage[std::size_t(s)];
                add(std::uint64_t(st.color) << 32 | st.alpha);
                add(std::uint64_t(st.kcsel) | std::uint64_t(st.kasel) << 8 | std::uint64_t(st.texture) << 16 |
                    std::uint64_t(p.stage_lookup[std::size_t(s)] < 0) << 17);
            }
            for (int i = 0; i < 4; ++i) {
                const Rgba& c = tev.reg[std::size_t(i)];
                const Rgba& k = tev.konst[std::size_t(i)];
                add(std::uint64_t(std::uint16_t(c.r)) | std::uint64_t(std::uint16_t(c.g)) << 16 |
                    std::uint64_t(std::uint16_t(c.b)) << 32 | std::uint64_t(std::uint16_t(c.a)) << 48);
                add(std::uint64_t(k.r) | std::uint64_t(k.g) << 16 | std::uint64_t(k.b) << 32 | std::uint64_t(k.a) << 48);
            }
            const auto cached = tables ? tables->find(h) : MetricTables::iterator{};
            if (tables && cached != tables->end()) {
                p.pass_by_texel = cached->second;
            } else {
                std::array<int, 16> texel{};
                for (int v = 0; v < 256; ++v) {
                    for (int s = 0; s < tev.stages; ++s) texel[std::size_t(s)] = p.stage_lookup[std::size_t(s)] < 0 ? 0 : v;
                    p.pass_by_texel[std::size_t(v)] = alpha_test(tev.alpha_test, tev_alpha(tev, texel.data()));
                }
                if (tables) {
                    if (tables->size() > 256) tables->clear();
                    tables->emplace(h, p.pass_by_texel);
                }
            }
            bool any = false;
            for (bool pass : p.pass_by_texel) any |= pass;
            p.nothing_passes = !any;
        }

        // Scissor in EFB pixels, inclusive bounds, and the 640x528 EFB.
        const int offset_x = int(bp[0x59] & 0x3ff) * 2, offset_y = int((bp[0x59] >> 10) & 0x3ff) * 2;
        p.scissor_left = std::max(0, int((bp[0x20] >> 12) & 0x7ff) - offset_x);
        p.scissor_top = std::max(0, int(bp[0x20] & 0x7ff) - offset_y);
        p.scissor_right = std::min(639, int((bp[0x21] >> 12) & 0x7ff) - offset_x);
        p.scissor_bottom = std::min(527, int(bp[0x21] & 0x7ff) - offset_y);
        p.empty_scissor = p.scissor_left > p.scissor_right || p.scissor_top > p.scissor_bottom;

        // Vertex transform (TransformUnit.cpp) and viewport (Clipper.cpp PerspectiveDivide).
        for (int i = 0; i < 6; ++i) p.projection[i] = xf_float(xf, 0x1020 + std::size_t(i));
        p.orthographic = xf[0x1026] != 0;
        p.vp_wd = xf_float(xf, 0x101a);
        p.vp_ht = xf_float(xf, 0x101b);
        p.vp_x = xf_float(xf, 0x101d) - float(offset_x);
        p.vp_y = xf_float(xf, 0x101e) - float(offset_y);
        p.dual = xf[0x1012] & 1;
        for (int l = 0; l < p.lookup_count; ++l) p.used_texcoord[std::size_t(p.lookups[std::size_t(l)].texcoord)] = true;
        for (int i = 0; i < 8; ++i) {
            if (!p.used_texcoord[std::size_t(i)]) continue;
            const std::uint32_t info = xf[0x1040 + std::size_t(i)];
            const unsigned type = (info >> 4) & 7, row = (info >> 7) & 0x1f;
            if (type != 0) return refuse("texture coordinate generation other than regular");
            if (row != 0 && (row < 5 || row > 12)) return refuse("texture coordinates from normals or colours");
            p.texgen[std::size_t(i)] = info;
            p.post[std::size_t(i)] = p.dual ? xf[0x1050 + std::size_t(i)] : 0;
            p.scale_s[std::size_t(i)] = float((bp[0x30 + 2 * i] & 0xffff) + 1);
            p.scale_t[std::size_t(i)] = float((bp[0x31 + 2 * i] & 0xffff) + 1);
        }
    }();
}

MetricDraw::~MetricDraw() = default;

const char* MetricDraw::unsupported() const { return plan_->unsupported; }

std::uint64_t MetricDraw::key(const MetricVertex* vertices, std::size_t vertex_count, const std::uint16_t* indices,
                              std::size_t index_count) const {
    const Plan& p = *plan_;
    std::uint64_t h = 0x243f6a8885a308d3ull;
    const auto add = [&h](std::uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h *= 0xff51afd7ed558ccdull;
    };
    const auto add_float = [&](float f) { std::uint32_t u; std::memcpy(&u, &f, 4); add(u); };
    add(std::uint64_t(p.nothing_passes) | std::uint64_t(p.empty_scissor) << 1 | std::uint64_t(p.single) << 2 |
        std::uint64_t(p.orthographic) << 3 | std::uint64_t(p.dual) << 4);
    if (p.single) {
        for (int i = 0; i < 256; i += 64) {
            std::uint64_t bits = 0;
            for (int b = 0; b < 64; ++b) bits |= std::uint64_t(p.pass_by_texel[std::size_t(i + b)]) << b;
            add(bits);
        }
    } else {
        add(std::uint64_t(p.tev.stages) | std::uint64_t(p.tev.alpha_test) << 8);
        for (int s = 0; s < p.tev.stages; ++s) {
            const Stage& st = p.tev.stage[std::size_t(s)];
            add(std::uint64_t(st.color) << 32 | st.alpha);
            add(std::uint64_t(st.kcsel) | std::uint64_t(st.kasel) << 8 | std::uint64_t(st.texture) << 16 |
                std::uint64_t(std::uint32_t(p.stage_lookup[std::size_t(s)])) << 32);
        }
        for (const Rgba& c : p.tev.reg) add(std::uint64_t(std::uint16_t(c.r)) | std::uint64_t(std::uint16_t(c.g)) << 16 |
                                            std::uint64_t(std::uint16_t(c.b)) << 32 | std::uint64_t(std::uint16_t(c.a)) << 48);
        for (const Rgba& c : p.tev.konst) add(std::uint64_t(c.r) | std::uint64_t(c.g) << 16 | std::uint64_t(c.b) << 32 | std::uint64_t(c.a) << 48);
    }
    for (int l = 0; l < p.lookup_count; ++l) {
        const Lookup& lk = p.lookups[std::size_t(l)];
        add(std::uint64_t(lk.map) | std::uint64_t(lk.texcoord) << 4 | std::uint64_t(lk.wrap_s) << 8 |
            std::uint64_t(lk.wrap_t) << 10 | std::uint64_t(lk.width) << 16 | std::uint64_t(lk.height) << 32);
        add(reinterpret_cast<std::uintptr_t>(lk.texture->texels));
        add(lk.texture->generation);
    }
    add(std::uint64_t(std::uint32_t(p.scissor_left)) | std::uint64_t(std::uint32_t(p.scissor_top)) << 16 |
        std::uint64_t(std::uint32_t(p.scissor_right)) << 32 | std::uint64_t(std::uint32_t(p.scissor_bottom)) << 48);
    for (float f : p.projection) add_float(f);
    add_float(p.vp_wd); add_float(p.vp_ht); add_float(p.vp_x); add_float(p.vp_y);
    for (int i = 0; i < 8; ++i)
        if (p.used_texcoord[std::size_t(i)]) {
            add(std::uint64_t(i) << 56 | std::uint64_t(p.texgen[std::size_t(i)]) << 24 | p.post[std::size_t(i)]);
            add_float(p.scale_s[std::size_t(i)]);
            add_float(p.scale_t[std::size_t(i)]);
        }
    // The XF rows the vertices name, each once, then the vertices' used attributes.
    std::array<bool, 256> row_seen{};
    const auto add_row = [&](std::size_t base) {
        for (std::size_t w = 0; w < 12 && base + w < 0x1000; ++w) add(p.xf[base + w]);
    };
    for (std::size_t v = 0; v < vertex_count; ++v) {
        const MetricVertex& mv = vertices[v];
        for (int k = 0; k < 9; ++k) {
            if (k > 0 && !p.used_texcoord[std::size_t(k - 1)]) continue;
            const std::uint8_t row = mv.matrix_rows[std::size_t(k)];
            if (!row_seen[row]) { row_seen[row] = true; add(row); add_row(std::size_t(row) * 4); }
        }
    }
    if (p.dual)
        for (int i = 0; i < 8; ++i)
            if (p.used_texcoord[std::size_t(i)]) add_row(0x500 + std::size_t(p.post[std::size_t(i)] & 0x3f) * 4);
    for (std::size_t v = 0; v < vertex_count; ++v) {
        const MetricVertex& mv = vertices[v];
        for (float f : mv.position) add_float(f);
        add(mv.matrix_rows[0]);
        for (int i = 0; i < 8; ++i) {
            if (!p.used_texcoord[std::size_t(i)]) continue;
            add(mv.matrix_rows[std::size_t(1 + i)]);
            const unsigned row = (p.texgen[std::size_t(i)] >> 7) & 0x1f;
            if (row >= 5) { add_float(mv.texcoords[row - 5][0]); add_float(mv.texcoords[row - 5][1]); }
        }
    }
    add(index_count);
    for (std::size_t i = 0; i < index_count; ++i) add(indices[i]);
    return h;
}

void MetricDraw::count(const MetricVertex* vertices, std::size_t vertex_count, const std::uint16_t* indices,
                       std::size_t index_count, MetricCount& count) const {
    const Plan& pl = *plan_;
    const std::size_t triangles = index_count / 3;
    count.triangles += triangles;
    if (!triangles || count.unsupported) return;
    if (pl.unsupported) { count.unsupported = pl.unsupported; return; }
    if (pl.nothing_passes || pl.empty_scissor) return;  // no quad reaches blending
    const auto refuse = [&](const char* why) { count.unsupported = why; };
    const std::uint32_t* xf = pl.xf;
    const Tev& tev = pl.tev;
    const auto& lookups = pl.lookups;
    const int lookup_count = pl.lookup_count;
    std::vector<Projected> projected(vertex_count);
    std::vector<std::uint8_t> quad_marks;
    std::vector<bool> done(vertex_count, false);
    const auto project = [&](std::size_t index) -> const Projected& {
        Projected& p = projected[index];
        if (done[index]) return p;
        done[index] = true;
        const MetricVertex& v = vertices[index];
        const std::size_t m = std::size_t(v.matrix_rows[0]) * 4;
        float mv[3];
        for (int i = 0; i < 3; ++i)
            mv[i] = xf_float(xf, m + 4 * i) * v.position[0] + xf_float(xf, m + 4 * i + 1) * v.position[1] +
                    xf_float(xf, m + 4 * i + 2) * v.position[2] + xf_float(xf, m + 4 * i + 3);
        float cx, cy, cz;
        if (pl.orthographic) {
            cx = pl.projection[0] * mv[0] + pl.projection[1];
            cy = pl.projection[2] * mv[1] + pl.projection[3];
            cz = pl.projection[4] * mv[2] + pl.projection[5];
            p.w = 1;
        } else {
            cx = pl.projection[0] * mv[0] + pl.projection[1] * mv[2];
            cy = pl.projection[2] * mv[1] + pl.projection[3] * mv[2];
            cz = (pl.projection[4] * mv[2] + pl.projection[5]) * (1.0f - 1e-7f);
            p.w = -mv[2];
        }
        p.z_side = p.w <= 0 || cz > 0 ? 1 : cz < -p.w ? -1 : 0;
        if (p.w > 0) {
            p.x = cx / p.w * pl.vp_wd + pl.vp_x;
            p.y = cy / p.w * pl.vp_ht + pl.vp_y;
        }
        for (int i = 0; i < 8; ++i) {
            if (!pl.used_texcoord[std::size_t(i)]) continue;
            const std::uint32_t info = xf[0x1040 + std::size_t(i)];
            const bool stq = (info >> 1) & 1, abc1 = (info >> 2) & 1;
            const unsigned row = (info >> 7) & 0x1f;
            float src[3];
            if (row == 0) {
                src[0] = v.position[0]; src[1] = v.position[1]; src[2] = v.position[2];
            } else {
                const auto& t = v.texcoords[row - 5];
                src[0] = t[0]; src[1] = t[1]; src[2] = 1.0f;
            }
            for (float& s : src) if (std::isnan(s)) s = 1.0f;
            const std::size_t tm = std::size_t(v.matrix_rows[std::size_t(1 + i)]) * 4;
            float out[3];
            for (int k = 0; k < 3; ++k) {
                if (k == 2 && !stq) { out[2] = 1.0f; break; }
                out[k] = xf_float(xf, tm + 4 * k) * src[0] + xf_float(xf, tm + 4 * k + 1) * src[1] +
                         (abc1 ? xf_float(xf, tm + 4 * k + 2) * src[2] : xf_float(xf, tm + 4 * k + 2)) +
                         xf_float(xf, tm + 4 * k + 3);
            }
            if (pl.dual) {
                const std::uint32_t post = pl.post[std::size_t(i)];
                const std::size_t pm = 0x500 + std::size_t(post & 0x3f) * 4;
                float in[3] = {out[0], out[1], out[2]};
                if ((post >> 8) & 1) {
                    const float length = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
                    if (length > 0) for (float& c : in) c /= length;
                }
                for (int k = 0; k < 3; ++k)
                    out[k] = xf_float(xf, pm + 4 * k) * in[0] + xf_float(xf, pm + 4 * k + 1) * in[1] +
                             xf_float(xf, pm + 4 * k + 2) * in[2] + xf_float(xf, pm + 4 * k + 3);
            }
            if (out[2] == 0.0f) {
                out[0] = std::clamp(out[0] / 2.0f, -1.0f, 1.0f);
                out[1] = std::clamp(out[1] / 2.0f, -1.0f, 1.0f);
            }
            out[0] *= pl.scale_s[std::size_t(i)];
            out[1] *= pl.scale_t[std::size_t(i)];
            p.tex[std::size_t(i)] = {out[0], out[1], out[2]};
        }
        return p;
    };

    for (std::size_t t = 0; t < triangles; ++t) {
        std::uint16_t i0 = indices[3 * t], i1 = indices[3 * t + 1], i2 = indices[3 * t + 2];
        if (i0 >= vertex_count || i1 >= vertex_count || i2 >= vertex_count) return refuse("vertex index out of range");
        const Projected* v[3] = {&project(i0), &project(i1), &project(i2)};
        if (v[0]->z_side || v[1]->z_side || v[2]->z_side) {
            if (v[0]->z_side == v[1]->z_side && v[1]->z_side == v[2]->z_side) continue;  // wholly clipped
            return refuse("triangle crossing the depth clip planes");
        }
        for (const Projected* p : v)
            if (!(std::abs(p->x) < 4096.0f && std::abs(p->y) < 4096.0f)) return refuse("vertex outside the guard band");

        // 28.4 fixed point, adjusted as Dolphin matches hardware coverage (Rasterizer.cpp).
        std::int64_t X[3], Y[3];
        for (int k = 0; k < 3; ++k) {
            X[k] = iround(16.0f * v[k]->x) - 9;
            Y[k] = iround(16.0f * v[k]->y) - 9;
        }
        std::int64_t area = (Y[0] - Y[1]) * (X[0] - X[2]) - (X[0] - X[1]) * (Y[0] - Y[2]);
        if (area == 0) continue;
        if (area < 0) {
            std::swap(X[1], X[2]);
            std::swap(Y[1], Y[2]);
            std::swap(v[1], v[2]);
        }
        const std::int64_t DX12 = X[0] - X[1], DX23 = X[1] - X[2], DX31 = X[2] - X[0];
        const std::int64_t DY12 = Y[0] - Y[1], DY23 = Y[1] - Y[2], DY31 = Y[2] - Y[0];
        std::int64_t C1 = DY12 * X[0] - DX12 * Y[0], C2 = DY23 * X[1] - DX23 * Y[1], C3 = DY31 * X[2] - DX31 * Y[2];
        if (DY12 < 0 || (DY12 == 0 && DX12 > 0)) ++C1;
        if (DY23 < 0 || (DY23 == 0 && DX23 > 0)) ++C2;
        if (DY31 < 0 || (DY31 == 0 && DX31 > 0)) ++C3;
        const int minx = std::max(int((std::min({X[0], X[1], X[2]}) + 0xf) >> 4), pl.scissor_left);
        const int maxx = std::min(int((std::max({X[0], X[1], X[2]}) + 0xf) >> 4), pl.scissor_right + 1);
        const int miny = std::max(int((std::min({Y[0], Y[1], Y[2]}) + 0xf) >> 4), pl.scissor_top);
        const int maxy = std::min(int((std::max({Y[0], Y[1], Y[2]}) + 0xf) >> 4), pl.scissor_bottom + 1);
        if (minx >= maxx || miny >= maxy) continue;

        // Perspective-correct coordinates for each lookup (Rasterizer.cpp: slopes of tex * 1/w).
        const float w[3] = {1.0f / v[0]->w, 1.0f / v[1]->w, 1.0f / v[2]->w};
        const Slope w_slope = slope(w[0], w[1], w[2], *v[0], *v[1], *v[2]);
        std::array<std::array<Slope, 3>, 16> tex_slope{};
        for (int l = 0; l < lookup_count; ++l) {
            const auto c = std::size_t(lookups[std::size_t(l)].texcoord);
            for (int k = 0; k < 3; ++k)
                tex_slope[std::size_t(l)][std::size_t(k)] =
                    slope(v[0]->tex[c][std::size_t(k)] * w[0], v[1]->tex[c][std::size_t(k)] * w[1],
                          v[2]->tex[c][std::size_t(k)] * w[2], *v[0], *v[1], *v[2]);
        }
        // With w and q 1 at every vertex (orthographic, 2x4 texgen) both planes are exactly 1.
        bool affine = w[0] == 1.0f && w[1] == 1.0f && w[2] == 1.0f;
        for (int l = 0; l < lookup_count && affine; ++l) {
            const auto c = std::size_t(lookups[std::size_t(l)].texcoord);
            affine = v[0]->tex[c][2] == 1.0f && v[1]->tex[c][2] == 1.0f && v[2]->tex[c][2] == 1.0f;
        }
        const auto texel = [&](int l, int x, int y) -> int {
            const Lookup& lk = lookups[std::size_t(l)];
            const auto& sl = tex_slope[std::size_t(l)];
            float scale = 1.0f;
            if (!affine) {
                const float inv_w = 1.0f / w_slope.at(x, y);
                scale = inv_w;
                const float q = sl[2].at(x, y) * inv_w;
                if (q != 0.0f) scale = inv_w / q;
            }
            const float s = std::clamp(sl[0].at(x, y) * scale * 128.0f, -2.0e9f, 2.0e9f);
            const float tt = std::clamp(sl[1].at(x, y) * scale * 128.0f, -2.0e9f, 2.0e9f);
            const int tx = wrap(int(s) >> 7, lk.wrap_s, lk.width), ty = wrap(int(tt) >> 7, lk.wrap_t, lk.height);
            return lk.texture->texels[std::size_t(ty) * std::size_t(lk.width) + std::size_t(tx)];
        };
        const auto passes = [&](int x, int y) -> bool {
            if (pl.single) return pl.pass_by_texel[std::size_t(lookup_count ? texel(0, x, y) : 0)];
            std::array<int, 16> values{};
            for (int s = 0; s < tev.stages; ++s) {
                const int l = pl.stage_lookup[std::size_t(s)];
                values[std::size_t(s)] = l < 0 ? 0 : texel(l, x, y);
            }
            return alpha_test(tev.alpha_test, tev_alpha(tev, values.data()));
        };
        const bool fast = pl.single && lookup_count == 1 && affine;
        // Row spans of the coverage test above: E(x) = A - B * x > 0 for each edge, with
        // A = C + DX * (y << 4) and B = DY << 4. Pixels that pass mark their 2x2 quad.
        const auto floor_div = [](std::int64_t a, std::int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
        const std::int64_t C[3] = {C1, C2, C3}, DX[3] = {DX12, DX23, DX31}, DY[3] = {DY12, DY23, DY31};
        const int first_quad = minx >> 1;
        quad_marks.assign(std::size_t(((maxx + 1) >> 1) - first_quad), 0);
        for (int by = miny & ~1; by < maxy; by += 2) {
            bool any = false;
            for (int y = std::max(by, miny); y < std::min(by + 2, maxy); ++y) {
                std::int64_t xs = minx, xe = maxx;
                for (int e = 0; e < 3; ++e) {
                    const std::int64_t A = C[e] + DX[e] * (std::int64_t(y) << 4), B = DY[e] << 4;
                    if (B == 0) {
                        if (A <= 0) xe = xs;
                    } else if (B > 0) {
                        xe = std::min(xe, floor_div(A - 1, B) + 1);
                    } else {
                        xs = std::max(xs, floor_div(-A, -B) + 1);
                    }
                }
                if (fast) {
                    // One lookup through a plane with w = q = 1: per pixel two multiply-adds, the
                    // texel and the table (the goop counters' draws).
                    const Lookup& lk = lookups[0];
                    const Slope& ss = tex_slope[0][0];
                    const Slope& st = tex_slope[0][1];
                    const float s_row = ss.f0 + ss.dfdy * (float(y) + 0.495f - ss.y0) + ss.dfdx * (0.495f - ss.x0);
                    const float t_row = st.f0 + st.dfdy * (float(y) + 0.495f - st.y0) + st.dfdx * (0.495f - st.x0);
                    const std::uint8_t* texels = lk.texture->texels;
                    // A row whose t does not change along x and whose s stays inside the texture
                    // at both ends of the span (s is monotonic in x): every pixel samples this
                    // texel row, and the clamp and wrap below change nothing. The same texels in
                    // fewer operations (the layer counters' 1:1 rectangles). The 1/128 texel
                    // margin covers a fused multiply-add rounding the loop's s differently.
                    if (xs < xe && st.dfdx == 0.0f) {
                        const float t = (t_row + st.dfdx * float(xs)) * 128.0f;
                        const float s0 = (s_row + ss.dfdx * float(xs)) * 128.0f, s1 = (s_row + ss.dfdx * float(xe - 1)) * 128.0f;
                        if (t >= 0.0f && t < float(lk.height) * 128.0f && std::min(s0, s1) >= 0.0f &&
                            std::max(s0, s1) < float(lk.width) * 128.0f - 1.0f) {
                            const std::uint8_t* row = texels + std::size_t(int(t) >> 7) * std::size_t(lk.width);
                            for (std::int64_t x = xs; x < xe; ++x) {
                                std::uint8_t& mark = quad_marks[std::size_t((int(x) >> 1) - first_quad)];
                                if (mark) continue;
                                const float s = (s_row + ss.dfdx * float(x)) * 128.0f;
                                if (pl.pass_by_texel[row[int(s) >> 7]]) mark = 1, any = true;
                            }
                            continue;
                        }
                    }
                    for (std::int64_t x = xs; x < xe; ++x) {
                        std::uint8_t& mark = quad_marks[std::size_t((int(x) >> 1) - first_quad)];
                        if (mark) continue;
                        const float s = std::clamp((s_row + ss.dfdx * float(x)) * 128.0f, -2.0e9f, 2.0e9f);
                        const float t = std::clamp((t_row + st.dfdx * float(x)) * 128.0f, -2.0e9f, 2.0e9f);
                        const int tx = wrap(int(s) >> 7, lk.wrap_s, lk.width), ty = wrap(int(t) >> 7, lk.wrap_t, lk.height);
                        if (pl.pass_by_texel[texels[std::size_t(ty) * std::size_t(lk.width) + std::size_t(tx)]])
                            mark = 1, any = true;
                    }
                } else {
                    for (std::int64_t x = xs; x < xe; ++x) {
                        std::uint8_t& mark = quad_marks[std::size_t((int(x) >> 1) - first_quad)];
                        if (!mark && passes(int(x), y)) mark = 1, any = true;
                    }
                }
            }
            if (!any) continue;
            for (std::uint8_t& mark : quad_marks) {
                count.quads += mark;
                mark = 0;
            }
        }
    }
}

void count_triangles(const MetricRegisters& registers, const MetricVertex* vertices, std::size_t vertex_count,
                     const std::uint16_t* indices, std::size_t index_count, MetricTextures& textures,
                     MetricCount& count) {
    MetricDraw draw(registers, textures);
    draw.count(vertices, vertex_count, indices, index_count, count);
}

}
