#include "pynote/core/domain/card_wheel_browse.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pynote::core::domain
{
	C_CARD_WHEEL_BROWSE::C_CARD_WHEEL_BROWSE(C_CARD_LIST_PROJECTION& projection,
		CardWheelClock clock, CardWheelScheduler scheduler)
		: m_Projection(projection), m_Clock(std::move(clock)), m_Scheduler(std::move(scheduler))
	{
		if (!m_Clock || !m_Scheduler) { throw std::invalid_argument("wheel browse ports are required"); }
	}

	S_CARD_WHEEL_RESULT C_CARD_WHEEL_BROWSE::OnVerticalAngle(int angle)
	{
		if (angle == 0 || m_Projection.RowCount() == 0) { return this->result_(false); }
		if (m_nAngleRemainder != 0 && (angle > 0) != (m_nAngleRemainder > 0)) {
			m_nAngleRemainder = 0;
		}
		m_nAngleRemainder += angle;
		const int steps = m_nAngleRemainder / CARD_WHEEL_VERTICAL_ANGLE_STEP;
		m_nAngleRemainder -= steps * CARD_WHEEL_VERTICAL_ANGLE_STEP;
		if (steps != 0) {
			const auto current = m_Projection.CurrentCardId()
				? m_Projection.RowForCard(*m_Projection.CurrentCardId()) : std::nullopt;
			std::ptrdiff_t target = current
				? static_cast<std::ptrdiff_t>(*current) - steps
				: (steps < 0 ? 0 : static_cast<std::ptrdiff_t>(m_Projection.RowCount() - 1));
			target = (std::clamp)(target, std::ptrdiff_t{0},
				static_cast<std::ptrdiff_t>(m_Projection.RowCount() - 1));
			const auto* card = m_Projection.CardAt(static_cast<std::size_t>(target));
			if (card != nullptr) {
				m_Projection.SelectVisibleCard(card->sId, E_CARD_SELECTION_INTENT::Replace);
				m_sPendingCardId = card->sId;
			}
		}
		if (m_sPendingCardId) { this->arm_(); }
		return this->result_(true);
	}

	std::optional<S_CARD_WHEEL_OPEN_REQUEST> C_CARD_WHEEL_BROWSE::OnTimer(std::uint64_t generation)
	{
		if (!m_sPendingCardId || generation != m_nGeneration) { return std::nullopt; }
		const auto cardId = *m_sPendingCardId;
		m_sPendingCardId.reset();
		m_nDeadlineMs.reset();
		if (!m_Projection.CurrentCardId() || *m_Projection.CurrentCardId() != cardId) {
			return std::nullopt;
		}
		m_ActiveOpen = S_CARD_WHEEL_OPEN_REQUEST{generation, cardId};
		return m_ActiveOpen;
	}

	void C_CARD_WHEEL_BROWSE::Cancel()
	{
		const auto cancelledGeneration = m_nGeneration;
		this->next_generation_();
		m_nAngleRemainder = 0;
		m_sPendingCardId.reset();
		m_nDeadlineMs.reset();
		m_ActiveOpen.reset();
		m_Scheduler({E_CARD_WHEEL_TIMER_OPERATION::Cancel, cancelledGeneration, std::nullopt});
	}

	void C_CARD_WHEEL_BROWSE::SetEditorCardId(std::optional<std::string> cardId)
	{
		m_sEditorCardId = std::move(cardId);
	}

	E_CARD_WHEEL_FOCUS_EFFECT C_CARD_WHEEL_BROWSE::CompleteOpen(
		const S_CARD_WHEEL_OPEN_REQUEST& request, bool opened)
	{
		if (!m_ActiveOpen || *m_ActiveOpen != request || request.nGeneration != m_nGeneration) {
			return E_CARD_WHEEL_FOCUS_EFFECT::None;
		}
		m_ActiveOpen.reset();
		if (opened || !m_sEditorCardId || !m_Projection.RowForCard(*m_sEditorCardId)) {
			return E_CARD_WHEEL_FOCUS_EFFECT::None;
		}
		m_Projection.SelectVisibleCard(*m_sEditorCardId, E_CARD_SELECTION_INTENT::Replace);
		return E_CARD_WHEEL_FOCUS_EFFECT::FocusEditor;
	}

	std::uint64_t C_CARD_WHEEL_BROWSE::next_generation_() noexcept
	{
		++m_nGeneration;
		if (m_nGeneration == 0) { ++m_nGeneration; }
		return m_nGeneration;
	}

	void C_CARD_WHEEL_BROWSE::arm_()
	{
		const auto generation = this->next_generation_();
		m_nDeadlineMs = m_Clock() + CARD_WHEEL_QUIET_OPEN_DELAY_MS;
		m_Scheduler({E_CARD_WHEEL_TIMER_OPERATION::Arm, generation, m_nDeadlineMs});
	}

	S_CARD_WHEEL_RESULT C_CARD_WHEEL_BROWSE::result_(bool handled) const
	{
		return {handled,
			m_Projection.CurrentCardId() ? m_Projection.RowForCard(*m_Projection.CurrentCardId()) : std::nullopt,
			m_nAngleRemainder, m_sPendingCardId,
			m_sPendingCardId ? std::optional<std::uint64_t>(m_nGeneration) : std::nullopt,
			m_nDeadlineMs};
	}
}
