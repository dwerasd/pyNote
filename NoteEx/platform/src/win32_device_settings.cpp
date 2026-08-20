#include "pynote/platform/win32_device_settings.h"

#include "pynote/core/storage/file_system.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <string_view>
#include <utility>

#pragma comment(lib, "Advapi32.lib")

namespace
{
	using pynote::core::storage::I_FILE_SYSTEM;

	enum class E_VALUE_TYPE
	{
		String,
		Bool,
		Integer,
		Double,
		Bytes,
	};

	struct S_VALUE
	{
		E_VALUE_TYPE eType{ E_VALUE_TYPE::String };
		std::wstring sString;
		bool bBool{ false };
		std::int64_t nInteger{ 0 };
		double dDouble{ 0.0 };
		std::vector<std::uint8_t> Bytes;
	};

	using VALUE_MAP = std::map<std::string, S_VALUE>;

	std::string narrow(const std::wstring_view _sText)
	{
		if (_sText.empty()) { return(std::string{}); }
		const int nSize = ::WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, _sText.data(), static_cast<int>(_sText.size()),
			nullptr, 0, nullptr, nullptr);
		if (nSize <= 0) { return(std::string{}); }

		std::string sResult(static_cast<std::size_t>(nSize), '\0');
		if (::WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, _sText.data(), static_cast<int>(_sText.size()),
			sResult.data(), nSize, nullptr, nullptr) != nSize)
		{
			return(std::string{});
		}
		return(sResult);
	}

	bool widen(const std::string_view _sText, std::wstring* _psResult)
	{
		_psResult->clear();
		if (_sText.empty()) { return(true); }
		const int nSize = ::MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, _sText.data(), static_cast<int>(_sText.size()), nullptr, 0);
		if (nSize <= 0) { return(false); }

		_psResult->resize(static_cast<std::size_t>(nSize));
		return(::MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, _sText.data(), static_cast<int>(_sText.size()),
			_psResult->data(), nSize) == nSize);
	}

	std::string lower_ascii(std::string _sText)
	{
		for (char& ch : _sText)
		{
			if (ch >= 'A' && ch <= 'Z') { ch = static_cast<char>(ch - 'A' + 'a'); }
		}
		return(_sText);
	}

	std::string hex_encode(const std::uint8_t* _pData, std::size_t _nSize)
	{
		static constexpr char DIGITS[] = "0123456789abcdef";
		std::string sResult;
		sResult.reserve(_nSize * 2);
		for (std::size_t i = 0; i < _nSize; ++i)
		{
			sResult.push_back(DIGITS[_pData[i] >> 4]);
			sResult.push_back(DIGITS[_pData[i] & 0x0f]);
		}
		return(sResult);
	}

	bool hex_decode(const std::string_view _sText, std::vector<std::uint8_t>* _pData)
	{
		_pData->clear();
		if ((_sText.size() % 2) != 0) { return(false); }
		auto digit = [](char _ch) -> int
		{
			if (_ch >= '0' && _ch <= '9') { return(_ch - '0'); }
			if (_ch >= 'a' && _ch <= 'f') { return(_ch - 'a' + 10); }
			return(-1);
		};
		_pData->reserve(_sText.size() / 2);
		for (std::size_t i = 0; i < _sText.size(); i += 2)
		{
			const int nHigh = digit(_sText[i]);
			const int nLow  = digit(_sText[i + 1]);
			if (nHigh < 0 || nLow < 0) { _pData->clear(); return(false); }
			_pData->push_back(static_cast<std::uint8_t>((nHigh << 4) | nLow));
		}
		return(true);
	}

	S_VALUE string_value(std::wstring _sValue)
	{
		S_VALUE Value;
		Value.eType = E_VALUE_TYPE::String;
		Value.sString = std::move(_sValue);
		return(Value);
	}

	S_VALUE bool_value(bool _bValue)
	{
		S_VALUE Value;
		Value.eType = E_VALUE_TYPE::Bool;
		Value.bBool = _bValue;
		return(Value);
	}

	S_VALUE integer_value(std::int64_t _nValue)
	{
		S_VALUE Value;
		Value.eType = E_VALUE_TYPE::Integer;
		Value.nInteger = _nValue;
		return(Value);
	}

	S_VALUE double_value(double _dValue)
	{
		S_VALUE Value;
		Value.eType = E_VALUE_TYPE::Double;
		Value.dDouble = _dValue;
		return(Value);
	}

	S_VALUE bytes_value(std::vector<std::uint8_t> _Bytes)
	{
		S_VALUE Value;
		Value.eType = E_VALUE_TYPE::Bytes;
		Value.Bytes = std::move(_Bytes);
		return(Value);
	}

	std::string encode_value(const S_VALUE& _Value)
	{
		switch (_Value.eType)
		{
		case E_VALUE_TYPE::String:
		{
			const std::string sUtf8 = narrow(_Value.sString);
			return("s:" + hex_encode(
				reinterpret_cast<const std::uint8_t*>(sUtf8.data()), sUtf8.size()));
		}
		case E_VALUE_TYPE::Bool:
			return(_Value.bBool ? "b:1" : "b:0");
		case E_VALUE_TYPE::Integer:
			return("i:" + std::to_string(_Value.nInteger));
		case E_VALUE_TYPE::Double:
		{
			char Buffer[64] = {};
			const auto Result = std::to_chars(
				Buffer, Buffer + sizeof(Buffer), _Value.dDouble,
				std::chars_format::general, std::numeric_limits<double>::max_digits10);
			return(Result.ec == std::errc{} ? "d:" + std::string(Buffer, Result.ptr) : std::string{});
		}
		case E_VALUE_TYPE::Bytes:
			return("x:" + hex_encode(_Value.Bytes.data(), _Value.Bytes.size()));
		}
		return(std::string{});
	}

	bool decode_value(const std::string_view _sText, S_VALUE* _pValue)
	{
		if (_sText.size() < 2 || _sText[1] != ':') { return(false); }
		const std::string_view sPayload = _sText.substr(2);
		switch (_sText[0])
		{
		case 's':
		{
			std::vector<std::uint8_t> Bytes;
			if (!hex_decode(sPayload, &Bytes)) { return(false); }
			std::wstring sValue;
			const std::string sUtf8 = Bytes.empty()
				? std::string{}
				: std::string(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
			if (!widen(sUtf8, &sValue)) { return(false); }
			*_pValue = string_value(std::move(sValue));
			return(true);
		}
		case 'b':
			if (sPayload == "0") { *_pValue = bool_value(false); return(true); }
			if (sPayload == "1") { *_pValue = bool_value(true); return(true); }
			return(false);
		case 'i':
		{
			std::int64_t nValue = 0;
			const auto Result = std::from_chars(sPayload.data(), sPayload.data() + sPayload.size(), nValue);
			if (Result.ec != std::errc{} || Result.ptr != sPayload.data() + sPayload.size()) { return(false); }
			*_pValue = integer_value(nValue);
			return(true);
		}
		case 'd':
		{
			double dValue = 0.0;
			const auto Result = std::from_chars(
				sPayload.data(), sPayload.data() + sPayload.size(), dValue, std::chars_format::general);
			if (Result.ec != std::errc{} || Result.ptr != sPayload.data() + sPayload.size() || !std::isfinite(dValue))
			{
				return(false);
			}
			*_pValue = double_value(dValue);
			return(true);
		}
		case 'x':
		{
			std::vector<std::uint8_t> Bytes;
			if (!hex_decode(sPayload, &Bytes)) { return(false); }
			*_pValue = bytes_value(std::move(Bytes));
			return(true);
		}
		default:
			return(false);
		}
	}

	bool is_dynamic_geometry_key(const std::string& _sLower)
	{
		constexpr std::size_t PREFIX_LENGTH = std::string_view("windows/").size();
		constexpr std::size_t SUFFIX_LENGTH = std::string_view("/geometry").size();
		if (_sLower.size() <= PREFIX_LENGTH + SUFFIX_LENGTH ||
			!_sLower.starts_with("windows/") || !_sLower.ends_with("/geometry"))
		{
			return(false);
		}
		const std::string_view sMiddle(
			_sLower.data() + PREFIX_LENGTH, _sLower.size() - PREFIX_LENGTH - SUFFIX_LENGTH);
		return(!sMiddle.empty() && sMiddle.find('/') == std::string_view::npos);
	}

	bool is_valid_device_type(const std::string& _sKey, E_VALUE_TYPE _eType)
	{
		const std::string sLower = lower_ascii(_sKey);
		if (sLower == "backup/location" || sLower == "display/time_format" ||
			sLower == "display/timezone" || sLower == "editor/font_family" ||
			sLower == "shortcuts/merge" || sLower == "shortcuts/split")
		{
			return(_eType == E_VALUE_TYPE::String);
		}
		if (sLower == "cards/multi_selection_enabled" || sLower == "first_run/guide_shown" ||
			sLower == "composer/immediate_paste_capture")
		{
			return(_eType == E_VALUE_TYPE::Bool);
		}
		if (sLower == "editor/font_size" || sLower == "location/x" || sLower == "location/y" ||
			sLower == "location/w" || sLower == "location/h")
		{
			return(_eType == E_VALUE_TYPE::Integer);
		}
		if (sLower == "editor/line_spacing") { return(_eType == E_VALUE_TYPE::Double); }
		if (sLower == "window/geometry" || is_dynamic_geometry_key(sLower))
		{
			return(_eType == E_VALUE_TYPE::Bytes);
		}
		return(false);
	}

	bool read_file(const std::string& _sPath, std::string* _psBytes, std::string* _psError)
	{
		std::wstring sPath;
		if (!widen(_sPath, &sPath)) { *_psError = "INI path UTF-8 변환 실패"; return(false); }
		const HANDLE hFile = ::CreateFileW(
			sPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			*_psError = "INI 열기 실패(Win32 오류 " + std::to_string(::GetLastError()) + "): " + _sPath;
			return(false);
		}

		LARGE_INTEGER Size = {};
		if (::GetFileSizeEx(hFile, &Size) == FALSE || Size.QuadPart < 0 ||
			static_cast<unsigned long long>(Size.QuadPart) > std::numeric_limits<std::size_t>::max())
		{
			const DWORD nCode = ::GetLastError();
			::CloseHandle(hFile);
			*_psError = "INI 크기 조회 실패(Win32 오류 " + std::to_string(nCode) + "): " + _sPath;
			return(false);
		}

		_psBytes->assign(static_cast<std::size_t>(Size.QuadPart), '\0');
		std::size_t nOffset = 0;
		while (nOffset < _psBytes->size())
		{
			const DWORD nRequest = static_cast<DWORD>(std::min<std::size_t>(
				_psBytes->size() - nOffset, std::numeric_limits<DWORD>::max()));
			DWORD nRead = 0;
			if (::ReadFile(hFile, _psBytes->data() + nOffset, nRequest, &nRead, nullptr) == FALSE || nRead == 0)
			{
				const DWORD nCode = ::GetLastError();
				::CloseHandle(hFile);
				*_psError = "INI 읽기 실패(Win32 오류 " + std::to_string(nCode) + "): " + _sPath;
				return(false);
			}
			nOffset += nRead;
		}
		::CloseHandle(hFile);
		return(true);
	}

	bool write_file(const std::string& _sPath, const std::string& _sBytes, std::string* _psError)
	{
		std::wstring sPath;
		if (!widen(_sPath, &sPath)) { *_psError = "임시 INI path UTF-8 변환 실패"; return(false); }
		const HANDLE hFile = ::CreateFileW(
			sPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			*_psError = "임시 INI 열기 실패(Win32 오류 " + std::to_string(::GetLastError()) + "): " + _sPath;
			return(false);
		}

		std::size_t nOffset = 0;
		while (nOffset < _sBytes.size())
		{
			const DWORD nRequest = static_cast<DWORD>(std::min<std::size_t>(
				_sBytes.size() - nOffset, std::numeric_limits<DWORD>::max()));
			DWORD nWritten = 0;
			if (::WriteFile(hFile, _sBytes.data() + nOffset, nRequest, &nWritten, nullptr) == FALSE || nWritten == 0)
			{
				const DWORD nCode = ::GetLastError();
				::CloseHandle(hFile);
				*_psError = "임시 INI 쓰기 실패(Win32 오류 " + std::to_string(nCode) + "): " + _sPath;
				return(false);
			}
			nOffset += nWritten;
		}
		if (::FlushFileBuffers(hFile) == FALSE)
		{
			const DWORD nCode = ::GetLastError();
			::CloseHandle(hFile);
			*_psError = "임시 INI flush 실패(Win32 오류 " + std::to_string(nCode) + "): " + _sPath;
			return(false);
		}
		::CloseHandle(hFile);
		return(true);
	}

	bool parse_ini(const std::string& _sBytes, VALUE_MAP* _pValues, bool* _pbMigrated, std::string* _psError)
	{
		_pValues->clear();
		*_pbMigrated = false;
		if (_sBytes.size() >= 3 &&
			static_cast<unsigned char>(_sBytes[0]) == 0xef &&
			static_cast<unsigned char>(_sBytes[1]) == 0xbb &&
			static_cast<unsigned char>(_sBytes[2]) == 0xbf)
		{
			*_psError = "INI UTF-8 BOM은 허용되지 않음";
			return(false);
		}

		std::string sSection;
		bool bDeviceSeen = false;
		bool bMetaSeen = false;
		bool bMarkerSeen = false;
		std::size_t nStart = 0;
		while (nStart <= _sBytes.size())
		{
			const std::size_t nEnd = _sBytes.find('\n', nStart);
			std::string_view sLine(_sBytes.data() + nStart,
				(nEnd == std::string::npos ? _sBytes.size() : nEnd) - nStart);
			if (!sLine.empty() && sLine.back() == '\r') { sLine.remove_suffix(1); }
			if (!sLine.empty())
			{
				if (sLine.front() == '[' && sLine.back() == ']')
				{
					sSection.assign(sLine.substr(1, sLine.size() - 2));
					if (sSection == "device")
					{
						if (bDeviceSeen) { *_psError = "중복 INI section: device"; return(false); }
						bDeviceSeen = true;
					}
					else if (sSection == "meta")
					{
						if (bMetaSeen) { *_psError = "중복 INI section: meta"; return(false); }
						bMetaSeen = true;
					}
					else { *_psError = "알 수 없는 INI section"; return(false); }
				}
				else
				{
					const std::size_t nEquals = sLine.find('=');
					if (nEquals == std::string_view::npos || nEquals == 0 || sSection.empty())
					{
						*_psError = "잘못된 INI line";
						return(false);
					}
					const std::string sKey(sLine.substr(0, nEquals));
					S_VALUE Value;
					if (!decode_value(sLine.substr(nEquals + 1), &Value))
					{
						*_psError = "잘못된 INI value: " + sKey;
						return(false);
					}
					if (sSection == "device")
					{
						std::wstring sValidatedKey;
						if (!widen(sKey, &sValidatedKey) || !is_valid_device_type(sKey, Value.eType))
						{
							*_psError = "잘못된 INI device key/type: " + sKey;
							return(false);
						}
						if (!_pValues->emplace(sKey, std::move(Value)).second)
						{
							*_psError = "중복 INI key: " + sKey;
							return(false);
						}
					}
					else
					{
						if (sKey != "registry_migration_v1" || Value.eType != E_VALUE_TYPE::Bool)
						{
							*_psError = "잘못된 INI meta key";
							return(false);
						}
						if (bMarkerSeen) { *_psError = "중복 migration marker"; return(false); }
						bMarkerSeen = true;
						*_pbMigrated = Value.bBool;
					}
				}
			}
			if (nEnd == std::string::npos) { break; }
			nStart = nEnd + 1;
		}
		return(true);
	}

	std::string serialize_ini(const VALUE_MAP& _Values)
	{
		std::string sResult = "[device]\n";
		for (const auto& [sKey, Value] : _Values)
		{
			sResult += sKey;
			sResult.push_back('=');
			sResult += encode_value(Value);
			sResult.push_back('\n');
		}
		sResult += "[meta]\nregistry_migration_v1=b:1\n";
		return(sResult);
	}

	bool parse_registry_string(const DWORD _nType, const std::vector<std::uint8_t>& _Data, std::wstring* _psValue)
	{
		if (_nType != REG_SZ || _Data.size() < sizeof(wchar_t) || (_Data.size() % sizeof(wchar_t)) != 0)
		{
			return(false);
		}
		const std::size_t nCharacters = _Data.size() / sizeof(wchar_t);
		std::wstring sText(nCharacters, L'\0');
		std::memcpy(sText.data(), _Data.data(), _Data.size());
		if (sText[nCharacters - 1] != L'\0') { return(false); }
		for (std::size_t i = 0; i + 1 < nCharacters; ++i)
		{
			if (sText[i] == L'\0') { return(false); }
		}
		_psValue->assign(sText.data(), nCharacters - 1);
		if (!_psValue->empty() && narrow(*_psValue).empty()) { return(false); }
		return(true);
	}

	bool parse_bool_string(const std::wstring& _sValue, bool* _pbValue)
	{
		std::wstring sLower = _sValue;
		for (wchar_t& ch : sLower)
		{
			if (ch >= L'A' && ch <= L'Z') { ch = static_cast<wchar_t>(ch - L'A' + L'a'); }
		}
		if (sLower == L"true") { *_pbValue = true; return(true); }
		if (sLower == L"false") { *_pbValue = false; return(true); }
		return(false);
	}

	bool classify_registry_value(
		const std::string& _sKey, DWORD _nType, const std::vector<std::uint8_t>& _Data,
		VALUE_MAP* _pValues, std::string* _psError)
	{
		const std::string sLower = lower_ascii(_sKey);
		if (_sKey.find_first_of("=\r\n") != std::string::npos)
		{
			*_psError = "INI로 표현할 수 없는 registry key: " + _sKey;
			return(false);
		}
		if (sLower == "draft/idle_seconds" || sLower == "cards/preview_lines" ||
			sLower == "backup/interval_hours" || sLower == "trash/retention_days")
		{
			return(true);
		}

		auto preserve = [&](const std::string& _sCanonicalKey, S_VALUE _Value)
		{
			if (_pValues->find(_sCanonicalKey) == _pValues->end())
			{
				_pValues->emplace(_sCanonicalKey, std::move(_Value));
			}
			return(true);
		};

		std::wstring sString;
		if (sLower == "backup/location" || sLower == "display/time_format" ||
			sLower == "display/timezone" || sLower == "editor/font_family" ||
			sLower == "shortcuts/merge" || sLower == "shortcuts/split")
		{
			if (!parse_registry_string(_nType, _Data, &sString)) { *_psError = "잘못된 REG_SZ: " + _sKey; return(false); }
			return(preserve(sLower, string_value(std::move(sString))));
		}

		if (sLower == "cards/multi_selection_enabled" || sLower == "first_run/guide_shown" ||
			sLower == "composer/immediate_paste_capture")
		{
			bool bValue = false;
			if (!parse_registry_string(_nType, _Data, &sString) || !parse_bool_string(sString, &bValue))
			{
				*_psError = "잘못된 bool REG_SZ: " + _sKey;
				return(false);
			}
			return(preserve(sLower, bool_value(bValue)));
		}

		if (sLower == "editor/font_size")
		{
			if (_nType != REG_DWORD || _Data.size() != sizeof(DWORD))
			{
				*_psError = "잘못된 REG_DWORD: " + _sKey;
				return(false);
			}
			DWORD nValue = 0;
			std::memcpy(&nValue, _Data.data(), sizeof(nValue));
			return(preserve(sLower, integer_value(static_cast<std::int64_t>(nValue))));
		}

		if (sLower == "editor/line_spacing")
		{
			if (!parse_registry_string(_nType, _Data, &sString)) { *_psError = "잘못된 double REG_SZ: " + _sKey; return(false); }
			const std::string sUtf8 = narrow(sString);
			double dValue = 0.0;
			const auto Result = std::from_chars(
				sUtf8.data(), sUtf8.data() + sUtf8.size(), dValue, std::chars_format::general);
			if (Result.ec != std::errc{} || Result.ptr != sUtf8.data() + sUtf8.size() || !std::isfinite(dValue))
			{
				*_psError = "잘못된 double REG_SZ: " + _sKey;
				return(false);
			}
			return(preserve(sLower, double_value(dValue)));
		}

		const bool bLegacyGeometry = (sLower == "window/geometry");
		const bool bDynamicGeometry = is_dynamic_geometry_key(sLower);
		if (bLegacyGeometry || bDynamicGeometry)
		{
			if (_nType != REG_BINARY) { *_psError = "잘못된 REG_BINARY: " + _sKey; return(false); }
			return(preserve(bLegacyGeometry ? sLower : _sKey, bytes_value(_Data)));
		}

		*_psError = "알 수 없는 registry key: " + _sKey;
		return(false);
	}

	bool enumerate_registry_key(
		HKEY _hKey, const std::string& _sPrefix, VALUE_MAP* _pValues, std::string* _psError)
	{
		DWORD nValueCount = 0;
		DWORD nMaxValueName = 0;
		DWORD nMaxValueData = 0;
		DWORD nSubkeyCount = 0;
		DWORD nMaxSubkeyName = 0;
		LSTATUS nStatus = ::RegQueryInfoKeyW(
			_hKey, nullptr, nullptr, nullptr, &nSubkeyCount, &nMaxSubkeyName, nullptr,
			&nValueCount, &nMaxValueName, &nMaxValueData, nullptr, nullptr);
		if (nStatus != ERROR_SUCCESS)
		{
			*_psError = "registry 정보 조회 실패(Win32 오류 " + std::to_string(nStatus) + ")";
			return(false);
		}

		std::vector<wchar_t> Name(static_cast<std::size_t>(nMaxValueName) + 2);
		std::vector<std::uint8_t> Data(static_cast<std::size_t>(nMaxValueData) + 2);
		for (DWORD i = 0; i < nValueCount; ++i)
		{
			DWORD nName = static_cast<DWORD>(Name.size());
			DWORD nData = static_cast<DWORD>(Data.size());
			DWORD nType = 0;
			nStatus = ::RegEnumValueW(_hKey, i, Name.data(), &nName, nullptr, &nType, Data.data(), &nData);
			if (nStatus != ERROR_SUCCESS)
			{
				*_psError = "registry value 열거 실패(Win32 오류 " + std::to_string(nStatus) + ")";
				return(false);
			}
			const std::string sName = narrow(std::wstring_view(Name.data(), nName));
			if (sName.empty()) { *_psError = "빈 registry value 이름"; return(false); }
			const std::string sKey = _sPrefix.empty() ? sName : _sPrefix + "/" + sName;
			std::vector<std::uint8_t> ValueData(Data.begin(), Data.begin() + nData);
			if (!classify_registry_value(sKey, nType, ValueData, _pValues, _psError)) { return(false); }
		}

		std::vector<wchar_t> SubkeyName(static_cast<std::size_t>(nMaxSubkeyName) + 2);
		for (DWORD i = 0; i < nSubkeyCount; ++i)
		{
			DWORD nName = static_cast<DWORD>(SubkeyName.size());
			nStatus = ::RegEnumKeyExW(_hKey, i, SubkeyName.data(), &nName, nullptr, nullptr, nullptr, nullptr);
			if (nStatus != ERROR_SUCCESS)
			{
				*_psError = "registry subkey 열거 실패(Win32 오류 " + std::to_string(nStatus) + ")";
				return(false);
			}

			HKEY hSubkey = nullptr;
			nStatus = ::RegOpenKeyExW(_hKey, SubkeyName.data(), 0, KEY_READ, &hSubkey);
			if (nStatus != ERROR_SUCCESS)
			{
				*_psError = "registry subkey 열기 실패(Win32 오류 " + std::to_string(nStatus) + ")";
				return(false);
			}
			const std::string sName = narrow(std::wstring_view(SubkeyName.data(), nName));
			if (sName.empty())
			{
				::RegCloseKey(hSubkey);
				*_psError = "빈/잘못된 UTF-16 registry subkey 이름";
				return(false);
			}
			const std::string sPrefix = _sPrefix.empty() ? sName : _sPrefix + "/" + sName;
			const bool bResult = enumerate_registry_key(hSubkey, sPrefix, _pValues, _psError);
			::RegCloseKey(hSubkey);
			if (!bResult) { return(false); }
		}
		return(true);
	}

	bool migrate_registry(
		const std::wstring& _sRegistryRoot, VALUE_MAP* _pValues, std::string* _psError)
	{
		HKEY hRoot = nullptr;
		const LSTATUS nStatus = ::RegOpenKeyExW(
			HKEY_CURRENT_USER, _sRegistryRoot.c_str(), 0, KEY_READ, &hRoot);
		if (nStatus == ERROR_FILE_NOT_FOUND || nStatus == ERROR_PATH_NOT_FOUND) { return(true); }
		if (nStatus != ERROR_SUCCESS)
		{
			*_psError = "registry root 열기 실패(Win32 오류 " + std::to_string(nStatus) + ")";
			return(false);
		}
		const bool bResult = enumerate_registry_key(hRoot, std::string{}, _pValues, _psError);
		::RegCloseKey(hRoot);
		return(bResult);
	}

	bool resolve_paths(std::string* _psDirectory, std::string* _psIniPath, std::string* _psError)
	{
		const DWORD nRequired = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
		if (nRequired == 0) { *_psError = "LOCALAPPDATA가 없거나 비어 있음"; return(false); }
		std::wstring sRoot(static_cast<std::size_t>(nRequired), L'\0');
		const DWORD nLength = ::GetEnvironmentVariableW(L"LOCALAPPDATA", sRoot.data(), nRequired);
		if (nLength == 0 || nLength >= nRequired) { *_psError = "LOCALAPPDATA 읽기 실패"; return(false); }
		sRoot.resize(nLength);

		const std::filesystem::path Root(sRoot);
		if (!Root.is_absolute()) { *_psError = "LOCALAPPDATA가 절대 경로가 아님"; return(false); }
		const std::filesystem::path Directory = (Root / L"pyNote" / L"pyNote").lexically_normal();
		const std::filesystem::path IniPath = Directory / L"NoteEx.ini";
		*_psDirectory = narrow(Directory.native());
		*_psIniPath = narrow(IniPath.native());
		if (_psDirectory->empty() || _psIniPath->empty())
		{
			*_psError = "LOCALAPPDATA path UTF-8 변환 실패";
			return(false);
		}
		return(true);
	}
}

namespace pynote::platform
{
	struct C_WIN32_DEVICE_SETTINGS::S_STATE
	{
		explicit S_STATE(core::storage::I_FILE_SYSTEM& _FileSystem, std::wstring _sRegistryRoot)
			: FileSystem(_FileSystem), sRegistryRoot(std::move(_sRegistryRoot)) {}

		core::storage::I_FILE_SYSTEM& FileSystem;
		std::wstring sRegistryRoot;
		std::string sDirectory;
		std::string sIniPath;
		std::string sLastError;
		VALUE_MAP Values;
		bool bInitialized{ false };
	};

	C_WIN32_DEVICE_SETTINGS::C_WIN32_DEVICE_SETTINGS(
		core::storage::I_FILE_SYSTEM& _FileSystem, std::wstring _sRegistryRoot)
		: m_pState(std::make_unique<S_STATE>(_FileSystem, std::move(_sRegistryRoot)))
	{
	}

	C_WIN32_DEVICE_SETTINGS::~C_WIN32_DEVICE_SETTINGS() = default;

	bool C_WIN32_DEVICE_SETTINGS::Initialize()
	{
		m_pState->sLastError.clear();
		m_pState->Values.clear();
		m_pState->bInitialized = false;
		if (!resolve_paths(&m_pState->sDirectory, &m_pState->sIniPath, &m_pState->sLastError)) { return(false); }

		bool bMigrated = false;
		if (m_pState->FileSystem.Exists(m_pState->sIniPath))
		{
			if (!m_pState->FileSystem.IsRegularFile(m_pState->sIniPath))
			{
				m_pState->sLastError = "INI path가 일반 파일이 아님: " + m_pState->sIniPath;
				return(false);
			}
			std::string sBytes;
			if (!read_file(m_pState->sIniPath, &sBytes, &m_pState->sLastError) ||
				!parse_ini(sBytes, &m_pState->Values, &bMigrated, &m_pState->sLastError))
			{
				return(false);
			}
			if (bMigrated) { m_pState->bInitialized = true; return(true); }
		}

		VALUE_MAP Candidate = m_pState->Values;
		if (!migrate_registry(m_pState->sRegistryRoot, &Candidate, &m_pState->sLastError)) { return(false); }

		auto add_default = [&](const std::string& _sKey, S_VALUE _Value)
		{
			Candidate.try_emplace(_sKey, std::move(_Value));
		};
		add_default("backup/location", string_value(L""));
		add_default("cards/multi_selection_enabled", bool_value(false));
		add_default("display/time_format", string_value(L"yyyy-MM-dd HH:mm"));
		add_default("display/timezone", string_value(L"system"));
		add_default("editor/font_family", string_value(L""));
		add_default("editor/font_size", integer_value(11));
		add_default("editor/line_spacing", double_value(1.0));

		m_pState->Values = std::move(Candidate);
		if (!m_pState->FileSystem.CreateDirectories(m_pState->sDirectory))
		{
			m_pState->sLastError = m_pState->FileSystem.LastError();
			return(false);
		}
		if (!this->Sync()) { return(false); }
		m_pState->bInitialized = true;
		return(true);
	}

	bool C_WIN32_DEVICE_SETTINGS::Sync()
	{
		if (m_pState->sIniPath.empty() || m_pState->sDirectory.empty())
		{
			m_pState->sLastError = "settings가 초기화되지 않음";
			return(false);
		}
		std::string sTemporaryPath;
		if (!m_pState->FileSystem.CreateUniqueTemporaryPath(
			m_pState->sDirectory, ".NoteEx-", ".tmp", &sTemporaryPath))
		{
			m_pState->sLastError = m_pState->FileSystem.LastError();
			return(false);
		}

		auto cleanup_failure = [&](const std::string& _sFailure)
		{
			m_pState->sLastError = _sFailure;
			if (!m_pState->FileSystem.Remove(sTemporaryPath))
			{
				m_pState->sLastError += "; 임시 파일 정리 실패: " + m_pState->FileSystem.LastError();
			}
			return(false);
		};

		const std::string sBytes = serialize_ini(m_pState->Values);
		std::string sWriteError;
		if (!write_file(sTemporaryPath, sBytes, &sWriteError)) { return(cleanup_failure(sWriteError)); }
		if (!m_pState->FileSystem.Replace(sTemporaryPath, m_pState->sIniPath))
		{
			return(cleanup_failure(m_pState->FileSystem.LastError()));
		}
		m_pState->sLastError.clear();
		return(true);
	}

	bool C_WIN32_DEVICE_SETTINGS::Contains(const std::string& _sKey) const
	{
		return(m_pState->Values.find(_sKey) != m_pState->Values.end());
	}

	bool C_WIN32_DEVICE_SETTINGS::GetString(const std::string& _sKey, std::wstring* _psValue) const
	{
		const auto It = m_pState->Values.find(_sKey);
		if (It == m_pState->Values.end() || It->second.eType != E_VALUE_TYPE::String) { return(false); }
		*_psValue = It->second.sString;
		return(true);
	}

	bool C_WIN32_DEVICE_SETTINGS::GetBool(const std::string& _sKey, bool* _pbValue) const
	{
		const auto It = m_pState->Values.find(_sKey);
		if (It == m_pState->Values.end() || It->second.eType != E_VALUE_TYPE::Bool) { return(false); }
		*_pbValue = It->second.bBool;
		return(true);
	}

	bool C_WIN32_DEVICE_SETTINGS::GetInteger(const std::string& _sKey, std::int64_t* _pnValue) const
	{
		const auto It = m_pState->Values.find(_sKey);
		if (It == m_pState->Values.end() || It->second.eType != E_VALUE_TYPE::Integer) { return(false); }
		*_pnValue = It->second.nInteger;
		return(true);
	}

	bool C_WIN32_DEVICE_SETTINGS::GetDouble(const std::string& _sKey, double* _pdValue) const
	{
		const auto It = m_pState->Values.find(_sKey);
		if (It == m_pState->Values.end() || It->second.eType != E_VALUE_TYPE::Double) { return(false); }
		*_pdValue = It->second.dDouble;
		return(true);
	}

	bool C_WIN32_DEVICE_SETTINGS::GetBytes(
		const std::string& _sKey, std::vector<std::uint8_t>* _pValue) const
	{
		const auto It = m_pState->Values.find(_sKey);
		if (It == m_pState->Values.end() || It->second.eType != E_VALUE_TYPE::Bytes) { return(false); }
		*_pValue = It->second.Bytes;
		return(true);
	}

	int C_WIN32_DEVICE_SETTINGS::GetInt(const std::string& _sKey, int _nDefault) const
	{
		std::int64_t nValue = 0;
		if (!this->GetInteger(_sKey, &nValue) ||
			nValue < std::numeric_limits<int>::min() || nValue > std::numeric_limits<int>::max())
		{
			return(_nDefault);
		}
		return(static_cast<int>(nValue));
	}

	void C_WIN32_DEVICE_SETTINGS::SetInt(const std::string& _sKey, int _nValue)
	{
		m_pState->Values[_sKey] = integer_value(_nValue);
	}

	const std::string& C_WIN32_DEVICE_SETTINGS::IniPath() const { return(m_pState->sIniPath); }
	const std::string& C_WIN32_DEVICE_SETTINGS::LastError() const { return(m_pState->sLastError); }
}
