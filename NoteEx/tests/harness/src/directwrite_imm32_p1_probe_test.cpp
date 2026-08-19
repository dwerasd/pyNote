#include <catch_amalgamated.hpp>

#include <pynote/harness/directwrite_imm32_p1_probe.h>

#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "NoteExCore")

namespace {

namespace domain = pynote::core::domain;
namespace harness = pynote::harness;
namespace storage = pynote::core::storage;

struct DbCounts {
    std::size_t cards = 0;
    std::size_t revisions = 0;
    std::size_t events = 0;
};

class TempRepositoryFixture {
public:
    explicit TempRepositoryFixture(const std::string& name)
        : path_(make_path(name)), repositories_(database_)
    {
        remove_files();
        REQUIRE(database_.Open(path_.string()));
        storage::C_MIGRATION_RUNNER runner;
        runner.SetExistingDatabase(false, path_.string());
        REQUIRE(runner.Run(database_) == storage::E_MIGRATE_RESULT::Ok);
        domain::S_DOCUMENT document;
        document.sId = document_id_;
        document.sTitle = "DirectWrite IMM32 P1 probe";
        document.nCreatedAtUs = 1000;
        document.nUpdatedAtUs = 1000;
        REQUIRE(repositories_.CreateDocument(document) == storage::E_REPO_RESULT::Ok);
    }

    ~TempRepositoryFixture()
    {
        database_.Close();
        remove_files();
    }

    DbCounts Counts()
    {
        DbCounts counts;
        std::vector<domain::S_CARD> cards;
        REQUIRE(repositories_.ListCards(document_id_, &cards) == storage::E_REPO_RESULT::Ok);
        counts.cards = cards.size();
        for (const auto& card : cards) {
            std::vector<domain::S_CARD_REVISION> revisions;
            REQUIRE(repositories_.ListRevisions(card.sId, &revisions) == storage::E_REPO_RESULT::Ok);
            counts.revisions += revisions.size();
        }
        std::vector<domain::S_EDIT_EVENT> events;
        REQUIRE(repositories_.ListEvents(document_id_, &events) == storage::E_REPO_RESULT::Ok);
        counts.events = events.size();
        return counts;
    }

    void CreateTypingCard(const std::string& body)
    {
        domain::S_NEW_CAPTURE_OPERATION operation;
        operation.sId = "directwrite-operation-1";
        operation.sDocumentId = document_id_;
        operation.eSource = domain::E_CAPTURE_OPERATION_SOURCE::Typing;
        operation.eSplitPolicy = domain::E_SPLIT_POLICY::Keep;
        operation.nCreatedAtUs = 2000;

        domain::S_NEW_CARD card;
        card.sId = "directwrite-card-1";
        card.sRevisionId = "directwrite-revision-1";
        card.sEventId = "directwrite-event-1";
        card.nPositionKey = 1024;
        card.sBody = body;
        card.eCardSource = domain::E_CARD_SOURCE::Typing;
        card.eEventSource = domain::E_EVENT_SOURCE::Typing;
        card.eRevisionSource = domain::E_REVISION_SOURCE::Edit;
        card.nCreatedAtUs = 2001;
        std::vector<domain::S_CARD> created;
        REQUIRE(repositories_.CreateCards(operation, {card}, &created) == storage::E_REPO_RESULT::Ok);
        REQUIRE(created.size() == 1);
    }

private:
    static std::filesystem::path make_path(const std::string& name)
    {
        static std::atomic<unsigned long> sequence{0};
        return std::filesystem::temp_directory_path() /
            ("noteex_directwrite_imm32_" + name + "_" +
             std::to_string(::GetCurrentProcessId()) + "_" +
             std::to_string(++sequence) + ".db");
    }

    void remove_files()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + "-wal", error);
        std::filesystem::remove(path_.string() + "-shm", error);
    }

    std::filesystem::path path_;
    storage::C_DATABASE database_;
    storage::C_REPOSITORIES repositories_;
    const std::string document_id_ = "directwrite-document";
};

std::string utf8(const std::wstring& value)
{
    if (value.empty()) { return {}; }
    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) { throw std::runtime_error("WideCharToMultiByte size query failed"); }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), required, nullptr, nullptr) != required) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    return result;
}

std::string json(const std::wstring& value)
{
    const std::string bytes = utf8(value);
    std::ostringstream output;
    output << '"';
    for (const unsigned char byte : bytes) {
        switch (byte) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(byte) << std::dec;
            }
            else { output << static_cast<char>(byte); }
        }
    }
    output << '"';
    return output.str();
}

std::string json(const std::string& value)
{
    return json(std::wstring(value.begin(), value.end()));
}

const char* boolean(bool value) { return value ? "true" : "false"; }

void emit_trace(const char* test, const char* phase,
                const harness::DirectWriteImm32P1Probe& probe,
                const DbCounts& before, const DbCounts& after,
                const harness::DirectWriteImeExerciseResult* ime = nullptr,
                const char* leave = "not-attempted")
{
    static unsigned long sequence = 0;
    const auto selection = probe.Selection();
    const harness::DirectWriteImeProfile empty_profile;
    const auto& profile = ime == nullptr ? empty_profile : ime->profile;
    const auto& frame = probe.last_frame();
    std::cout << "{\"trace\":\"p1-directwrite-imm32\",\"candidate\":\"DirectWrite+IMM32\""
              << ",\"sequence\":" << ++sequence << ",\"test\":" << json(test)
              << ",\"phase\":" << json(phase)
              << ",\"composition_state\":" << json(probe.composition_active() ? "active" : "inactive")
              << ",\"preedit\":" << json(probe.preedit())
              << ",\"preedit_cursor\":" << probe.preedit_cursor()
              << ",\"preedit_attributes\":[";
    for (std::size_t index = 0; index < probe.preedit_attributes().size(); ++index) {
        if (index != 0) { std::cout << ','; }
        std::cout << static_cast<unsigned>(probe.preedit_attributes()[index]);
    }
    std::cout << "],\"focus_target\":"
              << json(::GetFocus() == probe.editor_hwnd() ? "editor" :
                      ::GetFocus() == probe.sibling_hwnd() ? "sibling" : "other")
              << ",\"leave_result\":" << json(leave)
              << ",\"protection_timer_pending\":" << boolean(probe.protection_timer_pending())
              << ",\"protection_snapshot\":" << json(probe.protection_snapshot())
              << ",\"db_before\":{\"card\":" << before.cards << ",\"revision\":"
              << before.revisions << ",\"event\":" << before.events << "}"
              << ",\"db_after\":{\"card\":" << after.cards << ",\"revision\":"
              << after.revisions << ",\"event\":" << after.events << "}"
              << ",\"body\":" << json(probe.BodyLf())
              << ",\"selection\":{\"start\":" << selection.first << ",\"end\":"
              << selection.second << "},\"cursor_utf16\":" << probe.Cursor()
              << ",\"provenance\":" << json(probe.provenance())
              << ",\"undo_available\":" << boolean(probe.CanUndo())
              << ",\"line_spacing\":" << probe.AppliedLineSpacing()
              << ",\"actual_line_height_dips\":" << frame.line_height_dips
              << ",\"layout\":{\"valid\":" << boolean(frame.layout_valid)
              << ",\"presented\":" << boolean(frame.presented)
              << ",\"caret_hit_tested\":" << boolean(frame.caret_hit_tested)
              << ",\"caret_drawn\":" << boolean(frame.caret_drawn)
              << ",\"selection_hit_tested\":" << boolean(frame.selection_hit_tested)
              << ",\"selection_drawn\":" << boolean(frame.selection_drawn)
              << ",\"preedit_drawn\":" << boolean(frame.preedit_drawn) << "}"
              << ",\"delta\":{\"position\":" << probe.last_delta().position
              << ",\"removed\":" << probe.last_delta().removed
              << ",\"added\":" << probe.last_delta().added
              << ",\"body_changed\":" << boolean(probe.last_delta().body_changed) << "}"
              << ",\"ime_profile\":{\"language\":" << json(profile.locale_name)
              << ",\"klid\":" << json(profile.klid)
              << ",\"ime_file\":" << json(profile.ime_file)
              << ",\"installed\":" << boolean(profile.installed)
              << ",\"blocked_environment\":"
              << json(ime == nullptr ? std::wstring() : ime->blocked_environment)
              << ",\"attempted\":" << boolean(ime != nullptr && ime->actual_input_attempted)
              << ",\"send_input_accepted\":" << boolean(ime != nullptr && ime->send_input_accepted)
              << ",\"keydown_observed\":" << boolean(ime != nullptr && ime->keydown_observed)
              << ",\"delivered\":" << boolean(ime != nullptr && ime->actual_input_delivered)
              << ",\"visible_restored_nonzero\":" << boolean(ime != nullptr && ime->visible_restored_nonzero)
              << ",\"same_ui_thread\":" << boolean(ime != nullptr && ime->same_ui_thread)
              << ",\"target_hkl_active\":" << boolean(ime != nullptr && ime->target_hkl_active)
              << ",\"foreground_active_focus\":" << boolean(ime != nullptr && ime->foreground_active_focus)
              << ",\"gui_thread_focus\":" << boolean(ime != nullptr && ime->gui_thread_focus)
              << ",\"himc_connected\":" << boolean(ime != nullptr && ime->himc_connected)
              << ",\"open_native_readback\":" << boolean(ime != nullptr && ime->ime_open_native_readback)
              << ",\"ime_windows_positioned\":" << boolean(ime != nullptr && ime->ime_windows_positioned)
              << "},\"ime_sequence\":[";
    const auto& observations = probe.ime_observations();
    for (std::size_t index = 0; index < observations.size(); ++index) {
        if (index != 0) { std::cout << ','; }
        const auto& item = observations[index];
        std::cout << "{\"message\":" << item.message << ",\"flags\":" << item.flags
                  << ",\"composition\":" << json(item.composition)
                  << ",\"result\":" << json(item.result)
                  << ",\"cursor\":" << item.cursor_position << ",\"attributes\":[";
        for (std::size_t attr = 0; attr < item.attributes.size(); ++attr) {
            if (attr != 0) { std::cout << ','; }
            std::cout << static_cast<unsigned>(item.attributes[attr]);
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
}

void require_counts(const DbCounts& value, std::size_t cards,
                    std::size_t revisions, std::size_t events)
{
    REQUIRE(value.cards == cards);
    REQUIRE(value.revisions == revisions);
    REQUIRE(value.events == events);
}

}  // namespace

TEST_CASE("T4A-UNC-005 DirectWrite IMM32 actual installed IME result-only commit",
          "[P1][lower-floor][p1-directwrite-imm32][.]")
{
    harness::DirectWriteImm32P1Probe probe;
    TempRepositoryFixture repository("unc005");
    const DbCounts before = repository.Counts();

    const auto korean = probe.ExerciseInstalledIme(MAKELANGID(LANG_KOREAN, SUBLANG_DEFAULT));
    if (korean.commit_observed && !korean.committed_body.empty()) {
        repository.CreateTypingCard(utf8(korean.committed_body));
    }
    const DbCounts after_korean = repository.Counts();
    emit_trace("T4A-UNC-005", "korean-actual-ime", probe, before, after_korean, &korean,
               korean.leave_rejected ? "rejected" : "not-rejected");

    INFO("Korean preflight block=" << utf8(korean.blocked_environment));
    REQUIRE(korean.profile.installed);
    REQUIRE(korean.visible_restored_nonzero);
    REQUIRE(korean.same_ui_thread);
    REQUIRE(korean.target_hkl_active);
    REQUIRE(korean.foreground_active_focus);
    REQUIRE(korean.gui_thread_focus);
    REQUIRE(korean.himc_connected);
    REQUIRE(korean.ime_open_native_readback);
    REQUIRE(korean.ime_windows_positioned);
    REQUIRE(korean.actual_input_attempted);
    REQUIRE(korean.keydown_observed);
    REQUIRE(korean.actual_input_delivered);
    REQUIRE(korean.composition_started);
    REQUIRE(korean.preedit_observed);
    REQUIRE(korean.commit_observed);
    REQUIRE(korean.cancel_observed);
    REQUIRE(korean.leave_rejected);
    REQUIRE(probe.provenance() == L"typing");
    require_counts(after_korean, 1, 1, 1);

}

TEST_CASE("T4A-UNC-007 DirectWrite IMM32 LF non-BMP selection hit-test and delta",
          "[P1][lower-floor][p1-directwrite-imm32][.]")
{
    harness::DirectWriteImm32P1Probe probe;
    TempRepositoryFixture repository("unc007");
    const DbCounts counts = repository.Counts();
    const HWND hwnd = probe.editor_hwnd();

    probe.LoadBody(L"A\r\n\U0001F642B");
    REQUIRE(probe.BodyLf() == L"A\n\U0001F642B");
    probe.SetSelection(3, 3);
    REQUIRE((probe.Selection() == std::pair<std::size_t, std::size_t>{2, 4}));
    REQUIRE(probe.Render());
    REQUIRE(probe.last_frame().selection_hit_tested);
    REQUIRE(probe.last_frame().selection_drawn);
    REQUIRE(probe.last_frame().caret_hit_tested);
    REQUIRE(probe.last_frame().caret_drawn);
    probe.DeleteSelection();
    REQUIRE(probe.BodyLf() == L"A\nB");
    REQUIRE(probe.last_delta().position == 2);
    REQUIRE(probe.last_delta().removed == 2);
    REQUIRE(probe.last_delta().added == 0);
    REQUIRE(probe.HitTestPoint(11.0f, 11.0f));
    REQUIRE(probe.Cursor() <= probe.BodyLf().size());
    REQUIRE(probe.editor_hwnd() == hwnd);
    require_counts(repository.Counts(), 0, 0, 0);
    emit_trace("T4A-UNC-007", "lf-non-bmp-selection-delete", probe, counts,
               repository.Counts());
}

TEST_CASE("T4A-UNC-009 DirectWrite IMM32 seven P1 criteria retain one editor HWND",
          "[P1][lower-floor][p1-directwrite-imm32][.]")
{
    harness::DirectWriteImm32P1Probe probe;
    TempRepositoryFixture repository("unc009");
    const DbCounts counts = repository.Counts();
    const HWND hwnd = probe.editor_hwnd();
    REQUIRE(probe.ready());
    REQUIRE(::IsWindow(hwnd));

    probe.LoadBody(L"line one\nline two\n\U0001F642 line three");
    probe.SetSelection(5, 5);
    REQUIRE(probe.editor_hwnd() == hwnd);
    REQUIRE(probe.Render());
    REQUIRE(probe.last_frame().layout_valid);
    REQUIRE(probe.last_frame().presented);
    REQUIRE(probe.last_frame().caret_hit_tested);
    REQUIRE(probe.last_frame().caret_drawn);

    std::vector<float> heights;
    for (const double spacing : {0.8, 1.0, 3.0}) {
        const std::wstring body_before = probe.BodyLf();
        const bool undo_before = probe.CanUndo();
        probe.ApplyLineSpacing(spacing);
        heights.push_back(probe.ActualLineHeightDips());
        emit_trace("T4A-UNC-009", spacing == 0.8 ? "spacing-0.8" :
                   spacing == 1.0 ? "spacing-1.0" : "spacing-3.0",
                   probe, counts, repository.Counts());
        REQUIRE(probe.BodyLf() == body_before);
        REQUIRE(probe.CanUndo() == undo_before);
        REQUIRE_FALSE(probe.last_delta().body_changed);
        REQUIRE(probe.last_delta().provenance == L"formatting");
        REQUIRE(probe.editor_hwnd() == hwnd);
        require_counts(repository.Counts(), 0, 0, 0);
    }
    REQUIRE(heights[0] < heights[1]);
    REQUIRE(heights[1] < heights[2]);
    REQUIRE(std::abs(static_cast<double>(heights[0] / heights[1]) - 0.8) <= 0.05);
    REQUIRE(std::abs(static_cast<double>(heights[2] / heights[1]) - 3.0) <= 0.05);

    const auto korean = probe.ExerciseInstalledIme(MAKELANGID(LANG_KOREAN, SUBLANG_DEFAULT));
    emit_trace("T4A-UNC-009", "same-hwnd-korean-ime", probe, counts,
               repository.Counts(), &korean,
               korean.leave_rejected ? "rejected" : "not-rejected");
    REQUIRE(probe.editor_hwnd() == hwnd);
    REQUIRE(korean.actual_input_delivered);
    REQUIRE(korean.leave_rejected);
}

TEST_CASE("T4A-UNC-010 DirectWrite IMM32 first input load-clear replace-all undo units",
          "[P1][lower-floor][p1-directwrite-imm32][.]")
{
    harness::DirectWriteImm32P1Probe probe;
    TempRepositoryFixture repository("unc010");
    const DbCounts counts = repository.Counts();

    probe.LoadBody(L"");
    REQUIRE_FALSE(probe.CanUndo());
    probe.TypeCharacter(L'X');
    REQUIRE(probe.BodyLf() == L"X");
    REQUIRE(probe.provenance() == L"typing");
    REQUIRE(probe.CanUndo());
    REQUIRE(probe.Undo());
    REQUIRE(probe.BodyLf().empty());
    REQUIRE_FALSE(probe.CanUndo());
    REQUIRE(probe.CanRedo());
    REQUIRE(probe.Redo());
    REQUIRE(probe.BodyLf() == L"X");
    REQUIRE(probe.Undo());
    emit_trace("T4A-UNC-010", "first-meaningful-one-undo", probe, counts,
               repository.Counts());

    probe.TypeCharacter(L'Y');
    REQUIRE(probe.CanUndo());
    probe.LoadBody(L"loaded snapshot");
    REQUIRE_FALSE(probe.CanUndo());
    REQUIRE_FALSE(probe.CanRedo());
    emit_trace("T4A-UNC-010", "load-clears-undo-redo", probe, counts,
               repository.Counts());

    probe.LoadBody(L"old\n\U0001F642");
    probe.ReplaceAll(L"new\r\ntext");
    REQUIRE(probe.BodyLf() == L"new\ntext");
    REQUIRE(probe.CanUndo());
    REQUIRE(probe.Undo());
    REQUIRE(probe.BodyLf() == L"old\n\U0001F642");
    REQUIRE_FALSE(probe.CanUndo());
    emit_trace("T4A-UNC-010", "replace-all-one-undo", probe, counts,
               repository.Counts());
    require_counts(repository.Counts(), 0, 0, 0);
}
