#include "graphics/pixel_metric_renderer.hpp"

#include "graphics/pixel_metric.hpp"
#include "starlet/gx/vertex.hpp"
#include "starlet/os/game_memory.hpp"
#include "starlet/os/thread_name.hpp"
#include "starlet/sdk/gx.h"
#include "starlet/sdk/os.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <set>
#include <thread>
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
};

Shared& shared() {
    static Shared s;
    return s;
}

bool log_enabled() {
    static const bool on = std::getenv("SMS_GOOP_LOG") != nullptr;
    return on;
}

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

// A CPU copy of an I8 texture the counted draws sample.
struct Shadow {
    std::uint32_t address = 0;
    int width = 0, height = 0;
    std::vector<std::uint8_t> texels;   // rows
    std::vector<std::uint8_t> memory;   // its memory bytes when last read or copied to
    MetricTexture metric;
    std::uint64_t generation = 0;       // advances whenever texels change
    std::uint64_t epoch = 0;            // advances when texels are reset from memory
    std::uint64_t used_frame = 0, checked_frame = ~0ull, refreshed_frame = 0;
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
    double count_ms = 0;
    Clock::time_point stats_at = Clock::now();

    // One refresh at a time: the GPU thread takes the snapshot, the worker reads it.
    struct Job {
        bool active = false, done = false, failed = false;
        std::uint32_t address = 0;
        std::uint64_t epoch = 0;
        int snapshot = -1;
        starlet::gx::EfbRegion region;
        int channel = 0;  // 0..3: R, G, B, A; 4: luminance
        std::vector<std::uint8_t> rows;
        double peek_ms = 0;
    };
    std::mutex job_mutex;
    std::condition_variable job_cv;
    Job job;
    std::atomic<bool> job_finished{false};
    bool stopping = false;
    double peek_ms_total = 0;
    std::thread worker;

    explicit Impl(starlet::gx_fifo::Renderer& r) : inner(r) {
        worker = std::thread([this] { run_worker(); });
    }
    ~Impl() override {
        {
            std::lock_guard<std::mutex> lock(job_mutex);
            stopping = true;
        }
        job_cv.notify_all();
        if (worker.joinable()) worker.join();
    }

    static const std::uint8_t* native(std::uint32_t address, std::size_t size) {
        const void* p = starlet::os::physical_to_native(address);
        return p && starlet::os::game_visible(p, size) ? static_cast<const std::uint8_t*>(p) : nullptr;
    }

    void reset_from_memory(Shadow& s, const std::uint8_t* bytes) {
        const std::size_t size = std::size_t(s.width) * std::size_t(s.height);
        s.memory.assign(bytes, bytes + size);
        decode_i8(bytes, s.width, s.height, s.texels.data());
        ++s.generation;
        ++s.epoch;
    }

    // MetricTextures: the texture's CPU copy, created from memory on first use. Memory that changed
    // since the copy was last synchronised was rewritten by the CPU (a stage reload) or materialized
    // by Starlet; either way it is what the GPU samples next.
    const MetricTexture* texture(std::uint32_t address, int width, int height) override {
        if (width % 8 || height % 4) return nullptr;
        const std::size_t size = std::size_t(width) * std::size_t(height);
        const std::uint8_t* bytes = native(address, size);
        if (!bytes) return nullptr;
        auto it = shadows.find(address);
        if (it != shadows.end() && (it->second.width != width || it->second.height != height)) {
            shadows.erase(it);
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
        } else if (it->second.checked_frame != frame) {
            it->second.checked_frame = frame;
            if (std::memcmp(bytes, it->second.memory.data(), size) != 0) reset_from_memory(it->second, bytes);
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

    // GPU thread: takes a finished refresh into its texture's CPU copy.
    void service() {
        if (!job_finished.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(job_mutex);
        job_finished.store(false, std::memory_order_relaxed);
        if (!job.active || !job.done) return;
        if (!job.failed) {
            const auto it = shadows.find(job.address);
            if (it != shadows.end() && it->second.epoch == job.epoch) {
                Shadow& s = it->second;
                const int w = job.region.width, h = job.region.height;
                for (int y = 0; y < h && y < s.height; ++y)
                    std::memcpy(&s.texels[std::size_t(y) * std::size_t(s.width)], &job.rows[std::size_t(y) * std::size_t(w)],
                                std::size_t(std::min(w, s.width)));
                ++s.generation;
                s.refreshed_frame = frame;
                ++refreshes;
                peek_ms_total += job.peek_ms;
                next_refresh_frame = std::max(next_refresh_frame, frame + std::uint64_t(job.peek_ms / 4));
            }
        } else {
            note_once("an EFB snapshot of a goop texture was not read back in time; refresh skipped");
        }
        inner.release_snapshot(job.snapshot);
        job.active = false;
        job.done = false;
        job.failed = false;
    }

    // GPU thread, before a copy to a texture with a CPU copy runs: snapshots its source when no
    // refresh is in flight and no other texture waited longer for one.
    void maybe_refresh(const starlet::gx::TextureCopy& copy, std::uint32_t address) {
        const auto it = shadows.find(address);
        if (it == shadows.end()) return;
        Shadow& s = it->second;
        int channel = -1;
        if (copy.luminance) channel = 4;
        else if (copy.format == 1 || copy.format == 8) channel = 0;
        else if (copy.format == 9) channel = 1;
        else if (copy.format == 10) channel = 2;
        else if (copy.format == 7) channel = 3;
        const bool fits = !copy.depth && !copy.half_scale && !copy.filtered() && !copy.strip && channel >= 0 &&
                          copy.width == s.width && copy.height <= s.height &&
                          copy.stride == std::size_t(s.width / 8) * 32;
        if (!fits) {
            // Its texels are now this copy's, which the model does not decode: start again from memory.
            note_once("a copy over a goop texture that is not a single-channel unscaled copy");
            shadows.erase(it);
            return;
        }
        // Reading a snapshot back costs the worker about 13 ms per 256x256 texture on the Flip
        // (single pixels from the mapped readback): refreshes start at most every 6 frames, and
        // after a refresh that read for N ms, N / 4 frames later at the earliest (about 12% of a
        // core at 30 frames per second).
        if (frame < next_refresh_frame) return;
        {
            std::lock_guard<std::mutex> lock(job_mutex);
            if (job.active) return;
        }
        for (const auto& [a, other] : shadows)
            if (a != address && other.used_frame + 4 >= frame && other.refreshed_frame < s.refreshed_frame) return;
        const starlet::gx::EfbRegion region{copy.x, copy.y, copy.width, copy.height};
        const int snapshot = inner.snapshot_efb_exact(region);
        if (snapshot < 0) return;
        next_refresh_frame = frame + 6;
        std::lock_guard<std::mutex> lock(job_mutex);
        job.active = true;
        job.done = false;
        job.failed = false;
        job.address = address;
        job.epoch = s.epoch;
        job.snapshot = snapshot;
        job.region = region;
        job.channel = channel;
        job_cv.notify_one();
    }

    void run_worker() {
        starlet::os::name_thread("sms-goop");
        for (;;) {
            Job work;
            {
                std::unique_lock<std::mutex> lock(job_mutex);
                job_cv.wait(lock, [this] { return stopping || (job.active && !job.done); });
                if (stopping) return;
                work.snapshot = job.snapshot;
                work.region = job.region;
                work.channel = job.channel;
            }
            const auto start = Clock::now();
            bool ready = false;
            while (!(ready = inner.snapshot_ready(work.snapshot)) && Clock::now() - start < std::chrono::seconds(3)) {
                {
                    std::lock_guard<std::mutex> lock(job_mutex);
                    if (stopping) return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto peek_start = Clock::now();
            const int w = work.region.width, h = work.region.height;
            std::vector<std::uint8_t> rows(std::size_t(w) * std::size_t(h));
            bool failed = !ready;
            for (int y = 0; y < h && !failed; ++y) {
                for (int x = 0; x < w; ++x) {
                    std::uint32_t argb = 0;
                    if (!inner.peek_color_snapshot(work.snapshot, work.region.x + x, work.region.y + y, argb)) {
                        failed = true;
                        break;
                    }
                    const unsigned r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
                    unsigned v;
                    switch (work.channel) {
                    case 0: v = r; break;
                    case 1: v = g; break;
                    case 2: v = b; break;
                    case 3: v = argb >> 24; break;
                    default: v = (257 * r + 504 * g + 98 * b + 16000) / 1000; break;
                    }
                    rows[std::size_t(y) * std::size_t(w) + std::size_t(x)] = std::uint8_t(v);
                }
            }
            const double peek_ms = std::chrono::duration<double, std::milli>(Clock::now() - peek_start).count();
            std::lock_guard<std::mutex> lock(job_mutex);
            job.rows = std::move(rows);
            job.failed = failed;
            job.peek_ms = peek_ms;
            job.done = true;
            job_finished.store(true, std::memory_order_release);
        }
    }

    void frame_end() {
        ++frame;
        if (frame % 600 == 0) {
            for (auto it = shadows.begin(); it != shadows.end();)
                it = it->second.used_frame + 600 < frame ? shadows.erase(it) : std::next(it);
        }
        if (log_enabled() && Clock::now() - stats_at > std::chrono::seconds(10)) {
            stats_at = Clock::now();
            std::printf("pixel metric: %llu intervals, %llu draws (%llu cached), %.2f ms counting per frame, %zu textures, "
                        "%llu refreshes, %.2f ms reading each\n",
                        (unsigned long long)intervals, (unsigned long long)draws, (unsigned long long)cache_hits,
                        frame > stats_frame ? count_ms / double(frame - stats_frame) : 0.0, shadows.size(),
                        (unsigned long long)refreshes, refreshes ? peek_ms_total / double(refreshes) : 0.0);
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
    g.service();
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
    inner_.draw(state, draw, memory);
}

void PixelMetricRenderer::wait_until_idle() {
    // Draw-done and tokens. The decoder has applied the token's BP write already; a draw-done
    // leaves the token register as it was and the interval open.
    Impl& g = *impl_;
    if (g.open && g.state && g.state->bp(0x48) != g.open_token_word)
        g.close_interval(std::uint16_t(g.state->bp(0x48) & 0xffff));
    g.service();
    inner_.wait_until_idle();
}

void PixelMetricRenderer::copy_display(const starlet::gx::DisplayCopy& copy, std::uint8_t* destination, std::size_t bytes) {
    impl_->frame_end();
    inner_.copy_display(copy, destination, bytes);
}

void PixelMetricRenderer::copy_texture(const starlet::gx::TextureCopy& copy, std::uint8_t* destination, std::size_t bytes,
                                       std::uint32_t address) {
    Impl& g = *impl_;
    g.service();
    g.maybe_refresh(copy, address);
    inner_.copy_texture(copy, destination, bytes, address);
    // Starlet may stamp the destination; keep its bytes as they are now to notice later CPU writes.
    const auto it = g.shadows.find(address);
    if (it != g.shadows.end()) {
        const std::size_t size = it->second.memory.size();
        if (const std::uint8_t* now = Impl::native(address, size)) it->second.memory.assign(now, now + size);
    }
}

std::uint32_t PixelMetricRenderer::peek_depth(int x, int y) { return inner_.peek_depth(x, y); }
std::uint32_t PixelMetricRenderer::peek_color(int x, int y) { return inner_.peek_color(x, y); }
bool PixelMetricRenderer::resident_in(const void* data, std::size_t bytes) const { return inner_.resident_in(data, bytes); }
void PixelMetricRenderer::before_dma_read(const void* data, std::size_t bytes) { inner_.before_dma_read(data, bytes); }
void PixelMetricRenderer::before_dma_write(const void* data, std::size_t bytes) { inner_.before_dma_write(data, bytes); }
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
    impl_->service();
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
    if (sh.delivered) result = sh.current;  // otherwise the interval drew nothing
    if (result.unsupported) {
        sms::graphics::note_once(result.unsupported);
        return 0xffffffffu;
    }
    return sms::graphics::blend_input_pixels({result.quads, result.triangles, nullptr});
}
