#include <catch_amalgamated.hpp>

#include <pynote/harness/p2_card_list_prototype.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt")
#pragma comment(lib, "D2DWrapp")

namespace {

namespace harness = pynote::harness;

constexpr std::size_t kCorpusRowCount = 10000;
constexpr std::size_t kCorpusUtf8Bytes = 759944;
constexpr char kCorpusSha256[] = "3d6d906c4c87a659589ebb96e1170cec4832ed45fe8b6455ec8a71b2e52e4d8a";
constexpr wchar_t kFirstRow[] =
    L"00000|\uce74\ub4dc \ubcf8\ubb38 00|emoji\U0001F642|longword_x\n\ub458\uc9f8 \uc904";
constexpr wchar_t kLastRow[] =
    L"09999|\uce74\ub4dc \ubcf8\ubb38 08|emoji\U0001F642|longword_xxxxxxxxxxxxxxxxxx\n"
    L"\ub458\uc9f8 \uc904";
constexpr char kMeasurementStart[] = "before-corpus-model-generation";
constexpr char kMeasurementEnd[] = "after-last-row-layout-draw-present";

struct Corpus {
    std::vector<std::wstring> rows;
    std::string utf8;
};

std::string to_utf8(const std::wstring& value)
{
    if (value.empty()) { return {}; }
    const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                                value.data(), static_cast<int>(value.size()),
                                                nullptr, 0, nullptr, nullptr);
    if (required <= 0) { throw std::runtime_error("WideCharToMultiByte size query failed"); }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                              value.data(), static_cast<int>(value.size()),
                              result.data(), required, nullptr, nullptr) != required) {
        throw std::runtime_error("WideCharToMultiByte conversion failed");
    }
    return result;
}

Corpus make_corpus()
{
    Corpus corpus;
    corpus.rows.reserve(kCorpusRowCount);
    corpus.utf8.reserve(kCorpusUtf8Bytes);
    for (std::size_t index = 0; index < kCorpusRowCount; ++index) {
        wchar_t prefix[96]{};
        _snwprintf_s(prefix, _countof(prefix), _TRUNCATE,
                     L"%05zu|\uce74\ub4dc \ubcf8\ubb38 %02zu|emoji\U0001F642|longword_",
                     index, index % 97);
        std::wstring row(prefix);
        row.append(index % 23 + 1, L'x');
        row += L"\n\ub458\uc9f8 \uc904";
        if (index != 0) { corpus.utf8 += "\n---ROW---\n"; }
        corpus.utf8 += to_utf8(row);
        corpus.rows.push_back(std::move(row));
    }
    return corpus;
}

std::string sha256_hex(const std::string& bytes)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_bytes = 0;
    DWORD digest_bytes = 0;
    DWORD copied = 0;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                                       nullptr, 0)) ||
        !BCRYPT_SUCCESS(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                             reinterpret_cast<PUCHAR>(&object_bytes),
                                             sizeof(object_bytes), &copied, 0)) ||
        !BCRYPT_SUCCESS(::BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                                             reinterpret_cast<PUCHAR>(&digest_bytes),
                                             sizeof(digest_bytes), &copied, 0))) {
        if (algorithm != nullptr) { ::BCryptCloseAlgorithmProvider(algorithm, 0); }
        throw std::runtime_error("BCrypt SHA-256 setup failed");
    }
    std::vector<UCHAR> object(object_bytes);
    std::vector<UCHAR> digest(digest_bytes);
    if (!BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, object.data(), object_bytes,
                                           nullptr, 0, 0)) ||
        !BCRYPT_SUCCESS(::BCryptHashData(hash,
                                         reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                         static_cast<ULONG>(bytes.size()), 0)) ||
        !BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest.data(), digest_bytes, 0))) {
        if (hash != nullptr) { ::BCryptDestroyHash(hash); }
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("BCrypt SHA-256 calculation failed");
    }
    ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(algorithm, 0);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const UCHAR byte : digest) { output << std::setw(2) << static_cast<unsigned>(byte); }
    return output.str();
}

void require_fixed_corpus(const Corpus& corpus)
{
    REQUIRE(corpus.rows.size() == kCorpusRowCount);
    REQUIRE(corpus.rows.front() == kFirstRow);
    REQUIRE(corpus.rows.back() == kLastRow);
    REQUIRE(corpus.utf8.size() == kCorpusUtf8Bytes);
    REQUIRE(sha256_hex(corpus.utf8) == kCorpusSha256);
}

std::vector<std::wstring> small_rows()
{
    return {
        L"\uccab \uce74\ub4dc \ubcf8\ubb38\n\ub458\uc9f8 \uc904",
        L"word boundary then longword_xxxxxxxxxxxxxxxxxxxxxxxxx\n\ub458\uc9f8 \uc904",
        L"emoji \U0001F642 remains paired\n\ub458\uc9f8 \uc904"
    };
}

}  // namespace

TEST_CASE("P2 DirectWrite layout preserves wrapping trimming metrics and UTF-16 hit tests",
          "[P2][layout][lower-floor][T4A-UNC-011][T4A-UNC-012]")
{
    harness::P2CardListPrototype prototype;
    REQUIRE(prototype.ready());
    REQUIRE(::IsWindow(prototype.hwnd()));
    REQUIRE(prototype.child_window_count() == 0);

    const harness::P2LayoutObservation crlf = prototype.ProbeLayout(
        L"alpha beta\r\ngamma delta", 300.0f, 0, 1);
    REQUIRE(crlf.valid);
    REQUIRE(std::find(crlf.newline_lengths.begin(), crlf.newline_lengths.end(), 2u) !=
            crlf.newline_lengths.end());

    const harness::P2LayoutObservation lf = prototype.ProbeLayout(
        L"alpha beta\ngamma delta", 300.0f, 0, 1);
    REQUIRE(lf.valid);
    REQUIRE(std::find(lf.newline_lengths.begin(), lf.newline_lengths.end(), 1u) !=
            lf.newline_lengths.end());

    const std::wstring wrapped =
        L"alpha beta longword_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx ";
    const harness::P2LayoutObservation wrap = prototype.ProbeLayout(wrapped, 58.0f, 0, 1);
    REQUIRE(wrap.valid);
    REQUIRE(wrap.line_lengths.size() >= 4);
    // DWRITE_WORD_WRAPPING_WRAP keeps the first word boundary, then emergency-breaks the long token.
    REQUIRE(wrap.line_lengths.front() == 6);
    REQUIRE(std::any_of(wrap.line_lengths.begin() + 1, wrap.line_lengths.end(),
                        [](std::uint32_t length) { return length > 0 && length < 20; }));

    const harness::P2LayoutObservation trimmed = prototype.ProbeLayout(
        L"one two three four five six seven eight nine ten eleven twelve thirteen fourteen",
        72.0f, 2, 4);
    REQUIRE(trimmed.valid);
    REQUIRE(trimmed.line_lengths.size() == 2);
    REQUIRE(trimmed.final_line_trimmed);

    const harness::P2LayoutObservation non_bmp = prototype.ProbeLayout(
        L"A\U0001F642B", 200.0f, 0, 1);
    REQUIRE(non_bmp.valid);
    REQUIRE(non_bmp.text_to_point);
    REQUIRE(non_bmp.point_to_text);
    REQUIRE(non_bmp.roundtrip_position == 1);
    REQUIRE(non_bmp.safe_slice == L"\U0001F642");
    REQUIRE(non_bmp.safe_slice.size() == 2);

    prototype.SetRows(small_rows());
    REQUIRE(prototype.Render());
    REQUIRE(prototype.last_frame().presented);
    REQUIRE(prototype.last_frame().layout_count == 3);
    REQUIRE(prototype.last_frame().line_metrics_count >= 6);
    REQUIRE(prototype.child_window_count() == 0);
}

TEST_CASE("P2 virtual card list generates and reaches 10000 fixed rows below ten seconds",
          "[P2][performance][lower-floor]")
{
    harness::P2CardListPrototype prototype(640, 480);
    REQUIRE(prototype.ready());
    REQUIRE(prototype.child_window_count() == 0);

    std::array<double, 5> measured_ms{};
    for (std::size_t iteration = 0; iteration < 6; ++iteration) {
        // Frozen boundary: starts immediately before corpus/model generation and ends only after
        // the final row has been laid out, drawn, and presented by the real D2DWrapp HWND path.
        const auto started = std::chrono::steady_clock::now();
        Corpus corpus = make_corpus();
        require_fixed_corpus(corpus);
        prototype.SetRows(std::move(corpus.rows));
        prototype.ScrollToLastRow();
        REQUIRE(prototype.Render());
        const auto finished = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double, std::milli>(finished - started).count();

        REQUIRE(prototype.last_frame().presented);
        REQUIRE(prototype.last_frame().last_visible_row == kCorpusRowCount - 1);
        REQUIRE(prototype.last_frame().layout_count > 0);
        REQUIRE(prototype.last_frame().layout_count < kCorpusRowCount);
        REQUIRE(prototype.child_window_count() == 0);
        REQUIRE(elapsed < 10000.0);
        if (iteration == 0) {
            std::cout << "P2 performance warmup_ms=" << elapsed
                      << " start=" << kMeasurementStart << " end=" << kMeasurementEnd
                      << " corpus_bytes=" << kCorpusUtf8Bytes
                      << " corpus_sha256=" << kCorpusSha256 << '\n';
        }
        else {
            measured_ms[iteration - 1] = elapsed;
        }
    }

    std::array<double, 5> sorted = measured_ms;
    std::sort(sorted.begin(), sorted.end());
    std::cout << "P2 performance raw_ms=[";
    for (std::size_t index = 0; index < measured_ms.size(); ++index) {
        if (index != 0) { std::cout << ','; }
        std::cout << measured_ms[index];
    }
    std::cout << "] median_ms=" << sorted[2]
              << " max_ms=" << *std::max_element(measured_ms.begin(), measured_ms.end())
              << " start=" << kMeasurementStart << " end=" << kMeasurementEnd << '\n';
}

TEST_CASE("P2 HWND consumes WM_DPICHANGED at 96 and 144 DPI in DIP coordinates",
          "[P2][dpi][lower-floor][T4A-UNC-008]")
{
    harness::P2CardListPrototype prototype(640, 480);
    REQUIRE(prototype.ready());
    prototype.SetRows(small_rows());

    const RECT dpi96_rect{0, 0, 640, 480};
    REQUIRE(prototype.ApplyDpiForTest(96, dpi96_rect));
    REQUIRE(prototype.dpi() == 96);
    REQUIRE(prototype.client_width_pixels() == 640);
    REQUIRE(prototype.client_height_pixels() == 480);
    REQUIRE(std::abs(prototype.client_width_dips() - 640.0f) < 0.5f);
    REQUIRE(std::abs(prototype.client_height_dips() - 480.0f) < 0.5f);
    REQUIRE(prototype.Render());

    const RECT dpi144_rect{0, 0, 720, 480};
    REQUIRE(prototype.ApplyDpiForTest(144, dpi144_rect));
    REQUIRE(prototype.dpi() == 144);
    REQUIRE(prototype.client_width_pixels() == 720);
    REQUIRE(prototype.client_height_pixels() == 480);
    REQUIRE(std::abs(prototype.client_width_dips() - 480.0f) < 0.5f);
    REQUIRE(std::abs(prototype.client_height_dips() - 320.0f) < 0.5f);
    REQUIRE(prototype.Render());
    REQUIRE(prototype.last_frame().presented);
    REQUIRE(prototype.child_window_count() == 0);
}
