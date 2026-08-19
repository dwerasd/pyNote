#include "pynote/core/application/drop_import_completion.h"

#include <stdexcept>

namespace pynote::core::application
{
	S_DROP_IMPORT_COMPLETION_RESULT CompleteDropImport(
		const S_DROP_IMPORT_EXECUTION& execution,
		const std::vector<domain::S_CARD>& createdCards,
		domain::C_CARD_LIST_PROJECTION& projection)
	{
		if (execution.CreatedCardIds.size() != createdCards.size()) {
			throw std::invalid_argument("created card count does not match execution");
		}
		for (std::size_t index = 0; index < createdCards.size(); ++index) {
			if (execution.CreatedCardIds[index] != createdCards[index].sId) {
				throw std::invalid_argument("created card identity does not match execution");
			}
		}
		if (execution.ePostAction == E_DROP_IMPORT_POST_ACTION::RevealLastCreated
			&& execution.CreatedCardIds.empty()) {
			throw std::invalid_argument("reveal completion requires a created card");
		}

		S_DROP_IMPORT_COMPLETION_RESULT result;
		result.nAddedCount = projection.AddCards(createdCards);
		if (execution.ePostAction != E_DROP_IMPORT_POST_ACTION::RevealLastCreated) {
			return result;
		}
		result.sTargetCardId = execution.CreatedCardIds.back();
		if (!projection.RowForCard(*result.sTargetCardId)) {
			result.eEffect = E_DROP_IMPORT_LIST_EFFECT::PreserveHiddenTarget;
			return result;
		}
		projection.SelectVisibleCard(*result.sTargetCardId, domain::E_CARD_SELECTION_INTENT::Replace);
		result.eEffect = E_DROP_IMPORT_LIST_EFFECT::RevealVisibleTarget;
		return result;
	}
}
