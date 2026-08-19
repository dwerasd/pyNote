#include "pynote/core/domain/card_drag_session_registry.h"

#include <utility>

namespace pynote::core::domain
{
	C_CARD_DRAG_SESSION_REGISTRY::C_SOURCE_REGISTRATION::C_SOURCE_REGISTRATION(
		C_CARD_DRAG_SESSION_REGISTRY* registry,CardDragSourceIdentity source) noexcept
		: m_pRegistry(registry),m_nSource(source) {}

	C_CARD_DRAG_SESSION_REGISTRY::C_SOURCE_REGISTRATION::~C_SOURCE_REGISTRATION(){this->Reset();}
	C_CARD_DRAG_SESSION_REGISTRY::C_SOURCE_REGISTRATION::C_SOURCE_REGISTRATION(C_SOURCE_REGISTRATION&& other) noexcept
		: m_pRegistry(std::exchange(other.m_pRegistry,nullptr)),m_nSource(std::exchange(other.m_nSource,0)) {}
	C_CARD_DRAG_SESSION_REGISTRY::C_SOURCE_REGISTRATION& C_CARD_DRAG_SESSION_REGISTRY::C_SOURCE_REGISTRATION::operator=(C_SOURCE_REGISTRATION&& other) noexcept
	{
		if(this!=&other){this->Reset();m_pRegistry=std::exchange(other.m_pRegistry,nullptr);m_nSource=std::exchange(other.m_nSource,0);}return *this;
	}
	void C_CARD_DRAG_SESSION_REGISTRY::C_SOURCE_REGISTRATION::Reset() noexcept
	{
		if(m_pRegistry!=nullptr){m_pRegistry->UnregisterSource(m_nSource);m_pRegistry=nullptr;m_nSource=0;}
	}

	C_CARD_DRAG_SESSION_REGISTRY::C_CARD_DRAG_SESSION_REGISTRY(CardDragTokenSupplier supplier)
		: m_TokenSupplier(std::move(supplier)) {}

	C_CARD_DRAG_SESSION_REGISTRY::C_SOURCE_REGISTRATION C_CARD_DRAG_SESSION_REGISTRY::RegisterSource(CardDragSourceIdentity source)
	{
		if(source==0)return {};m_RegisteredSources.insert(source);return C_SOURCE_REGISTRATION(this,source);
	}

	void C_CARD_DRAG_SESSION_REGISTRY::UnregisterSource(CardDragSourceIdentity source) noexcept
	{
		m_RegisteredSources.erase(source);
		for(auto iterator=m_Sessions.begin();iterator!=m_Sessions.end();){if(iterator->second.nSource==source)iterator=m_Sessions.erase(iterator);else ++iterator;}
	}

	std::optional<CardDragSessionToken> C_CARD_DRAG_SESSION_REGISTRY::BeginSession(CardDragSourceIdentity source,
		std::string cardId,std::optional<std::string> revisionId)
	{
		if(source==0||!m_RegisteredSources.contains(source)||cardId.empty())return std::nullopt;
		const auto token=this->next_token_();m_Sessions.emplace(token,S_SESSION{source,std::move(cardId),std::move(revisionId)});return token;
	}

	bool C_CARD_DRAG_SESSION_REGISTRY::Validate(CardDragSessionToken token,CardDragSourceIdentity source,
		const std::string& cardId,const std::optional<std::string>& revisionId) const noexcept
	{
		const auto found=m_Sessions.find(token);return token!=0&&found!=m_Sessions.end()
			&&m_RegisteredSources.contains(source)&&found->second.nSource==source
			&&found->second.sCardId==cardId&&found->second.sRevisionId==revisionId;
	}

	void C_CARD_DRAG_SESSION_REGISTRY::EndSession(CardDragSessionToken token) noexcept{m_Sessions.erase(token);}

	CardDragSessionToken C_CARD_DRAG_SESSION_REGISTRY::next_token_()
	{
		if(m_TokenSupplier){const auto supplied=m_TokenSupplier();if(supplied!=0&&!m_Sessions.contains(supplied))return supplied;}
		for(;;){const auto token=m_nFallbackToken++;if(token!=0&&!m_Sessions.contains(token))return token;}
	}
}
