#include "pynote/core/storage/repositories.h"

#include <sqlite3/sqlite3.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "sqlite3")

namespace
{
	// u8 리터럴을 SQLite 가 받는 const char* 로 옮긴다. 이 기계에서 좁은 리터럴은 CP949 로
	// 컴파일되므로(spec_TR2 §1(a)) SQL 원문은 전건 u8 리터럴이고 형 변환은 여기 한 곳뿐이다.
	const char* as_sql(const char8_t* _pszSql)
	{
		return(reinterpret_cast<const char*>(_pszSql));
	}

	// 원본 time.time_ns() // 1_000 이식이다. system_clock 의 epoch 는 C++20 부터 Unix epoch 다.
	std::int64_t now_epoch_us()
	{
		const auto Since = std::chrono::system_clock::now().time_since_epoch();
		return(std::chrono::duration_cast<std::chrono::microseconds>(Since).count());
	}

	// ---------------------------------------------------------------------------------------
	// SHA-256. 원본 text_hash 가 hashlib 로 내는 다이제스트를 그대로 내야 하는데 core 는
	// Win32(bcrypt) 를 쓸 수 없고 표준 라이브러리에도 해시가 없어 여기서 직접 구현한다.
	// ---------------------------------------------------------------------------------------
	const std::uint32_t SHA256_K[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
	};

	std::uint32_t rotr32(std::uint32_t _nValue, int _nBits)
	{
		return((_nValue >> _nBits) | (_nValue << (32 - _nBits)));
	}

	void sha256_block(std::uint32_t* _pState, const unsigned char* _pBlock)
	{
		std::uint32_t W[64];
		for (int i = 0; i < 16; ++i)
		{
			W[i] = (static_cast<std::uint32_t>(_pBlock[i * 4 + 0]) << 24)
				 | (static_cast<std::uint32_t>(_pBlock[i * 4 + 1]) << 16)
				 | (static_cast<std::uint32_t>(_pBlock[i * 4 + 2]) << 8)
				 | (static_cast<std::uint32_t>(_pBlock[i * 4 + 3]));
		}
		for (int i = 16; i < 64; ++i)
		{
			const std::uint32_t nS0 = rotr32(W[i - 15], 7) ^ rotr32(W[i - 15], 18) ^ (W[i - 15] >> 3);
			const std::uint32_t nS1 = rotr32(W[i - 2], 17) ^ rotr32(W[i - 2], 19) ^ (W[i - 2] >> 10);
			W[i] = W[i - 16] + nS0 + W[i - 7] + nS1;
		}

		std::uint32_t a = _pState[0];
		std::uint32_t b = _pState[1];
		std::uint32_t c = _pState[2];
		std::uint32_t d = _pState[3];
		std::uint32_t e = _pState[4];
		std::uint32_t f = _pState[5];
		std::uint32_t g = _pState[6];
		std::uint32_t h = _pState[7];

		for (int i = 0; i < 64; ++i)
		{
			const std::uint32_t nSum1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
			const std::uint32_t nCh   = (e & f) ^ ((~e) & g);
			const std::uint32_t nT1   = h + nSum1 + nCh + SHA256_K[i] + W[i];
			const std::uint32_t nSum0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
			const std::uint32_t nMaj  = (a & b) ^ (a & c) ^ (b & c);
			const std::uint32_t nT2   = nSum0 + nMaj;

			h = g;
			g = f;
			f = e;
			e = d + nT1;
			d = c;
			c = b;
			b = a;
			a = nT1 + nT2;
		}

		_pState[0] += a;
		_pState[1] += b;
		_pState[2] += c;
		_pState[3] += d;
		_pState[4] += e;
		_pState[5] += f;
		_pState[6] += g;
		_pState[7] += h;
	}

	std::string sha256_hex(const std::string& _sBytes)
	{
		std::uint32_t State[8] = {
			0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
		};

		const unsigned char* pData = reinterpret_cast<const unsigned char*>(_sBytes.data());
		const std::size_t    nSize = _sBytes.size();

		std::size_t nOffset = 0;
		for (; nOffset + 64 <= nSize; nOffset += 64)
		{
			sha256_block(State, pData + nOffset);
		}

		// 말미 블록. 0x80 한 바이트, 0 채움, 마지막 8 바이트에 비트 길이를 빅엔디언으로 넣는다.
		unsigned char      Tail[128] = {};
		const std::size_t  nRemain   = nSize - nOffset;
		for (std::size_t i = 0; i < nRemain; ++i) { Tail[i] = pData[nOffset + i]; }
		Tail[nRemain] = 0x80;

		const std::size_t nTailSize = (nRemain + 1 + 8 <= 64) ? 64 : 128;
		const std::uint64_t nBits = static_cast<std::uint64_t>(nSize) * 8u;
		for (int i = 0; i < 8; ++i)
		{
			Tail[nTailSize - 1 - i] = static_cast<unsigned char>((nBits >> (i * 8)) & 0xffu);
		}
		for (std::size_t i = 0; i < nTailSize; i += 64) { sha256_block(State, Tail + i); }

		static const char HEX[] = "0123456789abcdef";
		std::string sResult;
		sResult.reserve(64);
		for (int i = 0; i < 8; ++i)
		{
			for (int nShift = 24; nShift >= 0; nShift -= 8)
			{
				const unsigned char byValue = static_cast<unsigned char>((State[i] >> nShift) & 0xffu);
				sResult.push_back(HEX[byValue >> 4]);
				sResult.push_back(HEX[byValue & 0x0fu]);
			}
		}
		return(sResult);
	}

	// ---------------------------------------------------------------------------------------
	// JSON. workspace_windows 의 탭 목록만 오가므로 원본 json 모듈 중 그 경로에 필요한 것만 담는다.
	// 인코딩은 json.dumps(ensure_ascii=False, separators=(",", ":")) 와 같은 바이트를 낸다.
	// ---------------------------------------------------------------------------------------
	void json_append_string(std::string* _pOut, const std::string& _sValue)
	{
		_pOut->push_back('"');
		for (char chRaw : _sValue)
		{
			const unsigned char ch = static_cast<unsigned char>(chRaw);
			switch (ch)
			{
			case '"':  _pOut->append("\\\""); break;
			case '\\': _pOut->append("\\\\"); break;
			case '\b': _pOut->append("\\b");  break;
			case '\f': _pOut->append("\\f");  break;
			case '\n': _pOut->append("\\n");  break;
			case '\r': _pOut->append("\\r");  break;
			case '\t': _pOut->append("\\t");  break;
			default:
				if (ch < 0x20)
				{
					static const char HEX[] = "0123456789abcdef";
					_pOut->append("\\u00");
					_pOut->push_back(HEX[ch >> 4]);
					_pOut->push_back(HEX[ch & 0x0fu]);
				}
				else
				{
					// ensure_ascii=False 라 UTF-8 바이트는 그대로 나간다.
					_pOut->push_back(chRaw);
				}
				break;
			}
		}
		_pOut->push_back('"');
	}

	std::string json_encode_string_array(const std::vector<std::string>& _Values)
	{
		std::string sResult;
		sResult.push_back('[');
		for (std::size_t i = 0; i < _Values.size(); ++i)
		{
			if (i != 0) { sResult.push_back(','); }
			json_append_string(&sResult, _Values[i]);
		}
		sResult.push_back(']');
		return(sResult);
	}

	enum class E_JSON_KIND
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	};

	struct S_JSON_VALUE
	{
		E_JSON_KIND               eKind{ E_JSON_KIND::Null };
		std::string               sText;   // String 일 때만 뜻이 있다(디코딩된 UTF-8).
		std::vector<S_JSON_VALUE> Items;   // Array 일 때만 뜻이 있다.
	};

	// 원본이 쓰는 json.loads 중 이 경로에 필요한 만큼이다. 값 종류를 구별해야 하는 이유는
	// 파싱 실패와 "파싱은 됐지만 문자열 배열이 아님"이 원본에서 서로 다른 사유로 갈라지기 때문이다.
	class C_JSON_PARSER
	{
	public:
		explicit C_JSON_PARSER(const std::string& _sText)
			: m_sText(_sText)
		{
		}

		bool ParseDocument(S_JSON_VALUE* _pValue)
		{
			this->skip_whitespace_();
			if (!this->parse_value_(_pValue, 0)) { return(false); }
			this->skip_whitespace_();
			return(m_nPos == m_sText.size());
		}

	private:
		// CEILING: 중첩 깊이 1000 을 넘으면 파싱 실패로 접는다. 원본은 같은 자리에서
		// RecursionError 를 올려 호출부로 전파한다.
		static const int MAX_DEPTH = 1000;

		void skip_whitespace_()
		{
			while (m_nPos < m_sText.size())
			{
				const char ch = m_sText[m_nPos];
				if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { ++m_nPos; }
				else { break; }
			}
		}

		bool match_literal_(const char* _pszText)
		{
			const std::size_t nLength = std::char_traits<char>::length(_pszText);
			if (m_sText.compare(m_nPos, nLength, _pszText) != 0) { return(false); }
			m_nPos += nLength;
			return(true);
		}

		bool parse_value_(S_JSON_VALUE* _pValue, int _nDepth)
		{
			if (_nDepth > MAX_DEPTH) { return(false); }
			if (m_nPos >= m_sText.size()) { return(false); }

			const char ch = m_sText[m_nPos];
			if (ch == '"')
			{
				_pValue->eKind = E_JSON_KIND::String;
				return(this->parse_string_(&_pValue->sText));
			}
			if (ch == '[') { return(this->parse_array_(_pValue, _nDepth)); }
			if (ch == '{') { return(this->parse_object_(_pValue, _nDepth)); }
			if (this->match_literal_("null"))  { _pValue->eKind = E_JSON_KIND::Null; return(true); }
			if (this->match_literal_("true"))  { _pValue->eKind = E_JSON_KIND::Bool; return(true); }
			if (this->match_literal_("false")) { _pValue->eKind = E_JSON_KIND::Bool; return(true); }

			// 원본 json 은 기본 설정에서 NaN/Infinity/-Infinity 를 받는다.
			if (this->match_literal_("NaN") || this->match_literal_("Infinity") || this->match_literal_("-Infinity"))
			{
				_pValue->eKind = E_JSON_KIND::Number;
				return(true);
			}
			_pValue->eKind = E_JSON_KIND::Number;
			return(this->parse_number_());
		}

		bool parse_array_(S_JSON_VALUE* _pValue, int _nDepth)
		{
			_pValue->eKind = E_JSON_KIND::Array;
			++m_nPos;
			this->skip_whitespace_();
			if (m_nPos < m_sText.size() && m_sText[m_nPos] == ']') { ++m_nPos; return(true); }

			for (;;)
			{
				this->skip_whitespace_();
				S_JSON_VALUE Item;
				if (!this->parse_value_(&Item, _nDepth + 1)) { return(false); }
				_pValue->Items.push_back(std::move(Item));

				this->skip_whitespace_();
				if (m_nPos >= m_sText.size()) { return(false); }
				if (m_sText[m_nPos] == ',') { ++m_nPos; continue; }
				if (m_sText[m_nPos] == ']') { ++m_nPos; return(true); }
				return(false);
			}
		}

		bool parse_object_(S_JSON_VALUE* _pValue, int _nDepth)
		{
			_pValue->eKind = E_JSON_KIND::Object;
			++m_nPos;
			this->skip_whitespace_();
			if (m_nPos < m_sText.size() && m_sText[m_nPos] == '}') { ++m_nPos; return(true); }

			for (;;)
			{
				this->skip_whitespace_();
				if (m_nPos >= m_sText.size() || m_sText[m_nPos] != '"') { return(false); }
				std::string sKey;
				if (!this->parse_string_(&sKey)) { return(false); }

				this->skip_whitespace_();
				if (m_nPos >= m_sText.size() || m_sText[m_nPos] != ':') { return(false); }
				++m_nPos;

				this->skip_whitespace_();
				S_JSON_VALUE Ignored;
				if (!this->parse_value_(&Ignored, _nDepth + 1)) { return(false); }

				this->skip_whitespace_();
				if (m_nPos >= m_sText.size()) { return(false); }
				if (m_sText[m_nPos] == ',') { ++m_nPos; continue; }
				if (m_sText[m_nPos] == '}') { ++m_nPos; return(true); }
				return(false);
			}
		}

		bool parse_number_()
		{
			const std::size_t nStart = m_nPos;
			if (m_nPos < m_sText.size() && m_sText[m_nPos] == '-') { ++m_nPos; }

			std::size_t nDigits = 0;
			while (m_nPos < m_sText.size() && m_sText[m_nPos] >= '0' && m_sText[m_nPos] <= '9') { ++m_nPos; ++nDigits; }
			if (nDigits == 0) { m_nPos = nStart; return(false); }

			if (m_nPos < m_sText.size() && m_sText[m_nPos] == '.')
			{
				++m_nPos;
				std::size_t nFraction = 0;
				while (m_nPos < m_sText.size() && m_sText[m_nPos] >= '0' && m_sText[m_nPos] <= '9') { ++m_nPos; ++nFraction; }
				if (nFraction == 0) { return(false); }
			}
			if (m_nPos < m_sText.size() && (m_sText[m_nPos] == 'e' || m_sText[m_nPos] == 'E'))
			{
				++m_nPos;
				if (m_nPos < m_sText.size() && (m_sText[m_nPos] == '+' || m_sText[m_nPos] == '-')) { ++m_nPos; }
				std::size_t nExponent = 0;
				while (m_nPos < m_sText.size() && m_sText[m_nPos] >= '0' && m_sText[m_nPos] <= '9') { ++m_nPos; ++nExponent; }
				if (nExponent == 0) { return(false); }
			}
			return(true);
		}

		void append_code_point_(std::string* _pOut, std::uint32_t _nCodePoint)
		{
			if (_nCodePoint < 0x80u)
			{
				_pOut->push_back(static_cast<char>(_nCodePoint));
			}
			else if (_nCodePoint < 0x800u)
			{
				_pOut->push_back(static_cast<char>(0xc0u | (_nCodePoint >> 6)));
				_pOut->push_back(static_cast<char>(0x80u | (_nCodePoint & 0x3fu)));
			}
			else if (_nCodePoint < 0x10000u)
			{
				_pOut->push_back(static_cast<char>(0xe0u | (_nCodePoint >> 12)));
				_pOut->push_back(static_cast<char>(0x80u | ((_nCodePoint >> 6) & 0x3fu)));
				_pOut->push_back(static_cast<char>(0x80u | (_nCodePoint & 0x3fu)));
			}
			else
			{
				_pOut->push_back(static_cast<char>(0xf0u | (_nCodePoint >> 18)));
				_pOut->push_back(static_cast<char>(0x80u | ((_nCodePoint >> 12) & 0x3fu)));
				_pOut->push_back(static_cast<char>(0x80u | ((_nCodePoint >> 6) & 0x3fu)));
				_pOut->push_back(static_cast<char>(0x80u | (_nCodePoint & 0x3fu)));
			}
		}

		bool parse_hex4_(std::uint32_t* _pValue)
		{
			if (m_nPos + 4 > m_sText.size()) { return(false); }
			std::uint32_t nValue = 0;
			for (int i = 0; i < 4; ++i)
			{
				const char ch = m_sText[m_nPos + i];
				std::uint32_t nDigit = 0;
				if (ch >= '0' && ch <= '9')      { nDigit = static_cast<std::uint32_t>(ch - '0'); }
				else if (ch >= 'a' && ch <= 'f') { nDigit = static_cast<std::uint32_t>(ch - 'a' + 10); }
				else if (ch >= 'A' && ch <= 'F') { nDigit = static_cast<std::uint32_t>(ch - 'A' + 10); }
				else { return(false); }
				nValue = (nValue << 4) | nDigit;
			}
			m_nPos += 4;
			*_pValue = nValue;
			return(true);
		}

		bool parse_string_(std::string* _pOut)
		{
			++m_nPos;
			for (;;)
			{
				if (m_nPos >= m_sText.size()) { return(false); }
				const unsigned char ch = static_cast<unsigned char>(m_sText[m_nPos]);
				if (ch == '"') { ++m_nPos; return(true); }

				// 원본 디코더는 strict 기본값이라 제어문자를 그대로 담은 문자열을 거부한다.
				if (ch < 0x20) { return(false); }

				if (ch != '\\') { _pOut->push_back(m_sText[m_nPos]); ++m_nPos; continue; }

				++m_nPos;
				if (m_nPos >= m_sText.size()) { return(false); }
				const char chEscape = m_sText[m_nPos];
				++m_nPos;
				switch (chEscape)
				{
				case '"':  _pOut->push_back('"');  break;
				case '\\': _pOut->push_back('\\'); break;
				case '/':  _pOut->push_back('/');  break;
				case 'b':  _pOut->push_back('\b'); break;
				case 'f':  _pOut->push_back('\f'); break;
				case 'n':  _pOut->push_back('\n'); break;
				case 'r':  _pOut->push_back('\r'); break;
				case 't':  _pOut->push_back('\t'); break;
				case 'u':
				{
					std::uint32_t nFirst = 0;
					if (!this->parse_hex4_(&nFirst)) { return(false); }
					if (nFirst >= 0xd800u && nFirst <= 0xdbffu)
					{
						// CEILING: 짝을 못 찾은 대행 코드는 파싱 실패로 접는다. 원본 파이썬은
						// 문자열에 그대로 담을 수 있지만 UTF-8 std::string 으로는 표현할 수 없다.
						if (m_nPos + 6 > m_sText.size()) { return(false); }
						if (m_sText[m_nPos] != '\\' || m_sText[m_nPos + 1] != 'u') { return(false); }
						m_nPos += 2;
						std::uint32_t nSecond = 0;
						if (!this->parse_hex4_(&nSecond)) { return(false); }
						if (nSecond < 0xdc00u || nSecond > 0xdfffu) { return(false); }
						const std::uint32_t nCodePoint =
							0x10000u + ((nFirst - 0xd800u) << 10) + (nSecond - 0xdc00u);
						this->append_code_point_(_pOut, nCodePoint);
					}
					else if (nFirst >= 0xdc00u && nFirst <= 0xdfffu)
					{
						return(false);
					}
					else
					{
						this->append_code_point_(_pOut, nFirst);
					}
					break;
				}
				default:
					return(false);
				}
			}
		}

		const std::string& m_sText;
		std::size_t        m_nPos{ 0 };
	};

	// ---------------------------------------------------------------------------------------
	// 파이썬 str.strip() 이식. 인자 없는 strip 은 ASCII 공백이 아니라 유니코드 공백 전부를
	// 걷어내므로(str.isspace() 기준) 검색어 정규화를 ASCII 로 좁히면 동작이 달라진다.
	// ---------------------------------------------------------------------------------------
	bool is_python_space(std::uint32_t _nCodePoint)
	{
		return((_nCodePoint >= 0x0009u && _nCodePoint <= 0x000du)
			|| (_nCodePoint >= 0x001cu && _nCodePoint <= 0x001fu)
			|| _nCodePoint == 0x0020u
			|| _nCodePoint == 0x0085u
			|| _nCodePoint == 0x00a0u
			|| _nCodePoint == 0x1680u
			|| (_nCodePoint >= 0x2000u && _nCodePoint <= 0x200au)
			|| _nCodePoint == 0x2028u
			|| _nCodePoint == 0x2029u
			|| _nCodePoint == 0x202fu
			|| _nCodePoint == 0x205fu
			|| _nCodePoint == 0x3000u);
	}

	// 성공하면 코드포인트와 바이트 길이를 채운다. 잘못된 바이트열이면 실패다.
	bool decode_utf8(const std::string& _sText, std::size_t _nPos, std::uint32_t* _pCodePoint, std::size_t* _pLength)
	{
		const std::size_t nSize = _sText.size();
		if (_nPos >= nSize) { return(false); }

		const unsigned char by0 = static_cast<unsigned char>(_sText[_nPos]);
		std::size_t   nLength    = 0;
		std::uint32_t nCodePoint = 0;
		if (by0 < 0x80u)                      { nLength = 1; nCodePoint = by0; }
		else if ((by0 & 0xe0u) == 0xc0u)      { nLength = 2; nCodePoint = by0 & 0x1fu; }
		else if ((by0 & 0xf0u) == 0xe0u)      { nLength = 3; nCodePoint = by0 & 0x0fu; }
		else if ((by0 & 0xf8u) == 0xf0u)      { nLength = 4; nCodePoint = by0 & 0x07u; }
		else                                  { return(false); }

		if (_nPos + nLength > nSize) { return(false); }
		for (std::size_t i = 1; i < nLength; ++i)
		{
			const unsigned char by = static_cast<unsigned char>(_sText[_nPos + i]);
			if ((by & 0xc0u) != 0x80u) { return(false); }
			nCodePoint = (nCodePoint << 6) | (by & 0x3fu);
		}

		*_pCodePoint = nCodePoint;
		*_pLength    = nLength;
		return(true);
	}

	std::string python_strip(const std::string& _sText)
	{
		// (바이트 오프셋, 길이, 코드포인트) 를 앞에서부터 모아 두고 양끝을 판정한다.
		std::vector<std::size_t>   Offsets;
		std::vector<std::size_t>   Lengths;
		std::vector<std::uint32_t> CodePoints;

		std::size_t nPos = 0;
		while (nPos < _sText.size())
		{
			std::uint32_t nCodePoint = 0;
			std::size_t   nLength    = 0;
			if (!decode_utf8(_sText, nPos, &nCodePoint, &nLength))
			{
				// 잘못된 바이트는 공백이 아닌 것으로 본다.
				nCodePoint = 0xfffdu;
				nLength    = 1;
			}
			Offsets.push_back(nPos);
			Lengths.push_back(nLength);
			CodePoints.push_back(nCodePoint);
			nPos += nLength;
		}

		std::size_t nFirst = 0;
		while (nFirst < CodePoints.size() && is_python_space(CodePoints[nFirst])) { ++nFirst; }
		if (nFirst == CodePoints.size()) { return(std::string{}); }

		std::size_t nLast = CodePoints.size() - 1;
		while (nLast > nFirst && is_python_space(CodePoints[nLast])) { --nLast; }

		const std::size_t nStart = Offsets[nFirst];
		const std::size_t nEnd   = Offsets[nLast] + Lengths[nLast];
		return(_sText.substr(nStart, nEnd - nStart));
	}

	// 원본 search_documents/search_cards 의 LIKE 이스케이프(:195, :228) 그대로다.
	std::string like_pattern(const std::string& _sNormalized)
	{
		std::string sEscaped;
		sEscaped.reserve(_sNormalized.size() + 2);
		for (char ch : _sNormalized)
		{
			if (ch == '/')      { sEscaped.append("//"); }
			else if (ch == '%') { sEscaped.append("/%"); }
			else if (ch == '_') { sEscaped.append("/_"); }
			else                { sEscaped.push_back(ch); }
		}
		return("%" + sEscaped + "%");
	}

	// ---------------------------------------------------------------------------------------
	// 준비된 문장 한 개. 소멸 시 finalize 한다.
	// ---------------------------------------------------------------------------------------
	class C_STATEMENT
	{
	public:
		C_STATEMENT(sqlite3* _pHandle, const char8_t* _pszSql)
		{
			if (_pHandle == nullptr) { return; }
			if (::sqlite3_prepare_v2(_pHandle, as_sql(_pszSql), -1, &m_pStmt, nullptr) != SQLITE_OK)
			{
				m_pStmt = nullptr;
			}
		}

		~C_STATEMENT()
		{
			if (m_pStmt != nullptr) { ::sqlite3_finalize(m_pStmt); }
		}

		C_STATEMENT(const C_STATEMENT&) = delete;
		C_STATEMENT& operator=(const C_STATEMENT&) = delete;

		bool IsPrepared() const noexcept { return(m_pStmt != nullptr); }
		bool BindOk() const noexcept { return(m_bBindOk); }
		sqlite3_stmt* Handle() const noexcept { return(m_pStmt); }

		void BindText(int _nIndex, std::string_view _sValue)
		{
			m_bBindOk = m_bBindOk && (::sqlite3_bind_text(
				m_pStmt, _nIndex, _sValue.data(), static_cast<int>(_sValue.size()), SQLITE_TRANSIENT) == SQLITE_OK);
		}

		void BindNullableText(int _nIndex, const std::optional<std::string>& _Value)
		{
			if (!_Value.has_value())
			{
				m_bBindOk = m_bBindOk && (::sqlite3_bind_null(m_pStmt, _nIndex) == SQLITE_OK);
				return;
			}
			this->BindText(_nIndex, *_Value);
		}

		void BindInt64(int _nIndex, std::int64_t _nValue)
		{
			m_bBindOk = m_bBindOk && (::sqlite3_bind_int64(m_pStmt, _nIndex, _nValue) == SQLITE_OK);
		}

		void BindNullableInt64(int _nIndex, const std::optional<std::int64_t>& _Value)
		{
			if (!_Value.has_value())
			{
				m_bBindOk = m_bBindOk && (::sqlite3_bind_null(m_pStmt, _nIndex) == SQLITE_OK);
				return;
			}
			this->BindInt64(_nIndex, *_Value);
		}

		int Step() { return(::sqlite3_step(m_pStmt)); }

	private:
		sqlite3_stmt* m_pStmt{ nullptr };
		bool          m_bBindOk{ true };
	};

	// ---------------------------------------------------------------------------------------
	// 열 접근. 원본이 sqlite3.Row 로 이름 접근을 하므로 이식도 이름으로 찾는다.
	// SELECT * 가 계약이라 열 위치를 코드에 박으면 스키마가 바뀔 때 조용히 어긋난다.
	// ---------------------------------------------------------------------------------------
	int column_index(sqlite3_stmt* _pStmt, const char* _pszName)
	{
		const int nCount = ::sqlite3_column_count(_pStmt);
		for (int i = 0; i < nCount; ++i)
		{
			const char* pszColumn = ::sqlite3_column_name(_pStmt, i);
			if (pszColumn != nullptr && std::string_view(pszColumn) == _pszName) { return(i); }
		}
		return(-1);
	}

	std::string column_text(sqlite3_stmt* _pStmt, const char* _pszName)
	{
		const int nIndex = column_index(_pStmt, _pszName);
		if (nIndex < 0) { return(std::string{}); }
		const unsigned char* pText = ::sqlite3_column_text(_pStmt, nIndex);
		const int            nSize = ::sqlite3_column_bytes(_pStmt, nIndex);
		if (pText == nullptr) { return(std::string{}); }
		return(std::string(reinterpret_cast<const char*>(pText), static_cast<std::size_t>(nSize)));
	}

	std::optional<std::string> column_nullable_text(sqlite3_stmt* _pStmt, const char* _pszName)
	{
		const int nIndex = column_index(_pStmt, _pszName);
		if (nIndex < 0 || ::sqlite3_column_type(_pStmt, nIndex) == SQLITE_NULL) { return(std::nullopt); }
		return(column_text(_pStmt, _pszName));
	}

	std::int64_t column_int64(sqlite3_stmt* _pStmt, const char* _pszName)
	{
		const int nIndex = column_index(_pStmt, _pszName);
		if (nIndex < 0) { return(0); }
		return(::sqlite3_column_int64(_pStmt, nIndex));
	}

	std::optional<std::int64_t> column_nullable_int64(sqlite3_stmt* _pStmt, const char* _pszName)
	{
		const int nIndex = column_index(_pStmt, _pszName);
		if (nIndex < 0 || ::sqlite3_column_type(_pStmt, nIndex) == SQLITE_NULL) { return(std::nullopt); }
		return(::sqlite3_column_int64(_pStmt, nIndex));
	}

	// 모르는 철자는 실패다. 원본은 StrEnum 생성자가 ValueError 를 올리는 자리라 사유만 우리 말로 옮긴다.
	template <typename T_ENUM>
	bool read_enum(
		pynote::core::storage::C_DATABASE& _database,
		sqlite3_stmt* _pStmt,
		const char* _pszColumn,
		const char* _pszWhere,
		T_ENUM* _peValue)
	{
		const std::string sText = column_text(_pStmt, _pszColumn);
		if (!pynote::core::domain::FromText(sText, _peValue))
		{
			_database.SetLastError(std::string(_pszWhere) + " 값을 해석하지 못했습니다: " + sText);
			return(false);
		}
		return(true);
	}

	// ---------------------------------------------------------------------------------------
	// 행 매퍼. 원본 _*_from_row(:827 이후) 를 열 단위로 그대로 옮긴 것이다.
	// NULL 이 어떻게 부재가 되는지는 구조체가 아니라 이 매퍼가 정한다.
	// ---------------------------------------------------------------------------------------
	// documents 에는 열거형 열이 없어 연결 인자를 쓰지 않는다. 다른 매퍼와 호출 모양을 맞춰 둔다.
	bool map_document(pynote::core::storage::C_DATABASE&, sqlite3_stmt* _pStmt, pynote::core::domain::S_DOCUMENT* _pOut)
	{
		_pOut->sId            = column_text(_pStmt, "id");
		_pOut->sTitle         = column_text(_pStmt, "title");
		_pOut->nCreatedAtUs   = column_int64(_pStmt, "created_at_us");
		_pOut->nUpdatedAtUs   = column_int64(_pStmt, "updated_at_us");
		_pOut->nArchivedAtUs  = column_nullable_int64(_pStmt, "archived_at_us");
		_pOut->nTrashedAtUs   = column_nullable_int64(_pStmt, "trashed_at_us");
		return(true);
	}

	bool map_capture_operation(pynote::core::storage::C_DATABASE& _database, sqlite3_stmt* _pStmt, pynote::core::domain::S_CAPTURE_OPERATION* _pOut)
	{
		_pOut->sId          = column_text(_pStmt, "id");
		_pOut->sDocumentId  = column_text(_pStmt, "document_id");
		if (!read_enum(_database, _pStmt, "source", "capture_operations.source", &_pOut->eSource)) { return(false); }
		if (!read_enum(_database, _pStmt, "split_policy", "capture_operations.split_policy", &_pOut->eSplitPolicy)) { return(false); }
		_pOut->sOriginalText         = column_nullable_text(_pStmt, "original_text");
		_pOut->sOriginalHash         = column_nullable_text(_pStmt, "original_hash");
		_pOut->nOriginalRedactedAtUs = column_nullable_int64(_pStmt, "original_redacted_at_us");
		_pOut->nCreatedAtUs          = column_int64(_pStmt, "created_at_us");
		return(true);
	}

	bool map_card(pynote::core::storage::C_DATABASE& _database, sqlite3_stmt* _pStmt, pynote::core::domain::S_CARD* _pOut)
	{
		_pOut->sId          = column_text(_pStmt, "id");
		_pOut->sDocumentId  = column_text(_pStmt, "document_id");
		_pOut->sOperationId = column_text(_pStmt, "operation_id");
		_pOut->nPositionKey = column_int64(_pStmt, "position_key");
		_pOut->nCaptureSeq  = column_int64(_pStmt, "capture_seq");
		_pOut->nCreatedAtUs = column_int64(_pStmt, "created_at_us");
		_pOut->nUpdatedAtUs = column_int64(_pStmt, "updated_at_us");
		if (!read_enum(_database, _pStmt, "source", "cards.source", &_pOut->eSource)) { return(false); }
		_pOut->sBody              = column_text(_pStmt, "body");
		_pOut->sBodyHash          = column_text(_pStmt, "body_hash");
		_pOut->sCurrentRevisionId = column_nullable_text(_pStmt, "current_revision_id");
		_pOut->nDeletedAtUs       = column_nullable_int64(_pStmt, "deleted_at_us");
		return(true);
	}

	bool map_revision(pynote::core::storage::C_DATABASE& _database, sqlite3_stmt* _pStmt, pynote::core::domain::S_CARD_REVISION* _pOut)
	{
		_pOut->sId               = column_text(_pStmt, "id");
		_pOut->sCardId           = column_text(_pStmt, "card_id");
		_pOut->nEventSeq         = column_int64(_pStmt, "event_seq");
		_pOut->sParentRevisionId = column_nullable_text(_pStmt, "parent_revision_id");
		_pOut->sBody             = column_text(_pStmt, "body");
		_pOut->sBodyHash         = column_text(_pStmt, "body_hash");
		if (!read_enum(_database, _pStmt, "source", "card_revisions.source", &_pOut->eSource)) { return(false); }
		_pOut->nCreatedAtUs = column_int64(_pStmt, "created_at_us");
		return(true);
	}

	bool map_draft(pynote::core::storage::C_DATABASE& _database, sqlite3_stmt* _pStmt, pynote::core::domain::S_DRAFT* _pOut)
	{
		_pOut->sId         = column_text(_pStmt, "id");
		_pOut->sDocumentId = column_text(_pStmt, "document_id");
		_pOut->sCardId     = column_nullable_text(_pStmt, "card_id");
		if (!read_enum(_database, _pStmt, "draft_kind", "drafts.draft_kind", &_pOut->eDraftKind)) { return(false); }
		_pOut->sBaseRevisionId      = column_nullable_text(_pStmt, "base_revision_id");
		_pOut->sDraftText           = column_text(_pStmt, "draft_text");
		_pOut->sDraftHash           = column_text(_pStmt, "draft_hash");
		_pOut->nCursorPositionQchar = column_int64(_pStmt, "cursor_position_qchar");
		_pOut->nUpdatedAtUs         = column_int64(_pStmt, "updated_at_us");
		return(true);
	}

	bool map_event(pynote::core::storage::C_DATABASE& _database, sqlite3_stmt* _pStmt, pynote::core::domain::S_EDIT_EVENT* _pOut)
	{
		_pOut->nEventSeq    = column_int64(_pStmt, "event_seq");
		_pOut->sEventId     = column_text(_pStmt, "event_id");
		_pOut->sOperationId = column_nullable_text(_pStmt, "operation_id");
		_pOut->sDocumentId  = column_text(_pStmt, "document_id");
		_pOut->sCardId      = column_nullable_text(_pStmt, "card_id");
		if (!read_enum(_database, _pStmt, "event_type", "edit_events.event_type", &_pOut->eEventType)) { return(false); }
		if (!read_enum(_database, _pStmt, "source", "edit_events.source", &_pOut->eSource)) { return(false); }
		_pOut->nOccurredAtUs = column_int64(_pStmt, "occurred_at_us");
		_pOut->sDetailsJson  = column_text(_pStmt, "details_json");
		return(true);
	}

	bool map_lineage(pynote::core::storage::C_DATABASE& _database, sqlite3_stmt* _pStmt, pynote::core::domain::S_CARD_LINEAGE* _pOut)
	{
		_pOut->sParentCardId = column_text(_pStmt, "parent_card_id");
		_pOut->sChildCardId  = column_text(_pStmt, "child_card_id");
		_pOut->nEventSeq     = column_int64(_pStmt, "event_seq");
		return(read_enum(_database, _pStmt, "relation_type", "card_lineage.relation_type", &_pOut->eRelationType));
	}

	// 원본 _workspace_window_from_row(:135~156) 이식이다. 세 가지 거절 사유가 서로 다르므로
	// JSON 파싱 실패와 형식 위반을 한 갈래로 접지 않는다.
	bool map_workspace_window(pynote::core::storage::C_DATABASE& _database, sqlite3_stmt* _pStmt, pynote::core::domain::S_WORKSPACE_WINDOW* _pOut)
	{
		const std::string sJson = column_text(_pStmt, "open_document_ids_json");

		C_JSON_PARSER Parser(sJson);
		S_JSON_VALUE  Decoded;
		if (!Parser.ParseDocument(&Decoded))
		{
			_database.SetLastError("workspace_windows 탭 JSON을 읽지 못했습니다.");
			return(false);
		}

		std::vector<std::string> OpenDocumentIds;
		bool bValid = (Decoded.eKind == E_JSON_KIND::Array);
		if (bValid)
		{
			for (const S_JSON_VALUE& Item : Decoded.Items)
			{
				if (Item.eKind != E_JSON_KIND::String) { bValid = false; break; }
				OpenDocumentIds.push_back(Item.sText);
			}
		}
		if (bValid)
		{
			for (std::size_t i = 0; i < OpenDocumentIds.size() && bValid; ++i)
			{
				for (std::size_t j = i + 1; j < OpenDocumentIds.size(); ++j)
				{
					if (OpenDocumentIds[i] == OpenDocumentIds[j]) { bValid = false; break; }
				}
			}
		}
		if (!bValid)
		{
			_database.SetLastError("workspace_windows의 열린 문서 목록 형식이 잘못되었습니다.");
			return(false);
		}

		std::optional<std::string> sActiveDocumentId = column_nullable_text(_pStmt, "active_document_id");
		if (sActiveDocumentId.has_value())
		{
			bool bFound = false;
			for (const std::string& sId : OpenDocumentIds)
			{
				if (sId == *sActiveDocumentId) { bFound = true; break; }
			}
			if (!bFound)
			{
				_database.SetLastError("workspace_windows의 활성 문서가 열린 탭에 없습니다.");
				return(false);
			}
		}

		_pOut->sWindowId         = column_text(_pStmt, "window_id");
		_pOut->OpenDocumentIds   = std::move(OpenDocumentIds);
		_pOut->sActiveDocumentId = std::move(sActiveDocumentId);
		_pOut->nUpdatedAtUs      = column_int64(_pStmt, "updated_at_us");
		return(true);
	}

	// 실패 사유를 연결의 LastError 에 옮긴다. C_DATABASE::Execute 가 sqlite3_exec 의 오류를
	// 두는 자리와 같아야 저장 계층의 보고가 어느 경로에서든 같은 모양이 된다.
	pynote::core::storage::E_REPO_RESULT report_failure(pynote::core::storage::C_DATABASE& _database)
	{
		if (!_database.IsOpen()) { _database.SetLastError("연결이 열려 있지 않습니다."); }
		else { _database.SetLastError(::sqlite3_errmsg(_database.Handle())); }
		return(pynote::core::storage::E_REPO_RESULT::Failed);
	}

	// 행을 돌려주지 않는 문장의 공통 실행 경로.
	pynote::core::storage::E_REPO_RESULT run_done(pynote::core::storage::C_DATABASE& _database, C_STATEMENT& _Stmt)
	{
		if (!_Stmt.BindOk()) { return(report_failure(_database)); }
		if (_Stmt.Step() != SQLITE_DONE) { return(report_failure(_database)); }
		return(pynote::core::storage::E_REPO_RESULT::Ok);
	}
}

namespace pynote::core::storage
{
	std::string TextHash(const std::string& _sText)
	{
		return(sha256_hex(_sText));
	}

	C_REPOSITORIES::C_REPOSITORIES(C_DATABASE& _database)
		: m_Database(_database)
	{
	}

	E_REPO_RESULT C_REPOSITORIES::fail_()
	{
		return(report_failure(m_Database));
	}

	// 쓰기 트랜잭션을 열지 못한 사유를 가른다. 원본은 중첩을 RuntimeError 로, BEGIN 실패를
	// sqlite3.Error 로 올리므로(database.py:57~59) 둘을 한 문구로 뭉뚱그리면 진단이 사라진다.
	E_REPO_RESULT C_REPOSITORIES::begin_failed_()
	{
		if (m_Database.IsOpen() && ::sqlite3_get_autocommit(m_Database.Handle()) == 0)
		{
			m_Database.SetLastError("중첩 트랜잭션은 지원하지 않습니다.");
			return(E_REPO_RESULT::Failed);
		}
		return(this->fail_());
	}

	// 카드 CAS 의 공통 판정(_require_card_cas :820~825). 영향 행수는 문장 직후에 읽는다.
	// 술어는 "1 이상"이 아니라 "정확히 1" 이다 - 예상 리비전 조건이 빗나가면 0 행이 되고,
	// 그것이 곧 다른 저장이 앞질렀다는 뜻이다.
	E_REPO_RESULT C_REPOSITORIES::require_card_cas_(int _nChanges, const std::string& _sCardId)
	{
		if (_nChanges != 1)
		{
			m_Database.SetLastError("카드가 예상한 현재 리비전에서 변경되었습니다: " + _sCardId);
			return(E_REPO_RESULT::CasConflict);
		}
		return(E_REPO_RESULT::Ok);
	}

	// ------------------------------------------------------------------------------------------
	// workspace_windows
	// ------------------------------------------------------------------------------------------
	E_REPO_RESULT C_REPOSITORIES::ListWorkspaceWindows(std::vector<domain::S_WORKSPACE_WINDOW>* _pOut)
	{
		_pOut->clear();
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            SELECT window_id, open_document_ids_json,
                   active_document_id, updated_at_us
            FROM workspace_windows
            ORDER BY updated_at_us, window_id
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }

		int nStep = 0;
		while ((nStep = Stmt.Step()) == SQLITE_ROW)
		{
			domain::S_WORKSPACE_WINDOW Row;
			if (!map_workspace_window(m_Database, Stmt.Handle(), &Row)) { return(E_REPO_RESULT::Invalid); }
			_pOut->push_back(std::move(Row));
		}
		if (nStep != SQLITE_DONE) { return(this->fail_()); }
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::GetWorkspaceWindow(const std::string& _sWindowId, domain::S_WORKSPACE_WINDOW* _pOut)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            SELECT window_id, open_document_ids_json,
                   active_document_id, updated_at_us
            FROM workspace_windows
            WHERE window_id = ?
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sWindowId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		const int nStep = Stmt.Step();
		if (nStep == SQLITE_DONE) { return(E_REPO_RESULT::NotFound); }
		if (nStep != SQLITE_ROW) { return(this->fail_()); }
		return(map_workspace_window(m_Database, Stmt.Handle(), _pOut) ? E_REPO_RESULT::Ok : E_REPO_RESULT::Invalid);
	}

	E_REPO_RESULT C_REPOSITORIES::SaveWorkspaceWindow(
		const std::string& _sWindowId,
		const std::vector<std::string>& _OpenDocumentIds,
		const std::optional<std::string>& _sActiveDocumentId,
		domain::S_WORKSPACE_WINDOW* _pOut)
	{
		// 세 검증은 트랜잭션 밖이다(:93~98).
		if (_sWindowId.empty())
		{
			m_Database.SetLastError("window_id는 비어 있을 수 없습니다.");
			return(E_REPO_RESULT::Invalid);
		}
		for (std::size_t i = 0; i < _OpenDocumentIds.size(); ++i)
		{
			for (std::size_t j = i + 1; j < _OpenDocumentIds.size(); ++j)
			{
				if (_OpenDocumentIds[i] == _OpenDocumentIds[j])
				{
					m_Database.SetLastError("한 창에는 같은 문서 탭을 두 번 저장할 수 없습니다.");
					return(E_REPO_RESULT::Invalid);
				}
			}
		}
		if (_sActiveDocumentId.has_value())
		{
			bool bFound = false;
			for (const std::string& sId : _OpenDocumentIds)
			{
				if (sId == *_sActiveDocumentId) { bFound = true; break; }
			}
			if (!bFound)
			{
				m_Database.SetLastError("활성 문서는 해당 창의 열린 탭 목록에 있어야 합니다.");
				return(E_REPO_RESULT::Invalid);
			}
		}

		const std::int64_t nUpdatedAtUs = now_epoch_us();
		const std::string  sEncodedIds  = json_encode_string_array(_OpenDocumentIds);

		C_TRANSACTION Transaction(m_Database);
		if (!Transaction.IsActive()) { return(this->begin_failed_()); }

		{
			C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
                INSERT INTO workspace_windows(
                    window_id, open_document_ids_json,
                    active_document_id, updated_at_us
                )
                VALUES (?, ?, ?, ?)
                ON CONFLICT(window_id) DO UPDATE SET
                    open_document_ids_json = excluded.open_document_ids_json,
                    active_document_id = excluded.active_document_id,
                    updated_at_us = excluded.updated_at_us
                )SQL");
			if (!Stmt.IsPrepared()) { return(this->fail_()); }
			Stmt.BindText(1, _sWindowId);
			Stmt.BindText(2, sEncodedIds);
			Stmt.BindNullableText(3, _sActiveDocumentId);
			Stmt.BindInt64(4, nUpdatedAtUs);

			const E_REPO_RESULT eResult = run_done(m_Database, Stmt);
			if (eResult != E_REPO_RESULT::Ok) { return(eResult); }
		}

		// 커밋 실패 사유는 C_TRANSACTION 이 이미 LastError 에 넣었다. 롤백이 성공하면
		// sqlite3_errmsg 는 이미 초기화된 뒤라 여기서 다시 읽으면 사유가 지워진다.
		if (!Transaction.Commit()) { return(E_REPO_RESULT::Failed); }

		_pOut->sWindowId         = _sWindowId;
		_pOut->OpenDocumentIds   = _OpenDocumentIds;
		_pOut->sActiveDocumentId = _sActiveDocumentId;
		_pOut->nUpdatedAtUs      = nUpdatedAtUs;
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::DeleteWorkspaceWindow(const std::string& _sWindowId)
	{
		C_TRANSACTION Transaction(m_Database);
		if (!Transaction.IsActive()) { return(this->begin_failed_()); }

		{
			C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(DELETE FROM workspace_windows WHERE window_id = ?)SQL");
			if (!Stmt.IsPrepared()) { return(this->fail_()); }
			Stmt.BindText(1, _sWindowId);

			const E_REPO_RESULT eResult = run_done(m_Database, Stmt);
			if (eResult != E_REPO_RESULT::Ok) { return(eResult); }
		}

		// 커밋 실패 사유는 C_TRANSACTION 이 이미 LastError 에 넣었다. 롤백이 성공하면
		// sqlite3_errmsg 는 이미 초기화된 뒤라 여기서 다시 읽으면 사유가 지워진다.
		if (!Transaction.Commit()) { return(E_REPO_RESULT::Failed); }
		return(E_REPO_RESULT::Ok);
	}

	// ------------------------------------------------------------------------------------------
	// documents
	// ------------------------------------------------------------------------------------------
	E_REPO_RESULT C_REPOSITORIES::CreateDocument(const domain::S_DOCUMENT& _Document)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            INSERT INTO documents(
                id, title, created_at_us, updated_at_us,
                archived_at_us, trashed_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?)
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _Document.sId);
		Stmt.BindText(2, _Document.sTitle);
		Stmt.BindInt64(3, _Document.nCreatedAtUs);
		Stmt.BindInt64(4, _Document.nUpdatedAtUs);
		Stmt.BindNullableInt64(5, _Document.nArchivedAtUs);
		Stmt.BindNullableInt64(6, _Document.nTrashedAtUs);
		return(run_done(m_Database, Stmt));
	}

	E_REPO_RESULT C_REPOSITORIES::GetDocument(const std::string& _sDocumentId, domain::S_DOCUMENT* _pOut)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(SELECT * FROM documents WHERE id = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sDocumentId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		const int nStep = Stmt.Step();
		if (nStep == SQLITE_DONE) { return(E_REPO_RESULT::NotFound); }
		if (nStep != SQLITE_ROW) { return(this->fail_()); }
		return(map_document(m_Database, Stmt.Handle(), _pOut) ? E_REPO_RESULT::Ok : E_REPO_RESULT::Invalid);
	}

	E_REPO_RESULT C_REPOSITORIES::ListDocuments(std::vector<domain::S_DOCUMENT>* _pOut)
	{
		_pOut->clear();
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(SELECT * FROM documents ORDER BY created_at_us, id)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }

		int nStep = 0;
		while ((nStep = Stmt.Step()) == SQLITE_ROW)
		{
			domain::S_DOCUMENT Row;
			if (!map_document(m_Database, Stmt.Handle(), &Row)) { return(E_REPO_RESULT::Invalid); }
			_pOut->push_back(std::move(Row));
		}
		if (nStep != SQLITE_DONE) { return(this->fail_()); }
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::SearchDocuments(const std::string& _sQuery, std::vector<domain::S_DOCUMENT>* _pOut)
	{
		_pOut->clear();
		const std::string sNormalized = python_strip(_sQuery);
		if (sNormalized.empty()) { return(E_REPO_RESULT::Ok); }
		const std::string sPattern = like_pattern(sNormalized);

		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            SELECT DISTINCT documents.*
            FROM documents
            WHERE documents.trashed_at_us IS NULL
              AND (
                  documents.title LIKE ? ESCAPE '/'
                  OR EXISTS (
                      SELECT 1
                      FROM cards
                      WHERE cards.document_id = documents.id
                        AND cards.deleted_at_us IS NULL
                        AND cards.body LIKE ? ESCAPE '/'
                  )
              )
            ORDER BY documents.updated_at_us DESC, documents.id
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, sPattern);
		Stmt.BindText(2, sPattern);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		int nStep = 0;
		while ((nStep = Stmt.Step()) == SQLITE_ROW)
		{
			domain::S_DOCUMENT Row;
			if (!map_document(m_Database, Stmt.Handle(), &Row)) { return(E_REPO_RESULT::Invalid); }
			_pOut->push_back(std::move(Row));
		}
		if (nStep != SQLITE_DONE) { return(this->fail_()); }
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::UpdateDocument(const domain::S_DOCUMENT& _Document)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            UPDATE documents
            SET title = ?, created_at_us = ?, updated_at_us = ?,
                archived_at_us = ?, trashed_at_us = ?
            WHERE id = ?
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _Document.sTitle);
		Stmt.BindInt64(2, _Document.nCreatedAtUs);
		Stmt.BindInt64(3, _Document.nUpdatedAtUs);
		Stmt.BindNullableInt64(4, _Document.nArchivedAtUs);
		Stmt.BindNullableInt64(5, _Document.nTrashedAtUs);
		Stmt.BindText(6, _Document.sId);
		return(run_done(m_Database, Stmt));
	}

	E_REPO_RESULT C_REPOSITORIES::TouchDocument(const std::string& _sDocumentId, std::int64_t _nUpdatedAtUs)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            UPDATE documents
            SET updated_at_us = MAX(updated_at_us, ?)
            WHERE id = ?
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindInt64(1, _nUpdatedAtUs);
		Stmt.BindText(2, _sDocumentId);
		return(run_done(m_Database, Stmt));
	}

	E_REPO_RESULT C_REPOSITORIES::DeleteDocument(const std::string& _sDocumentId)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(DELETE FROM documents WHERE id = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sDocumentId);
		return(run_done(m_Database, Stmt));
	}

	// ------------------------------------------------------------------------------------------
	// 카드 검색과 원문 재구성 가능 여부
	// ------------------------------------------------------------------------------------------
	E_REPO_RESULT C_REPOSITORIES::SearchCards(
		const std::string& _sQuery,
		const std::optional<std::string>& _sDocumentId,
		std::vector<domain::S_CARD>* _pOut)
	{
		_pOut->clear();
		const std::string sNormalized = python_strip(_sQuery);
		if (sNormalized.empty()) { return(E_REPO_RESULT::Ok); }
		const std::string sPattern = like_pattern(sNormalized);

		// 문서 지정 여부로 문장 자체가 갈린다(:230~254). 한 문장으로 합치면 계획이 달라진다.
		if (!_sDocumentId.has_value())
		{
			C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
                SELECT cards.*
                FROM cards
                JOIN documents ON documents.id = cards.document_id
                WHERE cards.deleted_at_us IS NULL
                  AND documents.trashed_at_us IS NULL
                  AND cards.body LIKE ? ESCAPE '/'
                ORDER BY cards.document_id, cards.position_key, cards.id
                )SQL");
			if (!Stmt.IsPrepared()) { return(this->fail_()); }
			Stmt.BindText(1, sPattern);
			if (!Stmt.BindOk()) { return(this->fail_()); }

			int nStep = 0;
			while ((nStep = Stmt.Step()) == SQLITE_ROW)
			{
				domain::S_CARD Row;
				if (!map_card(m_Database, Stmt.Handle(), &Row)) { return(E_REPO_RESULT::Invalid); }
				_pOut->push_back(std::move(Row));
			}
			if (nStep != SQLITE_DONE) { return(this->fail_()); }
			return(E_REPO_RESULT::Ok);
		}

		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
                SELECT *
                FROM cards
                WHERE document_id = ?
                  AND deleted_at_us IS NULL
                  AND body LIKE ? ESCAPE '/'
                ORDER BY position_key, id
                )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, *_sDocumentId);
		Stmt.BindText(2, sPattern);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		int nStep = 0;
		while ((nStep = Stmt.Step()) == SQLITE_ROW)
		{
			domain::S_CARD Row;
			if (!map_card(m_Database, Stmt.Handle(), &Row)) { return(E_REPO_RESULT::Invalid); }
			_pOut->push_back(std::move(Row));
		}
		if (nStep != SQLITE_DONE) { return(this->fail_()); }
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::OperationReconstructionAvailable(const std::string& _sCardId, bool* _pAvailable)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            SELECT capture_operations.original_redacted_at_us
            FROM cards
            JOIN capture_operations
              ON capture_operations.id = cards.operation_id
            WHERE cards.id = ?
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sCardId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		const int nStep = Stmt.Step();
		if (nStep == SQLITE_DONE)
		{
			// 원본은 여기서만 KeyError 를 올린다(:270). 사유 문구를 그대로 남긴다.
			m_Database.SetLastError("존재하지 않는 카드입니다: " + _sCardId);
			return(E_REPO_RESULT::NotFound);
		}
		if (nStep != SQLITE_ROW) { return(this->fail_()); }

		*_pAvailable = (::sqlite3_column_type(Stmt.Handle(), 0) == SQLITE_NULL);
		return(E_REPO_RESULT::Ok);
	}

	// ------------------------------------------------------------------------------------------
	// capture_operations
	// ------------------------------------------------------------------------------------------
	E_REPO_RESULT C_REPOSITORIES::CreateCaptureOperation(const domain::S_CAPTURE_OPERATION& _Operation)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            INSERT INTO capture_operations(
                id, document_id, source, split_policy, original_text,
                original_hash, original_redacted_at_us, created_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _Operation.sId);
		Stmt.BindText(2, _Operation.sDocumentId);
		Stmt.BindText(3, domain::ToText(_Operation.eSource));
		Stmt.BindText(4, domain::ToText(_Operation.eSplitPolicy));
		Stmt.BindNullableText(5, _Operation.sOriginalText);
		Stmt.BindNullableText(6, _Operation.sOriginalHash);
		Stmt.BindNullableInt64(7, _Operation.nOriginalRedactedAtUs);
		Stmt.BindInt64(8, _Operation.nCreatedAtUs);
		return(run_done(m_Database, Stmt));
	}

	E_REPO_RESULT C_REPOSITORIES::GetCaptureOperation(
		const std::string& _sOperationId, domain::S_CAPTURE_OPERATION* _pOut)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(SELECT * FROM capture_operations WHERE id = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sOperationId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		const int nStep = Stmt.Step();
		if (nStep == SQLITE_DONE) { return(E_REPO_RESULT::NotFound); }
		if (nStep != SQLITE_ROW) { return(this->fail_()); }
		return(map_capture_operation(m_Database, Stmt.Handle(), _pOut) ? E_REPO_RESULT::Ok : E_REPO_RESULT::Invalid);
	}

	E_REPO_RESULT C_REPOSITORIES::UpdateCaptureOperation(const domain::S_CAPTURE_OPERATION& _Operation)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            UPDATE capture_operations
            SET document_id = ?, source = ?, split_policy = ?,
                original_text = ?, original_hash = ?,
                original_redacted_at_us = ?, created_at_us = ?
            WHERE id = ?
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _Operation.sDocumentId);
		Stmt.BindText(2, domain::ToText(_Operation.eSource));
		Stmt.BindText(3, domain::ToText(_Operation.eSplitPolicy));
		Stmt.BindNullableText(4, _Operation.sOriginalText);
		Stmt.BindNullableText(5, _Operation.sOriginalHash);
		Stmt.BindNullableInt64(6, _Operation.nOriginalRedactedAtUs);
		Stmt.BindInt64(7, _Operation.nCreatedAtUs);
		Stmt.BindText(8, _Operation.sId);
		return(run_done(m_Database, Stmt));
	}

	E_REPO_RESULT C_REPOSITORIES::DeleteCaptureOperation(const std::string& _sOperationId)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(DELETE FROM capture_operations WHERE id = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sOperationId);
		return(run_done(m_Database, Stmt));
	}

	// ------------------------------------------------------------------------------------------
	// cards
	// ------------------------------------------------------------------------------------------
	E_REPO_RESULT C_REPOSITORIES::CreateCard(const domain::S_CARD& _Card)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            INSERT INTO cards(
                id, document_id, operation_id, position_key, capture_seq,
                created_at_us, updated_at_us, source, body, body_hash,
                current_revision_id, deleted_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _Card.sId);
		Stmt.BindText(2, _Card.sDocumentId);
		Stmt.BindText(3, _Card.sOperationId);
		Stmt.BindInt64(4, _Card.nPositionKey);
		Stmt.BindInt64(5, _Card.nCaptureSeq);
		Stmt.BindInt64(6, _Card.nCreatedAtUs);
		Stmt.BindInt64(7, _Card.nUpdatedAtUs);
		Stmt.BindText(8, domain::ToText(_Card.eSource));
		Stmt.BindText(9, _Card.sBody);
		Stmt.BindText(10, _Card.sBodyHash);
		Stmt.BindNullableText(11, _Card.sCurrentRevisionId);
		Stmt.BindNullableInt64(12, _Card.nDeletedAtUs);
		return(run_done(m_Database, Stmt));
	}

	E_REPO_RESULT C_REPOSITORIES::GetCard(const std::string& _sCardId, domain::S_CARD* _pOut)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(SELECT * FROM cards WHERE id = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sCardId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		const int nStep = Stmt.Step();
		if (nStep == SQLITE_DONE) { return(E_REPO_RESULT::NotFound); }
		if (nStep != SQLITE_ROW) { return(this->fail_()); }
		return(map_card(m_Database, Stmt.Handle(), _pOut) ? E_REPO_RESULT::Ok : E_REPO_RESULT::Invalid);
	}

	E_REPO_RESULT C_REPOSITORIES::ListCards(const std::string& _sDocumentId, std::vector<domain::S_CARD>* _pOut)
	{
		_pOut->clear();
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            SELECT *
            FROM cards
            WHERE document_id = ?
            ORDER BY position_key, id
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sDocumentId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		int nStep = 0;
		while ((nStep = Stmt.Step()) == SQLITE_ROW)
		{
			domain::S_CARD Row;
			if (!map_card(m_Database, Stmt.Handle(), &Row)) { return(E_REPO_RESULT::Invalid); }
			_pOut->push_back(std::move(Row));
		}
		if (nStep != SQLITE_DONE) { return(this->fail_()); }
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::LinkInitialRevision(const std::string& _sCardId, const std::string& _sRevisionId)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            UPDATE cards
            SET current_revision_id = ?
            WHERE id = ? AND current_revision_id IS NULL
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sRevisionId);
		Stmt.BindText(2, _sCardId);
		if (!Stmt.BindOk()) { return(this->fail_()); }
		if (Stmt.Step() != SQLITE_DONE) { return(this->fail_()); }

		return(this->require_card_cas_(::sqlite3_changes(m_Database.Handle()), _sCardId));
	}

	E_REPO_RESULT C_REPOSITORIES::AdvanceCardRevision(
		const domain::S_CARD& _Card, const std::string& _sExpectedRevisionId)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            UPDATE cards
            SET updated_at_us = ?, source = ?, body = ?, body_hash = ?,
                current_revision_id = ?
            WHERE id = ? AND current_revision_id = ?
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindInt64(1, _Card.nUpdatedAtUs);
		Stmt.BindText(2, domain::ToText(_Card.eSource));
		Stmt.BindText(3, _Card.sBody);
		Stmt.BindText(4, _Card.sBodyHash);
		Stmt.BindNullableText(5, _Card.sCurrentRevisionId);
		Stmt.BindText(6, _Card.sId);
		Stmt.BindText(7, _sExpectedRevisionId);
		if (!Stmt.BindOk()) { return(this->fail_()); }
		if (Stmt.Step() != SQLITE_DONE) { return(this->fail_()); }

		return(this->require_card_cas_(::sqlite3_changes(m_Database.Handle()), _Card.sId));
	}

	E_REPO_RESULT C_REPOSITORIES::UpdateCardPosition(
		const std::string& _sCardId, std::int64_t _nPositionKey, const std::string& _sExpectedRevisionId)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            UPDATE cards
            SET position_key = ?
            WHERE id = ? AND current_revision_id = ?
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindInt64(1, _nPositionKey);
		Stmt.BindText(2, _sCardId);
		Stmt.BindText(3, _sExpectedRevisionId);
		if (!Stmt.BindOk()) { return(this->fail_()); }
		if (Stmt.Step() != SQLITE_DONE) { return(this->fail_()); }

		return(this->require_card_cas_(::sqlite3_changes(m_Database.Handle()), _sCardId));
	}

	E_REPO_RESULT C_REPOSITORIES::UpdateCardDeletedState(
		const std::string& _sCardId,
		std::int64_t _nPositionKey,
		const std::optional<std::int64_t>& _nDeletedAtUs,
		const std::string& _sExpectedRevisionId)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            UPDATE cards
            SET position_key = ?, deleted_at_us = ?
            WHERE id = ? AND current_revision_id = ?
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindInt64(1, _nPositionKey);
		Stmt.BindNullableInt64(2, _nDeletedAtUs);
		Stmt.BindText(3, _sCardId);
		Stmt.BindText(4, _sExpectedRevisionId);
		if (!Stmt.BindOk()) { return(this->fail_()); }
		if (Stmt.Step() != SQLITE_DONE) { return(this->fail_()); }

		return(this->require_card_cas_(::sqlite3_changes(m_Database.Handle()), _sCardId));
	}

	E_REPO_RESULT C_REPOSITORIES::DeleteCard(const std::string& _sCardId)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(DELETE FROM cards WHERE id = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sCardId);
		return(run_done(m_Database, Stmt));
	}

	// ------------------------------------------------------------------------------------------
	// card_revisions
	// ------------------------------------------------------------------------------------------
	E_REPO_RESULT C_REPOSITORIES::CreateRevision(const domain::S_CARD_REVISION& _Revision)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            INSERT INTO card_revisions(
                id, card_id, event_seq, parent_revision_id,
                body, body_hash, source, created_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _Revision.sId);
		Stmt.BindText(2, _Revision.sCardId);
		Stmt.BindInt64(3, _Revision.nEventSeq);
		Stmt.BindNullableText(4, _Revision.sParentRevisionId);
		Stmt.BindText(5, _Revision.sBody);
		Stmt.BindText(6, _Revision.sBodyHash);
		Stmt.BindText(7, domain::ToText(_Revision.eSource));
		Stmt.BindInt64(8, _Revision.nCreatedAtUs);
		return(run_done(m_Database, Stmt));
	}

	E_REPO_RESULT C_REPOSITORIES::GetRevision(const std::string& _sRevisionId, domain::S_CARD_REVISION* _pOut)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(SELECT * FROM card_revisions WHERE id = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sRevisionId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		const int nStep = Stmt.Step();
		if (nStep == SQLITE_DONE) { return(E_REPO_RESULT::NotFound); }
		if (nStep != SQLITE_ROW) { return(this->fail_()); }
		return(map_revision(m_Database, Stmt.Handle(), _pOut) ? E_REPO_RESULT::Ok : E_REPO_RESULT::Invalid);
	}

	E_REPO_RESULT C_REPOSITORIES::ListRevisions(const std::string& _sCardId, std::vector<domain::S_CARD_REVISION>* _pOut)
	{
		_pOut->clear();
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            SELECT *
            FROM card_revisions
            WHERE card_id = ?
            ORDER BY event_seq, id
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sCardId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		int nStep = 0;
		while ((nStep = Stmt.Step()) == SQLITE_ROW)
		{
			domain::S_CARD_REVISION Row;
			if (!map_revision(m_Database, Stmt.Handle(), &Row)) { return(E_REPO_RESULT::Invalid); }
			_pOut->push_back(std::move(Row));
		}
		if (nStep != SQLITE_DONE) { return(this->fail_()); }
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::DeleteRevisionForPurge(const std::string& _sRevisionId)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(DELETE FROM card_revisions WHERE id = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sRevisionId);
		return(run_done(m_Database, Stmt));
	}

	// ------------------------------------------------------------------------------------------
	// drafts
	// ------------------------------------------------------------------------------------------
	E_REPO_RESULT C_REPOSITORIES::CreateDraft(const domain::S_DRAFT& _Draft)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            INSERT INTO drafts(
                id, document_id, card_id, draft_kind, base_revision_id,
                draft_text, draft_hash, cursor_position_qchar, updated_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _Draft.sId);
		Stmt.BindText(2, _Draft.sDocumentId);
		Stmt.BindNullableText(3, _Draft.sCardId);
		Stmt.BindText(4, domain::ToText(_Draft.eDraftKind));
		Stmt.BindNullableText(5, _Draft.sBaseRevisionId);
		Stmt.BindText(6, _Draft.sDraftText);
		Stmt.BindText(7, _Draft.sDraftHash);
		Stmt.BindInt64(8, _Draft.nCursorPositionQchar);
		Stmt.BindInt64(9, _Draft.nUpdatedAtUs);
		return(run_done(m_Database, Stmt));
	}

	E_REPO_RESULT C_REPOSITORIES::GetDraft(const std::string& _sDraftId, domain::S_DRAFT* _pOut)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(SELECT * FROM drafts WHERE id = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sDraftId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		const int nStep = Stmt.Step();
		if (nStep == SQLITE_DONE) { return(E_REPO_RESULT::NotFound); }
		if (nStep != SQLITE_ROW) { return(this->fail_()); }
		return(map_draft(m_Database, Stmt.Handle(), _pOut) ? E_REPO_RESULT::Ok : E_REPO_RESULT::Invalid);
	}

	E_REPO_RESULT C_REPOSITORIES::ListDrafts(const std::string& _sDocumentId, std::vector<domain::S_DRAFT>* _pOut)
	{
		_pOut->clear();
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            SELECT *
            FROM drafts
            WHERE document_id = ?
            ORDER BY updated_at_us, id
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sDocumentId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		int nStep = 0;
		while ((nStep = Stmt.Step()) == SQLITE_ROW)
		{
			domain::S_DRAFT Row;
			if (!map_draft(m_Database, Stmt.Handle(), &Row)) { return(E_REPO_RESULT::Invalid); }
			_pOut->push_back(std::move(Row));
		}
		if (nStep != SQLITE_DONE) { return(this->fail_()); }
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::UpdateDraft(const domain::S_DRAFT& _Draft)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            UPDATE drafts
            SET document_id = ?, card_id = ?, draft_kind = ?,
                base_revision_id = ?, draft_text = ?, draft_hash = ?,
                cursor_position_qchar = ?, updated_at_us = ?
            WHERE id = ?
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _Draft.sDocumentId);
		Stmt.BindNullableText(2, _Draft.sCardId);
		Stmt.BindText(3, domain::ToText(_Draft.eDraftKind));
		Stmt.BindNullableText(4, _Draft.sBaseRevisionId);
		Stmt.BindText(5, _Draft.sDraftText);
		Stmt.BindText(6, _Draft.sDraftHash);
		Stmt.BindInt64(7, _Draft.nCursorPositionQchar);
		Stmt.BindInt64(8, _Draft.nUpdatedAtUs);
		Stmt.BindText(9, _Draft.sId);
		return(run_done(m_Database, Stmt));
	}

	E_REPO_RESULT C_REPOSITORIES::DeleteDraft(const std::string& _sDraftId)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(DELETE FROM drafts WHERE id = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sDraftId);
		return(run_done(m_Database, Stmt));
	}

	// ------------------------------------------------------------------------------------------
	// edit_events
	// ------------------------------------------------------------------------------------------
	E_REPO_RESULT C_REPOSITORIES::CreateEvent(const domain::S_EDIT_EVENT& _Event, domain::S_EDIT_EVENT* _pOut)
	{
		if (_Event.nEventSeq.has_value())
		{
			m_Database.SetLastError("새 이벤트의 event_seq는 SQLite가 발급해야 합니다.");
			return(E_REPO_RESULT::Invalid);
		}

		{
			C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            INSERT INTO edit_events(
                event_id, operation_id, document_id, card_id,
                event_type, source, occurred_at_us, details_json
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            )SQL");
			if (!Stmt.IsPrepared()) { return(this->fail_()); }
			Stmt.BindText(1, _Event.sEventId);
			Stmt.BindNullableText(2, _Event.sOperationId);
			Stmt.BindText(3, _Event.sDocumentId);
			Stmt.BindNullableText(4, _Event.sCardId);
			Stmt.BindText(5, domain::ToText(_Event.eEventType));
			Stmt.BindText(6, domain::ToText(_Event.eSource));
			Stmt.BindInt64(7, _Event.nOccurredAtUs);
			Stmt.BindText(8, _Event.sDetailsJson);

			const E_REPO_RESULT eResult = run_done(m_Database, Stmt);
			if (eResult != E_REPO_RESULT::Ok) { return(eResult); }
		}

		// 원본은 cursor.lastrowid 가 None 이면 RuntimeError 를 올리지만(:617~618),
		// sqlite3_last_insert_rowid 는 값을 안 돌려주는 경우가 없어 그 분기에 대응물이 없다.
		*_pOut = _Event;
		_pOut->nEventSeq = ::sqlite3_last_insert_rowid(m_Database.Handle());
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::GetEvent(std::int64_t _nEventSeq, domain::S_EDIT_EVENT* _pOut)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(SELECT * FROM edit_events WHERE event_seq = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindInt64(1, _nEventSeq);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		const int nStep = Stmt.Step();
		if (nStep == SQLITE_DONE) { return(E_REPO_RESULT::NotFound); }
		if (nStep != SQLITE_ROW) { return(this->fail_()); }
		return(map_event(m_Database, Stmt.Handle(), _pOut) ? E_REPO_RESULT::Ok : E_REPO_RESULT::Invalid);
	}

	E_REPO_RESULT C_REPOSITORIES::ListEvents(const std::string& _sDocumentId, std::vector<domain::S_EDIT_EVENT>* _pOut)
	{
		_pOut->clear();
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            SELECT *
            FROM edit_events
            WHERE document_id = ?
            ORDER BY event_seq
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sDocumentId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		int nStep = 0;
		while ((nStep = Stmt.Step()) == SQLITE_ROW)
		{
			domain::S_EDIT_EVENT Row;
			if (!map_event(m_Database, Stmt.Handle(), &Row)) { return(E_REPO_RESULT::Invalid); }
			_pOut->push_back(std::move(Row));
		}
		if (nStep != SQLITE_DONE) { return(this->fail_()); }
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::DeleteEventForPurge(std::int64_t _nEventSeq)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(DELETE FROM edit_events WHERE event_seq = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindInt64(1, _nEventSeq);
		return(run_done(m_Database, Stmt));
	}

	// ------------------------------------------------------------------------------------------
	// counters
	// ------------------------------------------------------------------------------------------
	E_REPO_RESULT C_REPOSITORIES::GetCounter(const std::string& _sName, std::int64_t* _pValue)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(SELECT next_value FROM counters WHERE name = ?)SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sName);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		const int nStep = Stmt.Step();
		if (nStep == SQLITE_DONE) { return(E_REPO_RESULT::NotFound); }
		if (nStep != SQLITE_ROW) { return(this->fail_()); }

		*_pValue = ::sqlite3_column_int64(Stmt.Handle(), 0);
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::issue_capture_sequence_(std::int64_t* _pValue)
	{
		// 원본이 같은 자리에서 in_transaction 을 먼저 본다(:665~666).
		if (!m_Database.IsOpen() || ::sqlite3_get_autocommit(m_Database.Handle()) != 0)
		{
			m_Database.SetLastError("capture 순번은 카드 생성 트랜잭션 안에서만 발급합니다.");
			return(E_REPO_RESULT::Failed);
		}

		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            UPDATE counters
            SET next_value = next_value + 1
            WHERE name = 'capture'
            RETURNING next_value - 1
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }

		const int nStep = Stmt.Step();
		if (nStep == SQLITE_DONE)
		{
			m_Database.SetLastError("capture counter가 존재하지 않습니다.");
			return(E_REPO_RESULT::NotFound);
		}
		if (nStep != SQLITE_ROW) { return(this->fail_()); }

		*_pValue = ::sqlite3_column_int64(Stmt.Handle(), 0);
		return(E_REPO_RESULT::Ok);
	}

	// ------------------------------------------------------------------------------------------
	// card_lineage
	// ------------------------------------------------------------------------------------------
	E_REPO_RESULT C_REPOSITORIES::CreateLineage(const domain::S_CARD_LINEAGE& _Lineage)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            INSERT INTO card_lineage(
                parent_card_id, child_card_id, event_seq, relation_type
            )
            VALUES (?, ?, ?, ?)
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _Lineage.sParentCardId);
		Stmt.BindText(2, _Lineage.sChildCardId);
		Stmt.BindInt64(3, _Lineage.nEventSeq);
		Stmt.BindText(4, domain::ToText(_Lineage.eRelationType));
		return(run_done(m_Database, Stmt));
	}

	E_REPO_RESULT C_REPOSITORIES::ListLineageForCard(
		const std::string& _sCardId, std::vector<domain::S_CARD_LINEAGE>* _pOut)
	{
		_pOut->clear();
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            SELECT *
            FROM card_lineage
            WHERE parent_card_id = ? OR child_card_id = ?
            ORDER BY event_seq, parent_card_id, child_card_id
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _sCardId);
		Stmt.BindText(2, _sCardId);
		if (!Stmt.BindOk()) { return(this->fail_()); }

		int nStep = 0;
		while ((nStep = Stmt.Step()) == SQLITE_ROW)
		{
			domain::S_CARD_LINEAGE Row;
			if (!map_lineage(m_Database, Stmt.Handle(), &Row)) { return(E_REPO_RESULT::Invalid); }
			_pOut->push_back(std::move(Row));
		}
		if (nStep != SQLITE_DONE) { return(this->fail_()); }
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::DeleteLineage(const domain::S_CARD_LINEAGE& _Lineage)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            DELETE FROM card_lineage
            WHERE parent_card_id = ? AND child_card_id = ? AND event_seq = ?
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _Lineage.sParentCardId);
		Stmt.BindText(2, _Lineage.sChildCardId);
		Stmt.BindInt64(3, _Lineage.nEventSeq);
		return(run_done(m_Database, Stmt));
	}

	// ------------------------------------------------------------------------------------------
	// 원자적 카드 생성
	// ------------------------------------------------------------------------------------------
	E_REPO_RESULT C_REPOSITORIES::validate_create_cards_(
		const domain::S_NEW_CAPTURE_OPERATION& _Operation,
		const std::vector<domain::S_NEW_CARD>& _Cards)
	{
		if (_Cards.empty())
		{
			m_Database.SetLastError("새 카드 저장에는 카드가 한 개 이상 필요합니다.");
			return(E_REPO_RESULT::Invalid);
		}

		const bool bExistingCardTransform =
			(_Operation.eSource == domain::E_CAPTURE_OPERATION_SOURCE::Split
				|| _Operation.eSource == domain::E_CAPTURE_OPERATION_SOURCE::Merge);

		if (bExistingCardTransform && _Operation.sOriginalText.has_value())
		{
			m_Database.SetLastError("기존 카드 분할·병합 작업은 원문을 중복 저장하지 않습니다.");
			return(E_REPO_RESULT::Invalid);
		}
		if (_Operation.eSplitPolicy == domain::E_SPLIT_POLICY::Keep && _Operation.sOriginalText.has_value())
		{
			m_Database.SetLastError("keep 작업은 카드 본문과 같은 원문을 중복 저장하지 않습니다.");
			return(E_REPO_RESULT::Invalid);
		}
		if (!bExistingCardTransform
			&& _Operation.eSplitPolicy == domain::E_SPLIT_POLICY::SplitByBlankLine
			&& !_Operation.sOriginalText.has_value())
		{
			m_Database.SetLastError("새 입력 분할 작업은 정확한 원문이 필요합니다.");
			return(E_REPO_RESULT::Invalid);
		}
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::CreateCards(
		const domain::S_NEW_CAPTURE_OPERATION& _Operation,
		const std::vector<domain::S_NEW_CARD>& _Cards,
		std::vector<domain::S_CARD>* _pOut)
	{
		_pOut->clear();
		const E_REPO_RESULT eValidation = this->validate_create_cards_(_Operation, _Cards);
		if (eValidation != E_REPO_RESULT::Ok) { return(eValidation); }

		C_TRANSACTION Transaction(m_Database);
		if (!Transaction.IsActive()) { return(this->begin_failed_()); }

		std::vector<domain::S_CARD> CreatedCards;
		const E_REPO_RESULT eResult = this->CreateCardsInActiveTransaction(_Operation, _Cards, &CreatedCards);
		if (eResult != E_REPO_RESULT::Ok) { return(eResult); }
		if (!Transaction.Commit()) { return(E_REPO_RESULT::Failed); }

		*_pOut = std::move(CreatedCards);
		return(E_REPO_RESULT::Ok);
	}

	E_REPO_RESULT C_REPOSITORIES::CreateCardsInActiveTransaction(
		const domain::S_NEW_CAPTURE_OPERATION& _Operation,
		const std::vector<domain::S_NEW_CARD>& _Cards,
		std::vector<domain::S_CARD>* _pOut)
	{
		_pOut->clear();
		if (!m_Database.IsOpen() || ::sqlite3_get_autocommit(m_Database.Handle()) != 0)
		{
			m_Database.SetLastError("카드 생성 seam은 활성 트랜잭션이 필요합니다.");
			return(E_REPO_RESULT::Failed);
		}
		const E_REPO_RESULT eValidation = this->validate_create_cards_(_Operation, _Cards);
		if (eValidation != E_REPO_RESULT::Ok) { return(eValidation); }

		domain::S_CAPTURE_OPERATION StoredOperation;
		StoredOperation.sId          = _Operation.sId;
		StoredOperation.sDocumentId  = _Operation.sDocumentId;
		StoredOperation.eSource      = _Operation.eSource;
		StoredOperation.eSplitPolicy = _Operation.eSplitPolicy;
		StoredOperation.sOriginalText = _Operation.sOriginalText;
		StoredOperation.sOriginalHash =
			_Operation.sOriginalText.has_value()
				? std::optional<std::string>(TextHash(*_Operation.sOriginalText))
				: std::nullopt;
		StoredOperation.nOriginalRedactedAtUs = std::nullopt;
		StoredOperation.nCreatedAtUs          = _Operation.nCreatedAtUs;

		std::vector<domain::S_CARD> CreatedCards;

		E_REPO_RESULT eResult = this->CreateCaptureOperation(StoredOperation);
		if (eResult != E_REPO_RESULT::Ok) { return(eResult); }

		for (const domain::S_NEW_CARD& NewCard : _Cards)
		{
			domain::S_EDIT_EVENT NewEvent;
			NewEvent.nEventSeq     = std::nullopt;
			NewEvent.sEventId      = NewCard.sEventId;
			NewEvent.sOperationId  = _Operation.sId;
			NewEvent.sDocumentId   = _Operation.sDocumentId;
			NewEvent.sCardId       = NewCard.sId;
			NewEvent.eEventType    = domain::E_EVENT_TYPE::Create;
			NewEvent.eSource       = NewCard.eEventSource;
			NewEvent.nOccurredAtUs = NewCard.nCreatedAtUs;
			NewEvent.sDetailsJson  = NewCard.sEventDetailsJson;

			domain::S_EDIT_EVENT StoredEvent;
			eResult = this->CreateEvent(NewEvent, &StoredEvent);
			if (eResult != E_REPO_RESULT::Ok) { return(eResult); }

			std::int64_t nCaptureSeq = 0;
			eResult = this->issue_capture_sequence_(&nCaptureSeq);
			if (eResult != E_REPO_RESULT::Ok) { return(eResult); }

			const std::string sBodyHash = TextHash(NewCard.sBody);

			domain::S_CARD Card;
			Card.sId                = NewCard.sId;
			Card.sDocumentId        = _Operation.sDocumentId;
			Card.sOperationId       = _Operation.sId;
			Card.nPositionKey       = NewCard.nPositionKey;
			Card.nCaptureSeq        = nCaptureSeq;
			Card.nCreatedAtUs       = NewCard.nCreatedAtUs;
			Card.nUpdatedAtUs       = NewCard.nCreatedAtUs;
			Card.eSource            = NewCard.eCardSource;
			Card.sBody              = NewCard.sBody;
			Card.sBodyHash          = sBodyHash;
			Card.sCurrentRevisionId = std::nullopt;

			eResult = this->CreateCard(Card);
			if (eResult != E_REPO_RESULT::Ok) { return(eResult); }

			domain::S_CARD_REVISION Revision;
			Revision.sId               = NewCard.sRevisionId;
			Revision.sCardId           = NewCard.sId;
			Revision.nEventSeq         = *StoredEvent.nEventSeq;
			Revision.sParentRevisionId = std::nullopt;
			Revision.sBody             = NewCard.sBody;
			Revision.sBodyHash         = sBodyHash;
			Revision.eSource           = NewCard.eRevisionSource;
			Revision.nCreatedAtUs      = NewCard.nCreatedAtUs;

			eResult = this->CreateRevision(Revision);
			if (eResult != E_REPO_RESULT::Ok) { return(eResult); }

			eResult = this->LinkInitialRevision(NewCard.sId, NewCard.sRevisionId);
			if (eResult != E_REPO_RESULT::Ok) { return(eResult); }

			// 돌려주는 카드는 방금 연결한 리비전 id 를 달고 있다. 트랜잭션 안에서 만든 값에는 없던 값이다.
			domain::S_CARD LinkedCard = Card;
			LinkedCard.sCurrentRevisionId = NewCard.sRevisionId;
			CreatedCards.push_back(std::move(LinkedCard));
		}

		*_pOut = std::move(CreatedCards);
		return(E_REPO_RESULT::Ok);
	}
}
