// Host tests of the blend-input counter model against hand-computed counts.
#include "graphics/pixel_metric.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace sms::graphics;

namespace {

int failures = 0;

void expect(bool ok, const char* what, long long got, long long want) {
    if (!ok) {
        std::printf("FAIL %s: got %lld, want %lld\n", what, got, want);
        ++failures;
    }
}
#define EXPECT_EQ(got, want) expect((got) == (want), #got, (long long)(got), (long long)(want))

void put_float(std::vector<std::uint32_t>& xf, std::size_t address, float value) {
    std::memcpy(&xf[address], &value, sizeof value);
}

constexpr std::uint32_t texture_address = 0x00100000;

// GX state as the goop counters set it up: a W x H orthographic pass where one unit is one pixel,
// one texture coordinate from TEX0 through the identity matrix, TEXMAP0 an I8 W x H texture.
struct Setup {
    std::vector<std::uint32_t> bp = std::vector<std::uint32_t>(256);
    std::vector<std::uint32_t> xf = std::vector<std::uint32_t>(0x1058);
    std::array<std::array<std::uint32_t, 8>, 2> tev{};
    std::vector<std::uint8_t> texels;
    int width, height;

    Setup(int w, int h) : texels(std::size_t(w * h)), width(w), height(h) {
        for (int i = 0; i < 3; ++i) put_float(xf, std::size_t(i * 5), 1.0f);       // PNMTX0 identity
        put_float(xf, 60 * 4 + 0, 1.0f);                                             // GX_IDENTITY rows
        put_float(xf, 60 * 4 + 5, 1.0f);
        put_float(xf, 0x101a, float(w) / 2);                                         // GXSetViewport(0, 0, w, h)
        put_float(xf, 0x101b, -float(h) / 2);
        put_float(xf, 0x101d, 342.0f + float(w) / 2);
        put_float(xf, 0x101e, 342.0f + float(h) / 2);
        put_float(xf, 0x1020, 2.0f / float(w));                                      // C_MTXOrtho(0, h, 0, w, ...)
        put_float(xf, 0x1021, -1.0f);
        put_float(xf, 0x1022, -2.0f / float(h));
        put_float(xf, 0x1023, 1.0f);
        put_float(xf, 0x1024, -0.5f);
        put_float(xf, 0x1025, -0.5f);
        xf[0x1026] = 1;
        xf[0x1040] = 5u << 7;                                                        // regular, AB11, from TEX0
        bp[0x00] = 1;                                                                // 1 texgen, 1 stage, no culling
        bp[0x20] = (342u << 12) | 342u;                                              // GXSetScissor(0, 0, w, h)
        bp[0x21] = (std::uint32_t(342 + w - 1) << 12) | std::uint32_t(342 + h - 1);
        bp[0x59] = 171u | (171u << 10);                                              // GXSetScissorBoxOffset(0, 0)
        bp[0x28] = 1u << 6;                                                          // stage 0: TEXMAP0, TEXCOORD0
        bp[0x30] = std::uint32_t(w - 1);
        bp[0x31] = std::uint32_t(h - 1);
        bp[0x88] = std::uint32_t(w - 1) | std::uint32_t(h - 1) << 10 | 1u << 20;   // I8, nearest, clamp
        bp[0x94] = texture_address >> 5;
        // initDrawObjGX: colour TEXC, alpha TEXA; GXSetAlphaCompare(GREATER, 0, AND, ALWAYS, 0).
        bp[0xc0] = 15u | 15u << 4 | 15u << 8 | 8u << 12 | 1u << 19;
        bp[0xc1] = 7u << 4 | 7u << 7 | 7u << 10 | 4u << 13 | 1u << 19;
        bp[0xf3] = 4u << 16 | 7u << 19;
    }
    void texel(int x, int y, int v) { texels[std::size_t(y * width + x)] = std::uint8_t(v); }
    // GXSetTevColor(GX_TEVREG<n>, {v, v, v, v}).
    void tev_register(int n, int v) {
        tev[0][std::size_t(2 * n)] = std::uint32_t(v) | std::uint32_t(v) << 12;
        tev[0][std::size_t(2 * n + 1)] = std::uint32_t(v) | std::uint32_t(v) << 12;
    }
};

class Textures final : public MetricTextures {
public:
    explicit Textures(const Setup& s, std::uint64_t generation = 0) : texture_{s.texels.data(), s.width, s.height, generation} {}
    const MetricTexture* texture(std::uint32_t address, int width, int height) override {
        return address == texture_address && width == texture_.width && height == texture_.height ? &texture_ : nullptr;
    }
private:
    MetricTexture texture_;
};

MetricVertex vertex(float x, float y, float s, float t, std::uint8_t pos_row = 0) {
    MetricVertex v;
    v.position = {x, y, 0.0f};
    v.texcoords[0] = {s, t};
    v.matrix_rows.fill(60);
    v.matrix_rows[0] = pos_row;
    return v;
}

// drawTex: GX_QUADS over the w x h rectangle with texture coordinates 0..1, as two triangles.
MetricCount count_quad(Setup& s, float w, float h) {
    const MetricVertex v[4] = {vertex(0, 0, 0, 0), vertex(w, 0, 1, 0), vertex(w, h, 1, 1), vertex(0, h, 0, 1)};
    const std::uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    MetricRegisters r{s.bp.data(), s.tev, s.xf.data()};
    Textures textures(s);
    MetricCount count;
    count_triangles(r, v, 4, indices, 6, textures, count);
    return count;
}

// The layer counter's TEV (PollutionCount.cpp initGXforPollutionLayer): REG0 holds the step, REG1
// the threshold.
void layer_tev(Setup& s, int type, int threshold, int step) {
    s.tev_register(1, step);
    s.tev_register(2, threshold);
    const std::uint32_t compare = 3u << 16 | 1u << 19;  // TB_COMPARE, GT, clamp, R8, TEVPREV
    if (type == 0) {
        // Below the threshold the texel shrinks by the step: COMP_R8_GT(C1, TEXC, C0, 0), then TEXC - CPREV.
        s.bp[0x00] = 1 | 1u << 10;
        s.bp[0x28] = 1u << 6 | 1u << 18;
        s.bp[0xc0] = 15u | 2u << 4 | 8u << 8 | 4u << 12 | compare;
        s.bp[0xc1] = 7u << 4 | 1u << 7 | 4u << 10 | 2u << 13 | compare;
        s.bp[0xc2] = 8u | 15u << 4 | 15u << 8 | 0u << 12 | 1u << 18 | 1u << 19;
        s.bp[0xc3] = 4u << 4 | 7u << 7 | 7u << 10 | 0u << 13 | 1u << 18 | 1u << 19;
    } else if (type == 7) {
        // Above the threshold the texel grows: COMP_R8_GT(TEXC, C1, C0, 0), then TEXC + CPREV.
        s.bp[0x00] = 1 | 1u << 10;
        s.bp[0x28] = 1u << 6 | 1u << 18;
        s.bp[0xc0] = 15u | 2u << 4 | 4u << 8 | 8u << 12 | compare;
        s.bp[0xc1] = 7u << 4 | 1u << 7 | 2u << 10 | 4u << 13 | compare;
        s.bp[0xc2] = 8u | 15u << 4 | 15u << 8 | 0u << 12 | 1u << 19;
        s.bp[0xc3] = 4u << 4 | 7u << 7 | 7u << 10 | 0u << 13 | 1u << 19;
    } else {
        // FLAG_DECAY: TEXC - C0 in one stage.
        s.bp[0xc0] = 8u | 15u << 4 | 15u << 8 | 2u << 12 | 1u << 18 | 1u << 19;
        s.bp[0xc1] = 4u << 4 | 7u << 7 | 7u << 10 | 1u << 13 | 1u << 18 | 1u << 19;
    }
}

void test_single_texel() {
    Setup s(8, 8);
    s.texel(3, 5, 7);  // below the diagonal: the second triangle, quad (2..3, 4..5)
    const MetricCount c = count_quad(s, 8, 8);
    EXPECT_EQ(c.unsupported == nullptr, true);
    EXPECT_EQ(c.quads, 1);
    EXPECT_EQ(c.triangles, 2);
    EXPECT_EQ(blend_input_pixels(c), 12u);
    EXPECT_EQ(blend_input_pixels(c) - 2 * 4, 4u);  // the layer counter's own subtraction
}

void test_clean_layer_reads_zero_after_subtraction() {
    Setup s(8, 8);
    const MetricCount c = count_quad(s, 8, 8);
    EXPECT_EQ(c.quads, 0);
    EXPECT_EQ(blend_input_pixels(c) - 2 * 4, 0u);
}

void test_quad_split_by_the_diagonal_counts_per_triangle() {
    Setup s(8, 8);
    s.texel(3, 2, 9);  // first triangle (y < x)
    s.texel(2, 3, 9);  // second triangle, same 2x2 quad
    EXPECT_EQ(count_quad(s, 8, 8).quads, 2);
    Setup t(8, 8);
    t.texel(3, 2, 9);
    t.texel(2, 2, 9);  // on the shared edge; one triangle owns it
    EXPECT_EQ(count_quad(t, 8, 8).quads, 1);
}

void test_full_layer() {
    Setup s(8, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) s.texel(x, y, 200);
    // 16 quads, the 4 on the diagonal once per triangle.
    EXPECT_EQ(count_quad(s, 8, 8).quads, 20);
}

void test_stretched_texture() {
    // A 4x4 texture over an 8x8 quad: texel (1, 1) covers pixels 2..3 x 2..3, a diagonal quad.
    Setup s(4, 4);
    s.bp[0x20] = (342u << 12) | 342u;
    s.bp[0x21] = (std::uint32_t(342 + 7) << 12) | std::uint32_t(342 + 7);
    put_float(s.xf, 0x101a, 4.0f);
    put_float(s.xf, 0x101b, -4.0f);
    put_float(s.xf, 0x101d, 346.0f);
    put_float(s.xf, 0x101e, 346.0f);
    put_float(s.xf, 0x1020, 2.0f / 8);
    put_float(s.xf, 0x1022, -2.0f / 8);
    s.texel(1, 1, 30);
    EXPECT_EQ(count_quad(s, 8, 8).quads, 2);
    s.texel(1, 1, 0);
    s.texel(3, 0, 30);  // pixels 6..7 x 0..1: first triangle only
    EXPECT_EQ(count_quad(s, 8, 8).quads, 1);
}

void test_layer_threshold_type() {
    // Threshold 50, step 10: texels 11, 49, 50, 51 and 255 stay above zero; 0, 5 and 10 do not.
    Setup s(16, 16);
    layer_tev(s, 0, 50, 10);
    const int values[8] = {0, 5, 10, 11, 49, 50, 51, 255};
    const int at[8][2] = {{0, 2}, {0, 4}, {0, 6}, {0, 8}, {0, 10}, {0, 12}, {0, 14}, {2, 14}};
    for (int i = 0; i < 8; ++i) s.texel(at[i][0], at[i][1], values[i]);
    const MetricCount c = count_quad(s, 16, 16);
    EXPECT_EQ(c.unsupported == nullptr, true);
    EXPECT_EQ(c.quads, 5);
}

void test_layer_grow_and_decay_types() {
    Setup grow(8, 8);
    layer_tev(grow, 7, 100, 4);
    grow.texel(0, 4, 1);
    grow.texel(0, 6, 150);
    EXPECT_EQ(count_quad(grow, 8, 8).quads, 2);
    Setup decay(8, 8);
    layer_tev(decay, 1, 0, 2);  // FLAG_DECAY forces step 2
    decay.texel(0, 2, 1);
    decay.texel(0, 4, 2);
    decay.texel(0, 6, 3);
    EXPECT_EQ(count_quad(decay, 8, 8).quads, 1);
}

void test_object_transform() {
    // TPollutionCounterObj: PNMTX0 = scale 1/32, rotation -90 degrees about X, translation -min/32,
    // so world (x, y, z) lands on pixel ((x - minX) / 32, (z - minZ) / 32).
    Setup s(8, 8);
    const float min_x = 640, min_z = -320;
    for (int i = 0; i < 12; ++i) put_float(s.xf, std::size_t(i), 0.0f);
    put_float(s.xf, 0, 1.0f / 32);
    put_float(s.xf, 3, -min_x / 32);
    put_float(s.xf, 6, 1.0f / 32);
    put_float(s.xf, 7, -min_z / 32);
    MetricVertex v[3];
    const float world[3][3] = {{min_x, 0, min_z}, {min_x + 256, 100, min_z}, {min_x, -50, min_z + 256}};
    const float uv[3][2] = {{0, 0}, {1, 0}, {0, 1}};
    for (int i = 0; i < 3; ++i) {
        v[i] = vertex(0, 0, uv[i][0], uv[i][1]);
        v[i].position = {world[i][0], world[i][1], world[i][2]};
    }
    s.texel(1, 1, 60);  // inside the triangle
    s.texel(6, 6, 60);  // outside it
    const std::uint16_t indices[3] = {0, 1, 2};
    MetricRegisters r{s.bp.data(), s.tev, s.xf.data()};
    Textures textures(s);
    MetricCount c;
    count_triangles(r, v, 3, indices, 3, textures, c);
    EXPECT_EQ(c.unsupported == nullptr, true);
    EXPECT_EQ(c.quads, 1);
    EXPECT_EQ(c.triangles, 1);
}

void test_scissor() {
    Setup s(8, 8);
    s.texel(5, 6, 99);
    s.bp[0x21] = (std::uint32_t(342 + 3) << 12) | std::uint32_t(342 + 7);  // columns 0..3 only
    EXPECT_EQ(count_quad(s, 8, 8).quads, 0);
    s.texel(1, 6, 99);
    EXPECT_EQ(count_quad(s, 8, 8).quads, 1);
}

void test_clamped_coordinates() {
    // Coordinates past the texture's edge read its edge texels under GX_CLAMP.
    Setup s(8, 8);
    s.texel(7, 7, 10);
    const MetricVertex v[3] = {vertex(0, 6, 1.5f, 1.5f), vertex(2, 8, 1.5f, 1.5f), vertex(0, 8, 1.5f, 1.5f)};
    const std::uint16_t indices[3] = {0, 1, 2};
    MetricRegisters r{s.bp.data(), s.tev, s.xf.data()};
    Textures textures(s);
    MetricCount c;
    count_triangles(r, v, 3, indices, 3, textures, c);
    EXPECT_EQ(c.quads, 1);
}

void test_unsupported_state() {
    Setup s(8, 8);
    s.bp[0x40] = 1;  // depth test
    s.texel(1, 5, 4);
    const MetricCount c = count_quad(s, 8, 8);
    EXPECT_EQ(c.unsupported != nullptr, true);
    EXPECT_EQ(c.triangles, 2);
    Setup t(8, 8);
    t.bp[0xc1] = 7u << 4 | 7u << 7 | 7u << 10 | 5u << 13 | 1u << 19;  // RASA
    EXPECT_EQ(count_quad(t, 8, 8).unsupported != nullptr, true);
}

void test_decode_i8() {
    std::vector<std::uint8_t> tiled(16 * 8), rows(16 * 8);
    for (std::size_t i = 0; i < tiled.size(); ++i) tiled[i] = std::uint8_t(i);
    decode_i8(tiled.data(), 16, 8, rows.data());
    EXPECT_EQ(rows[0], 0);           // tile 0, row 0
    EXPECT_EQ(rows[1 * 16 + 0], 8);  // tile 0, row 1
    EXPECT_EQ(rows[0 * 16 + 8], 32); // tile 1
    EXPECT_EQ(rows[4 * 16 + 0], 64); // tile 2 (second tile row)
    EXPECT_EQ(rows[7 * 16 + 15], 127);
}

void test_primitive_triangles() {
    EXPECT_EQ(primitive_triangles(0x80, 8), 4u);
    EXPECT_EQ(primitive_triangles(0x90, 9), 3u);
    EXPECT_EQ(primitive_triangles(0x98, 10), 8u);
    EXPECT_EQ(primitive_triangles(0xa0, 5), 3u);
    EXPECT_EQ(primitive_triangles(0xa8, 4), 0u);
}

void test_cache_key() {
    // The key follows what the count reads and ignores the rest of the GX state.
    Setup s(8, 8);
    layer_tev(s, 0, 50, 10);
    const MetricVertex v[4] = {vertex(0, 0, 0, 0), vertex(8, 0, 1, 0), vertex(8, 8, 1, 1), vertex(0, 8, 0, 1)};
    const std::uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    const auto key = [&](std::uint64_t generation) {
        MetricRegisters r{s.bp.data(), s.tev, s.xf.data()};
        Textures textures(s, generation);
        return MetricDraw(r, textures).key(v, 4, indices, 6);
    };
    const std::uint64_t base = key(1);
    s.bp[0x8d] = 0x12345;                 // another map's image
    s.bp[0x57] = 0x57000042;              // the counter's own clear
    s.tev[1][0] = 0x7f07f;                // a konst colour no stage reads
    put_float(s.xf, 0x100a, 3.0f);        // a channel colour
    put_float(s.xf, 0x500, 2.0f);         // a post-transform matrix without dual texturing
    EXPECT_EQ(key(1) == base, true);
    EXPECT_EQ(key(2) == base, false);     // the texture changed
    s.tev_register(2, 51);                // a threshold above the step passes the same texels
    EXPECT_EQ(key(1) == base, true);
    s.tev_register(1, 11);                // the step: texel 11 no longer passes
    EXPECT_EQ(key(1) == base, false);
    s.tev_register(1, 10);
    put_float(s.xf, 0x1020, 1.0f / 8);    // the projection
    EXPECT_EQ(key(1) == base, false);
}

// Per-pixel reference of the coverage rule (Dolphin's 28.4 rounding, top-left fill) for
// screen-space triangles over a texture sampled 1:1, counting quads per triangle.
std::uint64_t reference_quads(const std::vector<std::uint8_t>& tex, int size, const float (*xy)[2], int triangles) {
    const auto iround = [](float x) { const int t = int(x); return (x - float(t)) >= 0.5f ? t + 1 : t; };
    std::uint64_t quads = 0;
    for (int t = 0; t < triangles; ++t) {
        std::int64_t X[3], Y[3];
        for (int k = 0; k < 3; ++k) { X[k] = iround(16.0f * xy[3 * t + k][0]) - 9; Y[k] = iround(16.0f * xy[3 * t + k][1]) - 9; }
        const std::int64_t area = (Y[0] - Y[1]) * (X[0] - X[2]) - (X[0] - X[1]) * (Y[0] - Y[2]);
        if (area == 0) continue;
        if (area < 0) { std::swap(X[1], X[2]); std::swap(Y[1], Y[2]); }
        std::int64_t C[3], DX[3], DY[3];
        for (int e = 0; e < 3; ++e) {
            const int a = e, b = (e + 1) % 3;
            DX[e] = X[a] - X[b]; DY[e] = Y[a] - Y[b];
            C[e] = DY[e] * X[a] - DX[e] * Y[a];
            if (DY[e] < 0 || (DY[e] == 0 && DX[e] > 0)) ++C[e];
        }
        for (int by = 0; by < size; by += 2)
            for (int bx = 0; bx < size; bx += 2) {
                bool pass = false;
                for (int p = 0; p < 4 && !pass; ++p) {
                    const int x = bx + (p & 1), y = by + (p >> 1);
                    bool in = true;
                    for (int e = 0; e < 3; ++e) in = in && C[e] + DX[e] * (std::int64_t(y) << 4) - DY[e] * (std::int64_t(x) << 4) > 0;
                    pass = in && tex[std::size_t(y * size + x)] > 0;
                }
                quads += pass;
            }
    }
    return quads;
}

void test_random_triangles_match_reference() {
    const int size = 64;
    Setup s(size, size);
    std::uint32_t seed = 12345;
    const auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (auto& t : s.texels) t = (next() % 7 == 0) ? std::uint8_t(1 + next() % 255) : 0;
    const int triangles = 400;
    std::vector<float> xy(std::size_t(triangles) * 6);
    for (float& c : xy) c = float(next() % 7000) / 100.0f - 3.0f;  // -3 .. 67, past the edges too
    std::vector<MetricVertex> v(std::size_t(triangles) * 3);
    std::vector<std::uint16_t> indices(v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = vertex(xy[2 * i], xy[2 * i + 1], xy[2 * i] / size, xy[2 * i + 1] / size);
        indices[i] = std::uint16_t(i);
    }
    MetricRegisters r{s.bp.data(), s.tev, s.xf.data()};
    Textures textures(s);
    MetricCount c;
    count_triangles(r, v.data(), v.size(), indices.data(), indices.size(), textures, c);
    // The scissor keeps both inside the 64x64 pass, where each pixel samples its own texel.
    EXPECT_EQ(c.unsupported == nullptr, true);
    EXPECT_EQ(c.quads, reference_quads(s.texels, size, reinterpret_cast<const float(*)[2]>(xy.data()), triangles));
}

}  // namespace

int main() {
    test_single_texel();
    test_clean_layer_reads_zero_after_subtraction();
    test_quad_split_by_the_diagonal_counts_per_triangle();
    test_full_layer();
    test_stretched_texture();
    test_layer_threshold_type();
    test_layer_grow_and_decay_types();
    test_object_transform();
    test_scissor();
    test_clamped_coordinates();
    test_unsupported_state();
    test_decode_i8();
    test_primitive_triangles();
    test_cache_key();
    test_random_triangles_match_reference();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("pixel metric tests passed\n");
    return 0;
}
