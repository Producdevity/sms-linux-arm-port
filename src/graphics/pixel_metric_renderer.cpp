#include "graphics/pixel_metric_renderer.hpp"

#include "graphics/pixel_metric.hpp"
#include "starlet/gx/pixel.hpp"
#include "starlet/gx/tev.hpp"
#include "starlet/gx/texture_copy.hpp"
#include "starlet/gx/vertex.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/sdk/gx.h"
#include "starlet/sdk/os.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace sms::graphics {
namespace {

using Clock = std::chrono::steady_clock;

// Super Mario Sunshine's counter tokens (MarDirectorSetup2.cpp): TPollutionCounterLayer
// 0x7e..0x91, TPollutionCounterObj 0x92..0xa5. Only their callbacks read the counter.
constexpr unsigned first_counter_token = 0x7e, last_counter_token = 0xa5;

struct IntervalResult {
    std::uint64_t quads = 0, triangles = 0;
    const char* unsupported = nullptr;
};

// Shared by the GPU thread, which publishes each counted interval under the token that ends it,
// and the guest's draw-sync interrupt, which takes it for that token's callback. Each token the
// GPU thread passes raises one interrupt, after its interval is published, so the two pair up.
struct Shared {
    std::mutex mutex;  // by_token
    std::array<std::deque<IntervalResult>, last_counter_token - first_counter_token + 1> by_token;
    // Guest side: the sequence for the next clear, and the draw-sync callback running now (on
    // the interrupt thread, with the token's interval when it ended one).
    std::uint32_t next_seq = 1;
    bool delivering = false, delivered = false;
    IntervalResult current;
    std::atomic<starlet::sdk::GXDrawSyncCallback> game_callback{nullptr};
    // Development (SMS_GOOP_LOG): counted intervals a callback read.
    std::atomic<std::uint64_t> reads{0};
};

Shared& shared() {
    static Shared s;
    return s;
}

bool log_enabled() {
    static const bool on = std::getenv("SMS_GOOP_LOG") != nullptr;
    return on;
}
// Development: every goop copy the CPU computes is rendered and read back by the GPU as well and
// compared, waiting for the readback.
bool verify_enabled() {
    static const bool on = std::getenv("SMS_GOOP_VERIFY") != nullptr;
    return on;
}
// One CPU-computed goop copy in this many per texture is checked against the GPU's.
constexpr unsigned copies_per_check = 150;
std::uint64_t mix(std::uint64_t h, std::uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

void note_once(const char* what) {
    static std::mutex mutex;
    static std::set<const char*> seen;
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.insert(what).second) {
        std::printf("pixel metric: %s\n", what);
        std::fflush(stdout);
    }
}

}  // namespace

// A CPU copy of an I8 texture the counted draws sample, decoded from its memory, which Starlet
// writes each copy back to (Renderer::write_back).
struct Shadow {
    std::uint32_t address = 0;
    int width = 0, height = 0;
    std::vector<std::uint8_t> texels;   // rows
    std::vector<std::uint8_t> memory;   // its memory bytes when last read
    MetricTexture metric;
    std::uint64_t generation = 0;       // advances whenever texels change
    std::uint64_t used_frame = 0, checked_frame = ~0ull, refreshed_frame = 0;
    // Copies into it (PixelMetricRenderer::Impl::copy): the last one Starlet reads back, and the
    // CPU result a readback is to confirm; a CPU result that differed ends CPU copies here.
    std::uint64_t last_write_back = 0;
    bool checking = false, cpu_off = false;
    unsigned since_check = 0;
    std::vector<std::uint8_t> next, prediction, input;
};

struct PixelMetricRenderer::Impl final : MetricTextures {
    starlet::gx_fifo::Renderer& inner;
    // GPU thread.
    std::uint32_t seq = 0;
    bool open = false;
    IntervalResult interval;
    // The decoder's state, as the last draw saw it, and its token register when the interval opened.
    const starlet::gx::State* state = nullptr;
    std::uint32_t open_token_word = 0;
    std::unordered_map<std::uint32_t, Shadow> shadows;
    std::unordered_map<std::uint64_t, std::uint64_t> cached_quads;
    MetricTables tables;
    std::uint64_t frame = 0, next_refresh_frame = 0;
    std::array<std::uint32_t, 256> bp{};
    std::vector<MetricVertex> vertices;
    // The counted draws repeat every frame: their vertices are decoded once.
    starlet::gx::VertexCache vertex_cache{starlet::gx::VertexCacheLimits{4u << 20, 256}};
    std::uint64_t intervals = 0, draws = 0, cache_hits = 0, refreshes = 0, stats_frame = 0;
    // The draws since the last copy, while a copy of them is a function of one texture's texels:
    // Super Mario Sunshine's goop pass (PollutionCount.cpp countTexDegree) without stamps is
    // drawBlack (one colour) and then the counted drawTex of the layer's map at 1:1 through the
    // layer's TEV, and its copy back into the map is a 256-entry table over the map's bytes. The
    // CPU makes that copy's bytes; Starlet keeps rendering it and reads nothing back. Stamps
    // (cleaning, regrowth, models) and anything else make the pass "other": Starlet reads it back.
    enum class Pass { fresh, black, base, other };
    Pass pass = Pass::fresh;
    starlet::gx::PixelColor black{};
    std::uint32_t pass_texture = 0;
    int pass_width = 0, pass_height = 0;
    std::uint64_t pass_key = 0;
    std::shared_ptr<const starlet::gx::TevMaterial> pass_material;
    std::shared_ptr<const starlet::gx::PixelSnapshot> pass_pixel;
    std::unordered_map<std::uint64_t, std::array<std::uint8_t, 256>> copy_tables;
    std::uint64_t cpu_copies = 0, gpu_copies = 0, checks = 0, differed = 0, unchecked = 0;
    // Why goop copies went to the GPU: the pass had other draws (stamps), an earlier copy was
    // still being read back, the copy did not match the pass, or it was a check.
    std::uint64_t gpu_stamped = 0, gpu_pending = 0, gpu_unmatched = 0, gpu_checks = 0;
    double count_ms = 0;
    Clock::time_point stats_at = Clock::now();

    explicit Impl(starlet::gx_fifo::Renderer& r) : inner(r) {}
    ~Impl() override = default;

    static const std::uint8_t* native(std::uint32_t address, std::size_t size) {
        const void* p = starlet::os::physical_to_native(address);
        return p && starlet::os::game_visible(p, size) ? static_cast<const std::uint8_t*>(p) : nullptr;
    }

    void reset_from_memory(Shadow& s, const std::uint8_t* bytes) {
        const std::size_t size = std::size_t(s.width) * std::size_t(s.height);
        s.memory.assign(bytes, bytes + size);
        decode_i8(bytes, s.width, s.height, s.texels.data());
        ++s.generation;
        s.refreshed_frame = frame;
    }
    void forget(std::unordered_map<std::uint32_t, Shadow>::iterator it) {
        inner.write_back(it->second.address, std::size_t(it->second.width) * std::size_t(it->second.height), false);
        shadows.erase(it);
    }

    // MetricTextures: the texture's CPU copy, from memory. Its copies are written back there (the
    // game's CPU reads them as well), so memory holds the texture the GPU samples, up to the copy
    // whose readback is still in flight. Each refresh recounts the draws that sample it: one
    // texture at a time, the stalest first, and texels / 16384 frames apart (16 for a 512 x 512
    // map), so the counts trail the goop by a fraction of a second.
    const MetricTexture* texture(std::uint32_t address, int width, int height) override {
        if (width % 8 || height % 4) return nullptr;
        const std::size_t size = std::size_t(width) * std::size_t(height);
        const std::uint8_t* bytes = native(address, size);
        if (!bytes) return nullptr;
        auto it = shadows.find(address);
        if (it != shadows.end() && (it->second.width != width || it->second.height != height)) {
            forget(it);
            it = shadows.end();
        }
        if (it == shadows.end()) {
            Shadow s;
            s.address = address;
            s.width = width;
            s.height = height;
            s.texels.resize(size);
            s.metric = {nullptr, width, height};
            it = shadows.emplace(address, std::move(s)).first;
            it->second.metric.texels = it->second.texels.data();
            reset_from_memory(it->second, bytes);
            it->second.checked_frame = frame;
            inner.write_back(address, size, true);
        } else if (it->second.checked_frame != frame) {
            it->second.checked_frame = frame;
            Shadow& s = it->second;
            bool stalest = true;
            for (const auto& [a, other] : shadows)
                if (a != address && other.used_frame + 4 >= frame && other.refreshed_frame < s.refreshed_frame) stalest = false;
            if (frame >= next_refresh_frame && stalest) {
                next_refresh_frame = frame + std::max<std::uint64_t>(4, size / 16384);
                if (std::memcmp(bytes, s.memory.data(), size) != 0) {
                    reset_from_memory(s, bytes);
                    ++refreshes;
                } else {
                    s.refreshed_frame = frame;
                }
            }
        }
        it->second.used_frame = frame;
        it->second.metric.generation = it->second.generation;
        return &it->second.metric;
    }

    // A token ended the open interval: publish it under that token for its callback.
    void close_interval(std::uint16_t token) {
        open = false;
        ++intervals;
        if (token < first_counter_token || token > last_counter_token) return;
        Shared& sh = shared();
        std::lock_guard<std::mutex> lock(sh.mutex);
        auto& queue = sh.by_token[token - first_counter_token];
        if (queue.size() >= 8) queue.pop_front();  // its interrupts stopped (none observed)
        queue.push_back(interval);
    }

    void count(const starlet::gx::State& state, const starlet::gx::Draw& draw, starlet::gx::MemoryReader& memory) {
        if (draw.merged()) {
            for (std::uint16_t i = 0; i < draw.run_count; ++i) {
                const starlet::gx::PrimitiveRun& run = draw.runs[i];
                starlet::gx::Draw one = draw;
                one.primitive = run.primitive;
                one.vertex_count = run.vertex_count;
                one.vertices = {draw.vertices.data + run.offset, std::size_t(run.vertex_count) * draw.layout.stride};
                one.runs = nullptr;
                one.run_count = 0;
                count(state, one, memory);
            }
            return;
        }
        ++draws;
        const auto captured = starlet::gx::capture_vertices(state, draw, memory, starlet::gx::VertexLimits{}, &vertex_cache);
        if (!captured) {
            interval.triangles += primitive_triangles(draw.primitive, draw.vertex_count);
            if (!interval.unsupported) interval.unsupported = "draw whose vertices cannot be read";
            return;
        }
        const auto& snapshot = *captured.snapshot;
        if (snapshot.topology() != starlet::gx::Topology::triangles) {
            if (!interval.unsupported) interval.unsupported = "lines or points";
            return;
        }
        const auto& expanded = snapshot.vertices();
        vertices.resize(expanded.size());
        for (std::size_t i = 0; i < expanded.size(); ++i) {
            vertices[i].position = expanded[i].position;
            vertices[i].texcoords = expanded[i].texcoords;
            vertices[i].matrix_rows = expanded[i].matrix_indices;
        }
        const auto& indices = snapshot.indices();
        for (unsigned i = 0; i < 256; ++i) bp[i] = state.bp(std::uint8_t(i));
        MetricRegisters registers;
        registers.bp = bp.data();
        registers.xf = state.xf_data();
        for (int bank = 0; bank < 2; ++bank)
            for (int half = 0; half < 8; ++half)
                registers.tev[std::size_t(bank)][std::size_t(half)] = state.tev_register(bank != 0, std::uint8_t(half));

        const MetricDraw decoded(registers, *this, &tables);
        const std::size_t triangles = indices.size() / 3;
        if (decoded.unsupported()) {
            interval.triangles += triangles;
            if (!interval.unsupported) interval.unsupported = decoded.unsupported();
            return;
        }
        const std::uint64_t key = decoded.key(vertices.data(), vertices.size(), indices.data(), indices.size());
        const auto hit = cached_quads.find(key);
        if (hit != cached_quads.end()) {
            ++cache_hits;
            interval.quads += hit->second;
            interval.triangles += triangles;
            return;
        }
        MetricCount result;
        decoded.count(vertices.data(), vertices.size(), indices.data(), indices.size(), result);
        interval.quads += result.quads;
        interval.triangles += result.triangles;
        if (result.unsupported) {
            if (!interval.unsupported) interval.unsupported = result.unsupported;
            return;
        }
        if (cached_quads.size() > 1024) cached_quads.clear();
        cached_quads.emplace(key, result.quads);
    }

    // Writes the source colour as it is (blend off or ONE/ZERO) into an RGB8 EFB without fog or Z texture.
    static bool writes_source(const starlet::gx::State& state, const starlet::gx::PixelSnapshot& pixel) {
        const auto& plan = pixel.plan();
        if (plan.format != starlet::gx::PixelFormat::rgb8 || !plan.color_write) return false;
        if (plan.operation == starlet::gx::PixelOperation::blend) {
            if (plan.source_factor != 1 || plan.destination_factor != 0) return false;
        } else if (plan.operation != starlet::gx::PixelOperation::copy) return false;
        return (state.bp(0x40) & 1) == 0 && ((state.bp(0xf1) >> 21) & 7) == 0 && (state.bp(0xf5) & 3) == 0;
    }
    static starlet::gx::TevColor colour_of(std::uint32_t c) {
        return {std::uint8_t(c >> 24), std::uint8_t(c >> 16), std::uint8_t(c >> 8), std::uint8_t(c)};
    }
    // drawBlack: no texture, one material colour, unlit.
    bool black_draw(const starlet::gx::State& state, const starlet::gx::Draw& draw) {
        const std::uint32_t gen = state.bp(0x00);
        if (draw.merged() || (gen & 0xf) != 0 || ((gen >> 16) & 7) != 0 || ((gen >> 4) & 7) == 0) return false;
        if ((state.xf(0x100e) & 3) != 0 || (state.xf(0x1010) & 3) != 0) return false;
        const auto tev = starlet::gx::capture_tev(state);
        const auto pixel = starlet::gx::capture_pixel(state, starlet::gx::PixelPolicy::candidate_v1);
        if (!tev || !pixel || !writes_source(state, *pixel.snapshot)) return false;
        starlet::gx::TevInputs in;
        in.raster[0] = colour_of(state.xf(0x100c));
        in.raster_valid = 1;
        const auto e = starlet::gx::evaluate_tev(tev.material->pipeline, tev.material->uniforms, in,
                                                 starlet::gx::TevArithmeticModel::fixed8_candidate_v1);
        if (!e || !e.alpha_pass) return false;
        const auto out = starlet::gx::evaluate_pixel(*pixel.snapshot, e.color, {0, 0, 0, 255}, 0, 0, true);
        if (!out) return false;
        black = out.codes;
        return true;
    }
    // drawTex: one nearest I8 texture, the counted interval.
    bool base_draw(const starlet::gx::State& state, const starlet::gx::Draw& draw) {
        const std::uint32_t gen = state.bp(0x00);
        if (!open || draw.merged() || (gen & 0xf) != 1 || ((gen >> 16) & 7) != 0) return false;
        const unsigned stages = ((gen >> 10) & 0xf) + 1;
        for (unsigned i = 0; i < stages; ++i) {
            const std::uint32_t order = state.bp(std::uint8_t(0x28 + i / 2)) >> (i & 1 ? 12 : 0);
            if ((order & 0x40) && (order & 0x3f) != 0) return false;  // texmap 0 through texcoord 0
        }
        const std::uint32_t image = state.bp(0x88), mode = state.bp(0x80);
        if (((image >> 20) & 0xf) != 1 || (mode & 0xf0) != 0) return false;
        const auto tev = starlet::gx::capture_tev(state);
        const auto pixel = starlet::gx::capture_pixel(state, starlet::gx::PixelPolicy::candidate_v1);
        if (!tev || !pixel || !writes_source(state, *pixel.snapshot)) return false;
        pass_texture = (state.bp(0x94) & 0xffffff) << 5;
        pass_width = int(image & 0x3ff) + 1;
        pass_height = int((image >> 10) & 0x3ff) + 1;
        pass_material = tev.material;
        pass_pixel = pixel.snapshot;
        std::uint64_t key = 0xcbf29ce484222325ull;
        for (unsigned a : {0x00u, 0x28u, 0x29u, 0x2au, 0x2bu, 0x2cu, 0x2du, 0x2eu, 0x2fu, 0x40u, 0x41u, 0x42u, 0x43u})
            key = mix(key, state.bp(std::uint8_t(a)));
        for (unsigned a = 0xc0; a < 0xe0; ++a) key = mix(key, state.bp(std::uint8_t(a)));
        for (unsigned a = 0xf3; a < 0xfe; ++a) key = mix(key, state.bp(std::uint8_t(a)));
        for (int bank = 0; bank < 2; ++bank)
            for (std::uint8_t half = 0; half < 8; ++half) key = mix(key, state.tev_register(bank != 0, half));
        pass_key = mix(key, std::uint64_t(black[0]) | std::uint64_t(black[1]) << 8 | std::uint64_t(black[2]) << 16 |
                                std::uint64_t(black[3]) << 24);
        return true;
    }
    void classify(const starlet::gx::State& state, const starlet::gx::Draw& draw) {
        if (pass == Pass::fresh) pass = black_draw(state, draw) ? Pass::black : Pass::other;
        else if (pass == Pass::black) pass = base_draw(state, draw) ? Pass::base : Pass::other;
        else pass = Pass::other;
    }
    // The copy's byte for each texel byte of the base draw's texture, through its TEV, alpha test
    // and blend over drawBlack's colour, then the copy's encoding; null where the model refuses.
    const std::array<std::uint8_t, 256>* copy_table(const starlet::gx::TextureCopy& copy) {
        const std::uint64_t key = mix(mix(pass_key, copy.format), std::uint64_t(copy.luminance) << 1 | std::uint64_t(copy.rgba6));
        const auto found = copy_tables.find(key);
        if (found != copy_tables.end()) return &found->second;
        starlet::gx::TextureCopy tile = copy;
        tile.x = 0;
        tile.y = 0;
        tile.width = 8;
        tile.height = 4;
        tile.stride = 32;
        tile.clear.reset();
        std::array<std::uint8_t, 32 * 4> rgba{};
        alignas(32) std::array<std::uint8_t, 32> encoded{};
        std::array<std::uint8_t, 256> table{};
        for (unsigned chunk = 0; chunk < 8; ++chunk) {
            for (unsigned i = 0; i < 32; ++i) {
                const auto v = std::uint8_t(chunk * 32 + i);
                starlet::gx::TevInputs in;
                in.texture.fill({v, v, v, v});
                in.texture_valid = 0xffff;
                const auto e = starlet::gx::evaluate_tev(pass_material->pipeline, pass_material->uniforms, in,
                                                         starlet::gx::TevArithmeticModel::fixed8_candidate_v1);
                if (!e) return nullptr;
                const auto out = starlet::gx::evaluate_pixel(*pass_pixel, e.color, black, 0, 0, e.alpha_pass);
                if (!out) return nullptr;
                // Bottom-to-top rows; pixel i of the tile is row i / 8, column i % 8.
                std::memcpy(&rgba[std::size_t((3 - i / 8) * 8 + i % 8) * 4], out.codes.data(), 4);
            }
            starlet::gx::encode_texture_copy(tile, {rgba.data(), rgba.size()}, encoded.data(), encoded.size());
            std::memcpy(&table[chunk * 32], encoded.data(), 32);
        }
        if (copy_tables.size() >= 64) copy_tables.clear();
        return &copy_tables.emplace(key, table).first->second;
    }
    void copy(const starlet::gx::TextureCopy& copy, std::uint8_t* destination, std::size_t bytes, std::uint32_t address) {
        const Pass step = pass;
        pass = Pass::fresh;
        const auto it = shadows.find(address);
        if (it == shadows.end()) {
            inner.copy_texture(copy, destination, bytes, address);
            return;
        }
        Shadow& s = it->second;
        const bool landed = s.last_write_back == 0 || inner.land_write_backs(s.last_write_back, verify_enabled());
        if (landed) s.last_write_back = 0;
        if (s.checking) {
            s.checking = false;
            if (!landed || s.prediction.size() != bytes) ++unchecked;
            else check(s, destination, bytes);
        }
        const bool single_byte = copy.format == 1 || copy.format == 7 || (copy.format >= 8 && copy.format <= 10);
        const bool matches = step == Pass::base && pass_texture == address && copy.x == 0 && copy.y == 0 &&
                             copy.width == pass_width && copy.height == pass_height && !copy.half_scale &&
                             !copy.filtered() && !copy.depth && !copy.strip && single_byte &&
                             s.width == pass_width && s.height == pass_height &&
                             bytes == std::size_t(s.width) * std::size_t(s.height) &&
                             copy.stride == std::size_t(s.width / 8) * 32;
        if (step != Pass::base) ++gpu_stamped;
        else if (!matches) ++gpu_unmatched;
        else if (!landed) ++gpu_pending;
        if (matches && landed && !s.cpu_off) {
            if (const auto* table = copy_table(copy)) {
                s.next.resize(bytes);
                for (std::size_t i = 0; i < bytes; ++i) s.next[i] = (*table)[destination[i]];
                if (verify_enabled() || ++s.since_check >= copies_per_check) {
                    // Starlet renders and reads this one back; the next copy compares.
                    s.since_check = 0;
                    ++gpu_checks;
                    s.prediction.swap(s.next);
                    if (verify_enabled()) s.input.assign(destination, destination + bytes);
                    s.checking = true;
                } else {
                    // The counts read memory at their own pace (texture()), as for copies Starlet makes.
                    inner.copy_texture_with_contents(copy, destination, bytes, address, s.next.data());
                    ++cpu_copies;
                    return;
                }
            }
        }
        const std::uint64_t before = inner.write_backs_issued();
        inner.copy_texture(copy, destination, bytes, address);
        if (inner.write_backs_issued() != before) s.last_write_back = inner.write_backs_issued();
        ++gpu_copies;
    }
    void check(Shadow& s, const std::uint8_t* memory, std::size_t bytes) {
        ++checks;
        std::size_t differ = 0;
        for (std::size_t i = 0; i < bytes; ++i) differ += memory[i] != s.prediction[i];
        if (!differ) return;
        ++differed;
        if (verify_enabled()) {
            int shown = 0;
            for (std::size_t i = 0; i < bytes && shown < 4; ++i)
                if (memory[i] != s.prediction[i]) {
                    std::printf("pixel metric: goop copy 0x%08x byte %zu: texel %u, CPU %u, GPU %u (%zu bytes differ)\n",
                                s.address, i, i < s.input.size() ? unsigned(s.input[i]) : 0u, unsigned(s.prediction[i]),
                                unsigned(memory[i]), differ);
                    ++shown;
                }
            std::fflush(stdout);
            return;
        }
        s.cpu_off = true;
        std::printf("pixel metric: CPU goop copy into 0x%08x differs from the GPU's in %zu of %zu bytes; the GPU's from now on\n",
                    s.address, differ, bytes);
        std::fflush(stdout);
    }
    void frame_end() {
        pass = Pass::fresh;
        ++frame;
        if (frame % 600 == 0) {
            for (auto it = shadows.begin(); it != shadows.end();) {
                const auto next = std::next(it);
                if (it->second.used_frame + 600 < frame) forget(it);
                it = next;
            }
        }
        if (log_enabled() && Clock::now() - stats_at > std::chrono::seconds(10)) {
            stats_at = Clock::now();
            std::printf("pixel metric: %llu intervals, %llu read, %llu draws (%llu cached), %.2f ms counting per frame, "
                        "%zu textures, %llu refreshes; goop copies %llu CPU, %llu GPU (%llu stamped, %llu pending, %llu unmatched, %llu checks), %llu checked (%llu differed, %llu not read back)\n",
                        (unsigned long long)intervals, (unsigned long long)shared().reads.load(std::memory_order_relaxed),
                        (unsigned long long)draws, (unsigned long long)cache_hits,
                        frame > stats_frame ? count_ms / double(frame - stats_frame) : 0.0, shadows.size(),
                        (unsigned long long)refreshes, (unsigned long long)cpu_copies, (unsigned long long)gpu_copies,
                        (unsigned long long)gpu_stamped, (unsigned long long)gpu_pending, (unsigned long long)gpu_unmatched,
                        (unsigned long long)gpu_checks, (unsigned long long)checks, (unsigned long long)differed, (unsigned long long)unchecked);
            count_ms = 0;
            stats_frame = frame;
            std::fflush(stdout);
        }
    }
};

PixelMetricRenderer::PixelMetricRenderer(starlet::gx_fifo::Renderer& inner)
    : inner_(inner), impl_(std::make_unique<Impl>(inner)) {}

PixelMetricRenderer::~PixelMetricRenderer() = default;

void PixelMetricRenderer::draw(const starlet::gx::State& state, const starlet::gx::Draw& draw,
                               starlet::gx::MemoryReader& memory) {
    Impl& g = *impl_;
    g.state = &state;
    const std::uint32_t seq = state.bp(0x57) & 0xffffff;
    if (seq != 0 && seq != g.seq) {
        // A new clear: the interval before it, if no token ended it, is never read.
        g.seq = seq;
        g.open = true;
        g.interval = {};
        g.open_token_word = state.bp(0x48);
    }
    if (g.open) {
        const auto start = Clock::now();
        // Counting never refuses or changes the draw.
        try {
            g.count(state, draw, memory);
        } catch (const std::exception&) {
            if (!g.interval.unsupported) g.interval.unsupported = "exception while counting a draw";
        }
        g.count_ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    if (g.pass != Impl::Pass::other) g.classify(state, draw);
    inner_.draw(state, draw, memory);
}

void PixelMetricRenderer::wait_until_idle() {
    // Draw-done and tokens. The decoder has applied the token's BP write already; a draw-done
    // leaves the token register as it was and the interval open.
    Impl& g = *impl_;
    if (g.open && g.state && g.state->bp(0x48) != g.open_token_word)
        g.close_interval(std::uint16_t(g.state->bp(0x48) & 0xffff));
    inner_.wait_until_idle();
}

void PixelMetricRenderer::copy_display(const starlet::gx::DisplayCopy& copy, std::uint8_t* destination, std::size_t bytes) {
    impl_->frame_end();
    inner_.copy_display(copy, destination, bytes);
}

void PixelMetricRenderer::copy_texture(const starlet::gx::TextureCopy& copy, std::uint8_t* destination, std::size_t bytes,
                                       std::uint32_t address) {
    impl_->copy(copy, destination, bytes, address);
}

std::uint32_t PixelMetricRenderer::peek_depth(int x, int y) { return inner_.peek_depth(x, y); }
std::uint32_t PixelMetricRenderer::peek_color(int x, int y) { return inner_.peek_color(x, y); }
bool PixelMetricRenderer::resident_in(const void* data, std::size_t bytes) const { return inner_.resident_in(data, bytes); }
void PixelMetricRenderer::before_dma_read(const void* data, std::size_t bytes) { inner_.before_dma_read(data, bytes); }
void PixelMetricRenderer::before_dma_write(const void* data, std::size_t bytes) { inner_.before_dma_write(data, bytes); }
void PixelMetricRenderer::write_back(std::uint32_t address, std::size_t bytes, bool on) { inner_.write_back(address, bytes, on); }
std::uint64_t PixelMetricRenderer::write_backs_issued() const { return inner_.write_backs_issued(); }
bool PixelMetricRenderer::land_write_backs(std::uint64_t through, bool wait) { return inner_.land_write_backs(through, wait); }
int PixelMetricRenderer::snapshot_efb(const starlet::gx::EfbRegion& region) { return inner_.snapshot_efb(region); }
int PixelMetricRenderer::snapshot_efb_exact(const starlet::gx::EfbRegion& region) { return inner_.snapshot_efb_exact(region); }
bool PixelMetricRenderer::snapshot_ready(int index) const { return inner_.snapshot_ready(index); }
void PixelMetricRenderer::wait_snapshot(int index) { inner_.wait_snapshot(index); }
bool PixelMetricRenderer::peek_depth_snapshot(int index, int x, int y, std::uint32_t& value) const {
    return inner_.peek_depth_snapshot(index, x, y, value);
}
bool PixelMetricRenderer::peek_color_snapshot(int index, int x, int y, std::uint32_t& value) const {
    return inner_.peek_color_snapshot(index, x, y, value);
}
void PixelMetricRenderer::select_snapshot(int index) { inner_.select_snapshot(index); }
void PixelMetricRenderer::release_snapshot(int index) { inner_.release_snapshot(index); }
void PixelMetricRenderer::retain_snapshot(int index) { inner_.retain_snapshot(index); }
int PixelMetricRenderer::request_depth_readback() { return inner_.request_depth_readback(); }
bool PixelMetricRenderer::depth_readback_value(int handle, int x, int y, std::uint32_t& value) const {
    return inner_.depth_readback_value(handle, x, y, value);
}
void PixelMetricRenderer::wait_depth_readback(int handle) { inner_.wait_depth_readback(handle); }
void PixelMetricRenderer::release_depth_readback(int handle) { inner_.release_depth_readback(handle); }
void PixelMetricRenderer::set_frame_ahead(bool ahead) { inner_.set_frame_ahead(ahead); }
bool PixelMetricRenderer::present(const starlet::gx::FramebufferView& frame, int drawable_width, int drawable_height, bool black) {
    return inner_.present(frame, drawable_width, drawable_height, black);
}
std::vector<std::uint8_t> PixelMetricRenderer::readback() { return inner_.readback(); }
void PixelMetricRenderer::print_stats(const starlet::gx_fifo::GpuCounters& gpu) { inner_.print_stats(gpu); }

}

namespace {

// Wraps the game's draw-sync callback (registered once, at TApplication::initialize) so that
// GXReadPixMetric knows which token's callback is running.
void metric_draw_sync(std::uint16_t token) {
    auto& sh = sms::graphics::shared();
    sh.delivering = true;
    sh.delivered = false;
    if (token >= sms::graphics::first_counter_token && token <= sms::graphics::last_counter_token) {
        std::lock_guard<std::mutex> lock(sh.mutex);
        auto& queue = sh.by_token[token - sms::graphics::first_counter_token];
        if (!queue.empty()) {
            sh.current = queue.front();
            queue.pop_front();
            sh.delivered = true;
        }
    }
    if (const auto game = sh.game_callback.load(std::memory_order_acquire)) game(token);
    sh.delivering = false;
}

}  // namespace

extern "C" unsigned sunshine_pixel_metric_clear(void) {
    auto& sh = sms::graphics::shared();
    if (!sh.game_callback.load(std::memory_order_acquire)) {
        // No draw-sync interrupt runs between taking the game's callback and storing it.
        const starlet::sdk::BOOL enabled = OSDisableInterrupts();
        const starlet::sdk::GXDrawSyncCallback game = GXSetDrawSyncCallback(metric_draw_sync);
        if (game && game != metric_draw_sync) sh.game_callback.store(game, std::memory_order_release);
        else GXSetDrawSyncCallback(game);
        OSRestoreInterrupts(enabled);
    }
    const std::uint32_t seq = sh.next_seq;
    sh.next_seq = (sh.next_seq + 1) & 0xffffff;
    if (sh.next_seq == 0) sh.next_seq = 1;
    return 0x57000000u | seq;
}

extern "C" unsigned sunshine_pixel_metric_read(void) {
    auto& sh = sms::graphics::shared();
    if (!sh.delivering) {
        sms::graphics::note_once("GXReadPixMetric outside a draw-sync callback; reads 0");
        return 0;
    }
    sms::graphics::IntervalResult result;
    if (sh.delivered) {
        result = sh.current;  // otherwise the interval drew nothing
        sh.reads.fetch_add(1, std::memory_order_relaxed);
    }
    if (result.unsupported) {
        sms::graphics::note_once(result.unsupported);
        return 0xffffffffu;
    }
    return sms::graphics::blend_input_pixels({result.quads, result.triangles, nullptr});
}
