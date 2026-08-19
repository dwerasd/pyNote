#include <pynote/harness/rich_edit_p1_probe.h>

#include <imm.h>
#include <richedit.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "Imm32")

namespace pynote::harness {
namespace {

constexpr wchar_t kEditProperty[] = L"PyNote.RichEditP1Probe";

std::wstring normalize_lf(std::wstring text)
{
    std::wstring normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'\r') {
            if (index + 1 < text.size() && text[index + 1] == L'\n') {
                ++index;
            }
            normalized.push_back(L'\n');
        }
        else {
            normalized.push_back(text[index]);
        }
    }
    return normalized;
}

std::wstring text_from_window(HWND hwnd)
{
    GETTEXTLENGTHEX length = {};
    length.flags = GTL_NUMCHARS | GTL_PRECISE;
    length.codepage = 1200;
    const LRESULT count = ::SendMessageW(hwnd, EM_GETTEXTLENGTHEX,
                                         reinterpret_cast<WPARAM>(&length), 0);
    if (count < 0) {
        throw std::runtime_error("EM_GETTEXTLENGTHEX returned a negative length");
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(count) + 1u, L'\0');
    GETTEXTEX get = {};
    get.cb = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    get.flags = GT_DEFAULT;
    get.codepage = 1200;
    const LRESULT copied = ::SendMessageW(hwnd, EM_GETTEXTEX,
                                          reinterpret_cast<WPARAM>(&get),
                                          reinterpret_cast<LPARAM>(buffer.data()));
    if (copied < 0) {
        throw std::runtime_error("EM_GETTEXTEX failed");
    }
    return normalize_lf(std::wstring(buffer.data(), static_cast<std::size_t>(copied)));
}

std::wstring layout_id(HKL layout)
{
    const HKL previous = ::ActivateKeyboardLayout(layout, 0);
    wchar_t name[KL_NAMELENGTH] = {};
    const bool succeeded = ::GetKeyboardLayoutNameW(name) != FALSE;
    if (previous != nullptr) {
        ::ActivateKeyboardLayout(previous, 0);
    }
    return succeeded ? std::wstring(name) : std::wstring();
}

std::wstring locale_name(LANGID language)
{
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    const LCID lcid = MAKELCID(language, SORT_DEFAULT);
    return ::LCIDToLocaleName(lcid, name, LOCALE_NAME_MAX_LENGTH, 0) != 0
               ? std::wstring(name)
               : std::wstring();
}

}  // namespace

RichEditP1Probe::RichEditP1Probe()
    : parent_(TestWindowOptions{L"pynote P1 Rich Edit probe", 640, 320, true})
{
    msftedit_ = ::LoadLibraryW(L"Msftedit.dll");
    if (msftedit_ == nullptr) {
        throw std::runtime_error("LoadLibraryW(Msftedit.dll) failed, GetLastError=" +
                                 std::to_string(::GetLastError()));
    }

    edit_ = ::CreateWindowExW(WS_EX_CLIENTEDGE,
                              MSFTEDIT_CLASS,
                              L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL |
                                  ES_WANTRETURN | WS_VSCROLL,
                              8,
                              8,
                              600,
                              240,
                              parent_.hwnd(),
                              reinterpret_cast<HMENU>(101),
                              ::GetModuleHandleW(nullptr),
                              nullptr);
    sibling_ = ::CreateWindowExW(0,
                                 L"BUTTON",
                                 L"focus target",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                 8,
                                 260,
                                 160,
                                 30,
                                 parent_.hwnd(),
                                 reinterpret_cast<HMENU>(102),
                                 ::GetModuleHandleW(nullptr),
                                 nullptr);
    if (edit_ == nullptr || sibling_ == nullptr) {
        const DWORD error = ::GetLastError();
        if (edit_ != nullptr) {
            ::DestroyWindow(edit_);
            edit_ = nullptr;
        }
        if (sibling_ != nullptr) {
            ::DestroyWindow(sibling_);
            sibling_ = nullptr;
        }
        ::FreeLibrary(msftedit_);
        msftedit_ = nullptr;
        throw std::runtime_error("CreateWindowExW P1 child failed, GetLastError=" +
                                 std::to_string(error));
    }

    if (::SetPropW(edit_, kEditProperty, this) == FALSE) {
        throw std::runtime_error("SetPropW P1 edit failed");
    }
    original_edit_proc_ = reinterpret_cast<WNDPROC>(
        ::SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&RichEditP1Probe::edit_proc)));
    if (original_edit_proc_ == nullptr) {
        throw std::runtime_error("Rich Edit subclass failed, GetLastError=" +
                                 std::to_string(::GetLastError()));
    }

    parent_.set_handler([this](UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
        return handle_parent_message(message, wparam, lparam, result);
    });

    const LRESULT mode = send_message(edit_, EM_SETTEXTMODE, TM_PLAINTEXT | TM_MULTILEVELUNDO, 0);
    if (mode != 0) {
        throw std::runtime_error("EM_SETTEXTMODE rejected TM_PLAINTEXT | TM_MULTILEVELUNDO");
    }
    const LRESULT mask = send_message(edit_, EM_GETEVENTMASK, 0, 0);
    send_message(edit_, EM_SETEVENTMASK, 0, mask | ENM_CHANGE);
    send_message(edit_, EM_SETUNDOLIMIT, 100, 0);
    LoadBody(L"");
}

RichEditP1Probe::~RichEditP1Probe()
{
    if (edit_ != nullptr) {
        ::RemovePropW(edit_, kEditProperty);
        if (original_edit_proc_ != nullptr) {
            ::SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original_edit_proc_));
        }
        ::DestroyWindow(edit_);
        edit_ = nullptr;
    }
    if (sibling_ != nullptr) {
        ::DestroyWindow(sibling_);
        sibling_ = nullptr;
    }
    if (msftedit_ != nullptr) {
        ::FreeLibrary(msftedit_);
        msftedit_ = nullptr;
    }
}

void RichEditP1Probe::LoadBody(const std::wstring& body)
{
    before_change_ = BodyLf();
    requested_provenance_ = L"load";
    SETTEXTEX set = {};
    set.flags = ST_DEFAULT;
    set.codepage = 1200;
    send_message(edit_, EM_SETTEXTEX, reinterpret_cast<WPARAM>(&set),
                 reinterpret_cast<LPARAM>(body.c_str()));
    ClearUndo();
    requested_provenance_ = L"none";
    provenance_ = L"load";
}

void RichEditP1Probe::ReplaceAll(const std::wstring& body)
{
    before_change_ = BodyLf();
    requested_provenance_ = L"replace-all";
    SetSelection(0, -1);
    send_message(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(body.c_str()));
    requested_provenance_ = L"none";
}

void RichEditP1Probe::TypeCharacter(wchar_t character)
{
    before_change_ = BodyLf();
    requested_provenance_ = L"typing";
    send_key(edit_, static_cast<UINT>(::VkKeyScanW(character) & 0xFF), character);
    requested_provenance_ = L"none";
}

void RichEditP1Probe::DeleteSelection()
{
    before_change_ = BodyLf();
    requested_provenance_ = L"typing";
    send_message(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    requested_provenance_ = L"none";
}

void RichEditP1Probe::SetSelection(long start, long end)
{
    CHARRANGE range = {start, end};
    send_message(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&range));
}

std::pair<long, long> RichEditP1Probe::Selection() const
{
    CHARRANGE range = {};
    send_message(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&range));
    return {range.cpMin, range.cpMax};
}

long RichEditP1Probe::Cursor() const
{
    return Selection().second;
}

std::wstring RichEditP1Probe::BodyLf() const
{
    return text_from_window(edit_);
}

bool RichEditP1Probe::CanUndo() const
{
    return send_message(edit_, EM_CANUNDO, 0, 0) != 0;
}

bool RichEditP1Probe::Undo()
{
    before_change_ = BodyLf();
    requested_provenance_ = L"undo";
    const bool changed = send_message(edit_, EM_UNDO, 0, 0) != 0;
    requested_provenance_ = L"none";
    return changed;
}

void RichEditP1Probe::ClearUndo()
{
    send_message(edit_, EM_EMPTYUNDOBUFFER, 0, 0);
}

void RichEditP1Probe::ApplyLineSpacing(double multiplier)
{
    if (multiplier <= 0.0) {
        throw std::invalid_argument("line spacing multiplier must be positive");
    }
    const auto previous_selection = Selection();
    const std::wstring body_before = BodyLf();
    const std::size_t notifications_before = change_notifications_;

    requested_provenance_ = L"formatting";
    SetSelection(0, -1);
    PARAFORMAT2 format = {};
    format.cbSize = sizeof(format);
    format.dwMask = PFM_LINESPACING;
    format.bLineSpacingRule = 5;
    format.dyLineSpacing = static_cast<LONG>(std::lround(multiplier * 20.0));
    if (send_message(edit_, EM_SETPARAFORMAT, 0, reinterpret_cast<LPARAM>(&format)) == 0) {
        requested_provenance_ = L"none";
        throw std::runtime_error("EM_SETPARAFORMAT line spacing failed");
    }
    SetSelection(previous_selection.first, previous_selection.second);
    requested_provenance_ = L"none";
    applied_line_spacing_ = multiplier;

    // 서식이 EN_CHANGE를 발행하지 않는 Rich Edit 빌드에서도 본문 불변과 출처를 trace로 남긴다.
    if (notifications_before == change_notifications_) {
        last_delta_ = RichEditDelta{0, 0, 0, BodyLf() != body_before, L"formatting"};
        provenance_ = L"formatting";
    }
}

long RichEditP1Probe::ActualLineHeightPixels() const
{
    const LRESULT second_line = send_message(edit_, EM_LINEINDEX, 1, 0);
    if (second_line < 0) {
        throw std::runtime_error("ActualLineHeightPixels requires at least two display lines");
    }
    POINTL first = {};
    POINTL second = {};
    send_message(edit_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&first), 0);
    send_message(edit_, EM_POSFROMCHAR, reinterpret_cast<WPARAM>(&second), second_line);
    const long height = second.y - first.y;
    if (height <= 0) {
        throw std::runtime_error("EM_POSFROMCHAR returned a non-positive line height");
    }
    return height;
}

bool RichEditP1Probe::AttemptFocusSibling()
{
    if (composition_active_) {
        protection_timer_pending_ = true;
        protection_snapshot_ = BodyLf();
        ::SetFocus(edit_);
        return false;
    }
    ::SetFocus(sibling_);
    return ::GetFocus() == sibling_;
}

ImeProfile RichEditP1Probe::FindInstalledIme(LANGID language) const
{
    ImeProfile profile;
    profile.language = language;
    profile.locale_name = locale_name(language);

    const int count = ::GetKeyboardLayoutList(0, nullptr);
    if (count <= 0) {
        return profile;
    }
    std::vector<HKL> layouts(static_cast<std::size_t>(count));
    const int copied = ::GetKeyboardLayoutList(count, layouts.data());
    for (int index = 0; index < copied; ++index) {
        const LANGID candidate = LOWORD(reinterpret_cast<ULONG_PTR>(layouts[static_cast<std::size_t>(index)]));
        if (PRIMARYLANGID(candidate) != PRIMARYLANGID(language)) {
            continue;
        }
        profile.layout = layouts[static_cast<std::size_t>(index)];
        profile.klid = layout_id(profile.layout);
        wchar_t ime_file[MAX_PATH] = {};
        if (::ImmGetIMEFileNameW(profile.layout, ime_file, MAX_PATH) != 0) {
            profile.ime_file = ime_file;
        }
        profile.installed = true;
        return profile;
    }
    return profile;
}

ImeExerciseResult RichEditP1Probe::ExerciseInstalledIme(LANGID language)
{
    ImeExerciseResult result;
    result.profile = FindInstalledIme(language);
    if (!result.profile.installed) {
        return result;
    }

    const HKL previous_layout = ::GetKeyboardLayout(0);
    const HWND previous_foreground = ::GetForegroundWindow();
    if (::ActivateKeyboardLayout(result.profile.layout, 0) == nullptr) {
        return result;
    }

    ::ShowWindow(parent_.hwnd(), SW_SHOW);
    ::SetForegroundWindow(parent_.hwnd());
    ::SetActiveWindow(parent_.hwnd());
    ::SetFocus(edit_);

    HIMC context = ::ImmGetContext(edit_);
    result.himc_available = context != nullptr;
    if (context == nullptr) {
        ::ActivateKeyboardLayout(previous_layout, 0);
        return result;
    }
    ::ImmSetOpenStatus(context, TRUE);
    DWORD conversion = 0;
    DWORD sentence = 0;
    ::ImmGetConversionStatus(context, &conversion, &sentence);
    ::ImmSetConversionStatus(context, conversion | IME_CMODE_NATIVE, sentence);
    ::ImmReleaseContext(edit_, context);

    LoadBody(L"");
    ime_observations_.clear();
    protection_timer_pending_ = false;
    protection_snapshot_.clear();
    result.actual_input_attempted = true;

    const bool first_key = send_physical_key('R');
    wait_until([this] { return composition_active_ || !ime_observations_.empty(); },
               std::chrono::milliseconds(750));
    result.composition_started = std::any_of(
        ime_observations_.begin(), ime_observations_.end(),
        [](const ImeObservation& item) { return item.message == WM_IME_STARTCOMPOSITION; });
    result.preedit_observed = std::any_of(
        ime_observations_.begin(), ime_observations_.end(),
        [](const ImeObservation& item) { return !item.composition.empty(); });

    result.leave_attempted = true;
    result.leave_rejected = !AttemptFocusSibling() && ::GetFocus() == edit_;

    const bool second_key = send_physical_key('K');
    const bool commit_key = send_physical_key(VK_RETURN);
    wait_until([this] { return !BodyLf().empty(); }, std::chrono::milliseconds(750));
    result.committed_body = BodyLf();
    result.commit_observed = std::any_of(
        ime_observations_.begin(), ime_observations_.end(),
        [](const ImeObservation& item) { return !item.result.empty(); }) || !result.committed_body.empty();

    const std::size_t end_before = static_cast<std::size_t>(std::count_if(
        ime_observations_.begin(), ime_observations_.end(),
        [](const ImeObservation& item) { return item.message == WM_IME_ENDCOMPOSITION; }));
    send_physical_key('R');
    wait_until([this] { return composition_active_; }, std::chrono::milliseconds(500));
    const bool escape_key = send_physical_key(VK_ESCAPE);
    wait_until([this] { return !composition_active_; }, std::chrono::milliseconds(500));
    const std::size_t end_after = static_cast<std::size_t>(std::count_if(
        ime_observations_.begin(), ime_observations_.end(),
        [](const ImeObservation& item) { return item.message == WM_IME_ENDCOMPOSITION; }));
    result.cancel_observed = end_after > end_before;
    result.actual_input_delivered = first_key && second_key && commit_key && escape_key &&
                                    result.composition_started;

    ::ActivateKeyboardLayout(previous_layout, 0);
    if (previous_foreground != nullptr && ::IsWindow(previous_foreground) != FALSE) {
        ::SetForegroundWindow(previous_foreground);
    }
    return result;
}

LRESULT CALLBACK RichEditP1Probe::edit_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* self = static_cast<RichEditP1Probe*>(::GetPropW(hwnd, kEditProperty));
    if (self == nullptr || self->original_edit_proc_ == nullptr) {
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return self->handle_edit_message(hwnd, message, wparam, lparam);
}

LRESULT RichEditP1Probe::handle_edit_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_IME_STARTCOMPOSITION) {
        composition_active_ = true;
        protection_timer_pending_ = true;
        protection_snapshot_ = BodyLf();
        record_ime(message, lparam);
    }
    else if (message == WM_IME_COMPOSITION) {
        record_ime(message, lparam);
        requested_provenance_ = L"typing";
    }

    const LRESULT result = ::CallWindowProcW(original_edit_proc_, hwnd, message, wparam, lparam);

    if (message == WM_IME_ENDCOMPOSITION) {
        record_ime(message, lparam);
        composition_active_ = false;
        protection_timer_pending_ = false;
        requested_provenance_ = L"none";
    }
    return result;
}

bool RichEditP1Probe::handle_parent_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result)
{
    if (message != WM_COMMAND || reinterpret_cast<HWND>(lparam) != edit_) {
        return false;
    }
    const WORD notification = HIWORD(wparam);
    if (notification == EN_CHANGE) {
        record_change();
    }
    result = 0;
    return false;
}

void RichEditP1Probe::record_change()
{
    const std::wstring after = BodyLf();
    std::size_t prefix = 0;
    while (prefix < before_change_.size() && prefix < after.size() &&
           before_change_[prefix] == after[prefix]) {
        ++prefix;
    }
    std::size_t suffix = 0;
    while (suffix < before_change_.size() - prefix && suffix < after.size() - prefix &&
           before_change_[before_change_.size() - 1 - suffix] == after[after.size() - 1 - suffix]) {
        ++suffix;
    }
    last_delta_.position = static_cast<long>(prefix);
    last_delta_.removed = static_cast<long>(before_change_.size() - prefix - suffix);
    last_delta_.added = static_cast<long>(after.size() - prefix - suffix);
    last_delta_.body_changed = after != before_change_;
    last_delta_.provenance = requested_provenance_;
    provenance_ = requested_provenance_;
    before_change_ = after;
    ++change_notifications_;
}

void RichEditP1Probe::record_ime(UINT message, LPARAM lparam)
{
    ImeObservation observation;
    observation.message = message;
    if (message == WM_IME_COMPOSITION) {
        if ((lparam & GCS_COMPSTR) != 0) {
            observation.composition = read_composition_string(GCS_COMPSTR);
        }
        if ((lparam & GCS_RESULTSTR) != 0) {
            observation.result = read_composition_string(GCS_RESULTSTR);
        }
    }
    ime_observations_.push_back(std::move(observation));
}

std::wstring RichEditP1Probe::read_composition_string(DWORD index) const
{
    HIMC context = ::ImmGetContext(edit_);
    if (context == nullptr) {
        return {};
    }
    const LONG bytes = ::ImmGetCompositionStringW(context, index, nullptr, 0);
    std::wstring value;
    if (bytes > 0) {
        value.resize(static_cast<std::size_t>(bytes) / sizeof(wchar_t));
        ::ImmGetCompositionStringW(context, index, value.data(), static_cast<DWORD>(bytes));
    }
    ::ImmReleaseContext(edit_, context);
    return value;
}

bool RichEditP1Probe::send_physical_key(WORD virtual_key)
{
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = virtual_key;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = virtual_key;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT sent = ::SendInput(2, inputs, sizeof(INPUT));
    drain_messages();
    return sent == 2;
}

}  // namespace pynote::harness
