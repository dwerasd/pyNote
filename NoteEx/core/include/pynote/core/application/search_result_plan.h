#pragma once

#include "pynote/core/domain/models.h"
#include "pynote/core/storage/repositories.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pynote::core::application
{
	enum class E_SEARCH_RESULT_STATE { NeedsQuery, Results };
	enum class E_SEARCH_RESULT_ROW_KIND { DocumentTitle, CardBody };
	enum class E_SEARCH_RESULT_ERROR
	{
		None,
		InvalidUtf8,
		SearchDocumentsFailed,
		SearchCardsFailed,
		GetDocumentFailed
	};

	struct S_SEARCH_RESULT_ROW
	{
		E_SEARCH_RESULT_ROW_KIND eKind{ E_SEARCH_RESULT_ROW_KIND::DocumentTitle };
		std::string sDocumentId;
		std::optional<std::string> sCardId;
		std::string sDocumentTitle;
		std::string sMatchText;
		bool operator==(const S_SEARCH_RESULT_ROW&) const = default;
	};

	struct S_SEARCH_RESULT_PLAN
	{
		E_SEARCH_RESULT_STATE eState{ E_SEARCH_RESULT_STATE::NeedsQuery };
		E_SEARCH_RESULT_ERROR eError{ E_SEARCH_RESULT_ERROR::None };
		storage::E_REPO_RESULT eSourceResult{ storage::E_REPO_RESULT::Ok };
		std::string sNormalizedQuery;
		std::vector<S_SEARCH_RESULT_ROW> Rows;
		bool Succeeded() const noexcept { return eError == E_SEARCH_RESULT_ERROR::None; }
	};

	using SearchDocumentsPort = std::function<storage::E_REPO_RESULT(
		const std::string&, std::vector<domain::S_DOCUMENT>*)>;
	using SearchCardsPort = std::function<storage::E_REPO_RESULT(
		const std::string&, std::vector<domain::S_CARD>*)>;
	using GetDocumentPort = std::function<storage::E_REPO_RESULT(
		const std::string&, domain::S_DOCUMENT*)>;

	class C_SEARCH_RESULT_PLANNER
	{
	public:
		C_SEARCH_RESULT_PLANNER(SearchDocumentsPort _SearchDocuments,
			SearchCardsPort _SearchCards, GetDocumentPort _GetDocument);
		explicit C_SEARCH_RESULT_PLANNER(storage::C_REPOSITORIES& _Repositories);

		S_SEARCH_RESULT_PLAN Plan(std::string_view _sRawQuery) const;

	private:
		SearchDocumentsPort m_SearchDocuments;
		SearchCardsPort m_SearchCards;
		GetDocumentPort m_GetDocument;
	};
}
