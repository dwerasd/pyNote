#pragma once

#include "pynote/core/application/import_pipeline.h"
#include "pynote/core/domain/models.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pynote::core::application
{
	inline constexpr std::size_t MAX_DROP_IMPORT_CANDIDATES = 20;
	inline constexpr std::size_t MAX_DROP_IMPORT_TOTAL_BYTES = 4u * 1024u * 1024u;
	inline constexpr std::size_t DROP_IMPORT_READ_LIMIT = MAX_IMPORT_FILE_BYTES + 1;

	enum class E_DROP_PATH_KIND { LocalRegularFile, NonLocal, Directory, Missing, Other };
	enum class E_DROP_IMPORT_FAILURE_KIND {
		TooManyFiles, NonLocal, Directory, Missing, NotRegular,
		ReadFailed, FileTooLarge, Blank, TotalTooLarge, CreateFailed
	};
	enum class E_DROP_IMPORT_POST_ACTION { None, ConnectOnlyCreated, RevealLastCreated };

	struct S_DROP_IMPORT_CANDIDATE
	{
		std::string sPath;
		std::string sStablePathKey;
		bool operator==(const S_DROP_IMPORT_CANDIDATE&) const = default;
	};

	struct S_DROP_IMPORT_REQUEST
	{
		std::string sWindowId;
		std::string sDocumentId;
		std::string sCorrelationId;
		std::vector<S_DROP_IMPORT_CANDIDATE> Candidates;
	};

	struct S_DROP_IMPORT_PREPARED_ITEM
	{
		S_DROP_IMPORT_CANDIDATE Candidate;
		std::vector<std::uint8_t> Snapshot;
		std::string sText;
	};

	struct S_DROP_IMPORT_FAILURE
	{
		std::string sPath;
		E_DROP_IMPORT_FAILURE_KIND eKind{ E_DROP_IMPORT_FAILURE_KIND::ReadFailed };
		std::string sDetail;
		bool operator==(const S_DROP_IMPORT_FAILURE&) const = default;
	};

	struct S_DROP_IMPORT_PLAN
	{
		S_DROP_IMPORT_REQUEST Request;
		std::vector<S_DROP_IMPORT_CANDIDATE> UniqueCandidates;
		std::vector<S_DROP_IMPORT_PREPARED_ITEM> PreparedItems;
		std::vector<S_DROP_IMPORT_FAILURE> ReadFailures;
		bool bBatchFatal{ false };
	};

	struct S_DROP_IMPORT_EXECUTION
	{
		std::string sWindowId;
		std::string sDocumentId;
		std::string sCorrelationId;
		std::vector<S_DROP_IMPORT_FAILURE> ReadFailures;
		std::vector<S_DROP_IMPORT_FAILURE> CreateFailures;
		std::vector<std::string> CreatedCardIds;
		bool bLeaveGateCalled{ false };
		bool bLeaveGateAccepted{ false };
		E_DROP_IMPORT_POST_ACTION ePostAction{ E_DROP_IMPORT_POST_ACTION::None };
	};

	using DropPathInspector = std::function<E_DROP_PATH_KIND(const S_DROP_IMPORT_CANDIDATE&)>;
	using DropLeaveGate = std::function<bool(bool)>;
	using ImportCardCreator = std::function<bool(
		const std::string&, const std::string&, domain::E_CAPTURE_OPERATION_SOURCE,
		std::string*, std::string*)>;

	class C_DROP_IMPORT_COORDINATOR
	{
	public:
		C_DROP_IMPORT_COORDINATOR(const C_IMPORT_PIPELINE& _Pipeline,
			DropPathInspector _Inspector, BoundedFileReader _Reader,
			DropLeaveGate _LeaveGate, ImportCardCreator _Creator);

		S_DROP_IMPORT_PLAN Prepare(const S_DROP_IMPORT_REQUEST& _Request) const;
		S_DROP_IMPORT_EXECUTION Execute(const S_DROP_IMPORT_PLAN& _Plan) const;

	private:
		const C_IMPORT_PIPELINE& m_Pipeline;
		DropPathInspector m_Inspector;
		BoundedFileReader m_Reader;
		DropLeaveGate m_LeaveGate;
		ImportCardCreator m_Creator;
	};
}
