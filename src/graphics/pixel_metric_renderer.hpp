// GXReadPixMetric for Super Mario Sunshine's goop counters (PollutionCount.cpp). The game clears the
// counter, draws, and reads it in the callback of the draw-sync token that follows. This renderer
// sits in front of Starlet's on the GPU thread: it counts the draws between the clear and the token
// with the CPU model of pixel_metric.hpp and publishes the count under that token before its
// interrupt is raised; the callback reads it. The goop textures are EFB copies that Starlet keeps
// on the GPU, so the model reads CPU copies of them, which this renderer refreshes from EFB
// snapshots read back without waiting, one texture at a time on a worker thread. The counts
// therefore trail the game's goop by a few frames; nothing waits for the GPU.
#pragma once

#include "starlet/gx_fifo/renderer.hpp"

#include <memory>

namespace sms::graphics {

class PixelMetricRenderer final : public starlet::gx_fifo::Renderer {
public:
    explicit PixelMetricRenderer(starlet::gx_fifo::Renderer& inner);
    ~PixelMetricRenderer() override;

    void draw(const starlet::gx::State& state, const starlet::gx::Draw& draw, starlet::gx::MemoryReader& memory) override;
    void wait_until_idle() override;
    void copy_display(const starlet::gx::DisplayCopy& copy, std::uint8_t* destination, std::size_t bytes) override;
    void copy_texture(const starlet::gx::TextureCopy& copy, std::uint8_t* destination, std::size_t bytes,
                      std::uint32_t address) override;
    std::uint32_t peek_depth(int x, int y) override;
    std::uint32_t peek_color(int x, int y) override;
    bool resident_in(const void* data, std::size_t bytes) const override;
    void before_dma_read(const void* data, std::size_t bytes) override;
    void before_dma_write(const void* data, std::size_t bytes) override;
    int snapshot_efb(const starlet::gx::EfbRegion& region) override;
    int snapshot_efb_exact(const starlet::gx::EfbRegion& region) override;
    bool snapshot_ready(int index) const override;
    void wait_snapshot(int index) override;
    bool peek_depth_snapshot(int index, int x, int y, std::uint32_t& value) const override;
    bool peek_color_snapshot(int index, int x, int y, std::uint32_t& value) const override;
    void select_snapshot(int index) override;
    void release_snapshot(int index) override;
    void retain_snapshot(int index) override;
    int request_depth_readback() override;
    bool depth_readback_value(int handle, int x, int y, std::uint32_t& value) const override;
    void wait_depth_readback(int handle) override;
    void release_depth_readback(int handle) override;
    void set_frame_ahead(bool ahead) override;
    bool present(const starlet::gx::FramebufferView& frame, int drawable_width, int drawable_height, bool black) override;
    std::vector<std::uint8_t> readback() override;
    void print_stats(const starlet::gx_fifo::GpuCounters& gpu) override;

private:
    struct Impl;
    starlet::gx_fifo::Renderer& inner_;
    std::unique_ptr<Impl> impl_;
};

}

// Guest side, called from src/graphics/gx_misc.c.
extern "C" {
// GXClearPixMetric: the BP word to write (0x57, the pixel counter clear, carrying a sequence
// number). The first call also wraps the game's draw-sync callback.
unsigned sunshine_pixel_metric_clear(void);
// GXReadPixMetric's clr_pixels_in, in a draw-sync callback: the interval its token ended.
unsigned sunshine_pixel_metric_read(void);
}
