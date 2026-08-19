#pragma once

#include <windows.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pynote::harness {

struct DirectWriteEditDelta {
    std::size_t position = 0;
    std::size_t removed = 0;
    std::size_t added = 0;
    bool body_changed = false;
    std::wstring provenance = L"none";
};

struct DirectWriteImeProfile {
    LANGID language = 0;
    HKL layout = nullptr;
    std::wstring klid;
    std::wstring locale_name;
    std::wstring ime_file;
    bool installed = false;
};

struct DirectWriteImeObservation {
    UINT message = 0;
    LPARAM flags = 0;
    std::wstring composition;
    std::wstring result;
    LONG cursor_position = -1;
    std::vector<unsigned char> attributes;
};

struct DirectWriteImeExerciseResult {
    DirectWriteImeProfile profile;
    std::wstring blocked_environment;
    bool actual_input_attempted = false;
    bool send_input_accepted = false;
    bool keydown_observed = false;
    bool actual_input_delivered = false;
    bool visible_restored_nonzero = false;
    bool same_ui_thread = false;
    bool target_hkl_active = false;
    bool foreground_active_focus = false;
    bool gui_thread_focus = false;
    bool himc_connected = false;
    bool ime_open_native_readback = false;
    bool ime_windows_positioned = false;
    bool composition_started = false;
    bool preedit_observed = false;
    bool commit_observed = false;
    bool cancel_observed = false;
    bool leave_attempted = false;
    bool leave_rejected = false;
    std::wstring committed_body;
};

struct DirectWriteFrameObservation {
    bool layout_valid = false;
    bool presented = false;
    bool caret_hit_tested = false;
    bool caret_drawn = false;
    bool selection_hit_tested = false;
    bool selection_drawn = false;
    bool preedit_drawn = false;
    std::size_t line_count = 0;
    float line_height_dips = 0.0f;
    POINT ime_caret_client{};
};

class DirectWriteImm32P1Probe {
public:
    DirectWriteImm32P1Probe();
    ~DirectWriteImm32P1Probe();

    DirectWriteImm32P1Probe(const DirectWriteImm32P1Probe&) = delete;
    DirectWriteImm32P1Probe& operator=(const DirectWriteImm32P1Probe&) = delete;

    bool ready() const noexcept;
    HWND editor_hwnd() const noexcept;
    HWND sibling_hwnd() const noexcept;

    void LoadBody(const std::wstring& body);
    void ReplaceAll(const std::wstring& body);
    void TypeCharacter(wchar_t character);
    void DeleteSelection();
    void SetSelection(std::size_t start, std::size_t end);
    std::pair<std::size_t, std::size_t> Selection() const noexcept;
    std::size_t Cursor() const noexcept;
    const std::wstring& BodyLf() const noexcept;

    bool CanUndo() const noexcept;
    bool Undo();
    bool CanRedo() const noexcept;
    bool Redo();

    void ApplyLineSpacing(double multiplier);
    double AppliedLineSpacing() const noexcept;
    float ActualLineHeightDips();
    bool Render();
    bool HitTestPoint(float x_dips, float y_dips);
    const DirectWriteFrameObservation& last_frame() const noexcept;

    const DirectWriteEditDelta& last_delta() const noexcept;
    const std::wstring& provenance() const noexcept;
    bool composition_active() const noexcept;
    const std::wstring& preedit() const noexcept;
    LONG preedit_cursor() const noexcept;
    const std::vector<unsigned char>& preedit_attributes() const noexcept;
    bool protection_timer_pending() const noexcept;
    const std::wstring& protection_snapshot() const noexcept;
    bool AttemptFocusSibling();

    const std::vector<DirectWriteImeObservation>& ime_observations() const noexcept;
    DirectWriteImeProfile FindInstalledIme(LANGID language) const;
    DirectWriteImeExerciseResult ExerciseInstalledIme(LANGID language);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pynote::harness
