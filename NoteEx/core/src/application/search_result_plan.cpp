#include "pynote/core/application/search_result_plan.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <utility>

namespace
{
	struct S_CASEFOLD_ENTRY { std::uint32_t nScalar; std::size_t nOffset; std::size_t nLength; };
#include "python_casefold_15_1.inc"

	bool decode(std::string_view text, std::vector<std::uint32_t>* scalars)
	{
		if (scalars == nullptr) return false;
		scalars->clear();
		for (std::size_t index = 0; index < text.size();) {
			const auto lead = static_cast<unsigned char>(text[index]);
			std::size_t length = 0; std::uint32_t scalar = 0;
			if (lead <= 0x7f) { length = 1; scalar = lead; }
			else if (lead >= 0xc2 && lead <= 0xdf) { length = 2; scalar = lead & 0x1f; }
			else if (lead >= 0xe0 && lead <= 0xef) { length = 3; scalar = lead & 0x0f; }
			else if (lead >= 0xf0 && lead <= 0xf4) { length = 4; scalar = lead & 0x07; }
			else return false;
			if (index + length > text.size()) return false;
			for (std::size_t offset = 1; offset < length; ++offset) {
				const auto continuation = static_cast<unsigned char>(text[index + offset]);
				if ((continuation & 0xc0) != 0x80) return false;
				scalar = (scalar << 6) | (continuation & 0x3f);
			}
			if ((length == 3 && scalar < 0x800) || (length == 4 && scalar < 0x10000) ||
				scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
			scalars->push_back(scalar); index += length;
		}
		return true;
	}

	void encode_scalar(std::uint32_t scalar, std::string* output)
	{
		if (scalar <= 0x7f) output->push_back(static_cast<char>(scalar));
		else if (scalar <= 0x7ff) { output->push_back(static_cast<char>(0xc0 | scalar >> 6)); output->push_back(static_cast<char>(0x80 | (scalar & 0x3f))); }
		else if (scalar <= 0xffff) { output->push_back(static_cast<char>(0xe0 | scalar >> 12)); output->push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f))); output->push_back(static_cast<char>(0x80 | (scalar & 0x3f))); }
		else { output->push_back(static_cast<char>(0xf0 | scalar >> 18)); output->push_back(static_cast<char>(0x80 | ((scalar >> 12) & 0x3f))); output->push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f))); output->push_back(static_cast<char>(0x80 | (scalar & 0x3f))); }
	}

	std::string encode(const std::vector<std::uint32_t>& scalars)
	{
		std::string result; for (const auto scalar : scalars) encode_scalar(scalar, &result); return result;
	}

	bool valid_utf8(std::string_view text)
	{
		std::vector<std::uint32_t> scalars; return decode(text, &scalars);
	}

	bool python_whitespace(std::uint32_t scalar) noexcept
	{
		return (scalar >= 0x0009 && scalar <= 0x000d) || (scalar >= 0x001c && scalar <= 0x001f) ||
			scalar == 0x0020 || scalar == 0x0085 || scalar == 0x00a0 || scalar == 0x1680 ||
			(scalar >= 0x2000 && scalar <= 0x200a) || scalar == 0x2028 || scalar == 0x2029 ||
			scalar == 0x202f || scalar == 0x205f || scalar == 0x3000;
	}

	std::vector<std::uint32_t> fold(const std::vector<std::uint32_t>& scalars)
	{
		std::vector<std::uint32_t> result;
		for (const auto scalar : scalars) {
			const auto found = std::lower_bound(std::begin(kPythonCasefoldEntries), std::end(kPythonCasefoldEntries), scalar,
				[](const S_CASEFOLD_ENTRY& entry, std::uint32_t value) { return entry.nScalar < value; });
			if (found == std::end(kPythonCasefoldEntries) || found->nScalar != scalar) result.push_back(scalar);
			else result.insert(result.end(), kPythonCasefoldMappedScalars + found->nOffset,
				kPythonCasefoldMappedScalars + found->nOffset + found->nLength);
		}
		return result;
	}

	bool contains_folded(const std::vector<std::uint32_t>& title, const std::vector<std::uint32_t>& query)
	{
		const auto folded = fold(title);
		return std::search(folded.begin(), folded.end(), query.begin(), query.end()) != folded.end();
	}

	bool preview(std::string_view body, std::string* output)
	{
		std::vector<std::uint32_t> scalars; if (!decode(body, &scalars)) return false;
		for (auto& scalar : scalars) if (scalar == 0x0a) scalar = 0x20;
		if (scalars.size() > 160) { scalars.resize(157); scalars.push_back(0x2026); }
		*output = encode(scalars); return true;
	}

	using namespace pynote::core;
	application::S_SEARCH_RESULT_PLAN failed(application::S_SEARCH_RESULT_PLAN plan,
		application::E_SEARCH_RESULT_ERROR error, storage::E_REPO_RESULT source)
	{
		plan.Rows.clear(); plan.eError = error; plan.eSourceResult = source; return plan;
	}
}

namespace pynote::core::application
{
	C_SEARCH_RESULT_PLANNER::C_SEARCH_RESULT_PLANNER(SearchDocumentsPort _SearchDocuments,
		SearchCardsPort _SearchCards, GetDocumentPort _GetDocument)
		: m_SearchDocuments(std::move(_SearchDocuments)), m_SearchCards(std::move(_SearchCards)),
		  m_GetDocument(std::move(_GetDocument)) {}

	C_SEARCH_RESULT_PLANNER::C_SEARCH_RESULT_PLANNER(storage::C_REPOSITORIES& _Repositories)
		: C_SEARCH_RESULT_PLANNER(
			[&_Repositories](const std::string& query, std::vector<domain::S_DOCUMENT>* out) { return _Repositories.SearchDocuments(query, out); },
			[&_Repositories](const std::string& query, std::vector<domain::S_CARD>* out) { return _Repositories.SearchCards(query, std::nullopt, out); },
			[&_Repositories](const std::string& id, domain::S_DOCUMENT* out) { return _Repositories.GetDocument(id, out); }) {}

	S_SEARCH_RESULT_PLAN C_SEARCH_RESULT_PLANNER::Plan(std::string_view _sRawQuery) const
	{
		S_SEARCH_RESULT_PLAN plan;
		std::vector<std::uint32_t> query;
		if (!decode(_sRawQuery, &query)) { plan.eError = E_SEARCH_RESULT_ERROR::InvalidUtf8; return plan; }
		std::size_t begin = 0, end = query.size();
		while (begin < end && python_whitespace(query[begin])) ++begin;
		while (end > begin && python_whitespace(query[end - 1])) --end;
		query = std::vector<std::uint32_t>(query.begin() + begin, query.begin() + end);
		plan.sNormalizedQuery = encode(query);
		if (query.empty()) return plan;
		plan.eState = E_SEARCH_RESULT_STATE::Results;
		std::vector<domain::S_DOCUMENT> documents;
		auto source = m_SearchDocuments(plan.sNormalizedQuery, &documents);
		if (source != storage::E_REPO_RESULT::Ok) return failed(std::move(plan), E_SEARCH_RESULT_ERROR::SearchDocumentsFailed, source);
		std::vector<domain::S_CARD> cards;
		source = m_SearchCards(plan.sNormalizedQuery, &cards);
		if (source != storage::E_REPO_RESULT::Ok) return failed(std::move(plan), E_SEARCH_RESULT_ERROR::SearchCardsFailed, source);
		std::unordered_map<std::string, std::string> titles;
		const auto foldedQuery = fold(query);
		for (const auto& document : documents) {
			if (!valid_utf8(document.sId) || !valid_utf8(document.sTitle)) {
				return failed(std::move(plan), E_SEARCH_RESULT_ERROR::InvalidUtf8, storage::E_REPO_RESULT::Invalid);
			}
			titles[document.sId] = document.sTitle;
			std::vector<std::uint32_t> title;
			if (!decode(document.sTitle, &title)) return failed(std::move(plan), E_SEARCH_RESULT_ERROR::InvalidUtf8, storage::E_REPO_RESULT::Invalid);
			if (contains_folded(title, foldedQuery)) plan.Rows.push_back({ E_SEARCH_RESULT_ROW_KIND::DocumentTitle,
				document.sId, std::nullopt, document.sTitle, document.sTitle });
		}
		for (const auto& card : cards) {
			if (!valid_utf8(card.sDocumentId) || !valid_utf8(card.sId) || !valid_utf8(card.sBody)) {
				return failed(std::move(plan), E_SEARCH_RESULT_ERROR::InvalidUtf8, storage::E_REPO_RESULT::Invalid);
			}
			std::string title;
			if (const auto found = titles.find(card.sDocumentId); found != titles.end()) title = found->second;
			else { domain::S_DOCUMENT document; source = m_GetDocument(card.sDocumentId, &document);
				if (source == storage::E_REPO_RESULT::Ok) {
					if (!valid_utf8(document.sId) || !valid_utf8(document.sTitle)) {
						return failed(std::move(plan), E_SEARCH_RESULT_ERROR::InvalidUtf8, storage::E_REPO_RESULT::Invalid);
					}
					title = document.sTitle;
				}
				else if (source != storage::E_REPO_RESULT::NotFound) return failed(std::move(plan), E_SEARCH_RESULT_ERROR::GetDocumentFailed, source); }
			std::string match; if (!preview(card.sBody, &match)) return failed(std::move(plan), E_SEARCH_RESULT_ERROR::InvalidUtf8, storage::E_REPO_RESULT::Invalid);
			plan.Rows.push_back({ E_SEARCH_RESULT_ROW_KIND::CardBody, card.sDocumentId, card.sId, std::move(title), std::move(match) });
		}
		return plan;
	}
}
