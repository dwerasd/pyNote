#include <catch_amalgamated.hpp>

#include "pynote/core/domain/paragraph_parser.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
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
	using pynote::core::domain::C_PARAGRAPH_PARSER;
	using pynote::core::domain::I_PARAGRAPH_SPLIT_POLICY;

	std::string u8s(const char8_t* _pszText)
	{
		return std::string(reinterpret_cast<const char*>(_pszText));
	}

	std::string hex_encode(std::string_view _sText)
	{
		constexpr char kDigits[] = "0123456789abcdef";
		std::string sResult;
		sResult.reserve(_sText.size() * 2);
		for (const char raw : _sText) {
			const auto ch = static_cast<unsigned char>(raw);
			sResult.push_back(kDigits[ch >> 4]);
			sResult.push_back(kDigits[ch & 0x0F]);
		}
		return sResult;
	}

	std::string golden_line(
		std::string_view _sId,
		const C_PARAGRAPH_PARSER& _Parser,
		std::string_view _sInput,
		bool _bIncludeKeep = false)
	{
		const std::vector<std::string> Paragraphs = _Parser.Split(_sInput);
		std::ostringstream Stream;
		Stream << _sId << "|input=" << hex_encode(_sInput) << "|split=";
		for (std::size_t nIndex = 0; nIndex < Paragraphs.size(); ++nIndex) {
			if (nIndex != 0) {
				Stream << ',';
			}
			Stream << hex_encode(Paragraphs[nIndex]);
		}
		Stream << "|zero=" << (_Parser.IsZeroParagraphInput(_sInput) ? 1 : 0);
		if (_bIncludeKeep) {
			Stream << "|keep=" << hex_encode(_Parser.Keep(_sInput));
		}
		return Stream.str();
	}

	void emit_line(std::string_view _sId, const std::string& _sLine)
	{
		wchar_t szOutput[32768] = {};
		const DWORD nLength = ::GetEnvironmentVariableW(
			L"PYNOTE_PARAGRAPH_GOLDEN_OUT", szOutput, static_cast<DWORD>(std::size(szOutput)));
		if (nLength == 0) {
			return;
		}
		REQUIRE(nLength < std::size(szOutput));
		const std::ios::openmode Mode = std::ios::binary | std::ios::out
			| (_sId == "WTL-W2-0119" ? std::ios::trunc : std::ios::app);
		std::ofstream Output(std::filesystem::path(szOutput), Mode);
		REQUIRE(Output.is_open());
		Output << _sLine << '\n';
		REQUIRE(Output.good());
	}

	void verify_case(
		std::string_view _sId,
		std::string_view _sInput,
		const std::vector<std::string>& _Expected,
		bool _bIncludeKeep = false)
	{
		const C_PARAGRAPH_PARSER Parser;
		REQUIRE(Parser.Split(_sInput) == _Expected);
		REQUIRE(Parser.IsZeroParagraphInput(_sInput) == _Expected.empty());
		if (_bIncludeKeep) {
			REQUIRE(Parser.Keep(_sInput) == _sInput);
		}
		emit_line(_sId, golden_line(_sId, Parser, _sInput, _bIncludeKeep));
	}

	std::vector<std::string> values(std::initializer_list<std::string> _Values)
	{
		return std::vector<std::string>(_Values);
	}

	std::string all_python_whitespace()
	{
		std::string Result;
		for (unsigned char ch = 0x09; ch <= 0x0D; ++ch) {
			Result.push_back(static_cast<char>(ch));
		}
		for (unsigned char ch = 0x1C; ch <= 0x1F; ++ch) {
			Result.push_back(static_cast<char>(ch));
		}
		Result.push_back(' ');
		Result += u8s(u8"\u0085\u00A0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007"
			u8"\u2008\u2009\u200A\u2028\u2029\u202F\u205F\u3000");
		return Result;
	}

	std::string mixed_runtime_whitespace()
	{
		std::string Result(" \t\v\f", 4);
		for (unsigned char ch = 0x1C; ch <= 0x1F; ++ch) {
			Result.push_back(static_cast<char>(ch));
		}
		Result += u8s(u8"\u00A0\u2003");
		Result += "\r\n";
		return Result;
	}

	class C_PIPE_POLICY final : public I_PARAGRAPH_SPLIT_POLICY
	{
	public:
		std::vector<std::string> Split(std::string_view _sText) const override
		{
			std::vector<std::string> Result;
			std::size_t nStart = 0;
			while (nStart <= _sText.size()) {
				const std::size_t nEnd = _sText.find('|', nStart);
				const std::size_t nPartEnd = nEnd == std::string_view::npos ? _sText.size() : nEnd;
				if (nPartEnd != nStart) {
					Result.emplace_back(_sText.substr(nStart, nPartEnd - nStart));
				}
				if (nEnd == std::string_view::npos) {
					break;
				}
				nStart = nEnd + 1;
			}
			return Result;
		}
	};
}

#define W2_TAGS(ID) "[core][domain][paragraph-parser][W2-R1][" ID "]"

TEST_CASE("WTL-W2-0119", W2_TAGS("WTL-W2-0119")) { verify_case("WTL-W2-0119", "", {}); }
TEST_CASE("WTL-W2-0120", W2_TAGS("WTL-W2-0120")) { verify_case("WTL-W2-0120", " ", {}); }
TEST_CASE("WTL-W2-0121", W2_TAGS("WTL-W2-0121")) { verify_case("WTL-W2-0121", "\t", {}); }
TEST_CASE("WTL-W2-0122", W2_TAGS("WTL-W2-0122")) { verify_case("WTL-W2-0122", " \t \t", {}); }
TEST_CASE("WTL-W2-0123", W2_TAGS("WTL-W2-0123")) { verify_case("WTL-W2-0123", "\r", {}); }
TEST_CASE("WTL-W2-0124", W2_TAGS("WTL-W2-0124")) { verify_case("WTL-W2-0124", "\r\r", {}); }
TEST_CASE("WTL-W2-0125", W2_TAGS("WTL-W2-0125")) { verify_case("WTL-W2-0125", "\n", {}); }
TEST_CASE("WTL-W2-0126", W2_TAGS("WTL-W2-0126")) { verify_case("WTL-W2-0126", "\r\n", {}); }
TEST_CASE("WTL-W2-0127", W2_TAGS("WTL-W2-0127")) { verify_case("WTL-W2-0127", " \t\r\n\t \n", {}); }
TEST_CASE("WTL-W2-0128", W2_TAGS("WTL-W2-0128")) { verify_case("WTL-W2-0128", " \r\r\n\t", {}); }
TEST_CASE("WTL-W2-0129", W2_TAGS("WTL-W2-0129")) { verify_case("WTL-W2-0129", "\v", {}); }
TEST_CASE("WTL-W2-0130", W2_TAGS("WTL-W2-0130")) { verify_case("WTL-W2-0130", "\f", {}); }
TEST_CASE("WTL-W2-0131", W2_TAGS("WTL-W2-0131")) { verify_case("WTL-W2-0131", "\v\f", {}); }
TEST_CASE("WTL-W2-0132", W2_TAGS("WTL-W2-0132")) { verify_case("WTL-W2-0132", std::string("\x1c", 1), {}); }
TEST_CASE("WTL-W2-0133", W2_TAGS("WTL-W2-0133")) { verify_case("WTL-W2-0133", std::string("\x1d", 1), {}); }
TEST_CASE("WTL-W2-0134", W2_TAGS("WTL-W2-0134")) { verify_case("WTL-W2-0134", std::string("\x1e", 1), {}); }
TEST_CASE("WTL-W2-0135", W2_TAGS("WTL-W2-0135")) { verify_case("WTL-W2-0135", std::string("\x1f", 1), {}); }
TEST_CASE("WTL-W2-0136", W2_TAGS("WTL-W2-0136")) { verify_case("WTL-W2-0136", u8s(u8"\u00A0"), {}); }
TEST_CASE("WTL-W2-0137", W2_TAGS("WTL-W2-0137")) { verify_case("WTL-W2-0137", u8s(u8"\u2003"), {}); }
TEST_CASE("WTL-W2-0138", W2_TAGS("WTL-W2-0138")) { verify_case("WTL-W2-0138", mixed_runtime_whitespace(), {}); }
TEST_CASE("WTL-W2-0139", W2_TAGS("WTL-W2-0139")) { verify_case("WTL-W2-0139", all_python_whitespace(), {}); }

TEST_CASE("WTL-W2-0140", W2_TAGS("WTL-W2-0140"))
{
	const std::string Text = u8s(u8"첫 문단의 첫 줄\n첫 문단의 둘째 줄");
	verify_case("WTL-W2-0140", Text, values({ Text }));
}

TEST_CASE("WTL-W2-0141", W2_TAGS("WTL-W2-0141"))
{
	verify_case("WTL-W2-0141", u8s(u8"첫 문단\n\n둘째 문단\n\n\n\n셋째 문단"),
		values({ u8s(u8"첫 문단"), u8s(u8"둘째 문단"), u8s(u8"셋째 문단") }));
}

TEST_CASE("WTL-W2-0142", W2_TAGS("WTL-W2-0142")) { verify_case("WTL-W2-0142", u8s(u8"첫 줄\r\n둘째 줄\r\n\r\n다음 문단"), values({ u8s(u8"첫 줄\n둘째 줄"), u8s(u8"다음 문단") })); }
TEST_CASE("WTL-W2-0143", W2_TAGS("WTL-W2-0143")) { verify_case("WTL-W2-0143", u8s(u8"첫 줄\n둘째 줄\n\n다음 문단"), values({ u8s(u8"첫 줄\n둘째 줄"), u8s(u8"다음 문단") })); }
TEST_CASE("WTL-W2-0144", W2_TAGS("WTL-W2-0144")) { verify_case("WTL-W2-0144", u8s(u8"첫 줄\r\n둘째 줄\n\r\n다음 문단"), values({ u8s(u8"첫 줄\n둘째 줄"), u8s(u8"다음 문단") })); }
TEST_CASE("WTL-W2-0145", W2_TAGS("WTL-W2-0145")) { verify_case("WTL-W2-0145", u8s(u8"첫 문단\n \t \n둘째 문단\n\t\n셋째 문단"), values({ u8s(u8"첫 문단"), u8s(u8"둘째 문단"), u8s(u8"셋째 문단") })); }
TEST_CASE("WTL-W2-0146", W2_TAGS("WTL-W2-0146")) { verify_case("WTL-W2-0146", u8s(u8"첫 문단\n\v\f\n둘째 문단"), values({ u8s(u8"첫 문단"), u8s(u8"둘째 문단") })); }
TEST_CASE("WTL-W2-0147", W2_TAGS("WTL-W2-0147")) { verify_case("WTL-W2-0147", u8s(u8"첫 문단\n\u00A0\u2003\n둘째 문단"), values({ u8s(u8"첫 문단"), u8s(u8"둘째 문단") })); }
TEST_CASE("WTL-W2-0148", W2_TAGS("WTL-W2-0148")) { const std::string Text = u8s(u8"\v첫 줄\f"); verify_case("WTL-W2-0148", Text, values({ Text })); }
TEST_CASE("WTL-W2-0149", W2_TAGS("WTL-W2-0149"))
{
	const C_PARAGRAPH_PARSER Parser;
	const std::string First("\0", 1);
	const std::string Second = std::string("\0\n\n", 3) + u8s(u8"본문");
	REQUIRE(Parser.Split(First) == values({ First }));
	REQUIRE_FALSE(Parser.IsZeroParagraphInput(First));
	REQUIRE(Parser.Split(Second) == values({ First, u8s(u8"본문") }));
	REQUIRE_FALSE(Parser.IsZeroParagraphInput(Second));
	const std::string Line = "WTL-W2-0149|input0=" + hex_encode(First)
		+ "|split0=" + hex_encode(First) + "|zero0=0|input1=" + hex_encode(Second)
		+ "|split1=" + hex_encode(First) + "," + hex_encode(u8s(u8"본문")) + "|zero1=0";
	emit_line("WTL-W2-0149", Line);
}
TEST_CASE("WTL-W2-0150", W2_TAGS("WTL-W2-0150")) { verify_case("WTL-W2-0150", u8s(u8"첫\r줄\n \r \n둘째 문단\r"), values({ u8s(u8"첫\r줄"), u8s(u8"둘째 문단\r") })); }
TEST_CASE("WTL-W2-0151", W2_TAGS("WTL-W2-0151")) { verify_case("WTL-W2-0151", u8s(u8"한글 😀 e\u0301\n두 번째 줄\n\n👩🏽‍💻과 값"), values({ u8s(u8"한글 😀 e\u0301\n두 번째 줄"), u8s(u8"👩🏽‍💻과 값") })); }
TEST_CASE("WTL-W2-0152", W2_TAGS("WTL-W2-0152")) { verify_case("WTL-W2-0152", u8s(u8"마지막 문단"), values({ u8s(u8"마지막 문단") })); }
TEST_CASE("WTL-W2-0153", W2_TAGS("WTL-W2-0153")) { verify_case("WTL-W2-0153", u8s(u8"마지막 문단\n"), values({ u8s(u8"마지막 문단") })); }
TEST_CASE("WTL-W2-0154", W2_TAGS("WTL-W2-0154")) { verify_case("WTL-W2-0154", u8s(u8"마지막 문단\r\n"), values({ u8s(u8"마지막 문단") })); }
TEST_CASE("WTL-W2-0155", W2_TAGS("WTL-W2-0155")) { verify_case("WTL-W2-0155", u8s(u8"마지막 문단\n\n"), values({ u8s(u8"마지막 문단") })); }
TEST_CASE("WTL-W2-0156", W2_TAGS("WTL-W2-0156")) { verify_case("WTL-W2-0156", u8s(u8"마지막 문단\r\n\r\n"), values({ u8s(u8"마지막 문단") })); }
TEST_CASE("WTL-W2-0157", W2_TAGS("WTL-W2-0157")) { verify_case("WTL-W2-0157", u8s(u8"마지막 문단\n \t\n"), values({ u8s(u8"마지막 문단") })); }
TEST_CASE("WTL-W2-0158", W2_TAGS("WTL-W2-0158")) { verify_case("WTL-W2-0158", u8s(u8"\r\n \t\n\n첫 문단"), values({ u8s(u8"첫 문단") })); }
TEST_CASE("WTL-W2-0159", W2_TAGS("WTL-W2-0159")) { verify_case("WTL-W2-0159", u8s(u8"\r\n 첫 줄 \r\n\t\r\n둘째 문단\n\n"), values({ u8s(u8" 첫 줄 "), u8s(u8"둘째 문단") }), true); }
TEST_CASE("WTL-W2-0160", W2_TAGS("WTL-W2-0160")) { verify_case("WTL-W2-0160", u8s(u8"\r\n첫 줄\r\n둘째 줄\r\n \t\r\n\r\n둘째 문단\n\n\n"), values({ u8s(u8"첫 줄\n둘째 줄"), u8s(u8"둘째 문단") }), true); }

TEST_CASE("WTL-W2-0161", W2_TAGS("WTL-W2-0161"))
{
	const C_PARAGRAPH_PARSER Parser(std::make_shared<C_PIPE_POLICY>());
	const std::string Input = u8s(u8"첫 문단|둘째 문단");
	REQUIRE(Parser.Split(Input) == values({ u8s(u8"첫 문단"), u8s(u8"둘째 문단") }));
	emit_line("WTL-W2-0161", golden_line("WTL-W2-0161", Parser, Input));
}

TEST_CASE("WTL-W2-0162", W2_TAGS("WTL-W2-0162"))
{
	std::string Text;
	for (int nIndex = 0; nIndex < 10'000; ++nIndex) {
		if (nIndex != 0) {
			Text += "\r\n \t\r\n";
		}
		Text += u8s(u8"문단 ") + std::to_string(nIndex) + u8s(u8" 😀");
	}
	const C_PARAGRAPH_PARSER Parser;
	const auto Started = std::chrono::steady_clock::now();
	const std::vector<std::string> Paragraphs = Parser.Split(Text);
	const auto Elapsed = std::chrono::steady_clock::now() - Started;
	const std::string FirstExpected = u8s(u8"문단 0 😀");
	const std::string LastExpected = u8s(u8"문단 9999 😀");
	REQUIRE(Paragraphs.size() == 10'000u);
	REQUIRE(Paragraphs.front() == FirstExpected);
	REQUIRE(Paragraphs.back() == LastExpected);
	REQUIRE(Elapsed < std::chrono::seconds(5));
	std::ostringstream Stream;
	Stream << "WTL-W2-0162|count=" << Paragraphs.size()
		<< "|first=" << hex_encode(Paragraphs.front())
		<< "|last=" << hex_encode(Paragraphs.back());
	emit_line("WTL-W2-0162", Stream.str());
}

#undef W2_TAGS
