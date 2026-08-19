#pragma once

#include "pynote/core/domain/card_list_projection.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace pynote::core::domain
{
	inline constexpr int CARD_WHEEL_VERTICAL_ANGLE_STEP = 120;
	inline constexpr std::int64_t CARD_WHEEL_QUIET_OPEN_DELAY_MS = 120;

	enum class E_CARD_WHEEL_TIMER_OPERATION { Arm, Cancel };
	enum class E_CARD_WHEEL_FOCUS_EFFECT { None, FocusEditor };

	struct S_CARD_WHEEL_TIMER_COMMAND
	{
		E_CARD_WHEEL_TIMER_OPERATION eOperation{ E_CARD_WHEEL_TIMER_OPERATION::Cancel };
		std::uint64_t nGeneration{};
		std::optional<std::int64_t> nDeadlineMs{};
		bool operator==(const S_CARD_WHEEL_TIMER_COMMAND&) const = default;
	};

	struct S_CARD_WHEEL_RESULT
	{
		bool bHandled{ false };
		std::optional<std::size_t> nCurrentRow{};
		int nAngleRemainder{};
		std::optional<std::string> sPendingCardId{};
		std::optional<std::uint64_t> nGeneration{};
		std::optional<std::int64_t> nDeadlineMs{};
	};

	struct S_CARD_WHEEL_OPEN_REQUEST
	{
		std::uint64_t nGeneration{};
		std::string sCardId;
		bool operator==(const S_CARD_WHEEL_OPEN_REQUEST&) const = default;
	};

	using CardWheelClock = std::function<std::int64_t()>;
	using CardWheelScheduler = std::function<void(const S_CARD_WHEEL_TIMER_COMMAND&)>;

	class C_CARD_WHEEL_BROWSE
	{
	public:
		C_CARD_WHEEL_BROWSE(C_CARD_LIST_PROJECTION& _Projection,
			CardWheelClock _Clock, CardWheelScheduler _Scheduler);

		S_CARD_WHEEL_RESULT OnVerticalAngle(int _nAngle);
		std::optional<S_CARD_WHEEL_OPEN_REQUEST> OnTimer(std::uint64_t _nGeneration);
		void Cancel();
		void SetEditorCardId(std::optional<std::string> _sCardId);
		const std::optional<std::string>& EditorCardId() const noexcept { return m_sEditorCardId; }
		E_CARD_WHEEL_FOCUS_EFFECT CompleteOpen(
			const S_CARD_WHEEL_OPEN_REQUEST& _Request, bool _bOpened);

		int AngleRemainder() const noexcept { return m_nAngleRemainder; }
		const std::optional<std::string>& PendingCardId() const noexcept { return m_sPendingCardId; }

	private:
		std::uint64_t next_generation_() noexcept;
		void arm_();
		S_CARD_WHEEL_RESULT result_(bool _bHandled) const;

		C_CARD_LIST_PROJECTION& m_Projection;
		CardWheelClock m_Clock;
		CardWheelScheduler m_Scheduler;
		int m_nAngleRemainder{};
		std::uint64_t m_nGeneration{};
		std::optional<std::string> m_sPendingCardId{};
		std::optional<std::int64_t> m_nDeadlineMs{};
		std::optional<S_CARD_WHEEL_OPEN_REQUEST> m_ActiveOpen{};
		std::optional<std::string> m_sEditorCardId{};
	};
}
