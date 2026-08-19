#include <pynote/harness/p2_card_list_prototype.h>

#include <D2DWrapp/D2DDevice.h>
#include <D2DWrapp/D2DSwapTarget.h>
#include <D2DWrapp/D2DText.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#pragma comment(lib, "D2DWrapp")

namespace pynote::harness {

namespace {

constexpr wchar_t kWindowClass[] = L"pyNote.P2CardListPrototype";
constexpr float kRowHeightDips = 72.0f;
constexpr float kHorizontalInsetDips = 12.0f;
constexpr float kVerticalInsetDips = 4.0f;

void register_window_class()
{
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = P2CardListPrototype::WindowProc;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    if (::RegisterClassExW(&window_class) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("RegisterClassExW failed for P2 card list");
    }
}

}  // namespace

struct P2CardListPrototype::Impl {
    HWND window = nullptr;
    d2d::C_D2D_DEVICE device;
    d2d::C_D2D_TEXT text;
    d2d::C_D2D_SWAP_TARGET target;
    IDWriteTextFormat* format = nullptr;
    std::vector<std::wstring> rows;
    std::size_t scroll_row = 0;
    UINT current_dpi = 96;
    bool initialized = false;
    P2FrameObservation frame;
};

P2CardListPrototype::P2CardListPrototype(UINT width_pixels, UINT height_pixels)
    : impl_(std::make_unique<Impl>())
{
    register_window_class();
    impl_->window = ::CreateWindowExW(
        WS_EX_TOOLWINDOW, kWindowClass, L"pyNote P2 virtual card list",
        WS_POPUP | WS_CLIPCHILDREN, 0, 0,
        static_cast<int>((std::max)(1u, width_pixels)),
        static_cast<int>((std::max)(1u, height_pixels)), nullptr, nullptr,
        ::GetModuleHandleW(nullptr), this);
    if (impl_->window == nullptr) {
        throw std::runtime_error("CreateWindowExW failed for P2 card list");
    }
    if (!impl_->device.Initialize() || !impl_->text.Initialize(&impl_->device) ||
        !impl_->target.Initialize(&impl_->device, impl_->window)) {
        throw std::runtime_error("D2DWrapp initialization failed for P2 card list");
    }
    impl_->format = impl_->text.GetFormat(L"Segoe UI", 14.0f);
    if (impl_->format == nullptr) {
        throw std::runtime_error("DirectWrite format creation failed for P2 card list");
    }
    impl_->current_dpi = ::GetDpiForWindow(impl_->window);
    if (impl_->current_dpi < 96) { impl_->current_dpi = 96; }
    impl_->initialized = true;
}

P2CardListPrototype::~P2CardListPrototype()
{
    if (impl_ == nullptr) { return; }
    impl_->initialized = false;
    impl_->target.Shutdown();
    impl_->text.Shutdown();
    impl_->device.Shutdown();
    if (impl_->window != nullptr) {
        ::DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
}

bool P2CardListPrototype::ready() const
{
    return impl_ != nullptr && impl_->initialized;
}

HWND P2CardListPrototype::hwnd() const
{
    return impl_->window;
}

UINT P2CardListPrototype::dpi() const
{
    return impl_->current_dpi;
}

UINT P2CardListPrototype::client_width_pixels() const
{
    RECT client{};
    ::GetClientRect(impl_->window, &client);
    return static_cast<UINT>((std::max)(0L, client.right - client.left));
}

UINT P2CardListPrototype::client_height_pixels() const
{
    RECT client{};
    ::GetClientRect(impl_->window, &client);
    return static_cast<UINT>((std::max)(0L, client.bottom - client.top));
}

float P2CardListPrototype::client_width_dips() const
{
    return impl_->target.GetSizeDips().width;
}

float P2CardListPrototype::client_height_dips() const
{
    return impl_->target.GetSizeDips().height;
}

std::size_t P2CardListPrototype::child_window_count() const
{
    std::size_t count = 0;
    for (HWND child = ::GetWindow(impl_->window, GW_CHILD); child != nullptr;
         child = ::GetWindow(child, GW_HWNDNEXT)) {
        ++count;
    }
    return count;
}

void P2CardListPrototype::SetRows(std::vector<std::wstring> rows)
{
    impl_->rows = std::move(rows);
    impl_->scroll_row = 0;
    impl_->frame = {};
}

void P2CardListPrototype::ScrollToLastRow()
{
    if (impl_->rows.empty()) {
        impl_->scroll_row = 0;
        return;
    }
    const float height = impl_->target.GetSizeDips().height;
    const std::size_t capacity = (std::max<std::size_t>)(
        1, static_cast<std::size_t>(std::floor(height / kRowHeightDips)));
    impl_->scroll_row = impl_->rows.size() > capacity ? impl_->rows.size() - capacity : 0;
}

bool P2CardListPrototype::Render()
{
    impl_->frame = {};
    if (!impl_->initialized || !impl_->target.BeginDraw()) { return false; }

    ID2D1DeviceContext* dc = impl_->target.GetDC();
    dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black),
                                         brush.GetAddressOf()))) {
        impl_->target.EndDraw(0);
        return false;
    }

    const D2D1_SIZE_F size = impl_->target.GetSizeDips();
    const std::size_t visible_capacity = (std::max<std::size_t>)(
        1, static_cast<std::size_t>(std::ceil(size.height / kRowHeightDips)));
    const std::size_t first = (std::min)(impl_->scroll_row, impl_->rows.size());
    const std::size_t end = (std::min)(impl_->rows.size(), first + visible_capacity);
    impl_->frame.first_visible_row = first;
    impl_->frame.last_visible_row = first;

    for (std::size_t index = first; index < end; ++index) {
        const float y = static_cast<float>(index - first) * kRowHeightDips + kVerticalInsetDips;
        d2d::C_D2D_TEXT_LAYOUT layout = impl_->text.CreateLayout(
            impl_->rows[index].c_str(), impl_->format,
            (std::max)(1.0f, size.width - 2.0f * kHorizontalInsetDips),
            kRowHeightDips - 2.0f * kVerticalInsetDips, 2);
        if (!layout.IsValid() || layout.Get() == nullptr) { continue; }
        impl_->frame.line_metrics_count += layout.GetLineMetrics().size();
        layout.Draw(dc, brush.Get(), D2D1::Point2F(kHorizontalInsetDips, y));
        ++impl_->frame.layout_count;
        impl_->frame.last_visible_row = index;
    }

    impl_->frame.presented = impl_->target.EndDraw(0);
    return impl_->frame.presented;
}

bool P2CardListPrototype::ApplyDpiForTest(UINT dpi_value, const RECT& suggested_window_rect)
{
    if (dpi_value == 0) { return false; }
    RECT suggested = suggested_window_rect;
    ::SendMessageW(impl_->window, WM_DPICHANGED, MAKELONG(dpi_value, dpi_value),
                   reinterpret_cast<LPARAM>(&suggested));
    return impl_->current_dpi == dpi_value;
}

P2LayoutObservation P2CardListPrototype::ProbeLayout(const std::wstring& value,
                                                      float width_dips,
                                                      UINT max_lines,
                                                      UINT hit_position) const
{
    P2LayoutObservation observation;
    d2d::C_D2D_TEXT_LAYOUT layout = impl_->text.CreateLayout(
        value.c_str(), impl_->format, width_dips, 100000.0f, max_lines);
    observation.valid = layout.IsValid() && layout.Get() != nullptr;
    if (!observation.valid) { return observation; }

    const std::vector<DWRITE_LINE_METRICS> lines = layout.GetLineMetrics();
    for (const DWRITE_LINE_METRICS& line : lines) {
        observation.line_lengths.push_back(line.length);
        observation.newline_lengths.push_back(line.newlineLength);
    }
    observation.final_line_trimmed = !lines.empty() && lines.back().isTrimmed != FALSE;

    D2D1_POINT_2F point{};
    DWRITE_HIT_TEST_METRICS forward{};
    observation.text_to_point = layout.HitTestTextPosition(hit_position, false, &point, &forward);
    bool trailing = false;
    bool inside = false;
    DWRITE_HIT_TEST_METRICS reverse{};
    observation.point_to_text = observation.text_to_point &&
        layout.HitTestPoint(point.x, point.y + forward.height * 0.5f,
                            &observation.roundtrip_position, &trailing, &inside, &reverse);
    observation.safe_slice = layout.Slice(hit_position, 1);
    return observation;
}

const P2FrameObservation& P2CardListPrototype::last_frame() const
{
    return impl_->frame;
}

LRESULT CALLBACK P2CardListPrototype::WindowProc(HWND window, UINT message,
                                                  WPARAM wparam, LPARAM lparam)
{
    P2CardListPrototype* self = reinterpret_cast<P2CardListPrototype*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<P2CardListPrototype*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self != nullptr) { return self->HandleMessage(window, message, wparam, lparam); }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT P2CardListPrototype::HandleMessage(HWND window, UINT message,
                                           WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_SIZE:
        if (impl_->initialized) {
            impl_->target.Resize(LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
    case WM_DPICHANGED: {
        const UINT new_dpi = LOWORD(wparam);
        if (new_dpi >= 96) {
            impl_->current_dpi = new_dpi;
            impl_->target.SetDpi(static_cast<float>(new_dpi));
        }
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
            ::SetWindowPos(window, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOACTIVATE | SWP_NOZORDER);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        ::BeginPaint(window, &paint);
        ::EndPaint(window, &paint);
        return 0;
    }
    default:
        return ::DefWindowProcW(window, message, wparam, lparam);
    }
}

}  // namespace pynote::harness
