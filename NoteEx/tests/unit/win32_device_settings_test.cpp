#include <catch_amalgamated.hpp>

#include "pynote/core/storage/file_system.h"
#include "pynote/platform/win32_device_settings.h"
#include "pynote/platform/win32_file_system.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using pynote::core::storage::I_FILE_SYSTEM;
	using pynote::platform::C_WIN32_DEVICE_SETTINGS;
	using pynote::platform::C_WIN32_FILE_SYSTEM;

	std::atomic<unsigned long> g_nSequence{ 0 };

	std::wstring unique_suffix()
	{
		return(L"w3d8_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
			std::to_wstring(::GetTickCount64()) + L"_" + std::to_wstring(++g_nSequence));
	}

	std::string narrow(const std::wstring& _sText)
	{
		if (_sText.empty()) { return(std::string{}); }
		const int nSize = ::WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, _sText.data(), static_cast<int>(_sText.size()),
			nullptr, 0, nullptr, nullptr);
		std::string sResult(static_cast<std::size_t>(nSize), '\0');
		::WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, _sText.data(), static_cast<int>(_sText.size()),
			sResult.data(), nSize, nullptr, nullptr);
		return(sResult);
	}

	std::string read_bytes(const std::filesystem::path& _Path)
	{
		std::ifstream Input(_Path, std::ios::binary);
		return(std::string(std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>()));
	}

	void write_bytes(const std::filesystem::path& _Path, const std::string& _sBytes)
	{
		std::filesystem::create_directories(_Path.parent_path());
		std::ofstream Output(_Path, std::ios::binary | std::ios::trunc);
		Output.write(_sBytes.data(), static_cast<std::streamsize>(_sBytes.size()));
	}

	class C_TEMP_LOCAL_APP_DATA
	{
	public:
		C_TEMP_LOCAL_APP_DATA()
		{
			wchar_t TempPath[32768] = {};
			const DWORD nLength = ::GetTempPathW(static_cast<DWORD>(std::size(TempPath)), TempPath);
			m_Root = std::filesystem::path(std::wstring(TempPath, nLength)) / (L"NoteEx-W3-D8-test-" + unique_suffix());
			std::filesystem::create_directories(m_Root);

			const DWORD nOldRequired = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
			if (nOldRequired != 0)
			{
				m_sOld.resize(nOldRequired);
				const DWORD nOldLength = ::GetEnvironmentVariableW(L"LOCALAPPDATA", m_sOld.data(), nOldRequired);
				m_sOld.resize(nOldLength);
				m_bHadOld = true;
			}
			::SetEnvironmentVariableW(L"LOCALAPPDATA", m_Root.c_str());
		}

		~C_TEMP_LOCAL_APP_DATA()
		{
			::SetEnvironmentVariableW(L"LOCALAPPDATA", m_bHadOld ? m_sOld.c_str() : nullptr);
			std::error_code Error;
			std::filesystem::remove_all(m_Root, Error);
		}

		const std::filesystem::path& Root() const { return(m_Root); }
		std::filesystem::path IniPath() const { return(m_Root / L"pyNote" / L"pyNote" / L"NoteEx.ini"); }

	private:
		std::filesystem::path m_Root;
		std::wstring m_sOld;
		bool m_bHadOld{ false };
	};

	class C_REGISTRY_FIXTURE
	{
	public:
		C_REGISTRY_FIXTURE() : m_sRoot(L"Software\\pyNote\\W3Tests\\" + unique_suffix()) {}
		~C_REGISTRY_FIXTURE() { if (!m_bCleaned) { this->Cleanup(); } }

		const std::wstring& Root() const { return(m_sRoot); }

		bool SetRaw(const std::wstring& _sCanonicalKey, DWORD _nType, const void* _pData, DWORD _nBytes)
		{
			const std::size_t nSlash = _sCanonicalKey.find_last_of(L'/');
			if (nSlash == std::wstring::npos || nSlash == 0 || nSlash + 1 >= _sCanonicalKey.size()) { return(false); }
			std::wstring sSubkey = m_sRoot + L"\\" + _sCanonicalKey.substr(0, nSlash);
			std::replace(sSubkey.begin(), sSubkey.end(), L'/', L'\\');
			const std::wstring sValueName = _sCanonicalKey.substr(nSlash + 1);

			HKEY hKey = nullptr;
			if (::RegCreateKeyExW(
				HKEY_CURRENT_USER, sSubkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
			{
				return(false);
			}
			const LSTATUS nStatus = ::RegSetValueExW(
				hKey, sValueName.c_str(), 0, _nType,
				static_cast<const BYTE*>(_pData), _nBytes);
			::RegCloseKey(hKey);
			return(nStatus == ERROR_SUCCESS);
		}

		bool SetString(const std::wstring& _sKey, const std::wstring& _sValue)
		{
			return(SetRaw(_sKey, REG_SZ, _sValue.c_str(),
				static_cast<DWORD>((_sValue.size() + 1) * sizeof(wchar_t))));
		}

		bool SetDword(const std::wstring& _sKey, DWORD _nValue)
		{
			return(SetRaw(_sKey, REG_DWORD, &_nValue, sizeof(_nValue)));
		}

		bool SetBinary(const std::wstring& _sKey, const std::vector<std::uint8_t>& _Data)
		{
			return(SetRaw(_sKey, REG_BINARY, _Data.data(), static_cast<DWORD>(_Data.size())));
		}

		std::vector<std::string> Snapshot() const
		{
			std::vector<std::string> Rows;
			HKEY hRoot = nullptr;
			if (::RegOpenKeyExW(HKEY_CURRENT_USER, m_sRoot.c_str(), 0, KEY_READ, &hRoot) != ERROR_SUCCESS)
			{
				return(Rows);
			}
			SnapshotKey(hRoot, std::string{}, &Rows);
			::RegCloseKey(hRoot);
			std::sort(Rows.begin(), Rows.end());
			return(Rows);
		}

		bool Cleanup()
		{
			const LSTATUS nTree = ::RegDeleteTreeW(HKEY_CURRENT_USER, m_sRoot.c_str());
			if (nTree != ERROR_SUCCESS && nTree != ERROR_FILE_NOT_FOUND && nTree != ERROR_PATH_NOT_FOUND) { return(false); }
			const LSTATUS nKey = ::RegDeleteKeyW(HKEY_CURRENT_USER, m_sRoot.c_str());
			m_bCleaned = (nKey == ERROR_SUCCESS || nKey == ERROR_FILE_NOT_FOUND || nKey == ERROR_PATH_NOT_FOUND);
			return(m_bCleaned);
		}

	private:
		static std::string bytes_hex(const BYTE* _pData, DWORD _nBytes)
		{
			static constexpr char DIGITS[] = "0123456789abcdef";
			std::string sResult;
			for (DWORD i = 0; i < _nBytes; ++i)
			{
				sResult.push_back(DIGITS[_pData[i] >> 4]);
				sResult.push_back(DIGITS[_pData[i] & 15]);
			}
			return(sResult);
		}

		static void SnapshotKey(HKEY _hKey, const std::string& _sPrefix, std::vector<std::string>* _pRows)
		{
			DWORD nSubkeys = 0, nMaxSubkey = 0, nValues = 0, nMaxName = 0, nMaxData = 0;
			if (::RegQueryInfoKeyW(_hKey, nullptr, nullptr, nullptr, &nSubkeys, &nMaxSubkey, nullptr,
				&nValues, &nMaxName, &nMaxData, nullptr, nullptr) != ERROR_SUCCESS) { return; }

			std::vector<wchar_t> Name(static_cast<std::size_t>(std::max(nMaxName, nMaxSubkey)) + 2);
			std::vector<BYTE> Data(static_cast<std::size_t>(nMaxData) + 2);
			for (DWORD i = 0; i < nValues; ++i)
			{
				DWORD nName = static_cast<DWORD>(Name.size()), nData = static_cast<DWORD>(Data.size()), nType = 0;
				if (::RegEnumValueW(_hKey, i, Name.data(), &nName, nullptr, &nType, Data.data(), &nData) != ERROR_SUCCESS) { continue; }
				const std::string sName = narrow(std::wstring(Name.data(), nName));
				_pRows->push_back(_sPrefix + "/" + sName + "|" + std::to_string(nType) + "|" + bytes_hex(Data.data(), nData));
			}
			for (DWORD i = 0; i < nSubkeys; ++i)
			{
				DWORD nName = static_cast<DWORD>(Name.size());
				if (::RegEnumKeyExW(_hKey, i, Name.data(), &nName, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) { continue; }
				HKEY hSubkey = nullptr;
				if (::RegOpenKeyExW(_hKey, Name.data(), 0, KEY_READ, &hSubkey) != ERROR_SUCCESS) { continue; }
				const std::string sName = narrow(std::wstring(Name.data(), nName));
				SnapshotKey(hSubkey, _sPrefix + "/" + sName, _pRows);
				::RegCloseKey(hSubkey);
			}
		}

		std::wstring m_sRoot;
		bool m_bCleaned{ false };
	};

	class C_TRACKING_FILE_SYSTEM final : public I_FILE_SYSTEM
	{
	public:
		bool Exists(const std::string& _sPath) const override { return(m_Base.Exists(_sPath)); }
		bool IsRegularFile(const std::string& _sPath) const override { return(m_Base.IsRegularFile(_sPath)); }
		bool IsSymlink(const std::string& _sPath) const override { return(m_Base.IsSymlink(_sPath)); }
		bool CreateDirectories(const std::string& _sPath) override { return(m_Base.CreateDirectories(_sPath)); }
		bool Remove(const std::string& _sPath) override { ++nRemoveCalls; return(m_Base.Remove(_sPath)); }
		bool CreateUniqueTemporaryPath(
			const std::string& _sDirectory, const std::string& _sPrefix,
			const std::string& _sSuffix, std::string* _psPath) override
		{
			return(m_Base.CreateUniqueTemporaryPath(_sDirectory, _sPrefix, _sSuffix, _psPath));
		}
		bool ModifiedTimeUs(const std::string& _sPath, std::int64_t* _pnValueUs) const override
		{
			return(m_Base.ModifiedTimeUs(_sPath, _pnValueUs));
		}
		bool ListDirectory(const std::string& _sDirectory, std::vector<std::string>* _pNames) const override
		{
			return(m_Base.ListDirectory(_sDirectory, _pNames));
		}
		bool Replace(const std::string& _sFrom, const std::string& _sTo) override
		{
			++nReplaceCalls;
			if (bFailReplace) { m_sError = "injected replace failure"; return(false); }
			return(m_Base.Replace(_sFrom, _sTo));
		}
		const std::string& LastError() const override
		{
			return(m_sError.empty() ? m_Base.LastError() : m_sError);
		}

		C_WIN32_FILE_SYSTEM m_Base;
		bool bFailReplace{ false };
		int nReplaceCalls{ 0 };
		int nRemoveCalls{ 0 };
		std::string m_sError;
	};

	bool has_marker(const std::filesystem::path& _Path)
	{
		return(read_bytes(_Path).find("registry_migration_v1=b:1") != std::string::npos);
	}
}

TEST_CASE("W3-D8-001 LocalAppData path and seven defaults", "[W3-D8-settings]")
{
	C_TEMP_LOCAL_APP_DATA Local;
	C_REGISTRY_FIXTURE Registry;
	C_WIN32_FILE_SYSTEM FileSystem;
	C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());

	const bool bInitialized = Settings.Initialize();
	std::wstring sString;
	bool bBool = true;
	std::int64_t nInteger = 0;
	double dDouble = 0.0;
	const bool bCleanup = Registry.Cleanup();

	REQUIRE(bCleanup);
	REQUIRE(bInitialized);
	REQUIRE(Settings.IniPath() == narrow(Local.IniPath().native()));
	REQUIRE(Settings.GetString("backup/location", &sString)); REQUIRE(sString.empty());
	REQUIRE(Settings.GetBool("cards/multi_selection_enabled", &bBool)); REQUIRE_FALSE(bBool);
	REQUIRE(Settings.GetString("display/time_format", &sString)); REQUIRE(sString == L"yyyy-MM-dd HH:mm");
	REQUIRE(Settings.GetString("display/timezone", &sString)); REQUIRE(sString == L"system");
	REQUIRE(Settings.GetString("editor/font_family", &sString)); REQUIRE(sString.empty());
	REQUIRE(Settings.GetInteger("editor/font_size", &nInteger)); REQUIRE(nInteger == 11);
	REQUIRE(Settings.GetDouble("editor/line_spacing", &dDouble)); REQUIRE(dDouble == 1.0);
	REQUIRE(has_marker(Local.IniPath()));
}

TEST_CASE("W3-D8-002 measured registry values map 12 of 12", "[W3-D8-settings]")
{
	C_TEMP_LOCAL_APP_DATA Local;
	C_REGISTRY_FIXTURE Registry;
	REQUIRE(Registry.SetString(L"backup/location", L"D:\\백업"));
	REQUIRE(Registry.SetString(L"cards/multi_selection_enabled", L"TrUe"));
	REQUIRE(Registry.SetString(L"display/time_format", L"yyyy/MM/dd HH:mm:ss"));
	REQUIRE(Registry.SetString(L"display/timezone", L"Asia/Seoul"));
	REQUIRE(Registry.SetString(L"editor/font_family", L"맑은 고딕"));
	REQUIRE(Registry.SetDword(L"editor/font_size", 17));
	REQUIRE(Registry.SetString(L"editor/line_spacing", L"1.25"));
	REQUIRE(Registry.SetString(L"composer/immediate_paste_capture", L"false"));
	REQUIRE(Registry.SetBinary(L"windows/main/geometry", { 0x00, 0x7f, 0x80, 0xff }));
	REQUIRE(Registry.SetBinary(L"window/geometry", { 0x10, 0x20 }));
	REQUIRE(Registry.SetString(L"shortcuts/merge", L"Ctrl+M"));
	REQUIRE(Registry.SetString(L"shortcuts/split", L"Ctrl+Shift+M"));
	REQUIRE(Registry.SetDword(L"draft/idle_seconds", 60));
	REQUIRE(Registry.SetDword(L"cards/preview_lines", 3));
	REQUIRE(Registry.SetDword(L"backup/interval_hours", 6));
	REQUIRE(Registry.SetDword(L"trash/retention_days", 30));
	const auto Before = Registry.Snapshot();

	C_WIN32_FILE_SYSTEM FileSystem;
	C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
	const bool bInitialized = Settings.Initialize();
	const auto After = Registry.Snapshot();
	std::wstring sString;
	bool bBool = true;
	std::int64_t nInteger = 0;
	double dDouble = 0.0;
	std::vector<std::uint8_t> Bytes;
	const bool bCleanup = Registry.Cleanup();

	REQUIRE(bCleanup);
	REQUIRE(bInitialized);
	REQUIRE(Before == After);
	REQUIRE(Settings.GetString("backup/location", &sString)); REQUIRE(sString == L"D:\\백업");
	REQUIRE(Settings.GetBool("cards/multi_selection_enabled", &bBool)); REQUIRE(bBool);
	REQUIRE(Settings.GetString("display/time_format", &sString)); REQUIRE(sString == L"yyyy/MM/dd HH:mm:ss");
	REQUIRE(Settings.GetString("display/timezone", &sString)); REQUIRE(sString == L"Asia/Seoul");
	REQUIRE(Settings.GetString("editor/font_family", &sString)); REQUIRE(sString == L"맑은 고딕");
	REQUIRE(Settings.GetInteger("editor/font_size", &nInteger)); REQUIRE(nInteger == 17);
	REQUIRE(Settings.GetDouble("editor/line_spacing", &dDouble)); REQUIRE(dDouble == 1.25);
	REQUIRE(Settings.GetBool("composer/immediate_paste_capture", &bBool)); REQUIRE_FALSE(bBool);
	REQUIRE(Settings.GetBytes("windows/main/geometry", &Bytes)); REQUIRE(Bytes == std::vector<std::uint8_t>{ 0x00, 0x7f, 0x80, 0xff });
	REQUIRE(Settings.GetBytes("window/geometry", &Bytes)); REQUIRE(Bytes == std::vector<std::uint8_t>{ 0x10, 0x20 });
	REQUIRE(Settings.GetString("shortcuts/merge", &sString)); REQUIRE(sString == L"Ctrl+M");
	REQUIRE(Settings.GetString("shortcuts/split", &sString)); REQUIRE(sString == L"Ctrl+Shift+M");
	REQUIRE_FALSE(Settings.Contains("first_run/guide_shown"));
	REQUIRE_FALSE(Settings.Contains("draft/idle_seconds"));
	REQUIRE_FALSE(Settings.Contains("cards/preview_lines"));
	REQUIRE_FALSE(Settings.Contains("backup/interval_hours"));
	REQUIRE_FALSE(Settings.Contains("trash/retention_days"));
}

TEST_CASE("W3-D8-003 local values win and completed migration never reopens registry", "[W3-D8-settings]")
{
	C_TEMP_LOCAL_APP_DATA Local;
	C_REGISTRY_FIXTURE Registry;
	write_bytes(Local.IniPath(), "[device]\nbackup/location=s:6c6f63616c\n");
	REQUIRE(Registry.SetString(L"backup/location", L"registry"));
	const auto RegistryBefore = Registry.Snapshot();

	C_WIN32_FILE_SYSTEM FileSystem;
	C_WIN32_DEVICE_SETTINGS First(FileSystem, Registry.Root());
	const bool bFirst = First.Initialize();
	std::wstring sValue;
	const bool bLocalWon = First.GetString("backup/location", &sValue) && sValue == L"local";
	const auto RegistryAfter = Registry.Snapshot();
	const std::string sAfterFirst = read_bytes(Local.IniPath());
	const bool bCleanup = Registry.Cleanup();

	C_WIN32_DEVICE_SETTINGS Second(FileSystem, Registry.Root());
	const bool bSecond = Second.Initialize();
	std::wstring sSecondValue;
	const bool bSecondLocal = Second.GetString("backup/location", &sSecondValue) && sSecondValue == L"local";
	const std::string sAfterSecond = read_bytes(Local.IniPath());

	REQUIRE(bCleanup);
	REQUIRE(bFirst);
	REQUIRE(bLocalWon);
	REQUIRE(RegistryBefore == RegistryAfter);
	REQUIRE(bSecond);
	REQUIRE(bSecondLocal);
	REQUIRE(sAfterSecond == sAfterFirst);
}

TEST_CASE("W3-D8-004 absent registry is a successful first run", "[W3-D8-settings]")
{
	C_TEMP_LOCAL_APP_DATA Local;
	C_REGISTRY_FIXTURE Registry;
	C_WIN32_FILE_SYSTEM FileSystem;
	C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
	const bool bInitialized = Settings.Initialize();
	const bool bCleanup = Registry.Cleanup();

	REQUIRE(bCleanup);
	REQUIRE(bInitialized);
	REQUIRE(std::filesystem::is_regular_file(Local.IniPath()));
	REQUIRE(has_marker(Local.IniPath()));
	REQUIRE(Settings.LastError().empty());
}

TEST_CASE("W3-D8-005 invalid or unknown input fails atomically without marker", "[W3-D8-settings]")
{
	SECTION("unknown registry key")
	{
		C_TEMP_LOCAL_APP_DATA Local;
		C_REGISTRY_FIXTURE Registry;
		const std::string sOriginal = "[device]\nbackup/location=s:6c6f63616c\n";
		write_bytes(Local.IniPath(), sOriginal);
		REQUIRE(Registry.SetString(L"unknown/value", L"no"));
		const auto Before = Registry.Snapshot();
		C_WIN32_FILE_SYSTEM FileSystem;
		C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
		const bool bInitialized = Settings.Initialize();
		const auto After = Registry.Snapshot();
		const bool bCleanup = Registry.Cleanup();
		REQUIRE(bCleanup);
		REQUIRE_FALSE(bInitialized);
		REQUIRE(Before == After);
		REQUIRE(read_bytes(Local.IniPath()) == sOriginal);
		REQUIRE_FALSE(has_marker(Local.IniPath()));
	}

	SECTION("wrong registry type")
	{
		C_TEMP_LOCAL_APP_DATA Local;
		C_REGISTRY_FIXTURE Registry;
		REQUIRE(Registry.SetString(L"editor/font_size", L"17"));
		const auto Before = Registry.Snapshot();
		C_WIN32_FILE_SYSTEM FileSystem;
		C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
		const bool bInitialized = Settings.Initialize();
		const auto After = Registry.Snapshot();
		const bool bCleanup = Registry.Cleanup();
		REQUIRE(bCleanup);
		REQUIRE_FALSE(bInitialized);
		REQUIRE(Before == After);
		REQUIRE_FALSE(std::filesystem::exists(Local.IniPath()));
	}

	SECTION("malformed existing INI")
	{
		C_TEMP_LOCAL_APP_DATA Local;
		C_REGISTRY_FIXTURE Registry;
		const std::string sOriginal = "[device]\nbackup/location=s:0\n";
		write_bytes(Local.IniPath(), sOriginal);
		C_WIN32_FILE_SYSTEM FileSystem;
		C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
		const bool bInitialized = Settings.Initialize();
		const bool bCleanup = Registry.Cleanup();
		REQUIRE(bCleanup);
		REQUIRE_FALSE(bInitialized);
		REQUIRE(read_bytes(Local.IniPath()) == sOriginal);
		REQUIRE_FALSE(has_marker(Local.IniPath()));
	}

	SECTION("unknown existing INI key")
	{
		C_TEMP_LOCAL_APP_DATA Local;
		C_REGISTRY_FIXTURE Registry;
		const std::string sOriginal = "[device]\nunknown/value=s:6e6f\n";
		write_bytes(Local.IniPath(), sOriginal);
		C_WIN32_FILE_SYSTEM FileSystem;
		C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
		const bool bInitialized = Settings.Initialize();
		const bool bCleanup = Registry.Cleanup();
		REQUIRE(bCleanup);
		REQUIRE_FALSE(bInitialized);
		REQUIRE(read_bytes(Local.IniPath()) == sOriginal);
		REQUIRE_FALSE(has_marker(Local.IniPath()));
	}

	SECTION("malformed dynamic geometry INI key")
	{
		C_TEMP_LOCAL_APP_DATA Local;
		C_REGISTRY_FIXTURE Registry;
		const std::string sOriginal = "[device]\nwindows/geometry=x:0102\n";
		write_bytes(Local.IniPath(), sOriginal);
		C_WIN32_FILE_SYSTEM FileSystem;
		C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
		const bool bInitialized = Settings.Initialize();
		const bool bCleanup = Registry.Cleanup();
		REQUIRE(bCleanup);
		REQUIRE_FALSE(bInitialized);
		REQUIRE(read_bytes(Local.IniPath()) == sOriginal);
		REQUIRE_FALSE(has_marker(Local.IniPath()));
	}

	SECTION("replace failure")
	{
		C_TEMP_LOCAL_APP_DATA Local;
		C_REGISTRY_FIXTURE Registry;
		const std::string sOriginal = "[device]\nbackup/location=s:6c6f63616c\n";
		write_bytes(Local.IniPath(), sOriginal);
		C_TRACKING_FILE_SYSTEM FileSystem;
		FileSystem.bFailReplace = true;
		C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
		const bool bInitialized = Settings.Initialize();
		const bool bCleanup = Registry.Cleanup();
		REQUIRE(bCleanup);
		REQUIRE_FALSE(bInitialized);
		REQUIRE(FileSystem.nReplaceCalls == 1);
		REQUIRE(FileSystem.nRemoveCalls == 1);
		REQUIRE(read_bytes(Local.IniPath()) == sOriginal);
		REQUIRE_FALSE(has_marker(Local.IniPath()));
		std::vector<std::string> Names;
		REQUIRE(FileSystem.ListDirectory(narrow(Local.IniPath().parent_path().native()), &Names));
		REQUIRE(std::none_of(Names.begin(), Names.end(), [](const std::string& _sName)
		{
			return(_sName.starts_with(".NoteEx-") && _sName.ends_with(".tmp"));
		}));
	}
}

TEST_CASE("W3-D8-006 shell rectangle values round trip in one sync", "[W3-D8-settings]")
{
	C_TEMP_LOCAL_APP_DATA Local;
	C_REGISTRY_FIXTURE Registry;
	C_TRACKING_FILE_SYSTEM FileSystem;
	C_WIN32_DEVICE_SETTINGS Settings(FileSystem, Registry.Root());
	REQUIRE(Settings.Initialize());
	FileSystem.nReplaceCalls = 0;
	Settings.SetInt("location/x", -120);
	Settings.SetInt("location/y", 45);
	Settings.SetInt("location/w", 1280);
	Settings.SetInt("location/h", 720);
	const bool bSynced = Settings.Sync();
	const int nSyncReplaces = FileSystem.nReplaceCalls;

	C_WIN32_DEVICE_SETTINGS Reloaded(FileSystem, Registry.Root());
	const bool bReloaded = Reloaded.Initialize();
	const bool bCleanup = Registry.Cleanup();

	REQUIRE(bCleanup);
	REQUIRE(bSynced);
	REQUIRE(nSyncReplaces == 1);
	REQUIRE(bReloaded);
	REQUIRE(Reloaded.GetInt("location/x", 0) == -120);
	REQUIRE(Reloaded.GetInt("location/y", 0) == 45);
	REQUIRE(Reloaded.GetInt("location/w", 0) == 1280);
	REQUIRE(Reloaded.GetInt("location/h", 0) == 720);
}
