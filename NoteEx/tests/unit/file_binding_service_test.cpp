#include <catch_amalgamated.hpp>

#include "pynote/core/application/file_binding_service.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"
#include "pynote/platform/win32_file_binding_support.h"

#include <sqlite3/sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace application = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;

	using Bytes = std::vector<std::uint8_t>;

	// 좁은 리터럴은 이 기계에서 CP949 로 컴파일되므로 한국어 본문은 u8 로 쓴다.
	std::string u8s(const char8_t* _pszText)
	{
		return(std::string(reinterpret_cast<const char*>(_pszText)));
	}

	Bytes to_bytes(std::string_view _sText)
	{
		return(Bytes(_sText.begin(), _sText.end()));
	}

	std::string hex_encode(std::span<const std::uint8_t> _Bytes)
	{
		constexpr char DIGITS[] = "0123456789abcdef";
		std::string sResult;
		sResult.reserve(_Bytes.size() * 2);
		for (const std::uint8_t ch : _Bytes)
		{
			sResult.push_back(DIGITS[ch >> 4]);
			sResult.push_back(DIGITS[ch & 0x0F]);
		}
		return(sResult);
	}

	std::string hex_encode(std::string_view _sText)
	{
		return(hex_encode(std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(_sText.data()), _sText.size())));
	}

	application::StrictLegacyDecoder ansi_decoder()
	{
		return([](std::span<const std::uint8_t> _Bytes, std::string* _psOut)
		{
			return(pynote::platform::DecodeSystemAnsiStrict(_Bytes, _psOut));
		});
	}

	application::LegacyEncoder ansi_encoder()
	{
		return([](std::string_view _sText, Bytes* _pOut)
		{
			return(pynote::platform::EncodeSystemAnsiStrict(_sText, _pOut));
		});
	}

	// 골든 D/R 벡터가 쓰는 본문이다. ANSI 갈래는 CP949 로 표현할 수 있어야 하므로 비BMP 를 뺀다.
	std::string body_unicode() { return(u8s(u8"첫 줄 한글 \U0001F600\n두 번째 줄 ASCII\n세 번째")); }
	std::string body_ansi()    { return(u8s(u8"첫 줄 한글\n두 번째 줄 ASCII\n세 번째")); }
	std::string body_json()    { return(u8s(u8"{\n  \"이름\": \"값\",\n  \"목록\": [1, 2, 3]\n}")); }

	struct S_ENCODING_CASE
	{
		const char* pszName;
		bool        bBom;
	};

	// 원본 GOLDEN_ENCODINGS(tests/unit/test_file_binding_service.py:31~37) 와 같은 순서다.
	const S_ENCODING_CASE ENCODING_CASES[] = {
		{ "utf-8",     false },
		{ "utf-8",     true  },
		{ "utf-16-le", true  },
		{ "utf-16-be", true  },
		{ "mbcs",      false },
	};

	const domain::E_NEWLINE_KIND NEWLINE_CASES[] = {
		domain::E_NEWLINE_KIND::Lf,
		domain::E_NEWLINE_KIND::Crlf,
	};

	const bool TRAILING_CASES[] = { true, false };

	domain::S_FILE_BINDING binding_for(const std::string& _sEncoding, bool _bBom,
		domain::E_NEWLINE_KIND _eNewline, bool _bTrailing)
	{
		domain::S_FILE_BINDING Binding;
		Binding.sCardId          = "card-1";
		Binding.sPath            = "C:\\notes\\sample.txt";
		Binding.sPathKey         = "c:\\notes\\sample.txt";
		Binding.sEncoding        = _sEncoding;
		Binding.bBom             = _bBom;
		Binding.eNewline         = _eNewline;
		Binding.bTrailingNewline = _bTrailing;
		Binding.nBoundAtUs       = 1000;
		return(Binding);
	}

	// 원본 source_bytes(:63~75) 이식이다 - 결속 형식 그대로의 파일 바이트를 만든다.
	// 원본이 본문의 "\n" 을 결속 줄끝으로 바꾸고 끝 개행을 붙인 뒤 인코딩하므로, 끝 개행을
	// 먼저 "\n" 으로 붙여 RenderBytes 에 넘기면 같은 바이트가 나온다.
	Bytes source_bytes(const std::string& _sBody, const std::string& _sEncoding, bool _bBom,
		domain::E_NEWLINE_KIND _eNewline, bool _bTrailing)
	{
		const std::string sText = _bTrailing ? _sBody + "\n" : _sBody;
		Bytes             Out;
		REQUIRE(application::RenderBytes(
			sText, binding_for(_sEncoding, _bBom, _eNewline, _bTrailing), ansi_encoder(), &Out));
		return(Out);
	}

	// 시험용 임시 데이터베이스 경로. 소멸 시 본체와 WAL/SHM 사이드카까지 지운다.
	class C_TEMP_DB
	{
	public:
		explicit C_TEMP_DB(const std::string& _sName)
		{
			m_Path = std::filesystem::temp_directory_path() / ("noteex_test_" + _sName + ".db");
			this->remove_all_();
		}

		~C_TEMP_DB() { this->remove_all_(); }

		C_TEMP_DB(const C_TEMP_DB&) = delete;
		C_TEMP_DB& operator=(const C_TEMP_DB&) = delete;

		std::string Utf8() const { return(m_Path.string()); }

	private:
		void remove_all_()
		{
			std::error_code ec;
			std::filesystem::remove(m_Path, ec);
			std::filesystem::remove(m_Path.string() + "-wal", ec);
			std::filesystem::remove(m_Path.string() + "-shm", ec);
		}

		std::filesystem::path m_Path;
	};

	// 되쓰기 시나리오 한 벌. 임시 디렉터리 + 임시 DB + 결속 카드 한 장이며 원본
	// tests/integration/test_file_sync.py 의 _bind 픽스처 자리다.
	class C_SYNC_SCENARIO
	{
	public:
		explicit C_SYNC_SCENARIO(const std::string& _sName)
			: m_TempDb(_sName)
			, m_Repositories(m_Database)
		{
			m_Directory = std::filesystem::temp_directory_path() / ("noteex_fb_" + _sName);
			std::error_code ec;
			std::filesystem::remove_all(m_Directory, ec);
			std::filesystem::create_directories(m_Directory, ec);

			REQUIRE(m_Database.Open(m_TempDb.Utf8()));
			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, m_TempDb.Utf8());
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);
		}

		~C_SYNC_SCENARIO()
		{
			// 읽기 전용으로 바꾼 대상이 남아 있으면 지워지지 않으므로 속성을 먼저 되돌린다.
			std::error_code ec;
			for (const auto& Entry : std::filesystem::directory_iterator(m_Directory, ec))
			{
				::SetFileAttributesW(Entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
			}
			std::filesystem::remove_all(m_Directory, ec);
		}

		C_SYNC_SCENARIO(const C_SYNC_SCENARIO&) = delete;
		C_SYNC_SCENARIO& operator=(const C_SYNC_SCENARIO&) = delete;

		storage::C_REPOSITORIES&                     Repo() { return(m_Repositories); }
		pynote::platform::C_WIN32_BINDING_FILE_SYSTEM& Files() { return(m_Files); }
		std::string Path() const { return(m_Path); }

		// 원본 _bind 다 - 파일을 만들고 열기 직후와 같은 결속 상태를 세운다.
		void Bind(std::span<const std::uint8_t> _Data, const std::string& _sName = "note.txt", bool _bSynced = true)
		{
			m_Path = (m_Directory / _sName).string();
			this->Write(_Data);

			application::S_DETECTED_TEXT Detected;
			REQUIRE(application::DetectText(_Data, ansi_decoder(), &Detected));

			this->create_card_(Detected.sText);

			std::string sResolved;
			std::string sKey;
			REQUIRE(pynote::platform::ResolveBindingPath(m_Path, &sResolved, &sKey));

			std::int64_t nSize = 0;
			std::int64_t nMtimeNs = 0;
			REQUIRE(m_Files.Stat(m_Path, &nSize, &nMtimeNs));

			m_Binding.sCardId          = m_Card.sId;
			m_Binding.sPath            = sResolved;
			m_Binding.sPathKey         = sKey;
			m_Binding.sEncoding        = Detected.sEncoding;
			m_Binding.bBom             = Detected.bBom;
			m_Binding.eNewline         = Detected.eNewline;
			m_Binding.bTrailingNewline = Detected.bTrailingNewline;
			m_Binding.nBoundAtUs       = 1000;
			m_Binding.nSyncedSize      = _bSynced ? std::optional<std::int64_t>(nSize) : std::nullopt;
			m_Binding.nSyncedMtimeNs   = _bSynced ? std::optional<std::int64_t>(nMtimeNs) : std::nullopt;
			m_Binding.sSyncedHash      = _bSynced
				? std::optional<std::string>(application::HashBytes(_Data)) : std::nullopt;
			m_Binding.nSyncedAtUs      = _bSynced ? std::optional<std::int64_t>(1000) : std::nullopt;
			REQUIRE(m_Repositories.UpsertFileBinding(m_Binding) == storage::E_REPO_RESULT::Ok);

			m_Path = sResolved;
		}

		const domain::S_CARD&         Card() const { return(m_Card); }
		const domain::S_FILE_BINDING& Binding() const { return(m_Binding); }

		domain::S_CARD EditedCard(const std::string& _sBody) const
		{
			domain::S_CARD Edited = m_Card;
			Edited.sBody = _sBody;
			return(Edited);
		}

		void Write(std::span<const std::uint8_t> _Data) const
		{
			std::ofstream Stream(std::filesystem::path(m_Path), std::ios::binary | std::ios::trunc);
			REQUIRE(Stream.is_open());
			if (!_Data.empty())
			{
				Stream.write(reinterpret_cast<const char*>(_Data.data()), static_cast<std::streamsize>(_Data.size()));
			}
			Stream.close();
			REQUIRE_FALSE(Stream.fail());
		}

		bool Exists() const
		{
			std::error_code ec;
			return(std::filesystem::exists(std::filesystem::path(m_Path), ec));
		}

		Bytes Read() const
		{
			std::ifstream Stream(std::filesystem::path(m_Path), std::ios::binary);
			REQUIRE(Stream.is_open());
			return(Bytes(std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()));
		}

		void RemoveFile() const
		{
			std::error_code ec;
			std::filesystem::remove(std::filesystem::path(m_Path), ec);
		}

		void SetReadOnly(bool _bReadOnly) const
		{
			const std::filesystem::path Target(m_Path);
			::SetFileAttributesW(Target.c_str(), _bReadOnly ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL);
		}

		int TemporaryFileCount() const
		{
			int             nCount = 0;
			std::error_code ec;
			for (const auto& Entry : std::filesystem::directory_iterator(m_Directory, ec))
			{
				const std::string sName = Entry.path().filename().string();
				if (sName.size() >= 4 && sName.compare(sName.size() - 4, 4, ".tmp") == 0) { ++nCount; }
			}
			return(nCount);
		}

		std::optional<domain::S_FILE_BINDING> StoredBinding()
		{
			domain::S_FILE_BINDING Loaded;
			const storage::E_REPO_RESULT eResult = m_Repositories.GetFileBinding(m_Card.sId, &Loaded);
			if (eResult == storage::E_REPO_RESULT::NotFound) { return(std::nullopt); }
			REQUIRE(eResult == storage::E_REPO_RESULT::Ok);
			return(Loaded);
		}

	private:
		void create_card_(const std::string& _sBody)
		{
			domain::S_DOCUMENT Document;
			Document.sId          = "document-1";
			Document.sTitle       = "T";
			Document.nCreatedAtUs = 1;
			Document.nUpdatedAtUs = 1;
			REQUIRE(m_Repositories.CreateDocument(Document) == storage::E_REPO_RESULT::Ok);

			domain::S_NEW_CAPTURE_OPERATION Operation;
			Operation.sId          = "operation-1";
			Operation.sDocumentId  = "document-1";
			Operation.eSource      = domain::E_CAPTURE_OPERATION_SOURCE::Import;
			Operation.eSplitPolicy = domain::E_SPLIT_POLICY::Keep;
			Operation.nCreatedAtUs = 10;

			domain::S_NEW_CARD New;
			New.sId             = "card-1";
			New.sRevisionId     = "revision-1";
			New.sEventId        = "event-1";
			New.nPositionKey    = 1024;
			New.sBody           = _sBody;
			New.eCardSource     = domain::E_CARD_SOURCE::Import;
			New.eEventSource    = domain::E_EVENT_SOURCE::Import;
			New.eRevisionSource = domain::E_REVISION_SOURCE::Edit;
			New.nCreatedAtUs    = 10;

			std::vector<domain::S_CARD> Created;
			REQUIRE(m_Repositories.CreateCards(Operation, { New }, &Created) == storage::E_REPO_RESULT::Ok);
			REQUIRE(Created.size() == 1);
			m_Card = Created.front();
		}

		C_TEMP_DB                                     m_TempDb;
		storage::C_DATABASE                           m_Database;
		storage::C_REPOSITORIES                       m_Repositories;
		pynote::platform::C_WIN32_BINDING_FILE_SYSTEM m_Files;
		std::filesystem::path                         m_Directory;
		std::string                                   m_Path;
		domain::S_CARD                                m_Card;
		domain::S_FILE_BINDING                        m_Binding;
	};

	// 원자 교체만 실패시키는 덧옷이다. 원본 시험이 os.replace 를 가로채는 자리와 같다.
	class C_REPLACE_FAILING_FILES : public application::I_BINDING_FILE_SYSTEM
	{
	public:
		explicit C_REPLACE_FAILING_FILES(application::I_BINDING_FILE_SYSTEM& _Inner) : m_Inner(_Inner) {}

		bool ReadAllBytes(const std::string& _sPath, Bytes* _pBytes, bool* _pFound) const override
		{
			return(m_Inner.ReadAllBytes(_sPath, _pBytes, _pFound));
		}
		bool Stat(const std::string& _sPath, std::int64_t* _pnSize, std::int64_t* _pnMtimeNs) const override
		{
			return(m_Inner.Stat(_sPath, _pnSize, _pnMtimeNs));
		}
		bool CreateUniqueTemporaryPathFor(const std::string& _sTargetPath, std::string* _psPath) override
		{
			return(m_Inner.CreateUniqueTemporaryPathFor(_sTargetPath, _psPath));
		}
		bool WriteAllBytes(const std::string& _sPath, std::span<const std::uint8_t> _Bytes) override
		{
			return(m_Inner.WriteAllBytes(_sPath, _Bytes));
		}
		bool Replace(const std::string&, const std::string&) override
		{
			m_sLastError = u8s(u8"원자적 교체 실패(Win32 오류 13): 교체 거부 주입");
			return(false);
		}
		bool Remove(const std::string& _sPath) override { return(m_Inner.Remove(_sPath)); }
		const std::string& LastError() const override { return(m_sLastError); }

	private:
		application::I_BINDING_FILE_SYSTEM& m_Inner;
		std::string                         m_sLastError;
	};

	// 읽기 또는 stat 만 실패시키는 덧옷이다. 원본 시험이 OSError 를 심는 자리와 같다 -
	// 읽기 실패는 Failed 로 접혀야 하고(부재로 접으면 권한 오류 대상을 덮어쓴다), stat 실패는
	// 지문을 남기지 않되 되쓰기 자체는 성공이어야 한다(P1 감사 lens3-1·lens3-2).
	class C_FAULT_INJECTING_FILES : public application::I_BINDING_FILE_SYSTEM
	{
	public:
		C_FAULT_INJECTING_FILES(application::I_BINDING_FILE_SYSTEM& _Inner, bool _bFailRead, bool _bFailStat)
			: m_Inner(_Inner), m_bFailRead(_bFailRead), m_bFailStat(_bFailStat) {}

		bool ReadAllBytes(const std::string& _sPath, Bytes* _pBytes, bool* _pFound) const override
		{
			if (m_bFailRead)
			{
				m_sLastError = u8s(u8"읽기 실패(Win32 오류 5): 읽기 거부 주입");
				return(false);
			}
			return(m_Inner.ReadAllBytes(_sPath, _pBytes, _pFound));
		}
		bool Stat(const std::string& _sPath, std::int64_t* _pnSize, std::int64_t* _pnMtimeNs) const override
		{
			if (m_bFailStat) { return(false); }
			return(m_Inner.Stat(_sPath, _pnSize, _pnMtimeNs));
		}
		bool CreateUniqueTemporaryPathFor(const std::string& _sTargetPath, std::string* _psPath) override
		{
			return(m_Inner.CreateUniqueTemporaryPathFor(_sTargetPath, _psPath));
		}
		bool WriteAllBytes(const std::string& _sPath, std::span<const std::uint8_t> _Bytes) override
		{
			return(m_Inner.WriteAllBytes(_sPath, _Bytes));
		}
		bool Replace(const std::string& _sFrom, const std::string& _sTo) override
		{
			return(m_Inner.Replace(_sFrom, _sTo));
		}
		bool Remove(const std::string& _sPath) override { return(m_Inner.Remove(_sPath)); }
		const std::string& LastError() const override { return(m_sLastError); }

	private:
		application::I_BINDING_FILE_SYSTEM& m_Inner;
		bool                                m_bFailRead;
		bool                                m_bFailStat;
		mutable std::string                 m_sLastError;
	};

	application::S_FILE_SYNC_RESULT run_sync(
		C_SYNC_SCENARIO&                    _Scenario,
		application::I_BINDING_FILE_SYSTEM& _Files,
		const domain::S_CARD&               _Card,
		bool                                _bForce,
		bool                                _bInteractive,
		std::int64_t                        _nClock = 2000)
	{
		application::S_FILE_SYNC_RESULT Result;
		REQUIRE(application::SyncFile(
			_Scenario.Repo(), _Card, application::S_FILE_SYNC_OPTIONS{ _bForce, _bInteractive },
			[_nClock]() { return(_nClock); }, ansi_encoder(), _Files, &Result));
		return(Result);
	}

	std::string optional_int_text(const std::optional<std::int64_t>& _Value)
	{
		return(_Value.has_value() ? std::to_string(*_Value) : std::string{});
	}

	std::string optional_text(const std::optional<std::string>& _Value)
	{
		return(_Value.has_value() ? *_Value : std::string{});
	}

	// ------------------------------------------------------------------------------------------
	// 골든 직렬화. 파이썬 쪽 capture_file_binding_golden.py 와 줄 형식이 바이트까지 같아야 한다.
	// ------------------------------------------------------------------------------------------
	std::string detect_line(const std::string& _sId, std::span<const std::uint8_t> _Data)
	{
		application::S_DETECTED_TEXT Detected;
		const bool bOk = application::DetectText(_Data, ansi_decoder(), &Detected);

		std::ostringstream Stream;
		Stream << _sId << "|input=" << hex_encode(_Data) << "|ok=" << (bOk ? 1 : 0) << "|text=";
		if (bOk)
		{
			Stream << hex_encode(Detected.sText)
				<< "|encoding=" << Detected.sEncoding
				<< "|bom=" << (Detected.bBom ? 1 : 0)
				<< "|newline=" << domain::ToText(Detected.eNewline)
				<< "|trailing=" << (Detected.bTrailingNewline ? 1 : 0);
			return(Stream.str());
		}
		Stream << "|encoding=|bom=|newline=|trailing=";
		return(Stream.str());
	}

	std::string render_line(const std::string& _sId, const std::string& _sText,
		const domain::S_FILE_BINDING& _Binding)
	{
		Bytes      Rendered;
		const bool bOk = application::RenderBytes(_sText, _Binding, ansi_encoder(), &Rendered);

		std::ostringstream Stream;
		Stream << _sId << "|ok=" << (bOk ? 1 : 0) << "|bytes=" << (bOk ? hex_encode(Rendered) : std::string{});
		return(Stream.str());
	}

	std::string hash_line(const std::string& _sId, std::span<const std::uint8_t> _Data)
	{
		std::ostringstream Stream;
		Stream << _sId << "|input=" << hex_encode(_Data) << "|hash=" << application::HashBytes(_Data);
		return(Stream.str());
	}

	std::string binding_line(const std::string& _sId, const domain::S_FILE_BINDING& _Binding)
	{
		std::ostringstream Stream;
		Stream << _sId
			<< "|card_id=" << _Binding.sCardId
			<< "|path=" << hex_encode(_Binding.sPath)
			<< "|path_key=" << hex_encode(_Binding.sPathKey)
			<< "|encoding=" << _Binding.sEncoding
			<< "|bom=" << (_Binding.bBom ? 1 : 0)
			<< "|newline=" << domain::ToText(_Binding.eNewline)
			<< "|trailing=" << (_Binding.bTrailingNewline ? 1 : 0)
			<< "|synced_size=" << optional_int_text(_Binding.nSyncedSize)
			<< "|synced_mtime_ns=" << optional_int_text(_Binding.nSyncedMtimeNs)
			<< "|synced_hash=" << optional_text(_Binding.sSyncedHash)
			<< "|bound_at_us=" << _Binding.nBoundAtUs
			<< "|synced_at_us=" << optional_int_text(_Binding.nSyncedAtUs);
		return(Stream.str());
	}

	std::string sync_line(const std::string& _sId, C_SYNC_SCENARIO& _Scenario,
		const application::S_FILE_SYNC_RESULT& _Result)
	{
		const std::optional<domain::S_FILE_BINDING> Stored = _Scenario.StoredBinding();

		std::ostringstream Stream;
		Stream << _sId << "|" << application::ToText(_Result.eOutcome) << "|file=";
		Stream << (_Scenario.Exists() ? hex_encode(_Scenario.Read()) : std::string("absent"));
		Stream << "|synced_hash=" << (Stored.has_value() ? optional_text(Stored->sSyncedHash) : std::string{})
			<< "|synced_size=" << (Stored.has_value() ? optional_int_text(Stored->nSyncedSize) : std::string{})
			<< "|synced_at_us=" << (Stored.has_value() ? optional_int_text(Stored->nSyncedAtUs) : std::string{})
			<< "|temp_left=" << (_Scenario.TemporaryFileCount() > 0 ? 1 : 0);
		return(Stream.str());
	}

	// 저장소 루트. 시험 실행 파일은 <root>/NoteEx/x64/ReleaseMD/ 에 놓인다.
	std::filesystem::path source_root()
	{
		wchar_t     szPath[MAX_PATH * 4] = {};
		const DWORD nLength = ::GetModuleFileNameW(nullptr, szPath, static_cast<DWORD>(std::size(szPath)));
		REQUIRE(nLength > 0);
		REQUIRE(nLength < std::size(szPath));

		const std::filesystem::path Root =
			std::filesystem::path(szPath).parent_path().parent_path().parent_path().parent_path();
		std::string sResolved;
		std::string sKey;
		REQUIRE(pynote::platform::ResolveBindingPath(Root.string(), &sResolved, &sKey));
		REQUIRE(std::filesystem::exists(std::filesystem::path(sResolved) / "NoteEx" / "tools" / "gates" / "README.md"));
		return(std::filesystem::path(sResolved));
	}

	std::string tokenise(const std::string& _sValue, const std::string& _sPrefix)
	{
		REQUIRE(_sValue.rfind(_sPrefix, 0) == 0);
		return("<ROOT>" + _sValue.substr(_sPrefix.size()));
	}

	std::string path_line(const std::string& _sId, const std::string& _sRoot, const std::string& _sRootKey,
		const std::string& _sRelative)
	{
		std::string sResolved;
		std::string sKey;
		REQUIRE(pynote::platform::ResolveBindingPath(_sRoot + "\\" + _sRelative, &sResolved, &sKey));

		std::ostringstream Stream;
		Stream << _sId
			<< "|path=" << hex_encode(tokenise(sResolved, _sRoot))
			<< "|path_key=" << hex_encode(tokenise(sKey, _sRootKey));
		return(Stream.str());
	}

	void write_golden(const std::vector<std::string>& _Lines)
	{
		wchar_t     szOutput[32768] = {};
		const DWORD nLength = ::GetEnvironmentVariableW(
			L"PYNOTE_FILE_BINDING_GOLDEN_OUT", szOutput, static_cast<DWORD>(std::size(szOutput)));
		if (nLength == 0) { return; }
		REQUIRE(nLength < std::size(szOutput));

		// 한 시험이 전량을 한 번에 쓴다 - 줄 순서가 시험 실행 순서에 의존하지 않아야 한다.
		std::ofstream Output(std::filesystem::path(szOutput), std::ios::binary | std::ios::out | std::ios::trunc);
		REQUIRE(Output.is_open());
		for (const std::string& sLine : _Lines)
		{
			Output << sLine << '\n';
		}
		REQUIRE(Output.good());
	}
}

TEST_CASE("결속 감지와 렌더가 파일 바이트를 왕복 보존한다", "[W2-file-binding][FS-port][WTL-CAP-FB-001]")
{
	for (const S_ENCODING_CASE& Encoding : ENCODING_CASES)
	{
		const std::string sBody = std::string(Encoding.pszName) == "mbcs" ? body_ansi() : body_unicode();
		for (const domain::E_NEWLINE_KIND eNewline : NEWLINE_CASES)
		{
			for (const bool bTrailing : TRAILING_CASES)
			{
				const Bytes Data = source_bytes(sBody, Encoding.pszName, Encoding.bBom, eNewline, bTrailing);

				application::S_DETECTED_TEXT Detected;
				REQUIRE(application::DetectText(Data, ansi_decoder(), &Detected));
				REQUIRE(Detected.sEncoding == Encoding.pszName);
				REQUIRE(Detected.bBom == Encoding.bBom);
				REQUIRE(Detected.eNewline == eNewline);
				REQUIRE(Detected.bTrailingNewline == bTrailing);
				REQUIRE(Detected.sText == (bTrailing ? sBody + "\n" : sBody));

				Bytes Restored;
				REQUIRE(application::RenderBytes(
					Detected.sText,
					binding_for(Detected.sEncoding, Detected.bBom, Detected.eNewline, Detected.bTrailingNewline),
					ansi_encoder(), &Restored));
				REQUIRE(Restored == Data);
			}
		}
	}
}

TEST_CASE("결속 감지는 BOM 이 정한 인코딩 하나만 시도한다", "[W2-file-binding][FS-port]")
{
	application::S_DETECTED_TEXT Little;
	application::S_DETECTED_TEXT Big;
	const Bytes LittleData = source_bytes(u8s(u8"가\n"), "utf-16-le", true, domain::E_NEWLINE_KIND::Lf, false);
	const Bytes BigData    = source_bytes(u8s(u8"가\n"), "utf-16-be", true, domain::E_NEWLINE_KIND::Lf, false);
	REQUIRE(application::DetectText(LittleData, ansi_decoder(), &Little));
	REQUIRE(application::DetectText(BigData, ansi_decoder(), &Big));
	REQUIRE(Little.sEncoding == "utf-16-le");
	REQUIRE(Big.sEncoding == "utf-16-be");
	REQUIRE(Little.sText == Big.sText);
	REQUIRE(Little.sText == u8s(u8"가\n"));

	// BOM 뒤가 깨졌으면 뒤 단계로 내려가지 않고 결속 불가다.
	Bytes Broken = { 0xEF, 0xBB, 0xBF, 0xFF };
	application::S_DETECTED_TEXT Ignored;
	REQUIRE_FALSE(application::DetectText(Broken, ansi_decoder(), &Ignored));

	// 홀수 꼬리와 짝 없는 대리 문자도 실패다.
	Bytes Odd = { 0xFF, 0xFE, 0x41 };
	REQUIRE_FALSE(application::DetectText(Odd, ansi_decoder(), &Ignored));
	Bytes Lone = { 0xFF, 0xFE, 0x00, 0xD8 };
	REQUIRE_FALSE(application::DetectText(Lone, ansi_decoder(), &Ignored));
}

TEST_CASE("결속 불가 집합은 감지에서 전건 거부된다", "[W2-file-binding][FS-port][WTL-CAP-FB-015]")
{
	application::S_DETECTED_TEXT Ignored;
	const Bytes Png = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D };
	REQUIRE_FALSE(application::DetectText(Png, ansi_decoder(), &Ignored));

	const char8_t* HAZARDS[] = { u8"앞\u00A0뒤", u8"앞\u2028뒤", u8"앞\u2029뒤", u8"앞\uFDD0뒤", u8"앞\uFDD1뒤" };
	for (const char8_t* pszHazard : HAZARDS)
	{
		const Bytes Data = to_bytes(u8s(pszHazard));
		REQUIRE(application::HasRoundtripHazard(u8s(pszHazard)));
		REQUIRE_FALSE(application::DetectText(Data, ansi_decoder(), &Ignored));
	}

	const char8_t* CONTROLS[] = { u8"앞\u0001뒤", u8"앞\u001F뒤", u8"앞\u007F뒤" };
	for (const char8_t* pszControl : CONTROLS)
	{
		const Bytes Data = to_bytes(u8s(pszControl));
		REQUIRE(application::HasControlChars(u8s(pszControl)));
		REQUIRE_FALSE(application::DetectText(Data, ansi_decoder(), &Ignored));
	}

	// 단독 0xFF 는 UTF-8 도 ANSI 도 아니라 결속 불가다.
	const Bytes LoneFf = { 0xFF };
	REQUIRE_FALSE(application::DetectText(LoneFf, ansi_decoder(), &Ignored));

	// 단독 0x80 은 mbcs strict 가 U+0080 으로 디코딩하므로 **결속 가능**이다.
	const Bytes Lone80 = { 0x80 };
	application::S_DETECTED_TEXT Detected;
	REQUIRE(application::DetectText(Lone80, ansi_decoder(), &Detected));
	REQUIRE(Detected.sEncoding == "mbcs");

	// 공백류 제어 문자는 허용이다.
	REQUIRE_FALSE(application::HasControlChars(u8s(u8"앞\t\f\v뒤")));
}

TEST_CASE("결속 줄끝 판정은 가장 먼저 나온 줄끝을 채택한다", "[W2-file-binding][FS-port]")
{
	REQUIRE(application::DetectNewline("a\nb") == domain::E_NEWLINE_KIND::Lf);
	REQUIRE(application::DetectNewline("a\r\nb") == domain::E_NEWLINE_KIND::Crlf);
	REQUIRE(application::DetectNewline("a\rb") == domain::E_NEWLINE_KIND::Cr);
	REQUIRE(application::DetectNewline("a\nb\r\nc") == domain::E_NEWLINE_KIND::Lf);
	REQUIRE(application::DetectNewline("a\r\nb\nc") == domain::E_NEWLINE_KIND::Crlf);
	REQUIRE(application::DetectNewline("a\rb\nc") == domain::E_NEWLINE_KIND::Cr);
	// 줄끝이 없으면 Windows 기본값이다.
	REQUIRE(application::DetectNewline("") == domain::E_NEWLINE_KIND::Crlf);

	// 본문은 전부 LF 로 정규화된다.
	application::S_DETECTED_TEXT Detected;
	const Bytes Mixed = to_bytes(u8s(u8"첫\r\n둘\r셋\n"));
	REQUIRE(application::DetectText(Mixed, ansi_decoder(), &Detected));
	REQUIRE(Detected.sText == u8s(u8"첫\n둘\n셋\n"));
	REQUIRE(Detected.eNewline == domain::E_NEWLINE_KIND::Crlf);
	REQUIRE(Detected.bTrailingNewline);

	// 빈 파일은 빈 본문이고 끝 개행이 없다.
	application::S_DETECTED_TEXT Empty;
	REQUIRE(application::DetectText(Bytes{}, ansi_decoder(), &Empty));
	REQUIRE(Empty.sText.empty());
	REQUIRE(Empty.sEncoding == "utf-8");
	REQUIRE_FALSE(Empty.bBom);
	REQUIRE(Empty.eNewline == domain::E_NEWLINE_KIND::Crlf);
	REQUIRE_FALSE(Empty.bTrailingNewline);
}

TEST_CASE("결속 렌더는 표현할 수 없는 문자를 치환하지 않고 실패한다", "[W2-file-binding][FS-port]")
{
	Bytes Out;
	// 비BMP 는 CP949 로 표현할 수 없다.
	REQUIRE_FALSE(application::RenderBytes(u8s(u8"이모지 \U0001F642"),
		binding_for("mbcs", false, domain::E_NEWLINE_KIND::Lf, false), ansi_encoder(), &Out));
	// U+00C0 은 최적 대응 치환 대상이라 WC_NO_BEST_FIT_CHARS 가 빠지면 'A' 로 조용히 성공한다.
	REQUIRE_FALSE(application::RenderBytes(u8s(u8"\u00C0"),
		binding_for("mbcs", false, domain::E_NEWLINE_KIND::Lf, false), ansi_encoder(), &Out));
	// BOM 을 기록할 수 없는 인코딩에 bom=1 이면 실패다.
	REQUIRE_FALSE(application::RenderBytes(u8s(u8"본문"),
		binding_for("mbcs", true, domain::E_NEWLINE_KIND::Lf, false), ansi_encoder(), &Out));
	// 모르는 인코딩 이름도 실패다.
	REQUIRE_FALSE(application::RenderBytes("body",
		binding_for("no-such-encoding", false, domain::E_NEWLINE_KIND::Lf, false), ansi_encoder(), &Out));

	// 결속 줄끝은 모든 줄에 적용된다.
	REQUIRE(application::RenderBytes("a\nb\n",
		binding_for("utf-8", false, domain::E_NEWLINE_KIND::Crlf, false), ansi_encoder(), &Out));
	REQUIRE(Out == to_bytes("a\r\nb\r\n"));
}

TEST_CASE("결속 되쓰기는 편집한 본문을 원래 형식으로 기록한다", "[W2-file-binding][FS-port][WTL-CAP-FB-003]")
{
	C_SYNC_SCENARIO Scenario("sync_written");
	Scenario.Bind(to_bytes(u8s(u8"본문\n")));

	const application::S_FILE_SYNC_RESULT Result =
		run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, false);

	REQUIRE(Result.eOutcome == application::E_FILE_SYNC_OUTCOME::Written);
	REQUIRE(Result.sError.empty());
	REQUIRE(Scenario.Read() == to_bytes(u8s(u8"편집한 본문\n")));

	const std::optional<domain::S_FILE_BINDING> Stored = Scenario.StoredBinding();
	REQUIRE(Stored.has_value());
	REQUIRE(Stored->sSyncedHash == application::HashBytes(to_bytes(u8s(u8"편집한 본문\n"))));
	REQUIRE(Stored->nSyncedSize == static_cast<std::int64_t>(u8s(u8"편집한 본문\n").size()));
	REQUIRE(Stored->nSyncedAtUs == 2000);
	REQUIRE(Scenario.TemporaryFileCount() == 0);
}

TEST_CASE("결속 되쓰기는 무편집·부재·미동기 상태를 원본대로 가른다", "[W2-file-binding][FS-port]")
{
	{
		C_SYNC_SCENARIO Scenario("sync_noop");
		const Bytes     Data = to_bytes(u8s(u8"본문\n"));
		Scenario.Bind(Data);
		const application::S_FILE_SYNC_RESULT Result =
			run_sync(Scenario, Scenario.Files(), Scenario.Card(), false, false);
		REQUIRE(Result.eOutcome == application::E_FILE_SYNC_OUTCOME::Noop);
		REQUIRE(Scenario.Read() == Data);
	}
	{
		C_SYNC_SCENARIO Scenario("sync_missing");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		Scenario.RemoveFile();
		const application::S_FILE_SYNC_RESULT Result =
			run_sync(Scenario, Scenario.Files(), Scenario.Card(), false, false);
		REQUIRE(Result.eOutcome == application::E_FILE_SYNC_OUTCOME::Written);
		REQUIRE(Scenario.Read() == to_bytes(u8s(u8"본문\n")));
	}
	{
		C_SYNC_SCENARIO Scenario("sync_never_synced");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")), "note.txt", false);
		REQUIRE_FALSE(Scenario.Binding().sSyncedHash.has_value());
		const application::S_FILE_SYNC_RESULT Result =
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"다른 본문\n")), false, false);
		REQUIRE(Result.eOutcome == application::E_FILE_SYNC_OUTCOME::Written);
		REQUIRE(Scenario.Read() == to_bytes(u8s(u8"다른 본문\n")));
	}
	{
		C_SYNC_SCENARIO Scenario("sync_unbound");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		REQUIRE(Scenario.Repo().DeleteFileBinding(Scenario.Card().sId) == storage::E_REPO_RESULT::Ok);
		const application::S_FILE_SYNC_RESULT Result =
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, false);
		REQUIRE(Result.eOutcome == application::E_FILE_SYNC_OUTCOME::Noop);
		REQUIRE(Scenario.Read() == to_bytes(u8s(u8"본문\n")));
	}
	{
		// 원본 _read_bytes 는 FileNotFoundError 만 부재로 삼키고 나머지 OSError 는 FAILED 다
		// (file_binding_service.py:198-201, :267-271). 권한 오류를 부재로 접으면 덮어쓴다.
		C_SYNC_SCENARIO Scenario("sync_read_failure");
		const Bytes     Data = to_bytes(u8s(u8"본문\n"));
		Scenario.Bind(Data);
		C_FAULT_INJECTING_FILES Files(Scenario.Files(), true, false);
		const application::S_FILE_SYNC_RESULT Result =
			run_sync(Scenario, Files, Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, false);
		REQUIRE(Result.eOutcome == application::E_FILE_SYNC_OUTCOME::Failed);
		REQUIRE(Result.sError.find("5") != std::string::npos);
		REQUIRE(Scenario.Read() == Data);
	}
	{
		// 원본 _record_sync 의 stat 실패는 지문을 남기지 않고 저장 흐름을 관통하지 않는다
		// (file_binding_service.py:308-312) - 되쓰기는 성공이고 결속의 동기 지문만 그대로다.
		C_SYNC_SCENARIO Scenario("sync_stat_failure");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		const domain::S_FILE_BINDING Before = Scenario.Binding();
		C_FAULT_INJECTING_FILES Files(Scenario.Files(), false, true);
		const application::S_FILE_SYNC_RESULT Result =
			run_sync(Scenario, Files, Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, false);
		REQUIRE(Result.eOutcome == application::E_FILE_SYNC_OUTCOME::Written);
		REQUIRE(Scenario.Read() == to_bytes(u8s(u8"편집한 본문\n")));
		const std::optional<domain::S_FILE_BINDING> Stored = Scenario.StoredBinding();
		REQUIRE(Stored.has_value());
		REQUIRE(Stored->sSyncedHash == Before.sSyncedHash);
		REQUIRE(Stored->nSyncedAtUs == Before.nSyncedAtUs);
		REQUIRE(Stored->nSyncedSize == Before.nSyncedSize);
	}
}

TEST_CASE("결속 되쓰기는 외부 변경을 묻지 않고 덮어쓰지 않는다", "[W2-file-binding][FS-port][WTL-CAP-FB-007]")
{
	C_SYNC_SCENARIO Scenario("sync_external");
	Scenario.Bind(to_bytes(u8s(u8"본문\n")));
	Scenario.Write(to_bytes(u8s(u8"바깥에서 바꾼 본문\n")));
	const domain::S_CARD Edited = Scenario.EditedCard(u8s(u8"편집한 본문\n"));

	const application::S_FILE_SYNC_RESULT Quiet =
		run_sync(Scenario, Scenario.Files(), Edited, false, false);
	REQUIRE(Quiet.eOutcome == application::E_FILE_SYNC_OUTCOME::ExternalChange);
	REQUIRE(Scenario.Read() == to_bytes(u8s(u8"바깥에서 바꾼 본문\n")));

	// 대화형도 사용자가 답하기 전에는 같은 결과이고, 지문이 그대로라 다음 저장이 다시 묻는다.
	const application::S_FILE_SYNC_RESULT Interactive =
		run_sync(Scenario, Scenario.Files(), Edited, false, true, 3000);
	REQUIRE(Interactive.eOutcome == application::E_FILE_SYNC_OUTCOME::ExternalChange);
	const std::optional<domain::S_FILE_BINDING> Stored = Scenario.StoredBinding();
	REQUIRE(Stored.has_value());
	REQUIRE(Stored->sSyncedHash == Scenario.Binding().sSyncedHash);
	REQUIRE(Stored->nSyncedAtUs == Scenario.Binding().nSyncedAtUs);

	// force 는 덮어쓴다.
	const application::S_FILE_SYNC_RESULT Forced =
		run_sync(Scenario, Scenario.Files(), Edited, true, false);
	REQUIRE(Forced.eOutcome == application::E_FILE_SYNC_OUTCOME::Written);
	REQUIRE(Scenario.Read() == to_bytes(u8s(u8"편집한 본문\n")));
}

TEST_CASE("외부 변경이 카드 본문과 같으면 되쓰지 않고 지문만 갱신한다", "[W2-file-binding][FS-port]")
{
	C_SYNC_SCENARIO Scenario("sync_external_match");
	Scenario.Bind(to_bytes(u8s(u8"본문\n")));
	Scenario.Write(to_bytes(u8s(u8"바깥에서 바꾼 본문\n")));

	const application::S_FILE_SYNC_RESULT Result = run_sync(
		Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"바깥에서 바꾼 본문\n")), false, false);

	REQUIRE(Result.eOutcome == application::E_FILE_SYNC_OUTCOME::Noop);
	const std::optional<domain::S_FILE_BINDING> Stored = Scenario.StoredBinding();
	REQUIRE(Stored.has_value());
	REQUIRE(Stored->sSyncedHash == application::HashBytes(to_bytes(u8s(u8"바깥에서 바꾼 본문\n"))));
}

TEST_CASE("결속 되쓰기 실패는 Win32 오류 코드를 싣고 임시 파일을 남기지 않는다", "[W2-file-binding][FS-port][WTL-CAP-FB-005]")
{
	{
		C_SYNC_SCENARIO Scenario("sync_readonly");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		Scenario.SetReadOnly(true);
		const application::S_FILE_SYNC_RESULT Result =
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, false);
		Scenario.SetReadOnly(false);

		REQUIRE(Result.eOutcome == application::E_FILE_SYNC_OUTCOME::Failed);
		REQUIRE(Result.sError.find("5") != std::string::npos);
		REQUIRE(Scenario.Read() == to_bytes(u8s(u8"본문\n")));
		REQUIRE(Scenario.TemporaryFileCount() == 0);
		const std::optional<domain::S_FILE_BINDING> Stored = Scenario.StoredBinding();
		REQUIRE(Stored.has_value());
		REQUIRE(Stored->sSyncedHash == Scenario.Binding().sSyncedHash);
	}
	{
		C_SYNC_SCENARIO         Scenario("sync_replace_fail");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		C_REPLACE_FAILING_FILES Failing(Scenario.Files());
		const application::S_FILE_SYNC_RESULT Result =
			run_sync(Scenario, Failing, Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, false);

		REQUIRE(Result.eOutcome == application::E_FILE_SYNC_OUTCOME::Failed);
		REQUIRE(Scenario.Read() == to_bytes(u8s(u8"본문\n")));
		REQUIRE(Scenario.TemporaryFileCount() == 0);
	}
	{
		// 표현할 수 없는 문자는 치환되지 않고 실패로 끝나며 파일을 건드리지 않는다.
		C_SYNC_SCENARIO Scenario("sync_unrepresentable");
		const Bytes     Ansi = source_bytes(u8s(u8"본문"), "mbcs", false, domain::E_NEWLINE_KIND::Lf, true);
		Scenario.Bind(Ansi, "ansi.txt");
		REQUIRE(Scenario.Binding().sEncoding == "mbcs");

		const application::S_FILE_SYNC_RESULT Result =
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"이모지 \U0001F642\n")), false, false);

		REQUIRE(Result.eOutcome == application::E_FILE_SYNC_OUTCOME::Failed);
		REQUIRE_FALSE(Result.sError.empty());
		REQUIRE(Scenario.Read() == Ansi);
		REQUIRE(Scenario.TemporaryFileCount() == 0);
	}
}

TEST_CASE("경로 점유는 활성 카드를 알리고 휴지통 카드의 행은 지운다", "[W2-file-binding][FS-port][WTL-CAP-FB-021]")
{
	C_SYNC_SCENARIO Scenario("prepare_path");
	Scenario.Bind(to_bytes(u8s(u8"본문\n")));
	const std::string sKey = Scenario.Binding().sPathKey;

	application::S_BINDING_PATH_RESOLUTION Held;
	REQUIRE(application::PrepareBindingPath(Scenario.Repo(), sKey, &Held));
	REQUIRE(Held.eStatus == application::E_BINDING_PATH_STATUS::HeldByActiveCard);
	REQUIRE(Held.sHolderCardId == Scenario.Card().sId);

	// 결속이 없는 경로는 그대로 비어 있다.
	application::S_BINDING_PATH_RESOLUTION Free;
	REQUIRE(application::PrepareBindingPath(Scenario.Repo(), "c:\\notes\\nothing.txt", &Free));
	REQUIRE(Free.eStatus == application::E_BINDING_PATH_STATUS::Free);
	REQUIRE_FALSE(Free.sHolderCardId.has_value());

	// 카드를 휴지통에 넣으면 점유가 풀리고 그 행이 사라진다.
	REQUIRE(Scenario.Card().sCurrentRevisionId.has_value());
	REQUIRE(Scenario.Repo().UpdateCardDeletedState(
		Scenario.Card().sId, Scenario.Card().nPositionKey, 12345, *Scenario.Card().sCurrentRevisionId)
		== storage::E_REPO_RESULT::Ok);

	application::S_BINDING_PATH_RESOLUTION Released;
	REQUIRE(application::PrepareBindingPath(Scenario.Repo(), sKey, &Released));
	REQUIRE(Released.eStatus == application::E_BINDING_PATH_STATUS::Free);
	REQUIRE_FALSE(Released.sHolderCardId.has_value());
	domain::S_FILE_BINDING Gone;
	REQUIRE(Scenario.Repo().FindBindingByPath(sKey, &Gone) == storage::E_REPO_RESULT::NotFound);
}

TEST_CASE("결속 해시는 파일 바이트의 sha256 이고 부재는 값 없음이다", "[W2-file-binding][FS-port]")
{
	C_SYNC_SCENARIO Scenario("hash_file");
	const Bytes     Data = to_bytes(u8s(u8"본문\n"));
	Scenario.Bind(Data);

	std::optional<std::string> Hash;
	REQUIRE(application::ReadFileHash(Scenario.Files(), Scenario.Path(), &Hash));
	REQUIRE(Hash.has_value());
	REQUIRE(*Hash == application::HashBytes(Data));

	std::optional<std::string> Missing;
	REQUIRE(application::ReadFileHash(Scenario.Files(), Scenario.Path() + ".none", &Missing));
	REQUIRE_FALSE(Missing.has_value());
}

TEST_CASE("결속 골든 벡터를 파이썬 캡처와 같은 줄로 낸다", "[W2-file-binding][FS-port]")
{
	std::vector<std::string> Lines;

	auto numbered = [](const char* _pszPrefix, int _nIndex)
	{
		return(std::string(_pszPrefix)
			+ std::string(_nIndex < 10 ? "00" : (_nIndex < 100 ? "0" : ""))
			+ std::to_string(_nIndex));
	};
	auto detect_id = [&numbered](int _nIndex) { return(numbered("FB-D", _nIndex)); };
	auto render_id = [&numbered](int _nIndex) { return(numbered("FB-R", _nIndex)); };

	// D 벡터 - 감지. 인코딩 5 x 줄끝 2 x 끝 개행 2 = 20.
	int nDetect = 1;
	for (const S_ENCODING_CASE& Encoding : ENCODING_CASES)
	{
		const std::string sBody = std::string(Encoding.pszName) == "mbcs" ? body_ansi() : body_unicode();
		for (const domain::E_NEWLINE_KIND eNewline : NEWLINE_CASES)
		{
			for (const bool bTrailing : TRAILING_CASES)
			{
				const Bytes Data = source_bytes(sBody, Encoding.pszName, Encoding.bBom, eNewline, bTrailing);
				Lines.push_back(detect_line(detect_id(nDetect++), Data));
			}
		}
	}

	Lines.push_back(detect_line(detect_id(nDetect++),
		source_bytes(body_json(), "utf-8", false, domain::E_NEWLINE_KIND::Crlf, true)));
	Lines.push_back(detect_line(detect_id(nDetect++), Bytes{}));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes("  \t  ")));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes("a\nb\r\nc")));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes("a\r\nb\nc")));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes("a\rb\nc")));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes("a\rb")));
	Lines.push_back(detect_line(detect_id(nDetect++), Bytes{ 0x80 }));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes(u8s(u8"앞\t\f\v뒤"))));

	// 거부 집합 12.
	Lines.push_back(detect_line(detect_id(nDetect++),
		Bytes{ 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D }));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes(u8s(u8"앞\u00A0뒤"))));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes(u8s(u8"앞\u2028뒤"))));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes(u8s(u8"앞\u2029뒤"))));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes(u8s(u8"앞\uFDD0뒤"))));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes(u8s(u8"앞\uFDD1뒤"))));
	Lines.push_back(detect_line(detect_id(nDetect++), Bytes{ 0xFF }));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes(u8s(u8"앞\u0001뒤"))));
	Lines.push_back(detect_line(detect_id(nDetect++), to_bytes(u8s(u8"앞\u007F뒤"))));
	Lines.push_back(detect_line(detect_id(nDetect++), Bytes{ 0xEF, 0xBB, 0xBF, 0xFF }));
	Lines.push_back(detect_line(detect_id(nDetect++), Bytes{ 0xFF, 0xFE, 0x41 }));
	Lines.push_back(detect_line(detect_id(nDetect++), Bytes{ 0xFF, 0xFE, 0x00, 0xD8 }));

	// R 벡터 - 렌더. 감지와 같은 20 조합의 왕복 + 실패 4.
	int nRender = 1;
	for (const S_ENCODING_CASE& Encoding : ENCODING_CASES)
	{
		const std::string sBody = std::string(Encoding.pszName) == "mbcs" ? body_ansi() : body_unicode();
		for (const domain::E_NEWLINE_KIND eNewline : NEWLINE_CASES)
		{
			for (const bool bTrailing : TRAILING_CASES)
			{
				const std::string sText = bTrailing ? sBody + "\n" : sBody;
				Lines.push_back(render_line(render_id(nRender++), sText,
					binding_for(Encoding.pszName, Encoding.bBom, eNewline, bTrailing)));
			}
		}
	}
	Lines.push_back(render_line(render_id(nRender++), u8s(u8"이모지 \U0001F642"),
		binding_for("mbcs", false, domain::E_NEWLINE_KIND::Lf, false)));
	Lines.push_back(render_line(render_id(nRender++), u8s(u8"\u00C0"),
		binding_for("mbcs", false, domain::E_NEWLINE_KIND::Lf, false)));
	Lines.push_back(render_line(render_id(nRender++), u8s(u8"본문"),
		binding_for("mbcs", true, domain::E_NEWLINE_KIND::Lf, false)));
	Lines.push_back(render_line(render_id(nRender++), "body",
		binding_for("no-such-encoding", false, domain::E_NEWLINE_KIND::Lf, false)));

	// H 벡터 - 해시 3.
	Lines.push_back(hash_line("FB-H001", Bytes{}));
	Lines.push_back(hash_line("FB-H002", to_bytes(u8s(u8"본문\n"))));
	Lines.push_back(hash_line("FB-H003", Bytes{ 0x00, 0x01, 0xFF, 0xFE, 0x80 }));

	// B 벡터 - 저장소 왕복 2.
	{
		C_SYNC_SCENARIO Scenario("golden_repo");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));

		domain::S_FILE_BINDING Distinct;
		Distinct.sCardId          = Scenario.Card().sId;
		Distinct.sPath            = "C:/Notes/Mixed/A.TXT";
		Distinct.sPathKey         = "c:\\notes\\mixed\\a.txt";
		Distinct.sEncoding        = "utf-16-be";
		Distinct.bBom             = true;
		Distinct.eNewline         = domain::E_NEWLINE_KIND::Crlf;
		Distinct.bTrailingNewline = false;
		Distinct.nBoundAtUs       = 1000000;
		Distinct.nSyncedSize      = 4242;
		Distinct.nSyncedMtimeNs   = 1700000000123456789LL;
		Distinct.sSyncedHash      = std::string("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
		Distinct.nSyncedAtUs      = 2000000;
		REQUIRE(Scenario.Repo().UpsertFileBinding(Distinct) == storage::E_REPO_RESULT::Ok);

		domain::S_FILE_BINDING ByCard;
		REQUIRE(Scenario.Repo().GetFileBinding(Distinct.sCardId, &ByCard) == storage::E_REPO_RESULT::Ok);
		Lines.push_back(binding_line("FB-B001", ByCard));

		domain::S_FILE_BINDING ByPath;
		REQUIRE(Scenario.Repo().FindBindingByPath(Distinct.sPathKey, &ByPath) == storage::E_REPO_RESULT::Ok);
		Lines.push_back(binding_line("FB-B002", ByPath));
	}

	// S 벡터 - 동기 13. 시계는 고정이고 mtime 은 싣지 않는다.
	{
		C_SYNC_SCENARIO Scenario("golden_s001");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		Lines.push_back(sync_line("FB-S001", Scenario,
			run_sync(Scenario, Scenario.Files(), Scenario.Card(), false, false)));
	}
	{
		C_SYNC_SCENARIO Scenario("golden_s002");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		Lines.push_back(sync_line("FB-S002", Scenario,
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, false)));
	}
	{
		C_SYNC_SCENARIO Scenario("golden_s003");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		Scenario.RemoveFile();
		Lines.push_back(sync_line("FB-S003", Scenario,
			run_sync(Scenario, Scenario.Files(), Scenario.Card(), false, false)));
	}
	{
		C_SYNC_SCENARIO Scenario("golden_s004");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")), "note.txt", false);
		Lines.push_back(sync_line("FB-S004", Scenario,
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"다른 본문\n")), false, false)));
	}
	{
		C_SYNC_SCENARIO Scenario("golden_s005");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		Scenario.Write(to_bytes(u8s(u8"바깥에서 바꾼 본문\n")));
		Lines.push_back(sync_line("FB-S005", Scenario,
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, false)));
	}
	{
		C_SYNC_SCENARIO Scenario("golden_s006");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		Scenario.Write(to_bytes(u8s(u8"바깥에서 바꾼 본문\n")));
		Lines.push_back(sync_line("FB-S006", Scenario,
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, true)));
	}
	{
		C_SYNC_SCENARIO Scenario("golden_s007");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		Scenario.Write(to_bytes(u8s(u8"바깥에서 바꾼 본문\n")));
		const domain::S_CARD Edited = Scenario.EditedCard(u8s(u8"편집한 본문\n"));
		run_sync(Scenario, Scenario.Files(), Edited, false, true);
		Lines.push_back(sync_line("FB-S007", Scenario,
			run_sync(Scenario, Scenario.Files(), Edited, false, true, 3000)));
	}
	{
		C_SYNC_SCENARIO Scenario("golden_s008");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		Scenario.Write(to_bytes(u8s(u8"바깥에서 바꾼 본문\n")));
		Lines.push_back(sync_line("FB-S008", Scenario,
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"편집한 본문\n")), true, false)));
	}
	{
		C_SYNC_SCENARIO Scenario("golden_s009");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		Scenario.Write(to_bytes(u8s(u8"바깥에서 바꾼 본문\n")));
		Lines.push_back(sync_line("FB-S009", Scenario,
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"바깥에서 바꾼 본문\n")), false, false)));
	}
	{
		C_SYNC_SCENARIO Scenario("golden_s010");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		REQUIRE(Scenario.Repo().DeleteFileBinding(Scenario.Card().sId) == storage::E_REPO_RESULT::Ok);
		Lines.push_back(sync_line("FB-S010", Scenario,
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, false)));
	}
	{
		C_SYNC_SCENARIO         Scenario("golden_s011");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		C_REPLACE_FAILING_FILES Failing(Scenario.Files());
		Lines.push_back(sync_line("FB-S011", Scenario,
			run_sync(Scenario, Failing, Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, false)));
	}
	{
		C_SYNC_SCENARIO Scenario("golden_s012");
		Scenario.Bind(to_bytes(u8s(u8"본문\n")));
		Scenario.SetReadOnly(true);
		const application::S_FILE_SYNC_RESULT Result =
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"편집한 본문\n")), false, false);
		Scenario.SetReadOnly(false);
		Lines.push_back(sync_line("FB-S012", Scenario, Result));
	}
	{
		C_SYNC_SCENARIO Scenario("golden_s013");
		Scenario.Bind(source_bytes(u8s(u8"본문"), "mbcs", false, domain::E_NEWLINE_KIND::Lf, true), "ansi.txt");
		Lines.push_back(sync_line("FB-S013", Scenario,
			run_sync(Scenario, Scenario.Files(), Scenario.EditedCard(u8s(u8"이모지 \U0001F642\n")), false, false)));
	}

	// P 벡터 - 경로 4.
	{
		const std::filesystem::path Root = source_root();
		std::string                 sRoot;
		std::string                 sRootKey;
		REQUIRE(pynote::platform::ResolveBindingPath(Root.string(), &sRoot, &sRootKey));

		Lines.push_back(path_line("FB-P001", sRoot, sRootKey, "NoteEx/TOOLS/GATES/README.MD"));
		Lines.push_back(path_line("FB-P002", sRoot, sRootKey, "NoteEx/tools/gates/../gates/README.md"));
		Lines.push_back(path_line("FB-P003", sRoot, sRootKey, "src/pynote/domain/models.py"));
		Lines.push_back(path_line("FB-P004", sRoot, sRootKey, "NoteEx/tools/GATES/NoSuchFile.TXT"));
	}

	// D 41 + R 24 + H 3 + B 2 + S 13 + P 4.
	REQUIRE(Lines.size() == 87);
	write_golden(Lines);
}
