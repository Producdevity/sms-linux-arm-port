// The pixel engine's blend-input counter (GXReadPixMetric's clr_pixels_in), counted on the CPU for
// the draws between a GXClearPixMetric and the next token. Starlet does not model the counter and
// GLES 3 has no sample-count queries, so this rasterizes those draws itself from the decoded GX
// state. It covers what Super Mario Sunshine's goop counters draw: triangles without depth test or
// indirect stages, I8 textures with nearest sampling, TEV stages without rasterized colour. Other
// state is reported as unsupported instead of being counted.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace sms::graphics {

// The GX registers a counted draw reads, as the command decoder holds them.
struct MetricRegisters {
    const std::uint32_t* bp = nullptr;                   // 256 BP registers
    std::array<std::array<std::uint32_t, 8>, 2> tev{};   // [konst bank][0xE0..0xE7 - 0xE0], low 23 bits
    const std::uint32_t* xf = nullptr;                   // 0x1058 XF words: memory, then registers at 0x1000
};

struct MetricVertex {
    std::array<float, 3> position{};
    std::array<std::array<float, 2>, 8> texcoords{};
    // XF matrix row of the position (0) and of each texture coordinate (1..8).
    std::array<std::uint8_t, 9> matrix_rows{};
};

// An I8 image's texels in row order; generation changes whenever they do.
struct MetricTexture {
    const std::uint8_t* texels = nullptr;
    int width = 0, height = 0;
    std::uint64_t generation = 0;
};

class MetricTextures {
public:
    virtual ~MetricTextures() = default;
    // The texels of the I8 image at this GX address, or null when unknown.
    virtual const MetricTexture* texture(std::uint32_t address, int width, int height) = 0;
};

struct MetricCount {
    // Quads (2x2 pixel blocks) of each triangle with a pixel that passes the alpha test.
    std::uint64_t quads = 0;
    std::uint64_t triangles = 0;
    // The first state the model does not cover; the counts are then incomplete.
    const char* unsupported = nullptr;
};

// Triangles a GX primitive (opcode without its VAT bits) of this many vertices draws.
unsigned primitive_triangles(std::uint8_t primitive, unsigned vertices);

// Alpha-test tables by TEV state, kept between draws by the caller.
using MetricTables = std::unordered_map<std::uint64_t, std::array<bool, 256>>;

// One draw's GX state, decoded once: textures are looked up here.
class MetricDraw {
public:
    MetricDraw(const MetricRegisters& registers, MetricTextures& textures, MetricTables* tables = nullptr);
    ~MetricDraw();
    MetricDraw(const MetricDraw&) = delete;
    MetricDraw& operator=(const MetricDraw&) = delete;
    // The first state the model does not cover, or null.
    const char* unsupported() const;
    // Equal keys give equal counts: a hash of the decoded state the count reads (the alpha test as
    // a table of the texel when one texture is read), the XF rows and vertex attributes it uses,
    // and the textures' generations.
    std::uint64_t key(const MetricVertex* vertices, std::size_t vertex_count, const std::uint16_t* indices,
                      std::size_t index_count) const;
    // Rasterizes the triangles (index triples into vertices) as the GX would and adds their
    // passing quads and their number to count.
    void count(const MetricVertex* vertices, std::size_t vertex_count, const std::uint16_t* indices,
               std::size_t index_count, MetricCount& count) const;

private:
    struct Plan;
    std::unique_ptr<Plan> plan_;
};

// MetricDraw(registers, textures).count(...).
void count_triangles(const MetricRegisters& registers, const MetricVertex* vertices, std::size_t vertex_count,
                     const std::uint16_t* indices, std::size_t index_count, MetricTextures& textures,
                     MetricCount& count);

// The register value clr_pixels_in reads back for count: GXReadPixMetric multiplies the counter
// (quads) by 4. Each triangle adds one quad, which the game subtracts itself.
inline std::uint32_t blend_input_pixels(const MetricCount& count) {
    return std::uint32_t((count.quads + count.triangles) * 4);
}

// GX tiled I8 (8x4 texel blocks of 32 bytes) to rows; width and height are multiples of 8 and 4.
void decode_i8(const std::uint8_t* tiled, int width, int height, std::uint8_t* rows);

}
