#include <catch_amalgamated.hpp>

#include "pynote/core/application/card_service.h"
#include "pynote/core/application/import_pipeline.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"
#include "pynote/platform/win32_import_support.h"

#include <sqlite3/sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace platform = pynote::platform;
	namespace storage = pynote::core::storage;

	std::string u8s(const char8_t* text) { return std::string(reinterpret_cast<const char*>(text)); }
	std::vector<std::uint8_t> bytes(std::string_view text) { return { text.begin(), text.end() }; }
	std::string hex(std::string_view text)
	{
		std::ostringstream stream;
		stream << std::hex << std::setfill('0');
		for (const unsigned char value : text) { stream << std::setw(2) << static_cast<unsigned int>(value); }
		return stream.str();
	}

	void emit(std::string_view id, std::string_view line)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_CARD_IMPORT_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream output(std::filesystem::path(path), std::ios::binary
			| (id == "WTL-W2-0001" ? std::ios::trunc : std::ios::app));
		REQUIRE(output.is_open()); output << id << '|' << line << '\n'; REQUIRE(output.good());
	}

	app::LegacyDecoder cp949_decoder()
	{
		return [](std::span<const std::uint8_t> input) { return platform::DecodeWindowsCodePage(input, 949); };
	}

	class TempFile
	{
	public:
		explicit TempFile(std::span<const std::uint8_t> contents)
			: path_(std::filesystem::temp_directory_path()
				/ ("noteex_w2r2_import_" + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(++sequence_) + ".txt"))
		{
			std::ofstream output(path_, std::ios::binary | std::ios::trunc);
			output.write(reinterpret_cast<const char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
		}
		~TempFile() { std::error_code error; std::filesystem::remove(path_, error); }
		std::string Utf8Path() const
		{
			const auto value = path_.u8string();
			return std::string(reinterpret_cast<const char*>(value.data()), value.size());
		}
	private:
		std::filesystem::path path_;
		inline static int sequence_ = 0;
	};

	struct ReadObservation
	{
		std::size_t requested{};
		std::size_t returned{};
	};

	app::BoundedFileReader reader(ReadObservation* observation = nullptr)
	{
		return [observation](const std::string& path, std::size_t maximum,
			std::vector<std::uint8_t>* output, std::string* error) {
			if (observation != nullptr) { observation->requested = maximum; }
			const bool result = platform::ReadFileBounded(path, maximum, output, error);
			if (observation != nullptr) { observation->returned = output->size(); }
			return result;
		};
	}

	class Fixture
	{
	public:
		Fixture()
			: path_(std::filesystem::temp_directory_path()
				/ ("noteex_w2r2_import_db_" + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(++sequence_) + ".db")),
			  repositories_(database_)
		{
			remove_();
			REQUIRE(database_.Open(path_.string()));
			storage::C_MIGRATION_RUNNER runner;
			runner.SetExistingDatabase(false, path_.string());
			REQUIRE(runner.Run(database_) == storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT document;
			document.sId = "document-import";
			document.sTitle = u8s(u8"가져오기 테스트");
			document.nCreatedAtUs = 1000;
			document.nUpdatedAtUs = 1000;
			REQUIRE(repositories_.CreateDocument(document) == storage::E_REPO_RESULT::Ok);
		}
		~Fixture() { database_.Close(); remove_(); }

		app::C_CARD_SERVICE Service()
		{
			auto ids = std::make_shared<int>(0);
			return app::C_CARD_SERVICE(database_, repositories_, parser_, [] { return 2000; },
				[ids] { return "import-id-" + std::to_string((*ids)++); });
		}
		std::int64_t Count(const char* table)
		{
			const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
			sqlite3_stmt* statement = nullptr;
			REQUIRE(::sqlite3_prepare_v2(database_.Handle(), sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
			REQUIRE(::sqlite3_step(statement) == SQLITE_ROW);
			const auto result = ::sqlite3_column_int64(statement, 0);
			::sqlite3_finalize(statement);
			return result;
		}

		domain::C_PARAGRAPH_PARSER parser_;
		storage::C_DATABASE database_;
		storage::C_REPOSITORIES repositories_;
	private:
		void remove_()
		{
			std::error_code error;
			std::filesystem::remove(path_, error);
			std::filesystem::remove(path_.string() + "-wal", error);
			std::filesystem::remove(path_.string() + "-shm", error);
		}
		std::filesystem::path path_;
		inline static int sequence_ = 0;
	};

	void check_blank(const char* id, std::vector<std::uint8_t> input, const char* inputHex)
	{
		domain::C_PARAGRAPH_PARSER parser; app::C_IMPORT_PIPELINE pipeline(parser, cp949_decoder());
		app::S_IMPORT_PREPARATION prepared;
		REQUIRE(pipeline.PrepareFromBytes("memory", input, &prepared) == app::E_IMPORT_RESULT::Blank);
		emit(id, std::string("input=") + inputHex + "|result=blank");
	}
}

#define TAGS(ID) "[W2-R2][core][domain][import-pipeline][" ID "]"

TEST_CASE("WTL-W2-0001", TAGS("WTL-W2-0001"))
{
	Fixture fixture;
	app::C_IMPORT_PIPELINE pipeline(fixture.parser_, cp949_decoder());
	const std::string expected = u8s(u8"첫 카드\r\n둘째 줄");
	auto input = bytes(expected); input.insert(input.begin(), { 0xEF, 0xBB, 0xBF });
	TempFile file(input); app::S_IMPORT_PREPARATION prepared; std::string error;
	REQUIRE(pipeline.PrepareFile(file.Utf8Path(), reader(), &prepared, &error) == app::E_IMPORT_RESULT::Ok);
	auto service = fixture.Service(); std::vector<domain::S_CARD> cards;
	REQUIRE(service.CreateCards("document-import", prepared.sText, domain::E_CAPTURE_OPERATION_SOURCE::Import,
		false, std::nullopt, &cards) == app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE(cards.size() == 1); REQUIRE(cards[0].sBody == expected);
	emit("WTL-W2-0001", "body=" + hex(cards[0].sBody));
}

TEST_CASE("WTL-W2-0002", TAGS("WTL-W2-0002"))
{
	domain::C_PARAGRAPH_PARSER parser; app::C_IMPORT_PIPELINE pipeline(parser, cp949_decoder());
	std::vector<std::uint8_t> input(app::MAX_IMPORT_FILE_BYTES, 'x'); TempFile file(input);
	ReadObservation observation; app::S_IMPORT_PREPARATION prepared; std::string error;
	REQUIRE(pipeline.PrepareFile(file.Utf8Path(), reader(&observation), &prepared, &error) == app::E_IMPORT_RESULT::Ok);
	REQUIRE(observation.requested == app::MAX_IMPORT_FILE_BYTES + 1); REQUIRE(prepared.sText.size() == app::MAX_IMPORT_FILE_BYTES);
	emit("WTL-W2-0002", "result=ok|bytes=4194304|read_limit=4194305");
}

TEST_CASE("WTL-W2-0003", TAGS("WTL-W2-0003"))
{
	domain::C_PARAGRAPH_PARSER parser; app::C_IMPORT_PIPELINE pipeline(parser, cp949_decoder());
	std::vector<std::uint8_t> input(app::MAX_IMPORT_FILE_BYTES + 1, 'x'); TempFile file(input);
	ReadObservation observation; app::S_IMPORT_PREPARATION prepared; std::string error;
	REQUIRE(pipeline.PrepareFile(file.Utf8Path(), reader(&observation), &prepared, &error) == app::E_IMPORT_RESULT::FileTooLarge);
	REQUIRE(observation.requested == app::MAX_IMPORT_FILE_BYTES + 1); REQUIRE(observation.returned == app::MAX_IMPORT_FILE_BYTES + 1);
	emit("WTL-W2-0003", "result=too-large|bytes=4194305|read_limit=4194305");
}

TEST_CASE("WTL-W2-0004", TAGS("WTL-W2-0004"))
{
	domain::C_PARAGRAPH_PARSER parser; app::C_IMPORT_PIPELINE pipeline(parser, cp949_decoder());
	std::vector<std::uint8_t> input(app::MAX_IMPORT_FILE_BYTES + 1, 'x'); app::S_IMPORT_PREPARATION prepared;
	REQUIRE(pipeline.PrepareFromBytes("memory", input, &prepared) == app::E_IMPORT_RESULT::Ok);
	REQUIRE(prepared.sText.size() == app::MAX_IMPORT_FILE_BYTES + 1);
	emit("WTL-W2-0004", "result=ok|bytes=4194305");
}

TEST_CASE("WTL-W2-0005", TAGS("WTL-W2-0005"))
{
	const std::string expected = u8s(u8"A가");
	const std::vector<std::uint8_t> utf8Bom = { 0xEF,0xBB,0xBF,0x41,0xEA,0xB0,0x80 };
	const std::vector<std::uint8_t> utf16Le = { 0xFF,0xFE,0x41,0x00,0x00,0xAC };
	const std::vector<std::uint8_t> utf16Be = { 0xFE,0xFF,0x00,0x41,0xAC,0x00 };
	const std::vector<std::uint8_t> utf8 = { 0x41,0xEA,0xB0,0x80 };
	const std::vector<std::uint8_t> ansi = { 0xB0,0xA1 };
	const auto decoder = cp949_decoder();
	REQUIRE(app::DecodeImportBytes(utf8Bom, decoder) == expected);
	REQUIRE(app::DecodeImportBytes(utf16Le, decoder) == expected);
	REQUIRE(app::DecodeImportBytes(utf16Be, decoder) == expected);
	REQUIRE(app::DecodeImportBytes(utf8, decoder) == expected);
	const std::string ansiExpected = u8s(u8"가");
	REQUIRE(app::DecodeImportBytes(ansi, decoder) == ansiExpected);
	bool legacyCalled = false;
	const std::vector<std::uint8_t> brokenBom = { 0xEF,0xBB,0xBF,0xF0,0x9F };
	const auto broken = app::DecodeImportBytes(brokenBom, [&](std::span<const std::uint8_t>) { legacyCalled = true; return std::string("legacy"); });
	REQUIRE_FALSE(legacyCalled); REQUIRE(hex(broken) == "efbfbdefbfbd");
	emit("WTL-W2-0005", "utf8bom=" + hex(expected) + "|utf16le=" + hex(expected)
		+ "|utf16be=" + hex(expected) + "|utf8=" + hex(expected) + "|ansi=" + hex(ansiExpected));
}

TEST_CASE("WTL-W2-0006", TAGS("WTL-W2-0006"))
{
	std::vector<std::uint8_t> input(256); for (std::size_t i = 0; i < input.size(); ++i) { input[i] = static_cast<std::uint8_t>(i); }
	const auto decoded = app::DecodeImportBytes(input, cp949_decoder());
	const std::string expectedHash = "047f43d7c2de5ef55cb70e393f20ce0009c4308f98c908a51dcfa753c1f9200f";
	REQUIRE(storage::TextHash(decoded) == expectedHash);
	emit("WTL-W2-0006", "sha256=" + storage::TextHash(decoded));
}

TEST_CASE("WTL-W2-0007", TAGS("WTL-W2-0007"))
{
	const std::vector<std::uint8_t> input = { 0x81 }; const auto decoded = app::DecodeImportBytes(input, cp949_decoder());
	REQUIRE(hex(decoded) == "efbfbd"); emit("WTL-W2-0007", "decoded=" + hex(decoded));
}

TEST_CASE("WTL-W2-0008", TAGS("WTL-W2-0008"))
{
	const std::vector<std::uint8_t> input = { 0xF0,0x9F }; const auto decoded = app::DecodeImportBytes(input, cp949_decoder());
	REQUIRE(hex(decoded) == "efbfbdefbfbd"); emit("WTL-W2-0008", "decoded=" + hex(decoded));
}

TEST_CASE("WTL-W2-0009", TAGS("WTL-W2-0009"))
{
	Fixture fixture; app::C_IMPORT_PIPELINE pipeline(fixture.parser_, cp949_decoder());
	const std::string original = u8s(u8"첫 문단\r\n\r\n둘째 문단"); app::S_IMPORT_PREPARATION prepared;
	REQUIRE(pipeline.PrepareFromBytes("memory", bytes(original), &prepared) == app::E_IMPORT_RESULT::Ok);
	auto service = fixture.Service(); std::vector<domain::S_CARD> cards;
	REQUIRE(service.CreateCards("document-import", prepared.sText, domain::E_CAPTURE_OPERATION_SOURCE::Import,
		true, std::nullopt, &cards) == app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE(cards.size() == 2); REQUIRE(cards[0].eSource == domain::E_CARD_SOURCE::Import);
	domain::S_CAPTURE_OPERATION operation;
	REQUIRE(fixture.repositories_.GetCaptureOperation(cards[0].sOperationId, &operation) == storage::E_REPO_RESULT::Ok);
	REQUIRE(operation.sOriginalText == original);
	emit("WTL-W2-0009", "bodies=" + hex(cards[0].sBody) + "," + hex(cards[1].sBody) + "|source=import|operations=1");
}

TEST_CASE("WTL-W2-0010", TAGS("WTL-W2-0010"))
{
	Fixture fixture; app::C_IMPORT_PIPELINE pipeline(fixture.parser_, cp949_decoder());
	const std::vector<std::uint8_t> input = { 0x81,'n','o','t','e','p','a','d' }; app::S_IMPORT_PREPARATION prepared;
	REQUIRE(pipeline.PrepareFromBytes("memory", input, &prepared) == app::E_IMPORT_RESULT::Ok);
	auto service = fixture.Service(); domain::S_CARD card;
	REQUIRE(service.CreateCard("document-import", prepared.sText, domain::E_CAPTURE_OPERATION_SOURCE::Import, std::nullopt, &card)
		== app::E_CARD_SERVICE_RESULT::Ok);
	REQUIRE(card.sBody == prepared.sText); emit("WTL-W2-0010", "body=" + hex(card.sBody));
}

TEST_CASE("WTL-W2-0011", TAGS("WTL-W2-0011"))
{
	Fixture fixture; const auto beforeCards = fixture.Count("cards"); const auto beforeEvents = fixture.Count("edit_events");
	auto service = fixture.Service(); std::vector<domain::S_CARD> cards;
	const std::string whitespace = u8s(u8" \t\r\n\u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000");
	REQUIRE(service.CreateCards("document-import", whitespace, domain::E_CAPTURE_OPERATION_SOURCE::Import, true, std::nullopt, &cards)
		== app::E_CARD_SERVICE_RESULT::Invalid);
	REQUIRE(fixture.Count("cards") == beforeCards); REQUIRE(fixture.Count("edit_events") == beforeEvents);
	emit("WTL-W2-0011", "result=invalid|cards=0|events=0");
}

TEST_CASE("WTL-W2-0012", TAGS("WTL-W2-0012")) { check_blank("WTL-W2-0012", {}, ""); }
TEST_CASE("WTL-W2-0013", TAGS("WTL-W2-0013")) { check_blank("WTL-W2-0013", { 0x20,0x09,0x0D,0x0A }, "20090d0a"); }
TEST_CASE("WTL-W2-0014", TAGS("WTL-W2-0014")) { check_blank("WTL-W2-0014", { 0xEF,0xBB,0xBF }, "efbbbf"); }
TEST_CASE("WTL-W2-0015", TAGS("WTL-W2-0015")) { check_blank("WTL-W2-0015", { 0xC2,0x85 }, "c285"); }
TEST_CASE("WTL-W2-0016", TAGS("WTL-W2-0016")) { check_blank("WTL-W2-0016", { 0xE2,0x80,0x87 }, "e28087"); }
TEST_CASE("WTL-W2-0017", TAGS("WTL-W2-0017")) { check_blank("WTL-W2-0017", { 0xE2,0x80,0xAF }, "e280af"); }
TEST_CASE("WTL-W2-0018", TAGS("WTL-W2-0018")) { check_blank("WTL-W2-0018", { 0xE3,0x80,0x80 }, "e38080"); }

#undef TAGS
