#include <pynote/harness/directwrite_imm32_p1_probe.h>

#include <pynote/harness/win32_harness.h>

#include <D2DWrapp/D2DDevice.h>
#include <D2DWrapp/D2DSwapTarget.h>
#include <D2DWrapp/D2DText.h>

#include <dwrite_1.h>
#include <imm.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

#pragma comment(lib, "D2DWrapp")
#pragma comment(lib, "Imm32")

namespace pynote::harness {
namespace {

constexpr float kTextLeft = 10.0f;
constexpr float kTextTop = 10.0f;
constexpr float kTextWidth = 640.0f;
constexpr float kTextHeight = 300.0f;

std::wstring normalize_lf(std::wstring text)
{
    std::wstring result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            if (index + 1 < text.size() && text[index + 1] == L'\n') { ++index; }
            result.push_back(L'\n');
        }
        else {
            result.push_back(text[index]);
        }
    }
    return result;
}

bool is_high_surrogate(wchar_t value)
{
    return value >= 0xD800 && value <= 0xDBFF;
}

bool is_low_surrogate(wchar_t value)
{
    return value >= 0xDC00 && value <= 0xDFFF;
}

std::size_t safe_boundary(const std::wstring& text, std::size_t position, bool toward_end)
{
    position = (std::min)(position, text.size());
    if (position > 0 && position < text.size() && is_high_surrogate(text[position - 1]) &&
        is_low_surrogate(text[position])) {
        return toward_end ? position + 1 : position - 1;
    }
    return position;
}

std::wstring locale_name_for(LANGID language)
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    return ::LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), name,
                              LOCALE_NAME_MAX_LENGTH, 0) != 0
               ? std::wstring(name)
               : std::wstring();
}

std::wstring keyboard_layout_id(HKL layout)
{
    const HKL before = ::GetKeyboardLayout(0);
    if (::ActivateKeyboardLayout(layout, 0) == nullptr) { return {}; }
    wchar_t name[KL_NAMELENGTH]{};
    const bool ok = ::GetKeyboardLayoutNameW(name) != FALSE;
    if (before != nullptr) { ::ActivateKeyboardLayout(before, 0); }
    return ok ? std::wstring(name) : std::wstring();
}

}  // namespace

struct DirectWriteImm32P1Probe::Impl {
    struct EditCommand {
        std::wstring before;
        std::wstring after;
        std::size_t before_start = 0;
        std::size_t before_end = 0;
        std::size_t after_start = 0;
        std::size_t after_end = 0;
        std::wstring provenance;
    };

    TestWindow editor{TestWindowOptions{L"pyNote DirectWrite IMM32 P1 probe", 700, 400, true}};
    HWND sibling = nullptr;
    d2d::C_D2D_DEVICE device;
    d2d::C_D2D_TEXT text;
    d2d::C_D2D_SWAP_TARGET target;
    IDWriteTextFormat* format = nullptr;
    HIMC owned_himc = nullptr;
    HIMC previous_himc = nullptr;
    bool initialized = false;

    std::wstring body;
    std::size_t selection_start = 0;
    std::size_t selection_end = 0;
    std::vector<EditCommand> undo;
    std::vector<EditCommand> redo;
    DirectWriteEditDelta delta;
    std::wstring edit_provenance = L"none";
    double line_spacing = 1.0;
    DirectWriteFrameObservation frame;

    bool composition_active = false;
    std::wstring composition;
    LONG composition_cursor = 0;
    std::vector<unsigned char> composition_attributes;
    std::vector<DirectWriteImeObservation> ime_observations;
    bool protection_timer = false;
    std::wstring protection_snapshot;
    bool suppress_ime_chars_until_keyup = false;
    std::size_t keydown_count = 0;

    ~Impl()
    {
        initialized = false;
        target.Shutdown();
        text.Shutdown();
        device.Shutdown();
        if (owned_himc != nullptr) {
            ::ImmAssociateContext(editor.hwnd(), previous_himc);
            ::ImmDestroyContext(owned_himc);
            owned_himc = nullptr;
        }
        if (sibling != nullptr) {
            ::DestroyWindow(sibling);
            sibling = nullptr;
        }
    }

    void replace_range(std::size_t start, std::size_t end, const std::wstring& replacement,
                       const wchar_t* source)
    {
        start = safe_boundary(body, start, false);
        end = safe_boundary(body, end, true);
        if (start > end) { std::swap(start, end); }

        EditCommand command;
        command.before = body;
        command.before_start = selection_start;
        command.before_end = selection_end;
        command.provenance = source;
        body.replace(start, end - start, replacement);
        selection_start = selection_end = start + replacement.size();
        command.after = body;
        command.after_start = selection_start;
        command.after_end = selection_end;
        undo.push_back(std::move(command));
        redo.clear();
        delta = {start, end - start, replacement.size(), true, source};
        edit_provenance = source;
    }

    std::wstring read_ime_string(DWORD index) const
    {
        HIMC context = ::ImmGetContext(editor.hwnd());
        if (context == nullptr) { return {}; }
        const LONG bytes = ::ImmGetCompositionStringW(context, index, nullptr, 0);
        std::wstring result;
        if (bytes > 0) {
            result.resize(static_cast<std::size_t>(bytes) / sizeof(wchar_t));
            const LONG copied = ::ImmGetCompositionStringW(
                context, index, result.data(), static_cast<DWORD>(bytes));
            if (copied < 0) { result.clear(); }
        }
        ::ImmReleaseContext(editor.hwnd(), context);
        return result;
    }

    LONG read_ime_cursor() const
    {
        HIMC context = ::ImmGetContext(editor.hwnd());
        if (context == nullptr) { return -1; }
        const LONG result = ::ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
        ::ImmReleaseContext(editor.hwnd(), context);
        return result;
    }

    std::vector<unsigned char> read_ime_attributes() const
    {
        HIMC context = ::ImmGetContext(editor.hwnd());
        if (context == nullptr) { return {}; }
        const LONG bytes = ::ImmGetCompositionStringW(context, GCS_COMPATTR, nullptr, 0);
        std::vector<unsigned char> result;
        if (bytes > 0) {
            result.resize(static_cast<std::size_t>(bytes));
            const LONG copied = ::ImmGetCompositionStringW(
                context, GCS_COMPATTR, result.data(), static_cast<DWORD>(result.size()));
            if (copied < 0) { result.clear(); }
        }
        ::ImmReleaseContext(editor.hwnd(), context);
        return result;
    }

    d2d::C_D2D_TEXT_LAYOUT make_layout(std::wstring* displayed = nullptr)
    {
        std::wstring value = body;
        if (composition_active) {
            value.replace(selection_start, selection_end - selection_start, composition);
        }
        if (displayed != nullptr) { *displayed = value; }
        d2d::C_D2D_TEXT_LAYOUT layout = text.CreateLayout(
            value.c_str(), format, kTextWidth, kTextHeight, 0);
        if (!layout.IsValid() || layout.Get() == nullptr) { return layout; }

        const std::vector<DWRITE_LINE_METRICS> base_lines = layout.GetLineMetrics();
        if (!base_lines.empty()) {
            Microsoft::WRL::ComPtr<IDWriteTextLayout1> layout1;
            if (SUCCEEDED(layout.Get()->QueryInterface(IID_PPV_ARGS(layout1.GetAddressOf())))) {
                const float height = base_lines.front().height * static_cast<float>(line_spacing);
                const float baseline = base_lines.front().baseline * static_cast<float>(line_spacing);
                if (FAILED(layout1->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                                                   height, baseline))) {
                    throw std::runtime_error("IDWriteTextLayout1::SetLineSpacing failed");
                }
            }
            else {
                throw std::runtime_error("IDWriteTextLayout1 is unavailable");
            }
        }
        return layout;
    }

    bool render()
    {
        frame = {};
        std::wstring displayed;
        d2d::C_D2D_TEXT_LAYOUT layout = make_layout(&displayed);
        frame.layout_valid = layout.IsValid() && layout.Get() != nullptr;
        if (!frame.layout_valid || !target.BeginDraw()) { return false; }

        ID2D1DeviceContext* dc = target.GetDC();
        dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> selection_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> caret_brush;
        if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black),
                                             text_brush.GetAddressOf())) ||
            FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0.62f, 0.78f, 1.0f, 1.0f),
                                             selection_brush.GetAddressOf())) ||
            FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DarkBlue),
                                             caret_brush.GetAddressOf()))) {
            target.EndDraw(0);
            return false;
        }

        if (selection_end > selection_start && !composition_active) {
            UINT32 count = 0;
            layout.Get()->HitTestTextRange(static_cast<UINT32>(selection_start),
                                           static_cast<UINT32>(selection_end - selection_start),
                                           kTextLeft, kTextTop, nullptr, 0, &count);
            if (count > 0) {
                std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
                if (SUCCEEDED(layout.Get()->HitTestTextRange(
                        static_cast<UINT32>(selection_start),
                        static_cast<UINT32>(selection_end - selection_start),
                        kTextLeft, kTextTop, metrics.data(), count, &count))) {
                    frame.selection_hit_tested = true;
                    for (UINT32 index = 0; index < count; ++index) {
                        const auto& item = metrics[index];
                        dc->FillRectangle(D2D1::RectF(item.left, item.top,
                                                      item.left + item.width,
                                                      item.top + item.height),
                                          selection_brush.Get());
                    }
                    frame.selection_drawn = true;
                }
            }
        }

        layout.Draw(dc, text_brush.Get(), D2D1::Point2F(kTextLeft, kTextTop));

        UINT32 caret_position = static_cast<UINT32>(selection_end);
        if (composition_active) {
            const LONG bounded_cursor = (std::max)(0L, (std::min)(
                composition_cursor, static_cast<LONG>(composition.size())));
            caret_position = static_cast<UINT32>(selection_start +
                                                  static_cast<std::size_t>(bounded_cursor));
        }
        D2D1_POINT_2F caret{};
        DWRITE_HIT_TEST_METRICS caret_metrics{};
        if (layout.HitTestTextPosition(caret_position, false, &caret, &caret_metrics)) {
            frame.caret_hit_tested = true;
            const float left = kTextLeft + caret.x;
            const float top = kTextTop + caret.y;
            dc->DrawLine(D2D1::Point2F(left, top),
                         D2D1::Point2F(left, top + caret_metrics.height),
                         caret_brush.Get(), 1.0f);
            frame.caret_drawn = true;
            frame.ime_caret_client.x = static_cast<LONG>(std::lround(left));
            frame.ime_caret_client.y = static_cast<LONG>(std::lround(top + caret_metrics.height));
        }

        if (composition_active && !composition.empty()) {
            UINT32 count = 0;
            layout.Get()->HitTestTextRange(static_cast<UINT32>(selection_start),
                                           static_cast<UINT32>(composition.size()),
                                           kTextLeft, kTextTop, nullptr, 0, &count);
            if (count > 0) {
                std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
                if (SUCCEEDED(layout.Get()->HitTestTextRange(
                        static_cast<UINT32>(selection_start),
                        static_cast<UINT32>(composition.size()), kTextLeft, kTextTop,
                        metrics.data(), count, &count))) {
                    for (UINT32 index = 0; index < count; ++index) {
                        const auto& item = metrics[index];
                        dc->DrawLine(D2D1::Point2F(item.left, item.top + item.height - 1.0f),
                                     D2D1::Point2F(item.left + item.width,
                                                  item.top + item.height - 1.0f),
                                     caret_brush.Get(), 1.0f);
                    }
                    frame.preedit_drawn = true;
                }
            }
        }

        const std::vector<DWRITE_LINE_METRICS> lines = layout.GetLineMetrics();
        frame.line_count = lines.size();
        if (!lines.empty()) { frame.line_height_dips = lines.front().height; }
        frame.presented = target.EndDraw(0);
        return frame.presented;
    }

    bool position_ime_windows()
    {
        if (!render()) { return false; }
        HIMC context = ::ImmGetContext(editor.hwnd());
        if (context == nullptr) { return false; }
        COMPOSITIONFORM composition_form{};
        composition_form.dwStyle = CFS_POINT;
        composition_form.ptCurrentPos = frame.ime_caret_client;
        CANDIDATEFORM candidate_form{};
        candidate_form.dwIndex = 0;
        candidate_form.dwStyle = CFS_CANDIDATEPOS;
        candidate_form.ptCurrentPos = frame.ime_caret_client;
        const bool set_composition = ::ImmSetCompositionWindow(context, &composition_form) != FALSE;
        const bool set_candidate = ::ImmSetCandidateWindow(context, &candidate_form) != FALSE;
        COMPOSITIONFORM composition_readback{};
        const bool read_composition = ::ImmGetCompositionWindow(context, &composition_readback) != FALSE;
        ::ImmReleaseContext(editor.hwnd(), context);
        return set_composition && set_candidate && read_composition &&
               composition_readback.ptCurrentPos.x == frame.ime_caret_client.x &&
               composition_readback.ptCurrentPos.y == frame.ime_caret_client.y;
    }

    bool send_physical_key(WORD virtual_key)
    {
        const UINT mapped = ::MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC_EX);
        INPUT inputs[2]{};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = static_cast<WORD>(mapped & 0xFFu);
        inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE |
            ((mapped & 0xFF00u) != 0 ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs[1] = inputs[0];
        inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        return ::SendInput(2, inputs, sizeof(INPUT)) == 2;
    }

    bool handle_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result)
    {
        switch (message) {
        case WM_SIZE:
            if (initialized) { target.Resize(LOWORD(lparam), HIWORD(lparam)); }
            result = 0;
            return true;
        case WM_ERASEBKGND:
            result = 1;
            return true;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            ::BeginPaint(editor.hwnd(), &paint);
            ::EndPaint(editor.hwnd(), &paint);
            result = 0;
            return true;
        }
        case WM_KEYDOWN:
            ++keydown_count;
            return false;
        case WM_KEYUP:
            if (wparam == VK_RETURN || wparam == VK_SPACE || wparam == VK_ESCAPE) {
                suppress_ime_chars_until_keyup = false;
            }
            return false;
        case WM_CHAR:
        case WM_IME_CHAR:
            if (composition_active || suppress_ime_chars_until_keyup || message == WM_IME_CHAR) {
                result = 0;
                return true;
            }
            if (wparam >= 0x20 || wparam == L'\n' || wparam == L'\r' || wparam == L'\t') {
                const wchar_t character = wparam == L'\r' ? L'\n' : static_cast<wchar_t>(wparam);
                replace_range(selection_start, selection_end, std::wstring(1, character), L"typing");
                render();
                result = 0;
                return true;
            }
            return false;
        case WM_IME_STARTCOMPOSITION:
            composition_active = true;
            composition.clear();
            composition_cursor = 0;
            composition_attributes.clear();
            protection_timer = true;
            protection_snapshot = body;
            ime_observations.push_back({message, lparam, {}, {}, 0, {}});
            position_ime_windows();
            result = 0;
            return true;
        case WM_IME_COMPOSITION: {
            DirectWriteImeObservation observation;
            observation.message = message;
            observation.flags = lparam;
            if ((lparam & GCS_COMPSTR) != 0) { observation.composition = read_ime_string(GCS_COMPSTR); }
            if ((lparam & GCS_RESULTSTR) != 0) { observation.result = read_ime_string(GCS_RESULTSTR); }
            if ((lparam & GCS_CURSORPOS) != 0) { observation.cursor_position = read_ime_cursor(); }
            if ((lparam & GCS_COMPATTR) != 0) { observation.attributes = read_ime_attributes(); }
            if ((lparam & GCS_COMPSTR) != 0) { composition = observation.composition; }
            if ((lparam & GCS_CURSORPOS) != 0 && observation.cursor_position >= 0) {
                composition_cursor = observation.cursor_position;
            }
            if ((lparam & GCS_COMPATTR) != 0) { composition_attributes = observation.attributes; }
            if ((lparam & GCS_RESULTSTR) != 0 && !observation.result.empty()) {
                replace_range(selection_start, selection_end, observation.result, L"typing");
                composition.clear();
                composition_cursor = 0;
                composition_attributes.clear();
                suppress_ime_chars_until_keyup = true;
            }
            ime_observations.push_back(std::move(observation));
            position_ime_windows();
            result = 0;
            return true;
        }
        case WM_IME_ENDCOMPOSITION:
            ime_observations.push_back({message, lparam, {}, {}, -1, {}});
            composition_active = false;
            composition.clear();
            composition_cursor = 0;
            composition_attributes.clear();
            protection_timer = false;
            render();
            result = 0;
            return true;
        default:
            return false;
        }
    }
};

DirectWriteImm32P1Probe::DirectWriteImm32P1Probe()
    : impl_(std::make_unique<Impl>())
{
    impl_->sibling = ::CreateWindowExW(
        0, L"BUTTON", L"focus target", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        10, 330, 150, 28, impl_->editor.hwnd(), reinterpret_cast<HMENU>(301),
        ::GetModuleHandleW(nullptr), nullptr);
    if (impl_->sibling == nullptr) {
        throw std::runtime_error("CreateWindowExW focus target failed");
    }
    if (!impl_->device.Initialize() || !impl_->text.Initialize(&impl_->device) ||
        !impl_->target.Initialize(&impl_->device, impl_->editor.hwnd())) {
        throw std::runtime_error("D2DWrapp initialization failed for DirectWrite IMM32 probe");
    }
    impl_->format = impl_->text.GetFormat(L"Segoe UI", 18.0f);
    if (impl_->format == nullptr) {
        throw std::runtime_error("DirectWrite format creation failed");
    }
    impl_->owned_himc = ::ImmCreateContext();
    if (impl_->owned_himc == nullptr) {
        throw std::runtime_error("ImmCreateContext failed");
    }
    impl_->previous_himc = ::ImmAssociateContext(impl_->editor.hwnd(), impl_->owned_himc);
    impl_->editor.set_handler([this](UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
        return impl_->handle_message(message, wparam, lparam, result);
    });
    impl_->initialized = true;
    LoadBody(L"");
}

DirectWriteImm32P1Probe::~DirectWriteImm32P1Probe() = default;

bool DirectWriteImm32P1Probe::ready() const noexcept { return impl_->initialized; }
HWND DirectWriteImm32P1Probe::editor_hwnd() const noexcept { return impl_->editor.hwnd(); }
HWND DirectWriteImm32P1Probe::sibling_hwnd() const noexcept { return impl_->sibling; }

void DirectWriteImm32P1Probe::LoadBody(const std::wstring& value)
{
    const std::wstring normalized = normalize_lf(value);
    impl_->delta = {0, impl_->body.size(), normalized.size(), impl_->body != normalized, L"load"};
    impl_->body = normalized;
    impl_->selection_start = impl_->selection_end = impl_->body.size();
    impl_->undo.clear();
    impl_->redo.clear();
    impl_->edit_provenance = L"load";
    impl_->composition_active = false;
    impl_->composition.clear();
    impl_->composition_attributes.clear();
    impl_->render();
}

void DirectWriteImm32P1Probe::ReplaceAll(const std::wstring& value)
{
    impl_->replace_range(0, impl_->body.size(), normalize_lf(value), L"replace-all");
    impl_->render();
}

void DirectWriteImm32P1Probe::TypeCharacter(wchar_t character)
{
    const wchar_t normalized = character == L'\r' ? L'\n' : character;
    impl_->replace_range(impl_->selection_start, impl_->selection_end,
                         std::wstring(1, normalized), L"typing");
    impl_->render();
}

void DirectWriteImm32P1Probe::DeleteSelection()
{
    impl_->replace_range(impl_->selection_start, impl_->selection_end, L"", L"typing");
    impl_->render();
}

void DirectWriteImm32P1Probe::SetSelection(std::size_t start, std::size_t end)
{
    impl_->selection_start = safe_boundary(impl_->body, start, false);
    impl_->selection_end = safe_boundary(impl_->body, end, true);
    if (impl_->selection_start > impl_->selection_end) {
        std::swap(impl_->selection_start, impl_->selection_end);
    }
    impl_->render();
}

std::pair<std::size_t, std::size_t> DirectWriteImm32P1Probe::Selection() const noexcept
{
    return {impl_->selection_start, impl_->selection_end};
}

std::size_t DirectWriteImm32P1Probe::Cursor() const noexcept { return impl_->selection_end; }
const std::wstring& DirectWriteImm32P1Probe::BodyLf() const noexcept { return impl_->body; }
bool DirectWriteImm32P1Probe::CanUndo() const noexcept { return !impl_->undo.empty(); }

bool DirectWriteImm32P1Probe::Undo()
{
    if (impl_->undo.empty()) { return false; }
    Impl::EditCommand command = std::move(impl_->undo.back());
    impl_->undo.pop_back();
    const std::size_t removed = command.after.size();
    const std::size_t added = command.before.size();
    impl_->body = command.before;
    impl_->selection_start = command.before_start;
    impl_->selection_end = command.before_end;
    impl_->delta = {0, removed, added, true, L"undo"};
    impl_->edit_provenance = L"undo";
    impl_->redo.push_back(std::move(command));
    impl_->render();
    return true;
}

bool DirectWriteImm32P1Probe::CanRedo() const noexcept { return !impl_->redo.empty(); }

bool DirectWriteImm32P1Probe::Redo()
{
    if (impl_->redo.empty()) { return false; }
    Impl::EditCommand command = std::move(impl_->redo.back());
    impl_->redo.pop_back();
    const std::size_t removed = command.before.size();
    const std::size_t added = command.after.size();
    impl_->body = command.after;
    impl_->selection_start = command.after_start;
    impl_->selection_end = command.after_end;
    impl_->delta = {0, removed, added, true, L"redo"};
    impl_->edit_provenance = L"redo";
    impl_->undo.push_back(std::move(command));
    impl_->render();
    return true;
}

void DirectWriteImm32P1Probe::ApplyLineSpacing(double multiplier)
{
    if (multiplier <= 0.0) { throw std::invalid_argument("line spacing must be positive"); }
    const std::wstring before = impl_->body;
    const std::size_t undo_count = impl_->undo.size();
    impl_->line_spacing = multiplier;
    impl_->delta = {0, 0, 0, false, L"formatting"};
    impl_->edit_provenance = L"formatting";
    if (!impl_->render()) { throw std::runtime_error("DirectWrite line spacing render failed"); }
    if (impl_->body != before || impl_->undo.size() != undo_count) {
        throw std::runtime_error("formatting changed body or undo state");
    }
}

double DirectWriteImm32P1Probe::AppliedLineSpacing() const noexcept { return impl_->line_spacing; }

float DirectWriteImm32P1Probe::ActualLineHeightDips()
{
    if (!impl_->render() || impl_->frame.line_height_dips <= 0.0f) {
        throw std::runtime_error("DirectWrite returned a non-positive line height");
    }
    return impl_->frame.line_height_dips;
}

bool DirectWriteImm32P1Probe::Render() { return impl_->render(); }

bool DirectWriteImm32P1Probe::HitTestPoint(float x_dips, float y_dips)
{
    d2d::C_D2D_TEXT_LAYOUT layout = impl_->make_layout();
    UINT32 position = 0;
    bool trailing = false;
    bool inside = false;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (!layout.HitTestPoint(x_dips - kTextLeft, y_dips - kTextTop, &position,
                             &trailing, &inside, &metrics)) {
        return false;
    }
    const std::wstring safe_hit = layout.Slice(position, metrics.length);
    if (metrics.length != 0 && safe_hit.empty()) { return false; }
    std::size_t caret = static_cast<std::size_t>(position);
    if (trailing) { caret += metrics.length; }
    caret = safe_boundary(impl_->body, caret, trailing);
    impl_->selection_start = impl_->selection_end = caret;
    return impl_->render();
}

const DirectWriteFrameObservation& DirectWriteImm32P1Probe::last_frame() const noexcept
{
    return impl_->frame;
}

const DirectWriteEditDelta& DirectWriteImm32P1Probe::last_delta() const noexcept
{
    return impl_->delta;
}

const std::wstring& DirectWriteImm32P1Probe::provenance() const noexcept
{
    return impl_->edit_provenance;
}

bool DirectWriteImm32P1Probe::composition_active() const noexcept
{
    return impl_->composition_active;
}

const std::wstring& DirectWriteImm32P1Probe::preedit() const noexcept
{
    return impl_->composition;
}

LONG DirectWriteImm32P1Probe::preedit_cursor() const noexcept
{
    return impl_->composition_cursor;
}

const std::vector<unsigned char>& DirectWriteImm32P1Probe::preedit_attributes() const noexcept
{
    return impl_->composition_attributes;
}

bool DirectWriteImm32P1Probe::protection_timer_pending() const noexcept
{
    return impl_->protection_timer;
}

const std::wstring& DirectWriteImm32P1Probe::protection_snapshot() const noexcept
{
    return impl_->protection_snapshot;
}

bool DirectWriteImm32P1Probe::AttemptFocusSibling()
{
    if (impl_->composition_active) {
        impl_->protection_timer = true;
        impl_->protection_snapshot = impl_->body;
        ::SetFocus(impl_->editor.hwnd());
        return false;
    }
    ::SetFocus(impl_->sibling);
    return ::GetFocus() == impl_->sibling;
}

const std::vector<DirectWriteImeObservation>& DirectWriteImm32P1Probe::ime_observations() const noexcept
{
    return impl_->ime_observations;
}

DirectWriteImeProfile DirectWriteImm32P1Probe::FindInstalledIme(LANGID language) const
{
    DirectWriteImeProfile profile;
    profile.language = language;
    profile.locale_name = locale_name_for(language);
    const int count = ::GetKeyboardLayoutList(0, nullptr);
    if (count <= 0) { return profile; }
    std::vector<HKL> layouts(static_cast<std::size_t>(count));
    const int copied = ::GetKeyboardLayoutList(count, layouts.data());
    for (int index = 0; index < copied; ++index) {
        const HKL layout = layouts[static_cast<std::size_t>(index)];
        const LANGID candidate = LOWORD(reinterpret_cast<ULONG_PTR>(layout));
        if (PRIMARYLANGID(candidate) != PRIMARYLANGID(language) || ::ImmIsIME(layout) == FALSE) {
            continue;
        }
        profile.layout = layout;
        profile.klid = keyboard_layout_id(layout);
        wchar_t ime_file[MAX_PATH]{};
        if (::ImmGetIMEFileNameW(layout, ime_file, MAX_PATH) != 0) { profile.ime_file = ime_file; }
        profile.installed = true;
        break;
    }
    return profile;
}

DirectWriteImeExerciseResult DirectWriteImm32P1Probe::ExerciseInstalledIme(LANGID language)
{
    DirectWriteImeExerciseResult result;
    result.profile = FindInstalledIme(language);
    if (!result.profile.installed) {
        result.blocked_environment = PRIMARYLANGID(language) == LANG_JAPANESE
            ? L"BLOCKED_ENV_JA_IME_NOT_INSTALLED"
            : L"BLOCKED_ENV_IME_NOT_INSTALLED";
        return result;
    }

    const HKL previous_layout = ::GetKeyboardLayout(0);
    const HWND previous_foreground = ::GetForegroundWindow();
    ::ShowWindow(impl_->editor.hwnd(), SW_RESTORE);
    RECT client{};
    ::GetClientRect(impl_->editor.hwnd(), &client);
    result.visible_restored_nonzero = ::IsWindowVisible(impl_->editor.hwnd()) != FALSE &&
        ::IsIconic(impl_->editor.hwnd()) == FALSE && client.right > client.left &&
        client.bottom > client.top;
    if (!result.visible_restored_nonzero) {
        result.blocked_environment = L"BLOCKED_ENV_VISIBLE_RESTORED_NONZERO";
        return result;
    }

    DWORD process_id = 0;
    const DWORD window_thread = ::GetWindowThreadProcessId(impl_->editor.hwnd(), &process_id);
    result.same_ui_thread = window_thread == ::GetCurrentThreadId() &&
                            process_id == ::GetCurrentProcessId();
    if (!result.same_ui_thread) {
        result.blocked_environment = L"BLOCKED_ENV_UI_THREAD";
        return result;
    }

    if (::ActivateKeyboardLayout(result.profile.layout, 0) == nullptr ||
        ::GetKeyboardLayout(0) != result.profile.layout) {
        result.blocked_environment = L"BLOCKED_ENV_TARGET_HKL";
        return result;
    }
    result.target_hkl_active = true;

    ::SetForegroundWindow(impl_->editor.hwnd());
    ::SetActiveWindow(impl_->editor.hwnd());
    ::SetFocus(impl_->editor.hwnd());
    drain_messages();
    result.foreground_active_focus = ::GetForegroundWindow() == impl_->editor.hwnd() &&
        ::GetActiveWindow() == impl_->editor.hwnd() && ::GetFocus() == impl_->editor.hwnd();
    GUITHREADINFO gui{};
    gui.cbSize = sizeof(gui);
    result.gui_thread_focus = ::GetGUIThreadInfo(::GetCurrentThreadId(), &gui) != FALSE &&
        gui.hwndActive == impl_->editor.hwnd() && gui.hwndFocus == impl_->editor.hwnd();
    if (!result.foreground_active_focus || !result.gui_thread_focus) {
        result.blocked_environment = L"BLOCKED_ENV_FOREGROUND";
        if (previous_layout != nullptr) { ::ActivateKeyboardLayout(previous_layout, 0); }
        return result;
    }

    HIMC context = ::ImmGetContext(impl_->editor.hwnd());
    result.himc_connected = context != nullptr && context == impl_->owned_himc;
    if (!result.himc_connected) {
        if (context != nullptr) { ::ImmReleaseContext(impl_->editor.hwnd(), context); }
        result.blocked_environment = L"BLOCKED_ENV_HIMC";
        if (previous_layout != nullptr) { ::ActivateKeyboardLayout(previous_layout, 0); }
        return result;
    }
    DWORD conversion = 0;
    DWORD sentence = 0;
    const bool configured = ::ImmSetOpenStatus(context, TRUE) != FALSE &&
        ::ImmGetConversionStatus(context, &conversion, &sentence) != FALSE &&
        ::ImmSetConversionStatus(context, conversion | IME_CMODE_NATIVE, sentence) != FALSE;
    DWORD read_conversion = 0;
    DWORD read_sentence = 0;
    result.ime_open_native_readback = configured && ::ImmGetOpenStatus(context) != FALSE &&
        ::ImmGetConversionStatus(context, &read_conversion, &read_sentence) != FALSE &&
        (read_conversion & IME_CMODE_NATIVE) != 0;
    ::ImmReleaseContext(impl_->editor.hwnd(), context);
    if (!result.ime_open_native_readback) {
        result.blocked_environment = L"BLOCKED_ENV_IME_OPEN_NATIVE";
        if (previous_layout != nullptr) { ::ActivateKeyboardLayout(previous_layout, 0); }
        return result;
    }

    LoadBody(L"");
    impl_->ime_observations.clear();
    impl_->keydown_count = 0;
    impl_->protection_timer = false;
    impl_->protection_snapshot.clear();
    result.ime_windows_positioned = impl_->position_ime_windows();
    if (!result.ime_windows_positioned) {
        result.blocked_environment = L"BLOCKED_ENV_IME_WINDOW_POSITION";
        if (previous_layout != nullptr) { ::ActivateKeyboardLayout(previous_layout, 0); }
        return result;
    }

    result.actual_input_attempted = true;
    const bool first_key = impl_->send_physical_key('R');
    wait_until([this] { return impl_->composition_active || !impl_->ime_observations.empty(); },
               std::chrono::milliseconds(1000));
    result.composition_started = std::any_of(
        impl_->ime_observations.begin(), impl_->ime_observations.end(),
        [](const DirectWriteImeObservation& item) { return item.message == WM_IME_STARTCOMPOSITION; });
    result.preedit_observed = std::any_of(
        impl_->ime_observations.begin(), impl_->ime_observations.end(),
        [](const DirectWriteImeObservation& item) { return !item.composition.empty(); });

    result.leave_attempted = true;
    result.leave_rejected = !AttemptFocusSibling() && ::GetFocus() == impl_->editor.hwnd();
    if (::GetFocus() != impl_->editor.hwnd()) { ::SetFocus(impl_->editor.hwnd()); }

    const bool second_key = impl_->send_physical_key('K');
    const bool commit_key = impl_->send_physical_key(VK_RETURN);
    wait_until([this] {
        const bool result_seen = std::any_of(
            impl_->ime_observations.begin(), impl_->ime_observations.end(),
            [](const DirectWriteImeObservation& item) { return !item.result.empty(); });
        return result_seen && !impl_->composition_active;
    }, std::chrono::milliseconds(1000));
    result.commit_observed = std::any_of(
        impl_->ime_observations.begin(), impl_->ime_observations.end(),
        [](const DirectWriteImeObservation& item) { return !item.result.empty(); });
    result.committed_body = impl_->body;

    const std::size_t end_before = static_cast<std::size_t>(std::count_if(
        impl_->ime_observations.begin(), impl_->ime_observations.end(),
        [](const DirectWriteImeObservation& item) { return item.message == WM_IME_ENDCOMPOSITION; }));
    const std::size_t results_before_cancel = static_cast<std::size_t>(std::count_if(
        impl_->ime_observations.begin(), impl_->ime_observations.end(),
        [](const DirectWriteImeObservation& item) { return !item.result.empty(); }));
    const std::wstring body_before_cancel = impl_->body;
    const bool cancel_start_key = impl_->send_physical_key('R');
    wait_until([this] { return impl_->composition_active; }, std::chrono::milliseconds(750));
    const bool cancel_key = impl_->send_physical_key(VK_BACK);
    wait_until([this] { return !impl_->composition_active; }, std::chrono::milliseconds(750));
    const std::size_t end_after = static_cast<std::size_t>(std::count_if(
        impl_->ime_observations.begin(), impl_->ime_observations.end(),
        [](const DirectWriteImeObservation& item) { return item.message == WM_IME_ENDCOMPOSITION; }));
    const std::size_t results_after_cancel = static_cast<std::size_t>(std::count_if(
        impl_->ime_observations.begin(), impl_->ime_observations.end(),
        [](const DirectWriteImeObservation& item) { return !item.result.empty(); }));
    result.cancel_observed = end_after > end_before &&
                             results_after_cancel == results_before_cancel &&
                             impl_->body == body_before_cancel;
    result.send_input_accepted = first_key && second_key && commit_key && cancel_start_key && cancel_key;
    result.keydown_observed = impl_->keydown_count >= 5;
    result.actual_input_delivered = result.send_input_accepted && result.keydown_observed &&
        result.composition_started && result.preedit_observed && result.commit_observed &&
        result.cancel_observed;

    if (previous_layout != nullptr) { ::ActivateKeyboardLayout(previous_layout, 0); }
    if (previous_foreground != nullptr && ::IsWindow(previous_foreground) != FALSE) {
        ::SetForegroundWindow(previous_foreground);
    }
    return result;
}

}  // namespace pynote::harness
