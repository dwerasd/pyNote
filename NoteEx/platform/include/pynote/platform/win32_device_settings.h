#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pynote::core::storage
{
	class I_FILE_SYSTEM;
}

namespace pynote::platform
{
	// LOCALAPPDATA 의 typed UTF-8 INI 를 소유한다. Registry 는 최초 이관 때만 읽고
	// 완료 marker 가 저장된 뒤에는 다시 열지 않는다.
	class C_WIN32_DEVICE_SETTINGS
	{
	public:
		explicit C_WIN32_DEVICE_SETTINGS(
			core::storage::I_FILE_SYSTEM& _FileSystem,
			std::wstring _sRegistryRoot = L"Software\\pyNote\\pyNote");
		~C_WIN32_DEVICE_SETTINGS();

		C_WIN32_DEVICE_SETTINGS(const C_WIN32_DEVICE_SETTINGS&) = delete;
		C_WIN32_DEVICE_SETTINGS& operator=(const C_WIN32_DEVICE_SETTINGS&) = delete;

		bool Initialize();
		bool Sync();

		bool Contains(const std::string& _sKey) const;
		bool GetString(const std::string& _sKey, std::wstring* _psValue) const;
		bool GetBool(const std::string& _sKey, bool* _pbValue) const;
		bool GetInteger(const std::string& _sKey, std::int64_t* _pnValue) const;
		bool GetDouble(const std::string& _sKey, double* _pdValue) const;
		bool GetBytes(const std::string& _sKey, std::vector<std::uint8_t>* _pValue) const;
		bool SetBytes(const std::string& _sKey, std::vector<std::uint8_t> _Value);
		bool Remove(const std::string& _sKey);
		bool MigrateBytes(
			const std::string& _sLegacyKey, const std::string& _sTargetKey,
			bool* _pbMigrated = nullptr);

		int  GetInt(const std::string& _sKey, int _nDefault) const;
		void SetInt(const std::string& _sKey, int _nValue);
		void SetBool(const std::string& _sKey, bool _bValue);

		const std::string& IniPath() const;
		const std::string& LastError() const;

	private:
		struct S_STATE;
		std::unique_ptr<S_STATE> m_pState;
	};
}
