#include <catch_amalgamated.hpp>

#include "pynote/core/application/first_input_capture.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment(lib, "NoteExCore")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;

	void emit(std::string_view id, std::string_view payload)
	{
		wchar_t path[32768] = {};
		const DWORD length = ::GetEnvironmentVariableW(L"PYNOTE_FIRST_INPUT_RECOVERY_GOLDEN_OUT", path, 32768);
		if (length == 0 || length >= 32768) { return; }
		std::ofstream output(std::filesystem::path(path), std::ios::binary |
			(id == "WTL-W2-0083" ? std::ios::trunc : std::ios::app));
		REQUIRE(output.is_open()); output << id << '|' << payload << '\n'; REQUIRE(output.good());
	}

	app::S_FIRST_INPUT_EVENT insertion(std::string text, std::int64_t cursor,
		domain::E_CAPTURE_OPERATION_SOURCE source)
	{
		return { std::move(text), cursor, source, true };
	}

	app::S_FIRST_INPUT_EVENT noninsertion(std::string text, std::int64_t cursor)
	{
		return { std::move(text), cursor, domain::E_CAPTURE_OPERATION_SOURCE::Typing, false };
	}

	class Fixture
	{
	public:
		Fixture()
			: path_(std::filesystem::temp_directory_path() /
				("noteex_w2r11_recovery_" + std::to_string(::GetCurrentProcessId()) + "_" +
				std::to_string(++sequence_) + ".db")), repositories_(database_)
		{
			remove_(); REQUIRE(database_.Open(path_.string()));
			storage::C_MIGRATION_RUNNER runner; runner.SetExistingDatabase(false, path_.string());
			REQUIRE(runner.Run(database_) == storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT document; document.sId = DocumentId; document.sTitle = "recovery";
			document.nCreatedAtUs = 1000; document.nUpdatedAtUs = 1000;
			REQUIRE(repositories_.CreateDocument(document) == storage::E_REPO_RESULT::Ok);
		}
		~Fixture() { database_.Close(); remove_(); }

		app::C_CARD_SERVICE Service()
		{
			return app::C_CARD_SERVICE(database_, repositories_, parser_,
				[this] { return clock_; }, [this] { return next_id_(); });
		}

		app::C_FIRST_INPUT_CAPTURE Capture(app::C_CARD_SERVICE& service,
			domain::C_CARD_LIST_PROJECTION& projection, app::LinkCardPort link,
			app::CreateOneCardPort create = {})
		{
			if (!create) {
				create = [&service](const std::string& text, domain::E_CAPTURE_OPERATION_SOURCE source) {
					domain::S_CARD card;
					if (service.CreateCard(DocumentId, text, source, std::nullopt, &card) != app::E_CARD_SERVICE_RESULT::Ok) {
						return std::optional<domain::S_CARD>{};
					}
					return std::optional<domain::S_CARD>(card);
				};
			}
			return app::C_FIRST_INPUT_CAPTURE(projection,
				[this](const std::string& text) { return parser_.Split(text).empty(); },
				std::move(create),
				[this](const std::string& id) {
					domain::S_CARD card;
					if (repositories_.GetCard(id, &card) != storage::E_REPO_RESULT::Ok || card.nDeletedAtUs) {
						return std::optional<domain::S_CARD>{};
					}
					return std::optional<domain::S_CARD>(card);
				}, std::move(link));
		}

		std::vector<domain::S_CARD> Cards()
		{
			std::vector<domain::S_CARD> cards;
			REQUIRE(repositories_.ListCards(DocumentId, &cards) == storage::E_REPO_RESULT::Ok); return cards;
		}
		std::vector<domain::S_EDIT_EVENT> Events()
		{
			std::vector<domain::S_EDIT_EVENT> events;
			REQUIRE(repositories_.ListEvents(DocumentId, &events) == storage::E_REPO_RESULT::Ok); return events;
		}
		domain::S_CAPTURE_OPERATION Operation(const domain::S_CARD& card)
		{
			domain::S_CAPTURE_OPERATION operation;
			REQUIRE(repositories_.GetCaptureOperation(card.sOperationId, &operation) == storage::E_REPO_RESULT::Ok); return operation;
		}
		void Delete(app::C_CARD_SERVICE& service, const std::string& id)
		{
			nextOverride_ = "delete-event";
			domain::S_CARD deleted;
			REQUIRE(service.SoftDelete(id, std::nullopt, false, std::nullopt, &deleted) == app::E_CARD_SERVICE_RESULT::Ok);
		}
		void SetClock(std::int64_t value) { clock_ = value; }
		inline static const std::string DocumentId = "document-recovery";

	private:
		std::string next_id_()
		{
			if (nextOverride_) { std::string value = std::move(*nextOverride_); nextOverride_.reset(); return value; }
			const int number = idCalls_ / 4 + 1; const int kind = idCalls_++ % 4;
			if (kind == 0) return "operation-" + std::to_string(number);
			if (kind == 1) return "card-" + std::to_string(number);
			if (kind == 2) return "revision-" + std::to_string(number);
			return "event-" + std::to_string(number);
		}
		void remove_() const
		{
			std::error_code error; std::filesystem::remove(path_, error);
			std::filesystem::remove(path_.string()+"-wal", error); std::filesystem::remove(path_.string()+"-shm", error);
		}
		std::filesystem::path path_; storage::C_DATABASE database_; storage::C_REPOSITORIES repositories_;
		domain::C_PARAGRAPH_PARSER parser_; std::int64_t clock_{ 2000 }; int idCalls_{ 0 };
		std::optional<std::string> nextOverride_{};
		inline static int sequence_{ 0 };
	};

	#define RECOVERY_TAGS(ID) "[W2-R11][core][application][first-input-recovery][" ID "]"
}

TEST_CASE("WTL-W2-0083", RECOVERY_TAGS("WTL-W2-0083"))
{
	Fixture f; auto s=f.Service(); domain::C_CARD_LIST_PROJECTION p; auto c=f.Capture(s,p,[](const auto&){return true;});
	const auto r=c.OnInputEvent(insertion("a",1,domain::E_CAPTURE_OPERATION_SOURCE::Typing)); const auto cards=f.Cards();
	REQUIRE(cards.size()==1);REQUIRE(cards[0].sBody=="a");REQUIRE(cards[0].eSource==domain::E_CARD_SOURCE::Typing);REQUIRE(r.bCreateAttempted);REQUIRE(r.bLinkAttempted);REQUIRE(r.eCreateOutcome==app::E_FIRST_INPUT_ATTEMPT_OUTCOME::Succeeded);REQUIRE(r.eLinkOutcome==app::E_FIRST_INPUT_ATTEMPT_OUTCOME::Succeeded);REQUIRE(r.ePhase==app::E_FIRST_INPUT_PHASE::Connected);REQUIRE(r.sConnectedCardId=="card-1");
	emit("WTL-W2-0083","event=insertion|source=typing|text=61|cards=1|active=1|phase=connected|connected=card-1");
}

TEST_CASE("WTL-W2-0084", RECOVERY_TAGS("WTL-W2-0084"))
{
	Fixture f; auto s=f.Service(); domain::C_CARD_LIST_PROJECTION p; auto c=f.Capture(s,p,[](const auto&){return true;}); const std::string text="\xeb\xb6\x99\xec\x97\xac\xeb\x84\xa3\xea\xb8\xb0";
	const auto r=c.OnInputEvent(insertion(text,4,domain::E_CAPTURE_OPERATION_SOURCE::Paste));const auto cards=f.Cards();REQUIRE(cards.size()==1);REQUIRE(cards[0].sBody==text);REQUIRE(cards[0].eSource==domain::E_CARD_SOURCE::Paste);REQUIRE(r.sConnectedCardId=="card-1");
	emit("WTL-W2-0084","event=insertion|source=paste|text=ebb699ec97aceb84a3eab8b0|cards=1|active=1|phase=connected|connected=card-1");
}

TEST_CASE("WTL-W2-0085", RECOVERY_TAGS("WTL-W2-0085"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;auto c=f.Capture(s,p,[](const auto&){return true;});auto r=c.OnInputEvent(insertion(" \n\t",3,domain::E_CAPTURE_OPERATION_SOURCE::Paste));
	REQUIRE(f.Cards().empty());REQUIRE(r.ePhase==app::E_FIRST_INPUT_PHASE::Awaiting);REQUIRE((c.ProtectionSnapshot()==app::S_NEW_BACKING_SNAPSHOT{" \n\t",3}));emit("WTL-W2-0085","event=insertion|source=paste|text=200a09|cards=0|active=0|phase=awaiting|backing=new");
}

TEST_CASE("WTL-W2-0086", RECOVERY_TAGS("WTL-W2-0086"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;int creates=0;app::CreateOneCardPort create=[&](const std::string& text,auto source){++creates;domain::S_CARD card;if(s.CreateCard(Fixture::DocumentId,text,source,std::nullopt,&card)!=app::E_CARD_SERVICE_RESULT::Ok)return std::optional<domain::S_CARD>{};return std::optional(card);};auto c=f.Capture(s,p,[](const auto&){return true;},create);
	c.OnInputEvent(insertion("a",1,domain::E_CAPTURE_OPERATION_SOURCE::Typing));std::string surface="abc";const auto r=c.OnInputEvent(insertion(surface,3,domain::E_CAPTURE_OPERATION_SOURCE::Typing));REQUIRE(f.Cards().size()==1);REQUIRE(creates==1);REQUIRE(r.ePhase==app::E_FIRST_INPUT_PHASE::Connected);emit("WTL-W2-0086","inputs=61,6263|surface=616263|cards=1|creates=1|phase=connected|connected=card-1");
}

TEST_CASE("WTL-W2-0087", RECOVERY_TAGS("WTL-W2-0087"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;auto c=f.Capture(s,p,[](const auto&){return true;});const std::string text="legacy NEW \xeb\xb3\xb8\xeb\xac\xb8";c.RestoreNewSnapshot({text,7});REQUIRE((c.ProtectionSnapshot()==app::S_NEW_BACKING_SNAPSHOT{text,7}));REQUIRE(f.Cards().empty());REQUIRE(c.Phase()==app::E_FIRST_INPUT_PHASE::Awaiting);emit("WTL-W2-0087","restore=draft-legacy|text=6c6567616379204e455720ebb3b8ebacb8|cursor=7|cards=0|phase=awaiting");
}

TEST_CASE("WTL-W2-0088", RECOVERY_TAGS("WTL-W2-0088"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;auto c=f.Capture(s,p,[](const auto&){return true;});const std::string text="\xeb\xb3\xb5\xec\x9b\x90\xeb\xac\xb8\n\xf0\x9f\x98\x80";c.RestoreNewSnapshot({text,6});REQUIRE((c.ProtectionSnapshot()==app::S_NEW_BACKING_SNAPSHOT{text,6}));REQUIRE(f.Cards().empty());REQUIRE(f.Events().empty());emit("WTL-W2-0088","restore=draft-settings|text=ebb3b5ec9b90ebacb80af09f9880|cursor=6|cards=0|events=0|phase=awaiting");
}

TEST_CASE("WTL-W2-0089", RECOVERY_TAGS("WTL-W2-0089"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;const std::string text="paste \xec\x8b\xa4\xed\x8c\xa8 \xf0\x9f\x98\x80";auto c=f.Capture(s,p,[](const auto&){return true;},[](const auto&,auto){return std::optional<domain::S_CARD>{};});auto r=c.OnInputEvent(insertion(text,11,domain::E_CAPTURE_OPERATION_SOURCE::Paste));REQUIRE(r.eCreateOutcome==app::E_FIRST_INPUT_ATTEMPT_OUTCOME::Rejected);auto snapshot=c.ProtectionSnapshot();REQUIRE((snapshot==app::S_NEW_BACKING_SNAPSHOT{text,11}));domain::C_CARD_LIST_PROJECTION p2;auto restart=f.Capture(s,p2,[](const auto&){return true;});restart.RestoreNewSnapshot(*snapshot);REQUIRE(f.Cards().empty());emit("WTL-W2-0089","failure=return|source=paste|text=706173746520ec8ba4ed8ca820f09f9880|cards=0|protected=1|restart_cards=0|phase=awaiting");
}

TEST_CASE("WTL-W2-0090", RECOVERY_TAGS("WTL-W2-0090"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;auto c=f.Capture(s,p,[](const auto&){return true;},[](const auto&,auto)->std::optional<domain::S_CARD>{throw std::runtime_error("create");});auto r=c.OnInputEvent(insertion("A",1,domain::E_CAPTURE_OPERATION_SOURCE::Typing));REQUIRE(r.eCreateOutcome==app::E_FIRST_INPUT_ATTEMPT_OUTCOME::Exception);auto snapshot=c.ProtectionSnapshot();REQUIRE((snapshot==app::S_NEW_BACKING_SNAPSHOT{"A",1}));domain::C_CARD_LIST_PROJECTION p2;auto restart=f.Capture(s,p2,[](const auto&){return true;});restart.RestoreNewSnapshot(*snapshot);REQUIRE(f.Cards().empty());emit("WTL-W2-0090","failure=exception|source=typing|text=41|cards=0|protected=1|restart_cards=0|phase=awaiting");
}

TEST_CASE("WTL-W2-0091", RECOVERY_TAGS("WTL-W2-0091"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;int calls=0;std::vector<domain::E_CAPTURE_OPERATION_SOURCE> sources;app::CreateOneCardPort create=[&](const std::string& text,auto source){sources.push_back(source);if(++calls==1)return std::optional<domain::S_CARD>{};domain::S_CARD card;REQUIRE(s.CreateCard(Fixture::DocumentId,text,source,std::nullopt,&card)==app::E_CARD_SERVICE_RESULT::Ok);return std::optional(card);};auto c=f.Capture(s,p,[](const auto&){return true;},create);const auto first=c.OnInputEvent(insertion("ABC",3,domain::E_CAPTURE_OPERATION_SOURCE::Paste));REQUIRE(first.eCreateOutcome==app::E_FIRST_INPUT_ATTEMPT_OUTCOME::Rejected);c.OnInputEvent(noninsertion("AB",2));c.OnInputEvent(noninsertion("ABC",3));c.OnInputEvent(noninsertion("ABC",3));REQUIRE(calls==1);c.OnInputEvent(insertion("ABCD",4,domain::E_CAPTURE_OPERATION_SOURCE::Typing));auto cards=f.Cards();REQUIRE(calls==2);REQUIRE((sources==std::vector<domain::E_CAPTURE_OPERATION_SOURCE>{domain::E_CAPTURE_OPERATION_SOURCE::Paste,domain::E_CAPTURE_OPERATION_SOURCE::Typing}));REQUIRE(cards.size()==1);REQUIRE(cards[0].sBody=="ABCD");REQUIRE(cards[0].eSource==domain::E_CARD_SOURCE::Typing);emit("WTL-W2-0091","first=paste-return|noninsert=delete,undo,format|calls_before_retry=1|retry=typing|surface=41424344|cards=1|source=typing");
}

TEST_CASE("WTL-W2-0092", RECOVERY_TAGS("WTL-W2-0092"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;std::vector<std::string> attempts;auto c=f.Capture(s,p,[&](const auto& card){attempts.push_back(card.sId);return attempts.size()>1;});const auto first=c.OnInputEvent(insertion("a",1,domain::E_CAPTURE_OPERATION_SOURCE::Typing));auto r=c.OnInputEvent(insertion("ab",2,domain::E_CAPTURE_OPERATION_SOURCE::Typing));REQUIRE(first.eLinkOutcome==app::E_FIRST_INPUT_ATTEMPT_OUTCOME::Rejected);REQUIRE((attempts==std::vector<std::string>{"card-1","card-1"}));REQUIRE(f.Cards().size()==1);REQUIRE(r.ePhase==app::E_FIRST_INPUT_PHASE::Connected);emit("WTL-W2-0092","failure=return|attempts=card-1,card-1|cards=1|surface=6162|phase=connected|connected=card-1");
}

TEST_CASE("WTL-W2-0093", RECOVERY_TAGS("WTL-W2-0093"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;std::vector<std::string> attempts;auto c=f.Capture(s,p,[&](const auto& card){attempts.push_back(card.sId);if(attempts.size()==1)throw std::runtime_error("link");return true;});const auto first=c.OnInputEvent(insertion("a",1,domain::E_CAPTURE_OPERATION_SOURCE::Typing));auto r=c.OnInputEvent(insertion("ab",2,domain::E_CAPTURE_OPERATION_SOURCE::Typing));REQUIRE(first.eLinkOutcome==app::E_FIRST_INPUT_ATTEMPT_OUTCOME::Exception);REQUIRE((attempts==std::vector<std::string>{"card-1","card-1"}));REQUIRE(f.Cards().size()==1);REQUIRE(r.ePhase==app::E_FIRST_INPUT_PHASE::Connected);emit("WTL-W2-0093","failure=exception|attempts=card-1,card-1|cards=1|surface=6162|phase=connected|connected=card-1");
}

TEST_CASE("WTL-W2-0094", RECOVERY_TAGS("WTL-W2-0094"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;std::vector<std::string> attempts;auto c=f.Capture(s,p,[&](const auto& card){attempts.push_back(card.sId);return false;});c.OnInputEvent(insertion("a",1,domain::E_CAPTURE_OPERATION_SOURCE::Typing));c.OnInputEvent(insertion("ab",2,domain::E_CAPTURE_OPERATION_SOURCE::Typing));c.OnInputEvent(insertion("abc",3,domain::E_CAPTURE_OPERATION_SOURCE::Typing));f.Delete(s,"card-1");auto r=c.OnInputEvent(insertion("abcd",4,domain::E_CAPTURE_OPERATION_SOURCE::Typing));auto cards=f.Cards();REQUIRE((attempts==std::vector<std::string>{"card-1","card-1","card-1","card-2"}));REQUIRE(cards.size()==2);REQUIRE(cards[0].nDeletedAtUs.has_value());REQUIRE_FALSE(cards[1].nDeletedAtUs);REQUIRE(r.sPendingCardId=="card-2");emit("WTL-W2-0094","attempts=card-1,card-1,card-1,card-2|cards=2|active=1|deleted=card-1|pending=card-2|phase=pending-link");
}

TEST_CASE("WTL-W2-0095", RECOVERY_TAGS("WTL-W2-0095"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;const std::string base="\xec\x9e\xac\xea\xb8\xb0\xeb\x8f\x99 \xea\xb2\xbd\xea\xb3\x84";auto c=f.Capture(s,p,[](const auto&){return false;});c.OnInputEvent(insertion(base,6,domain::E_CAPTURE_OPERATION_SOURCE::Paste));auto snapshot=c.ProtectionSnapshot();REQUIRE(snapshot);REQUIRE(f.Cards().size()==1);domain::C_CARD_LIST_PROJECTION p2;auto restart=f.Capture(s,p2,[](const auto&){return true;});restart.RestoreNewSnapshot(*snapshot);const auto r=restart.OnInputEvent(insertion(base+"!",7,domain::E_CAPTURE_OPERATION_SOURCE::Typing));auto cards=f.Cards();REQUIRE(cards.size()==2);REQUIRE(cards[0].sBody==base);REQUIRE(cards[1].sBody==base+"!");REQUIRE(r.ePhase==app::E_FIRST_INPUT_PHASE::Connected);emit("WTL-W2-0095","failure=return|before_cards=1|protected=1|restart_surface=ec9eaceab8b0eb8f9920eab2bdeab38421|after_cards=2|bodies=ec9eaceab8b0eb8f9920eab2bdeab384,ec9eaceab8b0eb8f9920eab2bdeab38421|phase=connected");
}

TEST_CASE("WTL-W2-0096", RECOVERY_TAGS("WTL-W2-0096"))
{
	Fixture f;auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;const std::string base="\xec\x9e\xac\xea\xb8\xb0\xeb\x8f\x99 \xea\xb2\xbd\xea\xb3\x84";auto c=f.Capture(s,p,[](const auto&)->bool{throw std::runtime_error("link");});c.OnInputEvent(insertion(base,6,domain::E_CAPTURE_OPERATION_SOURCE::Paste));auto snapshot=c.ProtectionSnapshot();REQUIRE(snapshot);REQUIRE(f.Cards().size()==1);domain::C_CARD_LIST_PROJECTION p2;auto restart=f.Capture(s,p2,[](const auto&){return true;});restart.RestoreNewSnapshot(*snapshot);const auto r=restart.OnInputEvent(insertion(base+"!",7,domain::E_CAPTURE_OPERATION_SOURCE::Typing));auto cards=f.Cards();REQUIRE(cards.size()==2);REQUIRE(cards[0].sBody==base);REQUIRE(cards[1].sBody==base+"!");REQUIRE(r.ePhase==app::E_FIRST_INPUT_PHASE::Connected);emit("WTL-W2-0096","failure=exception|before_cards=1|protected=1|restart_surface=ec9eaceab8b0eb8f9920eab2bdeab38421|after_cards=2|bodies=ec9eaceab8b0eb8f9920eab2bdeab384,ec9eaceab8b0eb8f9920eab2bdeab38421|phase=connected");
}

TEST_CASE("WTL-W2-0097", RECOVERY_TAGS("WTL-W2-0097"))
{
	Fixture f;f.SetClock(88888);auto s=f.Service();domain::C_CARD_LIST_PROJECTION p;auto c=f.Capture(s,p,[](const auto&){return true;});c.RestoreNewSnapshot({" \n\t",3});auto r=c.OnInputEvent(insertion(" \n\tA",4,domain::E_CAPTURE_OPERATION_SOURCE::Typing));auto cards=f.Cards();REQUIRE(cards.size()==1);REQUIRE(cards[0].sBody==" \n\tA");REQUIRE(cards[0].nCreatedAtUs==88888);REQUIRE(cards[0].nCaptureSeq==1);REQUIRE(cards[0].eSource==domain::E_CARD_SOURCE::Typing);REQUIRE(r.ePhase==app::E_FIRST_INPUT_PHASE::Connected);emit("WTL-W2-0097","restored=200a09|promoted=200a0941|created_at=88888|capture_seq=1|source=typing|cursor=4|cards=1");
}

#undef RECOVERY_TAGS
