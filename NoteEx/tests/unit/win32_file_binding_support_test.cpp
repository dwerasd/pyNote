#include <catch_amalgamated.hpp>

#include "pynote/platform/win32_file_binding_support.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
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
	using Bytes = std::vector<std::uint8_t>;

	std::string u8s(const char8_t* _pszText)
	{
		return(std::string(reinterpret_cast<const char*>(_pszText)));
	}

	Bytes to_bytes(std::string_view _sText)
	{
		return(Bytes(_sText.begin(), _sText.end()));
	}

	// 시험용 임시 디렉터리. 소멸 시 통째로 지운다.
	class C_TEMP_DIR
	{
	public:
		explicit C_TEMP_DIR(const std::string& _sName)
		{
			m_Path = std::filesystem::temp_directory_path() / ("noteex_fbs_" + _sName);
			std::error_code ec;
			std::filesystem::remove_all(m_Path, ec);
			std::filesystem::create_directories(m_Path, ec);
		}

		~C_TEMP_DIR()
		{
			std::error_code ec;
			std::filesystem::remove_all(m_Path, ec);
		}

		C_TEMP_DIR(const C_TEMP_DIR&) = delete;
		C_TEMP_DIR& operator=(const C_TEMP_DIR&) = delete;

		std::string File(const std::string& _sName) const { return((m_Path / _sName).string()); }
		std::string Utf8() const { return(m_Path.string()); }

		int Count(const std::string& _sSuffix) const
		{
			int             nCount = 0;
			std::error_code ec;
			for (const auto& Entry : std::filesystem::directory_iterator(m_Path, ec))
			{
				const std::string sName = Entry.path().filename().string();
				if (sName.size() >= _sSuffix.size()
					&& sName.compare(sName.size() - _sSuffix.size(), _sSuffix.size(), _sSuffix) == 0)
				{
					++nCount;
				}
			}
			return(nCount);
		}

	private:
		std::filesystem::path m_Path;
	};

	void write_file(const std::string& _sPath, std::string_view _sData)
	{
		std::ofstream Stream(std::filesystem::path(_sPath), std::ios::binary | std::ios::trunc);
		REQUIRE(Stream.is_open());
		Stream.write(_sData.data(), static_cast<std::streamsize>(_sData.size()));
	}
}

TEST_CASE("ANSI strict 디코딩은 치환 문자를 만들지 않는다", "[W2-file-binding][FS-port]")
{
	std::string sText;
	REQUIRE(pynote::platform::DecodeSystemAnsiStrict(Bytes{}, &sText));
	REQUIRE(sText.empty());

	// 한국어 왕복. 인코더가 낸 바이트를 디코더가 원문으로 되돌린다.
	Bytes Encoded;
	REQUIRE(pynote::platform::EncodeSystemAnsiStrict(u8s(u8"한국어 본문"), &Encoded));
	REQUIRE_FALSE(Encoded.empty());
	REQUIRE(pynote::platform::DecodeSystemAnsiStrict(Encoded, &sText));
	REQUIRE(sText == u8s(u8"한국어 본문"));

	// 단독 0x80 은 U+0080 으로 디코딩된다 - 가져오기용 치환 디코더의 0x80 특례를 승계하지 않는다.
	REQUIRE(pynote::platform::DecodeSystemAnsiStrict(Bytes{ 0x80 }, &sText));
	REQUIRE(sText == u8s(u8"\u0080"));

	// 단독 0xFF 는 CP949 에서 성립하지 않는 바이트라 실패다.
	REQUIRE_FALSE(pynote::platform::DecodeSystemAnsiStrict(Bytes{ 0xFF }, &sText));
	// 잘린 이중바이트 선행 바이트도 실패다.
	REQUIRE_FALSE(pynote::platform::DecodeSystemAnsiStrict(Bytes{ Encoded.front() }, &sText));
}

TEST_CASE("ANSI strict 인코딩은 최적 대응 치환을 거부한다", "[W2-file-binding][FS-port]")
{
	Bytes Out;
	REQUIRE(pynote::platform::EncodeSystemAnsiStrict("", &Out));
	REQUIRE(Out.empty());

	REQUIRE(pynote::platform::EncodeSystemAnsiStrict("plain ascii", &Out));
	REQUIRE(Out == to_bytes("plain ascii"));

	// 비BMP 는 CP949 에 없다.
	REQUIRE_FALSE(pynote::platform::EncodeSystemAnsiStrict(u8s(u8"\U0001F642"), &Out));
	// U+00C0 은 WC_NO_BEST_FIT_CHARS 가 없으면 'A' 로 조용히 성공하는 자리다.
	REQUIRE_FALSE(pynote::platform::EncodeSystemAnsiStrict(u8s(u8"\u00C0"), &Out));
	// U+00A6 도 같은 계열(막대 기호로 접힌다)이다.
	REQUIRE_FALSE(pynote::platform::EncodeSystemAnsiStrict(u8s(u8"\u00A6"), &Out));
}

TEST_CASE("결속 경로 확정은 실제 표기로 접고 키를 소문자로 만든다", "[W2-file-binding][FS-port]")
{
	C_TEMP_DIR Temp("resolve");
	write_file(Temp.File("Note.TXT"), "body");

	std::string sPath;
	std::string sKey;
	REQUIRE(pynote::platform::ResolveBindingPath(Temp.File("Note.TXT"), &sPath, &sKey));
	REQUIRE(sPath.find("..") == std::string::npos);
	REQUIRE(sPath.size() > 3);
	REQUIRE(sPath[1] == ':');
	// 존재하는 파일은 디스크의 실제 표기로 돌아온다.
	REQUIRE(sPath.size() >= 8);
	REQUIRE(sPath.compare(sPath.size() - 8, 8, "Note.TXT") == 0);

	// 키는 소문자이고 구분자가 역슬래시다.
	REQUIRE(sKey.find('/') == std::string::npos);
	for (const char ch : sKey)
	{
		REQUIRE_FALSE((ch >= 'A' && ch <= 'Z'));
	}

	// 같은 파일을 다른 대소문자로 가리켜도 키가 같다.
	std::string sOtherPath;
	std::string sOtherKey;
	REQUIRE(pynote::platform::ResolveBindingPath(Temp.File("note.txt"), &sOtherPath, &sOtherKey));
	REQUIRE(sOtherKey == sKey);

	// '..' 세그먼트와 '/' 구분자를 함께 담은 입력도 같은 자리로 접힌다.
	std::string sDotted;
	std::string sDottedKey;
	REQUIRE(pynote::platform::ResolveBindingPath(Temp.Utf8() + "/sub/../Note.TXT", &sDotted, &sDottedKey));
	REQUIRE(sDottedKey == sKey);

	// 없는 파일 이름은 입력 표기가 보존된다.
	std::string sMissing;
	std::string sMissingKey;
	REQUIRE(pynote::platform::ResolveBindingPath(Temp.File("NoSuch.TXT"), &sMissing, &sMissingKey));
	REQUIRE(sMissing.compare(sMissing.size() - 10, 10, "NoSuch.TXT") == 0);
}

TEST_CASE("결속 파일 계층은 부재·상태·임시 이름·교체를 원본 계약대로 다룬다", "[W2-file-binding][FS-port]")
{
	C_TEMP_DIR                                    Temp("files");
	pynote::platform::C_WIN32_BINDING_FILE_SYSTEM Files;

	// 부재는 found=false 이면서 성공이다.
	Bytes Data;
	bool  bFound = true;
	REQUIRE(Files.ReadAllBytes(Temp.File("none.txt"), &Data, &bFound));
	REQUIRE_FALSE(bFound);
	REQUIRE(Data.empty());

	const std::string sTarget = Temp.File("note.txt");
	write_file(sTarget, "hello");
	REQUIRE(Files.ReadAllBytes(sTarget, &Data, &bFound));
	REQUIRE(bFound);
	REQUIRE(Data == to_bytes("hello"));

	std::int64_t nSize = 0;
	std::int64_t nMtimeNs = 0;
	REQUIRE(Files.Stat(sTarget, &nSize, &nMtimeNs));
	REQUIRE(nSize == 5);
	REQUIRE(nMtimeNs > 0);
	REQUIRE_FALSE(Files.Stat(Temp.File("none.txt"), &nSize, &nMtimeNs));
	REQUIRE(Files.LastError().find("Win32") != std::string::npos);

	// 임시 이름은 대상과 같은 디렉터리에 ".<대상이름>.<무작위>.tmp" 로 만든다.
	std::string sTemporary;
	REQUIRE(Files.CreateUniqueTemporaryPathFor(sTarget, &sTemporary));
	REQUIRE(std::filesystem::path(sTemporary).parent_path() == std::filesystem::path(sTarget).parent_path());
	const std::string sTempName = std::filesystem::path(sTemporary).filename().string();
	REQUIRE(sTempName.rfind(".note.txt.", 0) == 0);
	REQUIRE(sTempName.compare(sTempName.size() - 4, 4, ".tmp") == 0);
	REQUIRE(Temp.Count(".tmp") == 1);

	REQUIRE(Files.WriteAllBytes(sTemporary, to_bytes("replaced")));
	REQUIRE(Files.Replace(sTemporary, sTarget));
	REQUIRE(Temp.Count(".tmp") == 0);
	REQUIRE(Files.ReadAllBytes(sTarget, &Data, &bFound));
	REQUIRE(Data == to_bytes("replaced"));

	// 없는 대상 삭제는 성공이다.
	REQUIRE(Files.Remove(Temp.File("none.txt")));

	// 읽기 전용 대상 교체는 오류 5 를 싣고 실패한다.
	std::string sSecond;
	REQUIRE(Files.CreateUniqueTemporaryPathFor(sTarget, &sSecond));
	REQUIRE(Files.WriteAllBytes(sSecond, to_bytes("blocked")));
	::SetFileAttributesW(std::filesystem::path(sTarget).c_str(), FILE_ATTRIBUTE_READONLY);
	const bool bReplaced = Files.Replace(sSecond, sTarget);
	::SetFileAttributesW(std::filesystem::path(sTarget).c_str(), FILE_ATTRIBUTE_NORMAL);
	REQUIRE_FALSE(bReplaced);
	REQUIRE(Files.LastError().find("5") != std::string::npos);
	REQUIRE(Files.Remove(sSecond));
}
