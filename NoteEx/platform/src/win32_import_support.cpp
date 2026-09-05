#include "pynote/platform/win32_import_support.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

namespace pynote::platform
{
	namespace
	{
		std::wstring utf8_to_wide(const std::string& text)
		{
			if (text.empty()) { return {}; }
			const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
				static_cast<int>(text.size()), nullptr, 0);
			if (count == 0) { return {}; }
			std::wstring result(static_cast<std::size_t>(count), L'\0');
			::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
				result.data(), count);
			return result;
		}

		std::string wide_to_utf8(const std::wstring& text)
		{
			if (text.empty()) { return {}; }
			const int count = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
				nullptr, 0, nullptr, nullptr);
			std::string result(static_cast<std::size_t>(count), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
				result.data(), count, nullptr, nullptr);
			return result;
		}
	}

	bool ReadFileBounded(const std::string& _sUtf8Path, std::size_t _nMaximumBytes,
		std::vector<std::uint8_t>* _pOut, std::string* _psError)
	{
		_pOut->clear();
		const std::wstring path = utf8_to_wide(_sUtf8Path);
		if (path.empty() && !_sUtf8Path.empty()) { *_psError = "invalid UTF-8 path"; return false; }
		// 원본 open("rb") 는 CPython 의 _SH_DENYNO(FILE_SHARE_READ | FILE_SHARE_WRITE) 라 다른 프로그램이
		// 쓰기로 잡은 파일도 읽는다 - FILE_SHARE_READ 단독은 err=32 로 거부한다(P2 감사 1-1 실측).
		HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) { *_psError = "CreateFileW failed: " + std::to_string(::GetLastError()); return false; }
		std::uint8_t buffer[64 * 1024];
		bool ok = true;
		while (_pOut->size() < _nMaximumBytes) {
			const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(sizeof(buffer), _nMaximumBytes - _pOut->size()));
			DWORD read = 0;
			if (::ReadFile(file, buffer, wanted, &read, nullptr) == FALSE) {
				*_psError = "ReadFile failed: " + std::to_string(::GetLastError()); ok = false; break;
			}
			_pOut->insert(_pOut->end(), buffer, buffer + read);
			if (read == 0) { break; }
		}
		::CloseHandle(file);
		return ok;
	}

	std::string DecodeWindowsCodePage(std::span<const std::uint8_t> _Bytes, unsigned int _nCodePage)
	{
		if (_Bytes.empty()) { return {}; }
		const unsigned int codePage = _nCodePage == CP_ACP ? ::GetACP() : _nCodePage;
		std::wstring wide;
		wide.reserve(_Bytes.size());
		auto appendStrict = [&](std::size_t offset, int length) {
			const char* data = reinterpret_cast<const char*>(_Bytes.data() + offset);
			const int count = ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
				data, length, nullptr, 0);
			if (count == 0) { return false; }
			const std::size_t destination = wide.size();
			wide.resize(destination + static_cast<std::size_t>(count));
			if (::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, data, length,
				wide.data() + destination, count) == 0) {
				wide.resize(destination);
				return false;
			}
			return true;
		};

		std::size_t offset = 0;
		while (offset < _Bytes.size()) {
			const std::uint8_t value = _Bytes[offset];
			if (value < 0x80) {
				wide.push_back(static_cast<wchar_t>(value));
				++offset;
				continue;
			}
			if (codePage == 949 && value == 0x80) {
				wide.push_back(L'\uFFFD');
				++offset;
				continue;
			}
			const bool lead = ::IsDBCSLeadByteEx(codePage, value) != FALSE;
			const int length = lead && offset + 1 < _Bytes.size() ? 2 : 1;
			if (appendStrict(offset, length)) {
				offset += static_cast<std::size_t>(length);
			}
			else {
				wide.push_back(L'\uFFFD');
				++offset;
			}
		}
		return wide_to_utf8(wide);
	}

	std::string DecodeSystemAnsi(std::span<const std::uint8_t> _Bytes)
	{
		return DecodeWindowsCodePage(_Bytes, CP_ACP);
	}
}
