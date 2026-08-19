#pragma once

#include "pynote/core/application/drop_import_plan.h"
#include "pynote/core/domain/card_list_projection.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pynote::core::application
{
	enum class E_DROP_IMPORT_LIST_EFFECT { None, RevealVisibleTarget, PreserveHiddenTarget };

	struct S_DROP_IMPORT_COMPLETION_RESULT
	{
		std::size_t nAddedCount{};
		std::optional<std::string> sTargetCardId{};
		E_DROP_IMPORT_LIST_EFFECT eEffect{ E_DROP_IMPORT_LIST_EFFECT::None };
	};

	S_DROP_IMPORT_COMPLETION_RESULT CompleteDropImport(
		const S_DROP_IMPORT_EXECUTION& _Execution,
		const std::vector<domain::S_CARD>& _CreatedCards,
		domain::C_CARD_LIST_PROJECTION& _Projection);
}
