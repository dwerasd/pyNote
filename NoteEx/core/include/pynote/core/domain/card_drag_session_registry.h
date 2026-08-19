#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace pynote::core::domain
{
	using CardDragSourceIdentity = std::uintptr_t;
	using CardDragSessionToken = std::uint64_t;
	using CardDragTokenSupplier = std::function<CardDragSessionToken()>;

	class C_CARD_DRAG_SESSION_REGISTRY
	{
	public:
		class C_SOURCE_REGISTRATION
		{
		public:
			C_SOURCE_REGISTRATION() = default;
			~C_SOURCE_REGISTRATION();
			C_SOURCE_REGISTRATION(const C_SOURCE_REGISTRATION&) = delete;
			C_SOURCE_REGISTRATION& operator=(const C_SOURCE_REGISTRATION&) = delete;
			C_SOURCE_REGISTRATION(C_SOURCE_REGISTRATION&& _Other) noexcept;
			C_SOURCE_REGISTRATION& operator=(C_SOURCE_REGISTRATION&& _Other) noexcept;
			void Reset() noexcept;
		private:
			friend class C_CARD_DRAG_SESSION_REGISTRY;
			C_SOURCE_REGISTRATION(C_CARD_DRAG_SESSION_REGISTRY* _pRegistry,CardDragSourceIdentity _nSource) noexcept;
			C_CARD_DRAG_SESSION_REGISTRY* m_pRegistry{};
			CardDragSourceIdentity m_nSource{};
		};

		explicit C_CARD_DRAG_SESSION_REGISTRY(CardDragTokenSupplier _TokenSupplier = {});
		C_SOURCE_REGISTRATION RegisterSource(CardDragSourceIdentity _nSource);
		void UnregisterSource(CardDragSourceIdentity _nSource) noexcept;
		std::optional<CardDragSessionToken> BeginSession(CardDragSourceIdentity _nSource,
			std::string _sCardId,std::optional<std::string> _sRevisionId);
		bool Validate(CardDragSessionToken _nToken,CardDragSourceIdentity _nSource,
			const std::string& _sCardId,const std::optional<std::string>& _sRevisionId) const noexcept;
		void EndSession(CardDragSessionToken _nToken) noexcept;

	private:
		struct S_SESSION
		{
			CardDragSourceIdentity nSource{};
			std::string sCardId;
			std::optional<std::string> sRevisionId{};
		};
		CardDragSessionToken next_token_();
		CardDragTokenSupplier m_TokenSupplier;
		CardDragSessionToken m_nFallbackToken{ 1 };
		std::set<CardDragSourceIdentity> m_RegisteredSources;
		std::map<CardDragSessionToken,S_SESSION> m_Sessions;
	};
}
