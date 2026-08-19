#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pynote::harness {

struct P2FrameObservation {
    std::size_t first_visible_row = 0;
    std::size_t last_visible_row = 0;
    std::size_t layout_count = 0;
    std::size_t line_metrics_count = 0;
    bool presented = false;
};

struct P2LayoutObservation {
    bool valid = false;
    std::vector<std::uint32_t> line_lengths;
    std::vector<std::uint32_t> newline_lengths;
    bool final_line_trimmed = false;
    bool text_to_point = false;
    bool point_to_text = false;
    std::uint32_t roundtrip_position = 0;
    std::wstring safe_slice;
};

// Minimal P2 virtual list: one real top-level HWND and no per-card child HWNDs.
class P2CardListPrototype {
public:
    explicit P2CardListPrototype(UINT width_pixels = 640, UINT height_pixels = 480);
    ~P2CardListPrototype();

    P2CardListPrototype(const P2CardListPrototype&) = delete;
    P2CardListPrototype& operator=(const P2CardListPrototype&) = delete;

    bool ready() const;
    HWND hwnd() const;
    UINT dpi() const;
    UINT client_width_pixels() const;
    UINT client_height_pixels() const;
    float client_width_dips() const;
    float client_height_dips() const;
    std::size_t child_window_count() const;

    void SetRows(std::vector<std::wstring> rows);
    void ScrollToLastRow();
    bool Render();
    bool ApplyDpiForTest(UINT dpi, const RECT& suggested_window_rect);

    P2LayoutObservation ProbeLayout(const std::wstring& text, float width_dips,
                                    UINT max_lines, UINT hit_position) const;
    const P2FrameObservation& last_frame() const;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
};

}  // namespace pynote::harness
