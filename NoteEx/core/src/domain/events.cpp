#include "pynote/core/domain/events.h"

#include "enum_spelling.h"

namespace
{
	using pynote::core::domain::E_EVENT_SOURCE;
	using pynote::core::domain::E_EVENT_TYPE;
	using pynote::core::domain::detail::S_SPELLING;

	// 철자와 순서는 domain/events.py 의 StrEnum 정의 그대로다.
	const S_SPELLING<E_EVENT_TYPE> EVENT_TYPE_TABLE[] = {
		{ "create",  E_EVENT_TYPE::Create  },
		{ "update",  E_EVENT_TYPE::Update  },
		{ "move",    E_EVENT_TYPE::Move    },
		{ "split",   E_EVENT_TYPE::Split   },
		{ "merge",   E_EVENT_TYPE::Merge   },
		{ "delete",  E_EVENT_TYPE::Delete  },
		{ "restore", E_EVENT_TYPE::Restore },
	};

	const S_SPELLING<E_EVENT_SOURCE> EVENT_SOURCE_TABLE[] = {
		{ "typing",  E_EVENT_SOURCE::Typing  },
		{ "paste",   E_EVENT_SOURCE::Paste   },
		{ "import",  E_EVENT_SOURCE::Import  },
		{ "mixed",   E_EVENT_SOURCE::Mixed   },
		{ "edit",    E_EVENT_SOURCE::Edit    },
		{ "restore", E_EVENT_SOURCE::Restore },
		{ "system",  E_EVENT_SOURCE::System  },
	};
}

namespace pynote::core::domain
{
	std::string_view ToText(E_EVENT_TYPE _eValue)
	{
		return(detail::SpellingToText(EVENT_TYPE_TABLE, _eValue));
	}

	std::string_view ToText(E_EVENT_SOURCE _eValue)
	{
		return(detail::SpellingToText(EVENT_SOURCE_TABLE, _eValue));
	}

	bool FromText(std::string_view _sText, E_EVENT_TYPE* _peValue)
	{
		return(detail::SpellingFromText(EVENT_TYPE_TABLE, _sText, _peValue));
	}

	bool FromText(std::string_view _sText, E_EVENT_SOURCE* _peValue)
	{
		return(detail::SpellingFromText(EVENT_SOURCE_TABLE, _sText, _peValue));
	}
}
