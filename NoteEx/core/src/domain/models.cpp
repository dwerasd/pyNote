#include "pynote/core/domain/models.h"

#include "enum_spelling.h"

namespace
{
	using pynote::core::domain::E_CAPTURE_OPERATION_SOURCE;
	using pynote::core::domain::E_CARD_SOURCE;
	using pynote::core::domain::E_DRAFT_KIND;
	using pynote::core::domain::E_LINEAGE_RELATION_TYPE;
	using pynote::core::domain::E_NEWLINE_KIND;
	using pynote::core::domain::E_REVISION_SOURCE;
	using pynote::core::domain::E_SPLIT_POLICY;
	using pynote::core::domain::detail::S_SPELLING;

	// 철자와 순서는 domain/models.py 의 StrEnum 정의 그대로다.
	const S_SPELLING<E_CAPTURE_OPERATION_SOURCE> CAPTURE_OPERATION_SOURCE_TABLE[] = {
		{ "typing", E_CAPTURE_OPERATION_SOURCE::Typing },
		{ "paste",  E_CAPTURE_OPERATION_SOURCE::Paste  },
		{ "import", E_CAPTURE_OPERATION_SOURCE::Import },
		{ "mixed",  E_CAPTURE_OPERATION_SOURCE::Mixed  },
		{ "split",  E_CAPTURE_OPERATION_SOURCE::Split  },
		{ "merge",  E_CAPTURE_OPERATION_SOURCE::Merge  },
		{ "system", E_CAPTURE_OPERATION_SOURCE::System },
	};

	const S_SPELLING<E_SPLIT_POLICY> SPLIT_POLICY_TABLE[] = {
		{ "keep",                E_SPLIT_POLICY::Keep             },
		{ "split_by_blank_line", E_SPLIT_POLICY::SplitByBlankLine },
	};

	const S_SPELLING<E_CARD_SOURCE> CARD_SOURCE_TABLE[] = {
		{ "typing",  E_CARD_SOURCE::Typing  },
		{ "paste",   E_CARD_SOURCE::Paste   },
		{ "import",  E_CARD_SOURCE::Import  },
		{ "mixed",   E_CARD_SOURCE::Mixed   },
		{ "restore", E_CARD_SOURCE::Restore },
		{ "split",   E_CARD_SOURCE::Split   },
		{ "merge",   E_CARD_SOURCE::Merge   },
		{ "system",  E_CARD_SOURCE::System  },
	};

	const S_SPELLING<E_REVISION_SOURCE> REVISION_SOURCE_TABLE[] = {
		{ "edit",    E_REVISION_SOURCE::Edit    },
		{ "restore", E_REVISION_SOURCE::Restore },
		{ "split",   E_REVISION_SOURCE::Split   },
		{ "merge",   E_REVISION_SOURCE::Merge   },
	};

	const S_SPELLING<E_DRAFT_KIND> DRAFT_KIND_TABLE[] = {
		{ "new",  E_DRAFT_KIND::New  },
		{ "edit", E_DRAFT_KIND::Edit },
	};

	const S_SPELLING<E_LINEAGE_RELATION_TYPE> LINEAGE_RELATION_TYPE_TABLE[] = {
		{ "split", E_LINEAGE_RELATION_TYPE::Split },
		{ "merge", E_LINEAGE_RELATION_TYPE::Merge },
	};

	const S_SPELLING<E_NEWLINE_KIND> NEWLINE_KIND_TABLE[] = {
		{ "lf",   E_NEWLINE_KIND::Lf   },
		{ "crlf", E_NEWLINE_KIND::Crlf },
		{ "cr",   E_NEWLINE_KIND::Cr   },
	};
}

namespace pynote::core::domain
{
	std::string_view ToText(E_CAPTURE_OPERATION_SOURCE _eValue)
	{
		return(detail::SpellingToText(CAPTURE_OPERATION_SOURCE_TABLE, _eValue));
	}

	std::string_view ToText(E_SPLIT_POLICY _eValue)
	{
		return(detail::SpellingToText(SPLIT_POLICY_TABLE, _eValue));
	}

	std::string_view ToText(E_CARD_SOURCE _eValue)
	{
		return(detail::SpellingToText(CARD_SOURCE_TABLE, _eValue));
	}

	std::string_view ToText(E_REVISION_SOURCE _eValue)
	{
		return(detail::SpellingToText(REVISION_SOURCE_TABLE, _eValue));
	}

	std::string_view ToText(E_DRAFT_KIND _eValue)
	{
		return(detail::SpellingToText(DRAFT_KIND_TABLE, _eValue));
	}

	std::string_view ToText(E_LINEAGE_RELATION_TYPE _eValue)
	{
		return(detail::SpellingToText(LINEAGE_RELATION_TYPE_TABLE, _eValue));
	}

	std::string_view ToText(E_NEWLINE_KIND _eValue)
	{
		return(detail::SpellingToText(NEWLINE_KIND_TABLE, _eValue));
	}

	bool FromText(std::string_view _sText, E_CAPTURE_OPERATION_SOURCE* _peValue)
	{
		return(detail::SpellingFromText(CAPTURE_OPERATION_SOURCE_TABLE, _sText, _peValue));
	}

	bool FromText(std::string_view _sText, E_SPLIT_POLICY* _peValue)
	{
		return(detail::SpellingFromText(SPLIT_POLICY_TABLE, _sText, _peValue));
	}

	bool FromText(std::string_view _sText, E_CARD_SOURCE* _peValue)
	{
		return(detail::SpellingFromText(CARD_SOURCE_TABLE, _sText, _peValue));
	}

	bool FromText(std::string_view _sText, E_REVISION_SOURCE* _peValue)
	{
		return(detail::SpellingFromText(REVISION_SOURCE_TABLE, _sText, _peValue));
	}

	bool FromText(std::string_view _sText, E_DRAFT_KIND* _peValue)
	{
		return(detail::SpellingFromText(DRAFT_KIND_TABLE, _sText, _peValue));
	}

	bool FromText(std::string_view _sText, E_LINEAGE_RELATION_TYPE* _peValue)
	{
		return(detail::SpellingFromText(LINEAGE_RELATION_TYPE_TABLE, _sText, _peValue));
	}

	bool FromText(std::string_view _sText, E_NEWLINE_KIND* _peValue)
	{
		return(detail::SpellingFromText(NEWLINE_KIND_TABLE, _sText, _peValue));
	}

	std::string_view NewlineCharacters(E_NEWLINE_KIND _eValue)
	{
		// 원본 NewlineKind.characters(:59~64) 의 분기 그대로다.
		if (_eValue == E_NEWLINE_KIND::Crlf) { return("\r\n"); }
		return(_eValue == E_NEWLINE_KIND::Cr ? "\r" : "\n");
	}
}
