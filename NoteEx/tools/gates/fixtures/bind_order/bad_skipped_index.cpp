// 결함 표본: 인덱스 건너뜀(4 자리에 9)
// good.cpp 에서 이 한 곳만 비틀었다. 컴파일 대상이 아니다.

namespace
{
	constexpr const char8_t* SQL_STAMP_VERSION = u8R"SQL(
            UPDATE schema_version
            SET version = 1, applied_at_us = ?
            WHERE id = 1
            )SQL";
}

namespace sample
{
	E_REPO_RESULT C_SAMPLE::ListCards(std::vector<domain::S_CARD>* _pOut)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            SELECT *
            FROM cards
            ORDER BY position_key, id
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		return(run_rows(m_Database, Stmt, _pOut));
	}

	E_REPO_RESULT C_SAMPLE::CreateCard(const domain::S_CARD& _Card)
	{
		C_STATEMENT Stmt(m_Database.Handle(), u8R"SQL(
            INSERT INTO cards(
                id, document_id, created_at_us, updated_at_us,
                source, body, body_hash, deleted_at_us
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            )SQL");
		if (!Stmt.IsPrepared()) { return(this->fail_()); }
		Stmt.BindText(1, _Card.sId);
		Stmt.BindText(2, _Card.sDocumentId);
		Stmt.BindInt64(3, _Card.nCreatedAtUs);
		Stmt.BindInt64(9, _Card.nUpdatedAtUs);
		Stmt.BindText(5, domain::ToText(_Card.eSource));
		Stmt.BindText(6, _Card.sBody);
		Stmt.BindText(7, _Card.sBodyHash);
		Stmt.BindNullableInt64(8, _Card.nDeletedAtUs);
		return(run_done(m_Database, Stmt));
	}

	bool StampVersion(C_DATABASE& _database, std::int64_t _nAppliedAtUs)
	{
		return(ExecuteBoundInt64(_database, reinterpret_cast<const char*>(SQL_STAMP_VERSION), _nAppliedAtUs));
	}
}
