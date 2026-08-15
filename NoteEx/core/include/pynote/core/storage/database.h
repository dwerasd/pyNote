#pragma once

#include <string>

struct sqlite3;

// core 계층. Win32/WTL/DirectWrite 헤더를 포함하지 않는다(tools/gates 격리 게이트가 강제).

namespace pynote::core::storage
{
	// SQLite 연결 수명주기. 파이썬 원본 infrastructure/database.py 의 Database 이식이다.
	//
	// 보존하는 계약(MODE A):
	//  - 열 때 foreign_keys 를 켜고 실제로 켜졌는지 되읽어 확인한다. 실패하면 연결을 닫고 실패로 끝낸다.
	//  - journal_mode 를 WAL 로 바꾸고 반환값이 wal 인지 확인한다. 실패하면 마찬가지로 실패다.
	//    검증까지가 계약이라 PRAGMA 를 던지고 성공을 가정하지 않는다.
	//  - 자동 커밋 상태로 열고 쓰기 트랜잭션은 호출부가 명시적으로 연다.
	//  - schema_version 테이블이 없으면 스키마 버전은 0 이다.
	//
	// 실패는 예외가 아니라 반환값으로 알린다. 사유는 LastError() 에 남는다.
	class C_DATABASE
	{
	public:
		C_DATABASE() = default;
		~C_DATABASE();

		C_DATABASE(const C_DATABASE&) = delete;
		C_DATABASE& operator=(const C_DATABASE&) = delete;

		// UTF-8 파일 경로를 연다. 부모 디렉터리는 호출부가 준비한다.
		// WAL 은 파일 데이터베이스에서만 성립하므로 메모리 데이터베이스는 이 계약을 통과하지 못한다.
		bool Open(const std::string& _sPath);
		void Close();
		bool IsOpen() const noexcept { return(m_pHandle != nullptr); }

		// schema_version 테이블이 없으면 0. 읽기에 실패해도 0 을 돌려주고 LastError 를 채운다.
		int SchemaVersion() const;

		sqlite3* Handle() const noexcept { return(m_pHandle); }
		const std::string& LastError() const noexcept { return(m_sLastError); }

		// 트랜잭션 밖 단문 실행. 실패 시 false 와 LastError.
		bool Execute(const std::string& _sSql);

	private:
		bool verify_pragma_(const std::string& _sPragma, const std::string& _sExpected);
		void set_error_(const std::string& _sMessage);

		sqlite3*            m_pHandle{ nullptr };
		mutable std::string m_sLastError;
	};

	// 쓰기 트랜잭션. 파이썬 원본의 transaction() 컨텍스트 매니저 이식이다.
	//
	// 보존하는 계약: BEGIN IMMEDIATE 로 즉시 쓰기 잠금을 잡고, 커밋하지 않은 채 범위를 벗어나면
	// 전체를 롤백한다. 중첩은 지원하지 않는다 - 이미 트랜잭션 중이면 시작 자체가 실패한다.
	class C_TRANSACTION
	{
	public:
		explicit C_TRANSACTION(C_DATABASE& _database);
		~C_TRANSACTION();

		C_TRANSACTION(const C_TRANSACTION&) = delete;
		C_TRANSACTION& operator=(const C_TRANSACTION&) = delete;

		bool IsActive() const noexcept { return(m_bActive); }
		bool Commit();

	private:
		C_DATABASE& m_Database;
		bool        m_bActive{ false };
	};
}
