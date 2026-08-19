#include "pynote/core/application/drop_import_plan.h"

#include <set>
#include <stdexcept>
#include <utility>

namespace pynote::core::application
{
	namespace
	{
		E_DROP_IMPORT_FAILURE_KIND structural_failure(E_DROP_PATH_KIND kind)
		{
			switch (kind) {
			case E_DROP_PATH_KIND::NonLocal: return E_DROP_IMPORT_FAILURE_KIND::NonLocal;
			case E_DROP_PATH_KIND::Directory: return E_DROP_IMPORT_FAILURE_KIND::Directory;
			case E_DROP_PATH_KIND::Missing: return E_DROP_IMPORT_FAILURE_KIND::Missing;
			default: return E_DROP_IMPORT_FAILURE_KIND::NotRegular;
			}
		}
	}

	C_DROP_IMPORT_COORDINATOR::C_DROP_IMPORT_COORDINATOR(const C_IMPORT_PIPELINE& pipeline,
		DropPathInspector inspector, BoundedFileReader reader,
		DropLeaveGate leaveGate, ImportCardCreator creator)
		: m_Pipeline(pipeline), m_Inspector(std::move(inspector)), m_Reader(std::move(reader)),
		  m_LeaveGate(std::move(leaveGate)), m_Creator(std::move(creator))
	{
		if (!m_Inspector || !m_Reader || !m_LeaveGate || !m_Creator) {
			throw std::invalid_argument("drop import ports are required");
		}
	}

	S_DROP_IMPORT_PLAN C_DROP_IMPORT_COORDINATOR::Prepare(const S_DROP_IMPORT_REQUEST& request) const
	{
		S_DROP_IMPORT_PLAN plan;
		plan.Request = request;
		if (request.Candidates.size() > MAX_DROP_IMPORT_CANDIDATES) {
			plan.ReadFailures.push_back({{}, E_DROP_IMPORT_FAILURE_KIND::TooManyFiles, "too-many-files"});
			plan.bBatchFatal = true;
			return plan;
		}

		for (const auto& candidate : request.Candidates) {
			const auto kind = m_Inspector(candidate);
			if (kind != E_DROP_PATH_KIND::LocalRegularFile) {
				plan.ReadFailures.push_back({candidate.sPath, structural_failure(kind), "structural-reject"});
			}
		}
		if (!plan.ReadFailures.empty()) {
			plan.bBatchFatal = true;
			return plan;
		}

		std::set<std::string> seen;
		for (const auto& candidate : request.Candidates) {
			if (seen.insert(candidate.sStablePathKey).second) { plan.UniqueCandidates.push_back(candidate); }
		}

		std::size_t totalBytes = 0;
		for (const auto& candidate : plan.UniqueCandidates) {
			std::vector<std::uint8_t> snapshot;
			std::string error;
			if (!m_Reader(candidate.sPath, DROP_IMPORT_READ_LIMIT, &snapshot, &error)) {
				plan.ReadFailures.push_back({candidate.sPath, E_DROP_IMPORT_FAILURE_KIND::ReadFailed, std::move(error)});
				continue;
			}
			if (snapshot.size() > MAX_IMPORT_FILE_BYTES) {
				plan.ReadFailures.push_back({candidate.sPath, E_DROP_IMPORT_FAILURE_KIND::FileTooLarge, "file-too-large"});
				continue;
			}
			if (snapshot.size() > MAX_DROP_IMPORT_TOTAL_BYTES - totalBytes) {
				plan.ReadFailures.push_back({candidate.sPath, E_DROP_IMPORT_FAILURE_KIND::TotalTooLarge, "total-too-large"});
				plan.PreparedItems.clear();
				plan.bBatchFatal = true;
				return plan;
			}
			totalBytes += snapshot.size();
			S_IMPORT_PREPARATION preparation;
			const auto result = m_Pipeline.PrepareFromBytes(candidate.sPath, snapshot, &preparation);
			if (result == E_IMPORT_RESULT::Blank) {
				plan.ReadFailures.push_back({candidate.sPath, E_DROP_IMPORT_FAILURE_KIND::Blank, "blank"});
				continue;
			}
			if (result != E_IMPORT_RESULT::Ok) {
				plan.ReadFailures.push_back({candidate.sPath, E_DROP_IMPORT_FAILURE_KIND::ReadFailed, "prepare-failed"});
				continue;
			}
			plan.PreparedItems.push_back({candidate, std::move(snapshot), std::move(preparation.sText)});
		}
		return plan;
	}

	S_DROP_IMPORT_EXECUTION C_DROP_IMPORT_COORDINATOR::Execute(const S_DROP_IMPORT_PLAN& plan) const
	{
		S_DROP_IMPORT_EXECUTION execution;
		execution.sWindowId = plan.Request.sWindowId;
		execution.sDocumentId = plan.Request.sDocumentId;
		execution.sCorrelationId = plan.Request.sCorrelationId;
		execution.ReadFailures = plan.ReadFailures;
		if (plan.bBatchFatal || plan.PreparedItems.empty()) { return execution; }
		execution.bLeaveGateCalled = true;
		execution.bLeaveGateAccepted = m_LeaveGate(true);
		if (!execution.bLeaveGateAccepted) { return execution; }

		for (const auto& item : plan.PreparedItems) {
			std::string cardId;
			std::string error;
			if (!m_Creator(plan.Request.sDocumentId, item.sText,
				domain::E_CAPTURE_OPERATION_SOURCE::Import, &cardId, &error)) {
				execution.CreateFailures.push_back({item.Candidate.sPath,
					E_DROP_IMPORT_FAILURE_KIND::CreateFailed, std::move(error)});
				continue;
			}
			execution.CreatedCardIds.push_back(std::move(cardId));
		}
		if (execution.CreatedCardIds.size() == 1) {
			execution.ePostAction = E_DROP_IMPORT_POST_ACTION::ConnectOnlyCreated;
		}
		else if (execution.CreatedCardIds.size() > 1) {
			execution.ePostAction = E_DROP_IMPORT_POST_ACTION::RevealLastCreated;
		}
		return execution;
	}
}
