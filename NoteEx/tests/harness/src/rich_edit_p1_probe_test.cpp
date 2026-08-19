#include <catch_amalgamated.hpp>

#include <pynote/harness/rich_edit_p1_probe.h>

#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"
#include "pynote/core/text/utf16_offset.h"

#include <sqlite3/sqlite3.h>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
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
namespace text = pynote::core::text;

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
        remove_all();
        REQUIRE(database_.Open(path_.string()));
        storage::C_MIGRATION_RUNNER runner;
        runner.SetExistingDatabase(false, path_.string());
        REQUIRE(runner.Run(database_) == storage::E_MIGRATE_RESULT::Ok);

        domain::S_DOCUMENT document;
        document.sId = document_id_;
        document.sTitle = "P1 Rich Edit probe";
        document.nCreatedAtUs = 1000;
        document.nUpdatedAtUs = 1000;
        REQUIRE(repositories_.CreateDocument(document) == storage::E_REPO_RESULT::Ok);
    }

    ~TempRepositoryFixture()
    {
        database_.Close();
        remove_all();
    }

    TempRepositoryFixture(const TempRepositoryFixture&) = delete;
    TempRepositoryFixture& operator=(const TempRepositoryFixture&) = delete;

    DbCounts Counts()
    {
        DbCounts counts;
        std::vector<domain::S_CARD> cards;
        REQUIRE(repositories_.ListCards(document_id_, &cards) == storage::E_REPO_RESULT::Ok);
        counts.cards = cards.size();
        for (const domain::S_CARD& card : cards) {
            std::vector<domain::S_CARD_REVISION> revisions;
            REQUIRE(repositories_.ListRevisions(card.sId, &revisions) == storage::E_REPO_RESULT::Ok);
            counts.revisions += revisions.size();
        }
        std::vector<domain::S_EDIT_EVENT> events;
        REQUIRE(repositories_.ListEvents(document_id_, &events) == storage::E_REPO_RESULT::Ok);
        counts.events = events.size();
        return counts;
    }

    void CreateFirstTypingCard(const std::string& body)
    {
        domain::S_NEW_CAPTURE_OPERATION operation;
        operation.sId = "p1-operation-1";
        operation.sDocumentId = document_id_;
        operation.eSource = domain::E_CAPTURE_OPERATION_SOURCE::Typing;
        operation.eSplitPolicy = domain::E_SPLIT_POLICY::Keep;
        operation.nCreatedAtUs = 2000;

        domain::S_NEW_CARD card;
        card.sId = "p1-card-1";
        card.sRevisionId = "p1-revision-1";
        card.sEventId = "p1-event-1";
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
               ("noteex_p1_" + name + "_" + std::to_string(::GetCurrentProcessId()) + "_" +
                std::to_string(++sequence) + ".db");
    }

    void remove_all()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + "-wal", error);
        std::filesystem::remove(path_.string() + "-shm", error);
    }

    std::filesystem::path path_;
    storage::C_DATABASE database_;
    storage::C_REPOSITORIES repositories_;
    const std::string document_id_ = "p1-document";
};

std::string utf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }
    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                                static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("WideCharToMultiByte size query failed");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), result.data(), required, nullptr, nullptr) !=
        required) {
        throw std::runtime_error("WideCharToMultiByte conversion failed");
    }
    return result;
}

std::string json_string(const std::wstring& value)
{
    const std::string bytes = utf8(value);
    std::ostringstream output;
    output << '"';
    for (const unsigned char byte : bytes) {
        switch (byte) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(byte) << std::dec;
            }
            else {
                output << static_cast<char>(byte);
            }
        }
    }
    output << '"';
    return output.str();
}

std::string json_string(const std::string& value)
{
    return json_string(std::wstring(value.begin(), value.end()));
}

std::string focus_name(const harness::RichEditP1Probe& probe)
{
    const HWND focus = ::GetFocus();
    if (focus == probe.edit_hwnd()) {
        return "edit";
    }
    if (focus == probe.sibling_hwnd()) {
        return "sibling";
    }
    return "other";
}

void emit_trace(const char* test,
                const char* phase,
                const harness::RichEditP1Probe& probe,
                const DbCounts& before,
                const DbCounts& after,
                const harness::ImeExerciseResult* ime = nullptr,
                const char* leave_result = "not-attempted")
{
    static unsigned long sequence = 0;
    const auto selection = probe.Selection();
    const harness::ImeProfile empty_profile;
    const harness::ImeProfile& profile = ime == nullptr ? empty_profile : ime->profile;
    std::cout << "{\"trace\":\"p1-rich-edit\",\"sequence\":" << ++sequence
              << ",\"test\":" << json_string(test)
              << ",\"phase\":" << json_string(phase)
              << ",\"composition_state\":"
              << json_string(probe.composition_active() ? "active" : "inactive")
              << ",\"focus_target\":" << json_string(focus_name(probe))
              << ",\"leave_result\":" << json_string(leave_result)
              << ",\"protection_timer_pending\":"
              << (probe.protection_timer_pending() ? "true" : "false")
              << ",\"protection_snapshot\":" << json_string(probe.protection_snapshot())
              << ",\"db_before\":{\"card\":" << before.cards << ",\"revision\":"
              << before.revisions << ",\"event\":" << before.events << "}"
              << ",\"db_after\":{\"card\":" << after.cards << ",\"revision\":"
              << after.revisions << ",\"event\":" << after.events << "}"
              << ",\"body\":" << json_string(probe.BodyLf())
              << ",\"selection\":{\"start\":" << selection.first << ",\"end\":"
              << selection.second << "},\"cursor_utf16\":" << probe.Cursor()
              << ",\"provenance\":" << json_string(probe.provenance())
              << ",\"undo_available\":" << (probe.CanUndo() ? "true" : "false")
              << ",\"line_spacing\":" << probe.AppliedLineSpacing()
              << ",\"line_height_px\":";
    try {
        std::cout << probe.ActualLineHeightPixels();
    }
    catch (...) {
        std::cout << "null";
    }
    std::cout << ",\"en_change_count\":" << probe.change_notifications()
              << ",\"delta\":{\"position\":" << probe.last_delta().position
              << ",\"removed\":" << probe.last_delta().removed
              << ",\"added\":" << probe.last_delta().added
              << ",\"body_changed\":" << (probe.last_delta().body_changed ? "true" : "false")
              << "},\"ime_profile\":{\"language\":" << json_string(profile.locale_name)
              << ",\"klid\":" << json_string(profile.klid)
              << ",\"ime_file\":" << json_string(profile.ime_file)
              << ",\"installed\":" << (profile.installed ? "true" : "false")
              << ",\"himc\":" << (ime != nullptr && ime->himc_available ? "true" : "false")
              << ",\"actual_input_attempted\":"
              << (ime != nullptr && ime->actual_input_attempted ? "true" : "false")
              << ",\"actual_input_delivered\":"
              << (ime != nullptr && ime->actual_input_delivered ? "true" : "false")
              << "}}\n";
}

void require_counts(const DbCounts& actual, std::size_t cards, std::size_t revisions, std::size_t events)
{
    REQUIRE(actual.cards == cards);
    REQUIRE(actual.revisions == revisions);
    REQUIRE(actual.events == events);
}

}  // namespace

TEST_CASE("T4A-UNC-005 actual installed IME composition trace",
          "[P1][lower-floor][p1-rich-edit][.]")
{
    harness::RichEditP1Probe probe;
    TempRepositoryFixture repository("unc005");
    const DbCounts before = repository.Counts();

    const harness::ImeExerciseResult korean = probe.ExerciseInstalledIme(MAKELANGID(LANG_KOREAN, SUBLANG_DEFAULT));
    if (!korean.committed_body.empty()) {
        repository.CreateFirstTypingCard(utf8(korean.committed_body));
    }
    const DbCounts after_korean = repository.Counts();
    emit_trace("T4A-UNC-005", "korean-actual-ime", probe, before, after_korean, &korean,
               korean.leave_rejected ? "rejected" : "not-rejected");

    // 설치 목록에 없는 언어를 LoadKeyboardLayout/WM_IME 합성으로 대신하지 않는다.
    const harness::ImeExerciseResult japanese = probe.ExerciseInstalledIme(MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT));
    const DbCounts after_japanese = repository.Counts();
    emit_trace("T4A-UNC-005", "japanese-actual-ime", probe, after_korean, after_japanese,
               &japanese, "not-attempted");

    INFO("Korean KLID=" << utf8(korean.profile.klid) << " IME=" << utf8(korean.profile.ime_file));
    REQUIRE(korean.profile.installed);
    REQUIRE(korean.himc_available);
    REQUIRE(korean.actual_input_attempted);
    REQUIRE(korean.actual_input_delivered);
    REQUIRE(korean.composition_started);
    REQUIRE(korean.preedit_observed);
    REQUIRE(korean.commit_observed);
    REQUIRE(korean.cancel_observed);
    REQUIRE(korean.leave_attempted);
    REQUIRE(korean.leave_rejected);
    REQUIRE(probe.provenance() == L"typing");
    require_counts(after_korean, 1, 1, 1);

    INFO("Japanese actual IME is required; installed=" << japanese.profile.installed
         << " attempted=" << japanese.actual_input_attempted);
    REQUIRE(japanese.profile.installed);
    REQUIRE(japanese.actual_input_attempted);
    REQUIRE(japanese.actual_input_delivered);
}

TEST_CASE("T4A-UNC-007 LF CRLF non-BMP deletion and formatting delta",
          "[P1][lower-floor][p1-rich-edit][.]")
{
    harness::RichEditP1Probe probe;
    TempRepositoryFixture repository("unc007");
    const DbCounts before = repository.Counts();

    probe.LoadBody(L"format line one\nformat line two");
    const HWND stable_hwnd = probe.edit_hwnd();
    probe.ApplyLineSpacing(1.0);
    const DbCounts after_format = repository.Counts();
    emit_trace("T4A-UNC-007", "formatting-not-body", probe, before, after_format);
    REQUIRE(probe.edit_hwnd() == stable_hwnd);
    REQUIRE_FALSE(probe.last_delta().body_changed);
    REQUIRE(probe.last_delta().provenance == L"formatting");
    require_counts(after_format, 0, 0, 0);

    probe.LoadBody(L"A\r\n\U0001F642B");
    REQUIRE(probe.BodyLf() == L"A\n\U0001F642B");
    const std::string body_utf8 = "A\n\xF0\x9F\x99\x82" "B";
    REQUIRE(text::utf8_index_to_utf16_offset(body_utf8, 2) == 2);
    REQUIRE(text::utf8_index_to_utf16_offset(body_utf8, 6) == 4);
    REQUIRE(text::utf16_offset_to_utf8_index(body_utf8, 4) == 6);

    probe.SetSelection(2, 4);
    REQUIRE(probe.Selection() == std::pair<long, long>{2, 4});
    probe.DeleteSelection();
    REQUIRE(probe.BodyLf() == L"A\nB");
    REQUIRE(probe.last_delta().body_changed);
    REQUIRE(probe.last_delta().position == 2);
    REQUIRE(probe.last_delta().removed == 2);
    REQUIRE(probe.last_delta().added == 0);
    emit_trace("T4A-UNC-007", "non-bmp-delete", probe, after_format, repository.Counts());
}

TEST_CASE("T4A-UNC-009 seven Rich Edit criteria retain one HWND",
          "[P1][lower-floor][p1-rich-edit][.]")
{
    harness::RichEditP1Probe probe;
    TempRepositoryFixture repository("unc009");
    const HWND hwnd = probe.edit_hwnd();
    const DbCounts before = repository.Counts();

    probe.LoadBody(L"");
    REQUIRE(probe.edit_hwnd() == hwnd);
    REQUIRE_FALSE(probe.CanUndo());
    probe.TypeCharacter(L'A');
    REQUIRE(probe.edit_hwnd() == hwnd);
    REQUIRE(probe.BodyLf() == L"A");
    REQUIRE(probe.provenance() == L"typing");
    REQUIRE(probe.CanUndo());
    REQUIRE(probe.Undo());
    REQUIRE(probe.BodyLf().empty());
    REQUIRE_FALSE(probe.CanUndo());

    probe.LoadBody(L"L1\n\U0001F642L2");
    probe.SetSelection(3, 5);
    REQUIRE(probe.Cursor() == 5);
    REQUIRE(probe.edit_hwnd() == hwnd);

    std::vector<long> heights;
    for (const double spacing : {0.8, 1.0, 3.0}) {
        probe.ApplyLineSpacing(spacing);
        heights.push_back(probe.ActualLineHeightPixels());
        emit_trace("T4A-UNC-009", spacing == 0.8 ? "spacing-0.8" : spacing == 1.0 ? "spacing-1.0" : "spacing-3.0",
                   probe, before, repository.Counts());
        REQUIRE(probe.edit_hwnd() == hwnd);
        REQUIRE_FALSE(probe.last_delta().body_changed);
    }
    REQUIRE(heights[0] < heights[1]);
    REQUIRE(heights[1] < heights[2]);
    REQUIRE(std::abs(static_cast<double>(heights[0]) / heights[1] - 0.8) <= 0.12);
    REQUIRE(std::abs(static_cast<double>(heights[2]) / heights[1] - 3.0) <= 0.12);
    require_counts(repository.Counts(), 0, 0, 0);

    const harness::ImeExerciseResult korean = probe.ExerciseInstalledIme(MAKELANGID(LANG_KOREAN, SUBLANG_DEFAULT));
    emit_trace("T4A-UNC-009", "same-hwnd-korean-ime", probe, before, repository.Counts(),
               &korean, korean.leave_rejected ? "rejected" : "not-rejected");
    REQUIRE(probe.edit_hwnd() == hwnd);
    REQUIRE(korean.actual_input_delivered);
    REQUIRE(korean.preedit_observed);
    REQUIRE(korean.leave_rejected);
}

TEST_CASE("T4A-UNC-010 first input load-clear and replace-all undo units",
          "[P1][lower-floor][p1-rich-edit][.]")
{
    harness::RichEditP1Probe probe;
    TempRepositoryFixture repository("unc010");
    const DbCounts counts = repository.Counts();

    probe.LoadBody(L"");
    REQUIRE_FALSE(probe.CanUndo());
    probe.TypeCharacter(L'X');
    REQUIRE(probe.BodyLf() == L"X");
    REQUIRE(probe.CanUndo());
    REQUIRE(probe.Undo());
    REQUIRE(probe.BodyLf().empty());
    REQUIRE_FALSE(probe.CanUndo());
    emit_trace("T4A-UNC-010", "first-meaningful-one-undo", probe, counts, repository.Counts());

    probe.TypeCharacter(L'Y');
    REQUIRE(probe.CanUndo());
    probe.LoadBody(L"loaded snapshot");
    REQUIRE(probe.BodyLf() == L"loaded snapshot");
    REQUIRE_FALSE(probe.CanUndo());
    emit_trace("T4A-UNC-010", "load-clears-undo", probe, counts, repository.Counts());

    probe.LoadBody(L"old\n\U0001F642");
    probe.ReplaceAll(L"new\ntext");
    REQUIRE(probe.BodyLf() == L"new\ntext");
    REQUIRE(probe.CanUndo());
    REQUIRE(probe.Undo());
    REQUIRE(probe.BodyLf() == L"old\n\U0001F642");
    REQUIRE_FALSE(probe.CanUndo());
    emit_trace("T4A-UNC-010", "replace-all-one-undo", probe, counts, repository.Counts());
    require_counts(repository.Counts(), 0, 0, 0);
}
