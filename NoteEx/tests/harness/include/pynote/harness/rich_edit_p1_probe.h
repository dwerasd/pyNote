#pragma once

#include <pynote/harness/win32_harness.h>

#include <string>
#include <utility>
#include <vector>

namespace pynote::harness {

struct RichEditDelta {
    long position = 0;
    long removed = 0;
    long added = 0;
    bool body_changed = false;
    std::wstring provenance = L"none";
};

struct ImeProfile {
    LANGID language = 0;
    HKL layout = nullptr;
    std::wstring klid;
    std::wstring locale_name;
    std::wstring ime_file;
    bool installed = false;
};

struct ImeObservation {
    UINT message = 0;
    std::wstring composition;
    std::wstring result;
};

struct ImeExerciseResult {
    ImeProfile profile;
    bool actual_input_attempted = false;
    bool actual_input_delivered = false;
    bool himc_available = false;
    bool composition_started = false;
    bool preedit_observed = false;
    bool commit_observed = false;
    bool cancel_observed = false;
    bool leave_attempted = false;
    bool leave_rejected = false;
    std::wstring committed_body;
};

class RichEditP1Probe {
public:
    RichEditP1Probe();
    ~RichEditP1Probe();

    RichEditP1Probe(const RichEditP1Probe&) = delete;
    RichEditP1Probe& operator=(const RichEditP1Probe&) = delete;

    HWND parent_hwnd() const noexcept { return parent_.hwnd(); }
    HWND edit_hwnd() const noexcept { return edit_; }
    HWND sibling_hwnd() const noexcept { return sibling_; }

    void LoadBody(const std::wstring& body);
    void ReplaceAll(const std::wstring& body);
    void TypeCharacter(wchar_t character);
    void DeleteSelection();
    void SetSelection(long start, long end);
    std::pair<long, long> Selection() const;
    long Cursor() const;
    std::wstring BodyLf() const;

    bool CanUndo() const;
    bool Undo();
    void ClearUndo();

    void ApplyLineSpacing(double multiplier);
    double AppliedLineSpacing() const noexcept { return applied_line_spacing_; }
    long ActualLineHeightPixels() const;

    std::size_t change_notifications() const noexcept { return change_notifications_; }
    const RichEditDelta& last_delta() const noexcept { return last_delta_; }
    const std::wstring& provenance() const noexcept { return provenance_; }

    bool composition_active() const noexcept { return composition_active_; }
    bool protection_timer_pending() const noexcept { return protection_timer_pending_; }
    const std::wstring& protection_snapshot() const noexcept { return protection_snapshot_; }
    bool AttemptFocusSibling();

    const std::vector<ImeObservation>& ime_observations() const noexcept { return ime_observations_; }
    ImeProfile FindInstalledIme(LANGID language) const;
    ImeExerciseResult ExerciseInstalledIme(LANGID language);

private:
    static LRESULT CALLBACK edit_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_edit_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    bool handle_parent_message(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result);
    void record_change();
    void record_ime(UINT message, LPARAM lparam);
    std::wstring read_composition_string(DWORD index) const;
    bool send_physical_key(WORD virtual_key);

    TestWindow parent_;
    HMODULE msftedit_ = nullptr;
    HWND edit_ = nullptr;
    HWND sibling_ = nullptr;
    WNDPROC original_edit_proc_ = nullptr;

    std::wstring before_change_;
    std::wstring requested_provenance_ = L"none";
    std::wstring provenance_ = L"none";
    RichEditDelta last_delta_;
    std::size_t change_notifications_ = 0;
    double applied_line_spacing_ = 1.0;

    bool composition_active_ = false;
    bool protection_timer_pending_ = false;
    std::wstring protection_snapshot_;
    std::vector<ImeObservation> ime_observations_;
};

}  // namespace pynote::harness
