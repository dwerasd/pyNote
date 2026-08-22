#include <catch_amalgamated.hpp>

#include "CCardList.h"
#include "CDocumentPage.h"
#include "Resource.h"
#include "pynote/harness/win32_harness.h"

// windows.h 의 CreateEvent 매크로가 repositories.h 의 멤버 이름을 바꾸기 전에 걷는다 -
// CDocumentPage.cpp·w3_shell_consumer_test.cpp 와 같은 순서 계약이어야 같은 바이너리 안에서
// 멤버 이름이 갈리지 않는다. ATL/WTL(CCardList.h)은 이 #undef 앞에서 읽어야 자기 ::CreateEvent
// 호출이 식별자를 잃지 않는다.
#ifdef CreateEvent
#undef CreateEvent
#endif

#include "pynote/core/application/card_service.h"
#include "pynote/core/application/draft_coordinator.h"
#include "pynote/core/application/save_coordinator.h"
#include "pynote/core/application/workspace_state.h"
#include "pynote/core/domain/card_list_projection.h"
#include "pynote/core/domain/paragraph_parser.h"
#include "pynote/core/storage/migration_runner.h"
#include "pynote/core/storage/repositories.h"

#include <D2DWrapp/D2DBrushCache.h>
#include <D2DWrapp/D2DDevice.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt")
#pragma comment(lib, "D2DWrapp")
#pragma comment(lib, "NoteExCore")

namespace
{
	namespace app = pynote::core::application;
	namespace domain = pynote::core::domain;
	namespace storage = pynote::core::storage;
	using pynote::harness::TestWindow;

	// 시험은 폰트를 명시 주입해 측정을 결정적으로 만든다(시스템 메시지 폰트 의존 제거).
	constexpr wchar_t TEST_FONT_FAMILY[] = L"Segoe UI";
	constexpr float TEST_FONT_SIZE_DIP = 12.0f;

	// 한글은 narrow 리터럴로 쓰면 실행 문자셋(CP949)으로 접혀 UTF-8 계약이 깨진다 -
	// 본문은 전부 wide 리터럴에서 변환한다(p2_card_list_test.cpp:42 선례).
	std::string to_utf8(const std::wstring& _sValue)
	{
		if (_sValue.empty()) { return(std::string{}); }
		const int nRequired = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
			_sValue.data(), static_cast<int>(_sValue.size()), nullptr, 0, nullptr, nullptr);
		if (nRequired <= 0) { throw std::runtime_error("WideCharToMultiByte size query failed"); }
		std::string Result(static_cast<std::size_t>(nRequired), '\0');
		if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, _sValue.data(),
			static_cast<int>(_sValue.size()), Result.data(), nRequired, nullptr, nullptr) != nRequired)
		{
			throw std::runtime_error("WideCharToMultiByte conversion failed");
		}
		return(Result);
	}

	std::size_t child_window_count(HWND _hWnd)
	{
		std::size_t nCount = 0;
		for (HWND hChild = ::GetWindow(_hWnd, GW_CHILD); hChild != nullptr;
			hChild = ::GetWindow(hChild, GW_HWNDNEXT))
		{
			++nCount;
		}
		return(nCount);
	}

	// 시험이 독립적으로 계산하는 ARGB 변환. 컨트롤 구현을 쓰지 않아야 채널이 뒤바뀐
	// 변환을 잡을 수 있다.
	d2d::Color expected_argb(COLORREF _nColor)
	{
		return(0xFF000000u |
			(static_cast<d2d::Color>(GetRValue(_nColor)) << 16) |
			(static_cast<d2d::Color>(GetGValue(_nColor)) << 8) |
			static_cast<d2d::Color>(GetBValue(_nColor)));
	}

	domain::S_CARD make_card(int _nNumber, const std::string& _sBody)
	{
		domain::S_CARD Card;
		Card.sId = "card-" + std::to_string(_nNumber);
		Card.sDocumentId = "document-1";
		Card.sOperationId = "operation-" + std::to_string(_nNumber);
		Card.nPositionKey = static_cast<std::int64_t>(_nNumber) * 1024;
		Card.nCaptureSeq = _nNumber;
		Card.nCreatedAtUs = 1000000 + _nNumber;
		Card.nUpdatedAtUs = 1000000 + _nNumber;
		Card.eSource = domain::E_CARD_SOURCE::Typing;
		Card.sBody = _sBody;
		Card.sCurrentRevisionId = "revision-" + std::to_string(_nNumber);
		return(Card);
	}

	std::vector<std::wstring> run_texts(const S_CARD_LIST_ROW_FRAME& _Row)
	{
		std::vector<std::wstring> Texts;
		for (const S_CARD_TEXT_RUN& Run : _Row.BodyLines) { Texts.push_back(Run.sText); }
		Texts.push_back(_Row.TimeRun.sText);
		Texts.push_back(_Row.SuffixRun.sText);
		return(Texts);
	}

	std::wstring repeat(const std::wstring& _sValue, int _nTimes)
	{
		std::wstring Result;
		for (int nIndex = 0; nIndex < _nTimes; ++nIndex) { Result += _sValue; }
		return(Result);
	}

	// 창 없이 측정·미리보기 추출만 보는 시험용 fixture.
	class C_TEXT_FIXTURE
	{
	public:
		C_TEXT_FIXTURE()
		{
			REQUIRE(m_Device.Initialize());
			REQUIRE(m_Text.Initialize(&m_Device));
			m_pFormat = m_Text.GetFormat(TEST_FONT_FAMILY, TEST_FONT_SIZE_DIP);
			REQUIRE(m_pFormat != nullptr);
		}

		C_TEXT_FIXTURE(const C_TEXT_FIXTURE&) = delete;
		C_TEXT_FIXTURE& operator=(const C_TEXT_FIXTURE&) = delete;

		d2d::C_D2D_TEXT& Text() noexcept { return(m_Text); }
		IDWriteTextFormat* Format() const noexcept { return(m_pFormat); }

		std::vector<std::wstring> Lines(const std::wstring& _sText, bool _bTruncated,
			int _nWidthDip, std::size_t _nMaxLines)
		{
			return(ComputeDisplayLines(_sText, _bTruncated, m_Text, m_pFormat, _nWidthDip, _nMaxLines));
		}

	private:
		d2d::C_D2D_DEVICE m_Device;
		d2d::C_D2D_TEXT m_Text;
		IDWriteTextFormat* m_pFormat{ nullptr };
	};

	// 진짜 HWND + 진짜 D2DWrapp + 진짜 프로젝션 위의 컨트롤.
	class C_RENDER_FIXTURE
	{
	public:
		explicit C_RENDER_FIXTURE(int _nWidthDip = 640, int _nHeightDip = 480)
			: m_Host(pynote::harness::TestWindowOptions{ L"W4 card list", 1400, 1000, false })
		{
			REQUIRE(m_Device.Initialize());
			REQUIRE(m_Text.Initialize(&m_Device));
			REQUIRE(m_Brushes.Initialize(&m_Device));
			m_Control.AttachRenderServices(&m_Device, &m_Brushes, &m_Text);
			m_Control.Bind(m_Projection);
			this->SetFont(TEST_FONT_FAMILY, TEST_FONT_SIZE_DIP);
			RECT Frame{ 0, 0, _nWidthDip, _nHeightDip };
			REQUIRE(m_Control.Create(m_Host.hwnd(), Frame, nullptr,
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL, 0,
				static_cast<UINT>(IDC_DOCUMENT_CARD_LIST)) != nullptr);
			this->ResizeClient(_nWidthDip, _nHeightDip);
		}

		~C_RENDER_FIXTURE()
		{
			if (m_Control.IsWindow()) { m_Control.DestroyWindow(); }
		}

		C_RENDER_FIXTURE(const C_RENDER_FIXTURE&) = delete;
		C_RENDER_FIXTURE& operator=(const C_RENDER_FIXTURE&) = delete;

		C_CARD_LIST& Control() noexcept { return(m_Control); }
		domain::C_CARD_LIST_PROJECTION& Projection() noexcept { return(m_Projection); }
		d2d::C_D2D_TEXT& Text() noexcept { return(m_Text); }
		IDWriteTextFormat* Format() const noexcept { return(m_pFormat); }

		void SetFont(const wchar_t* _pFamily, float _fSizeDip)
		{
			S_CARD_LIST_DISPLAY Display;
			Display.Font.sFamily = _pFamily;
			Display.Font.fSizeDip = _fSizeDip;
			m_Control.SetDisplaySettings(Display);
			m_pFormat = m_Text.GetFormat(_pFamily, _fSizeDip);
			REQUIRE(m_pFormat != nullptr);
		}

		// 컨트롤이 WS_VSCROLL 을 달고 있어 창 폭과 클라이언트 폭이 다르다. 파이썬 단언은
		// 뷰포트(= 행 사각) 폭 기준이므로 클라이언트가 요구 DIP 가 될 때까지 맞춘다.
		void ResizeClient(int _nWidthDip, int _nHeightDip)
		{
			int nWidth = _nWidthDip;
			int nHeight = _nHeightDip;
			for (int nAttempt = 0; nAttempt < 4; ++nAttempt)
			{
				::MoveWindow(m_Control.m_hWnd, 0, 0, nWidth, nHeight, TRUE);
				pynote::harness::drain_messages();
				const int nActualWidth = m_Control.ViewportWidthDip();
				const int nActualHeight = m_Control.ViewportHeightDip();
				if (nActualWidth == _nWidthDip && nActualHeight == _nHeightDip) { break; }
				nWidth += _nWidthDip - nActualWidth;
				nHeight += _nHeightDip - nActualHeight;
			}
			REQUIRE(m_Control.ViewportWidthDip() == _nWidthDip);
			REQUIRE(m_Control.ViewportHeightDip() == _nHeightDip);
		}

		void SetCards(const std::vector<domain::S_CARD>& _Cards)
		{
			m_Projection.SetCards(_Cards);
			m_Control.OnProjectionChanged();
		}

		// 컨트롤 밖에서 독립적으로 잰 줄 높이. 행 높이에서 역산하지 않는다.
		int MeasuredLineSpacingDip()
		{
			return((std::max)(1, static_cast<int>(
				std::ceil(m_Text.Measure(L"Ag", m_pFormat, 0.0f).height))));
		}

		const S_CARD_LIST_ROW_FRAME& FirstRow() const
		{
			REQUIRE_FALSE(m_Control.LastFrame().Rows.empty());
			return(m_Control.LastFrame().Rows.front());
		}

	private:
		d2d::C_D2D_DEVICE m_Device;
		d2d::C_D2D_TEXT m_Text;
		d2d::C_D2D_BRUSH_CACHE m_Brushes;
		TestWindow m_Host;
		domain::C_CARD_LIST_PROJECTION m_Projection;
		C_CARD_LIST m_Control;
		IDWriteTextFormat* m_pFormat{ nullptr };
	};

	// w3_shell_consumer_test.cpp:40~183 의 페이지 fixture 를 이 TU 로 옮겨 온 것이다
	// (그 파일은 무수정 통과가 계약이라 건드리지 않는다). 좌 pane 을 낮게 잡아 카드 하나로도
	// 픽셀 스크롤이 성립하게 한다.
	class C_PAGE_FIXTURE
	{
	public:
		C_PAGE_FIXTURE()
			: m_Path(std::filesystem::temp_directory_path() /
				("NoteEx-W4-page-" + std::to_string(::GetCurrentProcessId()) + "-" +
				std::to_string(++s_nSequence) + ".db")), m_Repositories(m_Database),
			  m_DraftStore(m_Database, m_Repositories),
			  m_Parent(pynote::harness::TestWindowOptions{ L"W4 page host", 960, 480, true })
		{
			this->remove_();
			REQUIRE(m_Database.Open(m_Path.string()));
			storage::C_MIGRATION_RUNNER Runner;
			Runner.SetExistingDatabase(false, m_Path.string());
			REQUIRE(Runner.Run(m_Database) == storage::E_MIGRATE_RESULT::Ok);
			domain::S_DOCUMENT Document;
			Document.sId = DocumentId;
			Document.sTitle = "w4 document";
			Document.nCreatedAtUs = 1000;
			Document.nUpdatedAtUs = 1000;
			REQUIRE(m_Repositories.CreateDocument(Document) == storage::E_REPO_RESULT::Ok);
			domain::S_WORKSPACE_WINDOW Workspace;
			REQUIRE(m_Repositories.SaveWorkspaceWindow(
				WorkspaceId, { DocumentId }, DocumentId, &Workspace) == storage::E_REPO_RESULT::Ok);
			m_CardService = std::make_unique<app::C_CARD_SERVICE>(m_Database, m_Repositories, m_Parser,
				[this]() { return(++m_nClock); }, [this]() { return(this->next_id_("card-data")); });
			m_Drafts = std::make_unique<app::C_DRAFT_COORDINATOR>(m_DraftStore, 2000,
				[this]() { return(++m_nClock); }, [this]() { return(++m_nClock); },
				[this]() { return(++m_nClock * 1000); }, [this]() { return(this->next_id_("draft")); });
			m_Save = std::make_unique<app::C_SAVE_COORDINATOR>(m_Database, m_Repositories, *m_Drafts,
				[this]() { return(++m_nClock); }, [this]() { return(this->next_id_("save")); });
			REQUIRE(m_Device.Initialize());
			REQUIRE(m_Text.Initialize(&m_Device));
			REQUIRE(m_Brushes.Initialize(&m_Device));
			m_hLeft = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
				0, 0, 320, LIST_HOST_HEIGHT, m_Parent.hwnd(), reinterpret_cast<HMENU>(3001),
				::GetModuleHandleW(nullptr), nullptr);
			m_hRight = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
				320, 0, 640, 480, m_Parent.hwnd(), reinterpret_cast<HMENU>(3002),
				::GetModuleHandleW(nullptr), nullptr);
			REQUIRE(m_hLeft != nullptr);
			REQUIRE(m_hRight != nullptr);
			this->create_page_();
		}

		~C_PAGE_FIXTURE()
		{
			m_Page.reset();
			m_Save.reset();
			m_Drafts.reset();
			m_CardService.reset();
			m_Database.Close();
			this->remove_();
		}

		C_PAGE_FIXTURE(const C_PAGE_FIXTURE&) = delete;
		C_PAGE_FIXTURE& operator=(const C_PAGE_FIXTURE&) = delete;

		void RecreatePage()
		{
			m_Page.reset();
			this->create_page_();
		}

		void Paste(const std::wstring& _sText)
		{
			REQUIRE(::OpenClipboard(m_Parent.hwnd()));
			REQUIRE(::EmptyClipboard());
			const SIZE_T nBytes = (_sText.size() + 1) * sizeof(wchar_t);
			HGLOBAL hText = ::GlobalAlloc(GMEM_MOVEABLE, nBytes);
			REQUIRE(hText != nullptr);
			void* pText = ::GlobalLock(hText);
			REQUIRE(pText != nullptr);
			std::memcpy(pText, _sText.c_str(), nBytes);
			::GlobalUnlock(hText);
			REQUIRE(::SetClipboardData(CF_UNICODETEXT, hText) == hText);
			REQUIRE(::CloseClipboard());
			::SetFocus(m_Page->EditorHwnd());
			::SendMessageW(m_Page->EditorHwnd(), WM_PASTE, 0, 0);
		}

		C_DOCUMENT_PAGE& Page() { return(*m_Page); }

		static constexpr int LIST_HOST_HEIGHT = 48;
		C_DOCUMENT_PAGE::E_LEAVE_CHOICE LeaveChoice{ C_DOCUMENT_PAGE::E_LEAVE_CHOICE::Save };
		inline static const std::string DocumentId = "document-w4";
		inline static const std::string WorkspaceId = "window-w4";

	private:
		void create_page_()
		{
			m_Page = std::make_unique<C_DOCUMENT_PAGE>();
			// C_MAIN 이 Init 앞에서 하는 배선과 같은 순서다(CMain.cpp bind_card_list).
			m_Page->SetRenderServices(&m_Device, &m_Brushes, &m_Text);
			S_CARD_LIST_DISPLAY Display;
			Display.Font.sFamily = TEST_FONT_FAMILY;
			Display.Font.fSizeDip = TEST_FONT_SIZE_DIP;
			m_Page->SetDisplaySettings(Display);
			REQUIRE(m_Page->Init(::GetModuleHandleW(nullptr), m_hLeft, m_hRight,
				m_Database, m_Repositories, *m_CardService, *m_Drafts, *m_Save,
				WorkspaceId, DocumentId, [this](HWND) { return(LeaveChoice); }));
		}

		std::string next_id_(const char* _pszPrefix)
		{
			return(std::string(_pszPrefix) + "-" + std::to_string(++m_nId));
		}

		void remove_() const
		{
			std::error_code Error;
			std::filesystem::remove(m_Path, Error);
			std::filesystem::remove(m_Path.string() + "-wal", Error);
			std::filesystem::remove(m_Path.string() + "-shm", Error);
		}

		std::filesystem::path m_Path;
		storage::C_DATABASE m_Database;
		storage::C_REPOSITORIES m_Repositories;
		app::C_REPOSITORY_DRAFT_STORE m_DraftStore;
		domain::C_PARAGRAPH_PARSER m_Parser;
		std::unique_ptr<app::C_CARD_SERVICE> m_CardService;
		std::unique_ptr<app::C_DRAFT_COORDINATOR> m_Drafts;
		std::unique_ptr<app::C_SAVE_COORDINATOR> m_Save;
		d2d::C_D2D_DEVICE m_Device;
		d2d::C_D2D_TEXT m_Text;
		d2d::C_D2D_BRUSH_CACHE m_Brushes;
		TestWindow m_Parent;
		HWND m_hLeft{};
		HWND m_hRight{};
		std::unique_ptr<C_DOCUMENT_PAGE> m_Page;
		std::int64_t m_nClock{ 2000 };
		std::uint64_t m_nId{};
		inline static std::atomic<unsigned long> s_nSequence{};
	};

	// ---- 성능(군 L) 고정 말뭉치. p2_card_list_test.cpp:26~124 의 생성기·검사를 그대로 쓴다. ----
	constexpr std::size_t kCorpusRowCount = 10000;
	constexpr std::size_t kCorpusUtf8Bytes = 759944;
	constexpr char kCorpusSha256[] = "3d6d906c4c87a659589ebb96e1170cec4832ed45fe8b6455ec8a71b2e52e4d8a";
	constexpr wchar_t kFirstRow[] =
		L"00000|카드 본문 00|emoji\U0001F642|longword_x\n둘째 줄";
	constexpr wchar_t kLastRow[] =
		L"09999|카드 본문 08|emoji\U0001F642|longword_xxxxxxxxxxxxxxxxxx\n"
		L"둘째 줄";
	constexpr char kMeasurementStart[] = "before-corpus-model-generation";
	constexpr char kMeasurementEnd[] = "after-last-row-layout-draw-present";

	struct S_CORPUS
	{
		std::vector<std::wstring> Rows;
		std::string sUtf8;
	};

	S_CORPUS make_corpus()
	{
		S_CORPUS Corpus;
		Corpus.Rows.reserve(kCorpusRowCount);
		Corpus.sUtf8.reserve(kCorpusUtf8Bytes);
		for (std::size_t nIndex = 0; nIndex < kCorpusRowCount; ++nIndex)
		{
			wchar_t Prefix[96]{};
			_snwprintf_s(Prefix, _countof(Prefix), _TRUNCATE,
				L"%05zu|카드 본문 %02zu|emoji\U0001F642|longword_",
				nIndex, nIndex % 97);
			std::wstring Row(Prefix);
			Row.append(nIndex % 23 + 1, L'x');
			Row += L"\n둘째 줄";
			if (nIndex != 0) { Corpus.sUtf8 += "\n---ROW---\n"; }
			Corpus.sUtf8 += to_utf8(Row);
			Corpus.Rows.push_back(std::move(Row));
		}
		return(Corpus);
	}

	std::string sha256_hex(const std::string& _sBytes)
	{
		BCRYPT_ALG_HANDLE hAlgorithm = nullptr;
		BCRYPT_HASH_HANDLE hHash = nullptr;
		DWORD nObjectBytes = 0;
		DWORD nDigestBytes = 0;
		DWORD nCopied = 0;
		if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&hAlgorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ||
			!BCRYPT_SUCCESS(::BCryptGetProperty(hAlgorithm, BCRYPT_OBJECT_LENGTH,
				reinterpret_cast<PUCHAR>(&nObjectBytes), sizeof(nObjectBytes), &nCopied, 0)) ||
			!BCRYPT_SUCCESS(::BCryptGetProperty(hAlgorithm, BCRYPT_HASH_LENGTH,
				reinterpret_cast<PUCHAR>(&nDigestBytes), sizeof(nDigestBytes), &nCopied, 0)))
		{
			if (hAlgorithm != nullptr) { ::BCryptCloseAlgorithmProvider(hAlgorithm, 0); }
			throw std::runtime_error("BCrypt SHA-256 setup failed");
		}
		std::vector<UCHAR> Object(nObjectBytes);
		std::vector<UCHAR> Digest(nDigestBytes);
		if (!BCRYPT_SUCCESS(::BCryptCreateHash(hAlgorithm, &hHash, Object.data(), nObjectBytes, nullptr, 0, 0)) ||
			!BCRYPT_SUCCESS(::BCryptHashData(hHash,
				reinterpret_cast<PUCHAR>(const_cast<char*>(_sBytes.data())),
				static_cast<ULONG>(_sBytes.size()), 0)) ||
			!BCRYPT_SUCCESS(::BCryptFinishHash(hHash, Digest.data(), nDigestBytes, 0)))
		{
			if (hHash != nullptr) { ::BCryptDestroyHash(hHash); }
			::BCryptCloseAlgorithmProvider(hAlgorithm, 0);
			throw std::runtime_error("BCrypt SHA-256 calculation failed");
		}
		::BCryptDestroyHash(hHash);
		::BCryptCloseAlgorithmProvider(hAlgorithm, 0);

		std::ostringstream Output;
		Output << std::hex << std::setfill('0');
		for (const UCHAR nByte : Digest) { Output << std::setw(2) << static_cast<unsigned>(nByte); }
		return(Output.str());
	}

	void require_fixed_corpus(const S_CORPUS& _Corpus)
	{
		REQUIRE(_Corpus.Rows.size() == kCorpusRowCount);
		REQUIRE(_Corpus.Rows.front() == kFirstRow);
		REQUIRE(_Corpus.Rows.back() == kLastRow);
		REQUIRE(_Corpus.sUtf8.size() == kCorpusUtf8Bytes);
		REQUIRE(sha256_hex(_Corpus.sUtf8) == kCorpusSha256);
	}

	std::vector<domain::S_CARD> corpus_cards(const S_CORPUS& _Corpus)
	{
		std::vector<domain::S_CARD> Cards;
		Cards.reserve(_Corpus.Rows.size());
		for (std::size_t nIndex = 0; nIndex < _Corpus.Rows.size(); ++nIndex)
		{
			domain::S_CARD Card;
			Card.sId = "performance-" + std::to_string(nIndex + 1);
			Card.sDocumentId = "performance-document";
			Card.sOperationId = "performance-operation-" + std::to_string(nIndex + 1);
			Card.nPositionKey = static_cast<std::int64_t>(nIndex + 1) * 1024;
			Card.nCaptureSeq = static_cast<std::int64_t>(nIndex + 1);
			Card.nCreatedAtUs = static_cast<std::int64_t>(nIndex + 1);
			Card.nUpdatedAtUs = static_cast<std::int64_t>(nIndex + 1);
			Card.eSource = domain::E_CARD_SOURCE::System;
			Card.sBody = to_utf8(_Corpus.Rows[nIndex]);
			Card.sCurrentRevisionId = "performance-revision-" + std::to_string(nIndex + 1);
			Cards.push_back(std::move(Card));
		}
		return(Cards);
	}
}

TEST_CASE("PLAN-W4-0020 paints body then time row without removed metadata",
	"[W4-render][WTL-CAP-RE-009]")
{
	C_RENDER_FIXTURE Fixture(500, 400);
	Fixture.SetCards({ make_card(1, to_utf8(L"본문")) });
	REQUIRE(Fixture.Control().Render());
	const S_CARD_LIST_ROW_FRAME& Row = Fixture.FirstRow();

	REQUIRE(Row.BodyLines.size() == 1);
	REQUIRE(Row.BodyLines.front().sText == L"본문");
	REQUIRE(Row.BodyLines.front().Rect.nTop < Row.TimeRun.Rect.nTop);
	REQUIRE(Row.TimeRun.Rect.Bottom() == Row.ContentRect.Bottom());

	const std::vector<std::wstring> Texts = run_texts(Row);
	for (const wchar_t* pRemoved : { L"재구성 불가", L"위치 ", L"기록 #", L"출처", L"리비전", L"더 보기" })
	{
		for (const std::wstring& sText : Texts)
		{
			REQUIRE(sText.find(pRemoved) == std::wstring::npos);
		}
	}
}

TEST_CASE("PLAN-W4-0021 time row places suffix immediately after time when wide",
	"[W4-render][WTL-CAP-RE-009]")
{
	C_RENDER_FIXTURE Fixture(600, 400);
	domain::S_CARD Card = make_card(1, to_utf8(L"카드 1"));
	Card.nCreatedAtUs = 1000000;
	Card.nUpdatedAtUs = 2000000;
	Fixture.SetCards({ Card });
	Fixture.Projection().SetCardDirty(Card.sId, true);
	REQUIRE(Fixture.Control().Render());
	const S_CARD_LIST_ROW_FRAME& Row = Fixture.FirstRow();

	REQUIRE(Row.TimeRun.sText == Fixture.Control().TimeLabel(2000000));
	REQUIRE_FALSE(Row.TimeRun.sText.empty());
	REQUIRE(Row.SuffixRun.sText == L" (수정됨) · 편집 중");
	REQUIRE(Row.SuffixRun.bDrawn);
	REQUIRE(Row.TimeRun.Rect.Right() + 1 == Row.SuffixRun.Rect.nLeft);
}

TEST_CASE("PLAN-W4-0022 time row keeps modified and dirty suffix when narrow",
	"[W4-render][WTL-CAP-RE-009]")
{
	C_RENDER_FIXTURE Fixture(220, 400);
	domain::S_CARD Card = make_card(1, to_utf8(L"카드 1"));
	Card.nCreatedAtUs = 1000000;
	Card.nUpdatedAtUs = 2000000;
	Fixture.SetCards({ Card });
	Fixture.Projection().SetCardDirty(Card.sId, true);
	REQUIRE(Fixture.Control().Render());
	const S_CARD_LIST_ROW_FRAME& Row = Fixture.FirstRow();

	REQUIRE(Row.TimeRun.sText.ends_with(L"…"));
	REQUIRE(Row.SuffixRun.sText == L" (수정됨) · 편집 중");
	REQUIRE(Row.TimeRun.Rect.Right() < Row.SuffixRun.Rect.nLeft);
	REQUIRE(Row.ContentRect.Contains(Row.SuffixRun.Rect));
}

TEST_CASE("PLAN-W4-0023 time row elides oversized suffix inside content",
	"[W4-render][WTL-CAP-RE-009]")
{
	C_RENDER_FIXTURE Fixture(80, 400);
	domain::S_CARD Card = make_card(1, to_utf8(L"카드 1"));
	Card.nCreatedAtUs = 1000000;
	Card.nUpdatedAtUs = 2000000;
	Fixture.SetCards({ Card });
	Fixture.Projection().SetCardDirty(Card.sId, true);
	REQUIRE(Fixture.Control().Render());
	const S_CARD_LIST_ROW_FRAME& Row = Fixture.FirstRow();

	REQUIRE(Row.TimeRun.sText.empty());
	REQUIRE(Row.SuffixRun.sText.ends_with(L"…"));
	REQUIRE(Row.SuffixRun.Rect.nLeft == Row.ContentRect.nLeft);
	REQUIRE(Row.ContentRect.Contains(Row.SuffixRun.Rect));
}

TEST_CASE("PLAN-W4-0024 ellipsizes source newline and wrapped overflow",
	"[W4-render][WTL-CAP-RE-005][WTL-CAP-RE-007]")
{
	C_TEXT_FIXTURE Fixture;

	const std::vector<std::wstring> Source = Fixture.Lines(L"첫째\n둘째\n셋째\n넷째", false, 500, 3);
	const std::vector<std::wstring> ExpectedSource{ L"첫째", L"둘째", L"셋째…" };
	REQUIRE(Source == ExpectedSource);

	const std::vector<std::wstring> Wrapped =
		Fixture.Lines(repeat(L"자동 감김을 만드는 긴 본문 ", 20), false, 90, 3);
	REQUIRE(Wrapped.size() == 3);
	REQUIRE(Wrapped.back().ends_with(L"…"));

	const std::vector<std::wstring> Single = Fixture.Lines(L"첫째\n둘째", false, 500, 1);
	const std::vector<std::wstring> ExpectedSingle{ L"첫째…" };
	REQUIRE(Single == ExpectedSingle);

	const std::vector<std::wstring> Unbroken = Fixture.Lines(std::wstring(200, L'가'), false, 90, 3);
	REQUIRE(Unbroken.size() == 3);
	REQUIRE(Unbroken.back().ends_with(L"…"));
}

TEST_CASE("PLAN-W4-0025 preview lines keep non-BMP source newline boundaries",
	"[W4-render][WTL-CAP-RE-005][WTL-CAP-RE-006]")
{
	C_TEXT_FIXTURE Fixture;
	const std::vector<std::wstring> Lines = Fixture.Lines(
		L"\U0001F600\U0001F600\U0001F600\n다음 줄\n셋째 줄\n넷째 줄", false, 400, 3);
	const std::vector<std::wstring> Expected{
		L"\U0001F600\U0001F600\U0001F600", L"다음 줄", L"셋째 줄…" };

	REQUIRE(Lines == Expected);
	for (const std::wstring& sLine : Lines)
	{
		REQUIRE(sLine.find(L'\u2028') == std::wstring::npos);
	}
}

TEST_CASE("PLAN-W4-0026 preview lines keep non-BMP wrapped boundaries",
	"[W4-render][WTL-CAP-RE-006]")
{
	C_TEXT_FIXTURE Fixture;
	const std::wstring sProbe = L"\U0001F600\U0001F600가가";
	// ceil 이다 - lround 로 내림이 걸리면 네 글자가 소수점 차이로 줄을 넘겨 폰트 의존
	// 동전 던지기가 된다.
	const int nWidth = static_cast<int>(
		std::ceil(Fixture.Text().Measure(sProbe.c_str(), Fixture.Format(), 0.0f).width));
	const std::wstring sBody = L"\U0001F600\U0001F600가가나나다다라마";

	const std::vector<std::wstring> Lines = Fixture.Lines(sBody, false, nWidth, 3);
	std::wstring sJoined;
	for (const std::wstring& sLine : Lines) { sJoined += sLine; }

	REQUIRE(sJoined == sBody);
	REQUIRE(Lines.size() <= 3);
	for (std::size_t nIndex = 0; nIndex + 1 < Lines.size(); ++nIndex)
	{
		if (Lines[nIndex].empty() || Lines[nIndex + 1].empty()) { continue; }
		const wchar_t nLeft = Lines[nIndex].back();
		const wchar_t nRight = Lines[nIndex + 1].front();
		const bool bSplitPair = nLeft >= 0xD800 && nLeft <= 0xDBFF &&
			nRight >= 0xDC00 && nRight <= 0xDFFF;
		REQUIRE_FALSE(bSplitPair);
	}
	for (const std::wstring& sLine : Lines)
	{
		REQUIRE(sLine.find(L'\u2028') == std::wstring::npos);
	}
}

TEST_CASE("PLAN-W4-0027 paint ellipsizes at actual content width",
	"[W4-render][WTL-CAP-RE-007][WTL-CAP-RE-019]")
{
	C_RENDER_FIXTURE Fixture(220, 400);
	Fixture.SetCards({ make_card(1, to_utf8(repeat(L"공백없는초장문", 50))) });
	REQUIRE(Fixture.Control().Render());
	const S_CARD_LIST_ROW_FRAME& Row = Fixture.FirstRow();

	// 카드 폭이 아니라 내용 폭 기준이다 - 세 번째(마지막) 본문 런이 말줄임으로 끝난다.
	REQUIRE(Row.BodyLines.size() == 3);
	REQUIRE(Row.BodyLines[2].sText.ends_with(L"…"));
	REQUIRE(Row.ContentRect.nWidth == Row.RowRect.nWidth - 2 *
		(CARD_HORIZONTAL_INSET_DIP + CARD_CONTENT_HORIZONTAL_MARGIN_DIP));
}

TEST_CASE("PLAN-W4-0028 handles empty and whitespace-only bodies",
	"[W4-render][WTL-CAP-RE-008]")
{
	C_TEXT_FIXTURE Fixture;
	const std::vector<std::wstring> Empty = Fixture.Lines(L"", false, 100, 3);
	const std::vector<std::wstring> ExpectedEmpty{ L"" };
	const std::vector<std::wstring> Blank = Fixture.Lines(L"   ", false, 100, 3);
	const std::vector<std::wstring> ExpectedBlank{ L"   " };

	// 빈 본문도 공백 본문도 대체 문구로 바뀌지 않는다(W3 의 "(빈 카드)" 자리표시자 제거).
	REQUIRE(Empty == ExpectedEmpty);
	REQUIRE(Blank == ExpectedBlank);
}

TEST_CASE("PLAN-W4-0029 row height is constant for short and hundred-line bodies",
	"[W4-render][WTL-CAP-RE-003]")
{
	C_RENDER_FIXTURE Fixture(500, 600);
	std::wstring sLong;
	for (int nIndex = 0; nIndex < 100; ++nIndex)
	{
		if (nIndex != 0) { sLong += L"\n"; }
		sLong += L"긴 본문";
	}
	Fixture.SetCards({ make_card(1, to_utf8(L"한 줄")), make_card(2, to_utf8(sLong)) });
	REQUIRE(Fixture.Control().Render());
	REQUIRE(Fixture.Control().LastFrame().Rows.size() == 2);
	const int nHeight = Fixture.Control().LastFrame().Rows[0].RowRect.nHeight;
	REQUIRE(Fixture.Control().LastFrame().Rows[1].RowRect.nHeight == nHeight);

	Fixture.ResizeClient(240, 600);
	REQUIRE(Fixture.Control().Render());
	REQUIRE(Fixture.Control().LastFrame().Rows.size() == 2);
	REQUIRE(Fixture.Control().LastFrame().Rows[0].RowRect.nHeight == nHeight);
	REQUIRE(Fixture.Control().LastFrame().Rows[1].RowRect.nHeight == nHeight);
}

TEST_CASE("PLAN-W4-0030 height budgets follow two font metrics independently",
	"[W4-render][WTL-CAP-RE-003][WTL-CAP-RE-019]")
{
	C_RENDER_FIXTURE Fixture(500, 800);
	std::vector<int> Heights;
	for (const float fSizeDip : { TEST_FONT_SIZE_DIP, TEST_FONT_SIZE_DIP * 2.0f })
	{
		Fixture.SetFont(TEST_FONT_FAMILY, fSizeDip);
		const int nLine = Fixture.MeasuredLineSpacingDip();
		const int nExpected = 2 * CARD_VERTICAL_INSET_DIP + 2 * CARD_CONTENT_VERTICAL_MARGIN_DIP +
			3 * nLine + (nLine + CARD_AUXILIARY_ROW_PADDING_DIP);

		Fixture.SetCards({ make_card(1, to_utf8(L"카드 1")) });
		REQUIRE(Fixture.Control().RowHeightDip() == nExpected);

		Fixture.SetCards({ make_card(1, to_utf8(L"카드 1")), make_card(2, to_utf8(L"카드 2")) });
		REQUIRE(Fixture.Control().RowHeightDip() == nExpected);

		// 파이썬 "재구성 불가" fixture 의 네이티브 등가 - 현재 리비전이 없는 카드다.
		domain::S_CARD Unavailable = make_card(1, to_utf8(L"카드 1"));
		Unavailable.sCurrentRevisionId.reset();
		Fixture.SetCards({ Unavailable });
		REQUIRE(Fixture.Control().RowHeightDip() == nExpected);

		Heights.push_back(Fixture.Control().RowHeightDip());
	}
	REQUIRE(Heights.size() == 2);
	REQUIRE(Heights[1] > Heights[0]);
}

TEST_CASE("PLAN-W4-0031 keeps row height when width changes",
	"[W4-render][WTL-CAP-RE-003]")
{
	C_RENDER_FIXTURE Fixture(900, 500);
	Fixture.SetCards({ make_card(1, to_utf8(
		repeat(L"폭에 따라 감기는 본문은 목록 너비가 달라지면 필요한 높이도 달라집니다. ", 8))) });
	REQUIRE(Fixture.Control().Render());
	const int nHeight = Fixture.FirstRow().RowRect.nHeight;

	Fixture.ResizeClient(320, 500);
	REQUIRE(Fixture.Control().Render());
	REQUIRE(Fixture.FirstRow().RowRect.nHeight == nHeight);
}

TEST_CASE("PLAN-W4-0047 ten thousand cards use the control without per-card windows",
	"[W4-render][WTL-CAP-RE-001][WTL-CAP-FI-058]")
{
	C_RENDER_FIXTURE Fixture(600, 500);
	std::vector<domain::S_CARD> Cards;
	Cards.reserve(10000);
	for (int nIndex = 1; nIndex <= 10000; ++nIndex)
	{
		Cards.push_back(make_card(nIndex, "card " + std::to_string(nIndex)));
	}
	Fixture.SetCards(Cards);

	REQUIRE(Fixture.Control().RowCount() == 10000);
	REQUIRE(Fixture.Control().Render());
	REQUIRE(child_window_count(Fixture.Control().m_hWnd) == 0);
	REQUIRE(Fixture.Control().LastFrame().nLayoutCount > 0);
	REQUIRE(Fixture.Control().LastFrame().nLayoutCount < 10000);
	REQUIRE(Fixture.Control().LastFrame().Rows.size() < 10000);
}

TEST_CASE("PLAN-W4-0051 ellipsizes truncated preview that fits the line budget",
	"[W4-render][WTL-CAP-RE-007][WTL-CAP-RE-004]")
{
	C_TEXT_FIXTURE Fixture;
	const std::vector<std::wstring> WithinBudget = Fixture.Lines(L"첫째\n둘째", false, 500, 3);
	const std::vector<std::wstring> ExpectedWithin{ L"첫째", L"둘째" };
	const std::vector<std::wstring> Truncated = Fixture.Lines(L"첫째\n둘째", true, 500, 3);
	const std::vector<std::wstring> ExpectedTruncated{ L"첫째", L"둘째…" };

	// 줄 수는 넘지 않지만 모델이 뒷부분을 잘라냈으면 말줄임표가 서야 한다.
	REQUIRE(WithinBudget == ExpectedWithin);
	REQUIRE(Truncated == ExpectedTruncated);
}

TEST_CASE("PLAN-W4-0052 paints ellipsis for body beyond preview budget",
	"[W4-render][WTL-CAP-RE-004][WTL-CAP-RE-007]")
{
	C_RENDER_FIXTURE Fixture(400, 400);

	// (a) 순수 함수: 한 줄에 들어가는 미리보기라도 잘렸으면 말줄임표가 붙는다.
	const std::wstring sPreview(33, L'a');
	const std::vector<std::wstring> Lines =
		ComputeDisplayLines(sPreview, true, Fixture.Text(), Fixture.Format(), 4000, 3);
	const std::vector<std::wstring> Expected{ sPreview + L"…" };
	REQUIRE(Lines == Expected);

	// (b) 페인트 경로: core 는 줄당 예산을 4096 코드포인트로 동결하므로 파이썬의
	// monkeypatch(예산 8) 에 해당하는 네이티브 쌍둥이가 없다. 실제 예산 +1 로 자른다.
	const std::size_t nBudget = Fixture.Projection().PreviewBudget();
	Fixture.SetCards({ make_card(1, std::string(nBudget + 1, 'a')) });
	const auto Preview = Fixture.Projection().PreviewForCard("card-1");
	REQUIRE(Preview.has_value());
	REQUIRE(Preview->bTruncated);
	REQUIRE(Fixture.Control().Render());
	const S_CARD_LIST_ROW_FRAME& Row = Fixture.FirstRow();
	REQUIRE_FALSE(Row.BodyLines.empty());
	REQUIRE(std::any_of(Row.BodyLines.begin(), Row.BodyLines.end(),
		[](const S_CARD_TEXT_RUN& _Run) { return(_Run.sText.ends_with(L"…")); }));
}

TEST_CASE("PLAN-W4-0061 ten thousand card model creation and scroll latency",
	"[W4-render][performance]")
{
	C_RENDER_FIXTURE Fixture(640, 480);
	REQUIRE(Fixture.Projection().PreviewLineCount() == 3);

	std::array<double, 5> MeasuredMs{};
	for (std::size_t nIteration = 0; nIteration < 6; ++nIteration)
	{
		// 동결 경계: 말뭉치·모델 생성 직전에 시작해 마지막 행이 레이아웃·그리기·present
		// 될 때까지다(P2 와 같은 경계 문자열).
		const auto Started = std::chrono::steady_clock::now();
		S_CORPUS Corpus = make_corpus();
		require_fixed_corpus(Corpus);
		Fixture.SetCards(corpus_cards(Corpus));
		Fixture.Control().EnsureVisible(kCorpusRowCount - 1);
		REQUIRE(Fixture.Control().Render());
		const auto Finished = std::chrono::steady_clock::now();
		const double dElapsed = std::chrono::duration<double, std::milli>(Finished - Started).count();

		REQUIRE(Fixture.Control().LastFrame().bPresented);
		REQUIRE(Fixture.Control().LastFrame().nLastVisibleRow == kCorpusRowCount - 1);
		REQUIRE(Fixture.Control().LastFrame().nLayoutCount > 0);
		REQUIRE(Fixture.Control().LastFrame().nLayoutCount < kCorpusRowCount);
		REQUIRE(child_window_count(Fixture.Control().m_hWnd) == 0);
		REQUIRE(dElapsed < 10000.0);
		if (nIteration == 0)
		{
			std::cout << "W4 performance warmup_ms=" << dElapsed
				<< " start=" << kMeasurementStart << " end=" << kMeasurementEnd
				<< " corpus_bytes=" << kCorpusUtf8Bytes
				<< " corpus_sha256=" << kCorpusSha256 << '\n';
		}
		else { MeasuredMs[nIteration - 1] = dElapsed; }
	}

	std::array<double, 5> Sorted = MeasuredMs;
	std::sort(Sorted.begin(), Sorted.end());
	std::cout << "W4 performance raw_ms=[";
	for (std::size_t nIndex = 0; nIndex < MeasuredMs.size(); ++nIndex)
	{
		if (nIndex != 0) { std::cout << ','; }
		std::cout << MeasuredMs[nIndex];
	}
	std::cout << "] median_ms=" << Sorted[2]
		<< " max_ms=" << *std::max_element(MeasuredMs.begin(), MeasuredMs.end())
		<< " start=" << kMeasurementStart << " end=" << kMeasurementEnd << '\n';

	// 미리보기 100줄 경계 1회 - 계측하지 않는 sanity 다.
	Fixture.Projection().SetPreviewLineCount(100);
	Fixture.Control().OnProjectionChanged();
	REQUIRE(Fixture.Control().Render());
	REQUIRE(Fixture.Control().LastFrame().bPresented);
}

TEST_CASE("W4 card palette resolver preserves roles in light dark and high contrast",
	"[W4-render][WTL-CAP-RE-002][T4A-UNC-006]")
{
	struct S_VECTOR
	{
		const char* pszName;
		S_SYSTEM_COLORS Colors;
		bool bHighContrast;
		d2d::Color nHoverBase;
	};

	const std::vector<S_VECTOR> Vectors{
		{ "light",
			S_SYSTEM_COLORS{ RGB(0xFF, 0xFF, 0xFF), RGB(0x00, 0x00, 0x00), RGB(0x00, 0x78, 0xD7),
				RGB(0xFF, 0xFF, 0xFF), RGB(0xA0, 0xA0, 0xA0), RGB(0x6D, 0x6D, 0x6D) },
			false, 0xFFFFFFFFu },
		{ "dark",
			S_SYSTEM_COLORS{ RGB(0x20, 0x20, 0x20), RGB(0xFF, 0xFF, 0xFF), RGB(0x00, 0x78, 0xD7),
				RGB(0xFF, 0xFF, 0xFF), RGB(0x60, 0x60, 0x60), RGB(0xA0, 0xA0, 0xA0) },
			false, 0xFF212121u },
		{ "high-contrast-black",
			S_SYSTEM_COLORS{ RGB(0x00, 0x00, 0x00), RGB(0xFF, 0xFF, 0xFF), RGB(0x1A, 0xEB, 0xFF),
				RGB(0x00, 0x00, 0x00), RGB(0xFF, 0xFF, 0xFF), RGB(0x3F, 0xF2, 0x3F) },
			true, 0xFF000000u },
	};

	for (const S_VECTOR& Vector : Vectors)
	{
		INFO(Vector.pszName);
		const S_CARD_PALETTE Palette = ResolveCardPalette(Vector.Colors, Vector.bHighContrast);
		REQUIRE(Palette.nBase == expected_argb(Vector.Colors.nWindow));
		REQUIRE(Palette.nText == expected_argb(Vector.Colors.nWindowText));
		REQUIRE(Palette.nHighlight == expected_argb(Vector.Colors.nHighlight));
		REQUIRE(Palette.nHighlightText == expected_argb(Vector.Colors.nHighlightText));
		REQUIRE(Palette.nBorder == expected_argb(Vector.Colors.nBtnShadow));
		REQUIRE(Palette.nPlaceholder == expected_argb(Vector.Colors.nGrayText));
		REQUIRE(Palette.nHoverBase == Vector.nHoverBase);
		// 고대비 플래그는 역할 매핑을 바꾸지 않는다(Qt 동등).
		REQUIRE(ResolveCardPalette(Vector.Colors, true) == ResolveCardPalette(Vector.Colors, false));
	}
}

TEST_CASE("W4 selected and hovered rows render from the active system palette",
	"[W4-render][WTL-CAP-RE-002]")
{
	C_RENDER_FIXTURE Fixture(500, 600);
	Fixture.SetCards({ make_card(1, to_utf8(L"카드 1")), make_card(2, to_utf8(L"카드 2")),
		make_card(3, to_utf8(L"카드 3")) });
	REQUIRE(Fixture.Control().RowCount() == 3);

	REQUIRE(::SendMessageW(Fixture.Control().m_hWnd, LB_SETCURSEL, 0, 0) == 0);
	const int nRowHeight = Fixture.Control().RowHeightDip();
	::SendMessageW(Fixture.Control().m_hWnd, WM_MOUSEMOVE, 0,
		MAKELPARAM(10, nRowHeight + nRowHeight / 2));
	REQUIRE(Fixture.Control().Render());

	const S_CARD_LIST_FRAME& Frame = Fixture.Control().LastFrame();
	REQUIRE(Frame.Rows.size() == 3);
	// 기대값은 시험이 같은 GetSysColor COLORREF 에서 직접 만든다 - 채널이 뒤바뀐 변환은
	// 여기서 걸린다.
	const d2d::Color nHighlight = expected_argb(::GetSysColor(COLOR_HIGHLIGHT));
	const d2d::Color nHighlightText = expected_argb(::GetSysColor(COLOR_HIGHLIGHTTEXT));
	const d2d::Color nBase = expected_argb(::GetSysColor(COLOR_WINDOW));
	const d2d::Color nBorder = expected_argb(::GetSysColor(COLOR_BTNSHADOW));

	REQUIRE(Frame.Rows[0].bSelected);
	REQUIRE(Frame.Rows[0].nFillColor == nHighlight);
	REQUIRE(Frame.Rows[0].nTextColor == nHighlightText);
	REQUIRE(Frame.Rows[0].nBorderColor == nBorder);
	REQUIRE_FALSE(Frame.Rows[1].bSelected);
	REQUIRE(Frame.Rows[1].bHovered);
	REQUIRE(Frame.Rows[1].nFillColor == Fixture.Control().Palette().nHoverBase);
	REQUIRE_FALSE(Frame.Rows[2].bHovered);
	REQUIRE(Frame.Rows[2].nFillColor == nBase);
	REQUIRE(Frame.Rows[2].nBorderColor == nBorder);
}

TEST_CASE("W4 pixel scroll offset ensure visible and top index compatibility",
	"[W4-render][WTL-CAP-RE-011]")
{
	C_RENDER_FIXTURE Fixture(600, 480);
	std::vector<domain::S_CARD> Cards;
	for (int nIndex = 1; nIndex <= 50; ++nIndex)
	{
		Cards.push_back(make_card(nIndex, to_utf8(L"카드 ") + std::to_string(nIndex)));
	}
	Fixture.SetCards(Cards);

	C_CARD_LIST& Control = Fixture.Control();
	const int nRowHeight = Control.RowHeightDip();
	const int nViewport = Control.ViewportHeightDip();
	const int nContent = 50 * nRowHeight;
	const int nMaximum = nContent - nViewport;
	REQUIRE(nViewport == 480);
	REQUIRE(nMaximum > 0);
	REQUIRE(Control.ContentHeightDip() == nContent);

	Control.ScrollToPixel(-100);
	REQUIRE(Control.ScrollOffsetDip() == 0);
	Control.ScrollToPixel(nMaximum + 1000);
	REQUIRE(Control.ScrollOffsetDip() == nMaximum);
	Control.ScrollToPixel(nRowHeight * 4 + 7);
	REQUIRE(Control.ScrollOffsetDip() == nRowHeight * 4 + 7);
	REQUIRE(::SendMessageW(Control.m_hWnd, LB_GETTOPINDEX, 0, 0) ==
		(nRowHeight * 4 + 7) / nRowHeight);

	// EnsureVisible - 위로 벗어난 행은 위쪽을, 아래로 벗어난 행은 아래쪽을 맞춘다.
	Control.EnsureVisible(2);
	REQUIRE(Control.ScrollOffsetDip() == 2 * nRowHeight);
	const int nBefore = Control.ScrollOffsetDip();
	Control.EnsureVisible(2);
	REQUIRE(Control.ScrollOffsetDip() == nBefore);
	Control.EnsureVisible(20);
	REQUIRE(Control.ScrollOffsetDip() == 21 * nRowHeight - nViewport);

	REQUIRE(::SendMessageW(Control.m_hWnd, LB_SETTOPINDEX, 6, 0) == 0);
	REQUIRE(Control.ScrollOffsetDip() == 6 * nRowHeight);
	REQUIRE(::SendMessageW(Control.m_hWnd, LB_GETTOPINDEX, 0, 0) == 6);
	REQUIRE(::SendMessageW(Control.m_hWnd, LB_SETTOPINDEX, 50, 0) == LB_ERR);
	REQUIRE(Control.ScrollOffsetDip() == 6 * nRowHeight);

	// 크기 변경은 오프셋을 유지하고 새 한계로만 클램프한다.
	Fixture.ResizeClient(600, 240);
	REQUIRE(Control.ScrollOffsetDip() == 6 * nRowHeight);
	Control.ScrollToPixel(nContent - 240);
	REQUIRE(Control.ScrollOffsetDip() == nContent - 240);
	Fixture.ResizeClient(600, 720);
	REQUIRE(Control.ScrollOffsetDip() == nContent - 720);
}

TEST_CASE("W4 document page hosts NoteExCardList with LB compatible messages and pixel persistence",
	"[W4-render][WTL-CAP-FI-058][WTL-CAP-RE-011]")
{
	C_PAGE_FIXTURE Fixture;
	C_DOCUMENT_PAGE& Page = Fixture.Page();

	wchar_t ClassName[64]{};
	REQUIRE(::GetClassNameW(Page.CardListHwnd(), ClassName, _countof(ClassName)) > 0);
	REQUIRE(std::wstring(ClassName) == L"NoteExCardList");
	REQUIRE(child_window_count(Page.CardListHwnd()) == 0);

	Fixture.Paste(L"w4 host body");
	REQUIRE(::SendMessageW(Page.CardListHwnd(), LB_GETCOUNT, 0, 0) == 1);
	REQUIRE(::SendMessageW(Page.CardListHwnd(), LB_GETCURSEL, 0, 0) == 0);
	REQUIRE(Page.Save());
	REQUIRE(Page.RequestLeave() == app::E_LEAVE_RESULT::ApprovedClean);
	REQUIRE(::GetFocus() == Page.CardListHwnd());
	REQUIRE(::SendMessageW(Page.CardListHwnd(), LB_GETCURSEL, 0, 0) == 0);

	REQUIRE(::SendMessageW(Page.CardListHwnd(), LB_SETCURSEL, static_cast<WPARAM>(-1), 0) == LB_ERR);
	REQUIRE(::SendMessageW(Page.CardListHwnd(), LB_GETCURSEL, 0, 0) == LB_ERR);
	REQUIRE(::SendMessageW(Page.CardListHwnd(), LB_SETCURSEL, 0, 0) == 0);
	::SendMessageW(Page.CardListHwnd(), WM_KEYDOWN, VK_RETURN, 1);
	REQUIRE(::GetFocus() == Page.EditorHwnd());

	REQUIRE(Page.Save());
	REQUIRE(Page.RequestLeave() == app::E_LEAVE_RESULT::ApprovedClean);
	Page.CardList().ScrollToPixel(37);
	const int nExpectedOffset = Page.CardList().ScrollOffsetDip();
	REQUIRE(nExpectedOffset > 0);
	REQUIRE(Page.PersistState(std::nullopt));

	Fixture.RecreatePage();
	C_DOCUMENT_PAGE& Reopened = Fixture.Page();
	REQUIRE(Reopened.CardList().ScrollOffsetDip() == nExpectedOffset);
	REQUIRE(::SendMessageW(Reopened.CardListHwnd(), LB_GETCURSEL, 0, 0) == 0);

	// 부분 갱신 관측 계약(결정 5 의 S1 몫): 새로 읽어도 스크롤·선택이 그대로다.
	REQUIRE(Reopened.Refresh());
	REQUIRE(Reopened.CardList().ScrollOffsetDip() == nExpectedOffset);
	REQUIRE(::SendMessageW(Reopened.CardListHwnd(), LB_GETCURSEL, 0, 0) == 0);
}

TEST_CASE("W4 preview line count boundaries one and one hundred keep height and index arithmetic",
	"[W4-render][WTL-CAP-RE-003]")
{
	C_RENDER_FIXTURE Fixture(600, 480);
	std::vector<domain::S_CARD> Cards;
	for (int nIndex = 1; nIndex <= 40; ++nIndex)
	{
		Cards.push_back(make_card(nIndex, to_utf8(L"카드 ") + std::to_string(nIndex)));
	}
	Fixture.SetCards(Cards);
	const int nLine = Fixture.MeasuredLineSpacingDip();

	for (const std::size_t nPreviewLines : { std::size_t{ 1 }, std::size_t{ 100 } })
	{
		INFO("preview lines " << nPreviewLines);
		Fixture.Projection().SetPreviewLineCount(nPreviewLines);
		Fixture.Control().OnProjectionChanged();

		const int nExpectedHeight = 2 * CARD_VERTICAL_INSET_DIP + 2 * CARD_CONTENT_VERTICAL_MARGIN_DIP +
			static_cast<int>(nPreviewLines) * nLine + (nLine + CARD_AUXILIARY_ROW_PADDING_DIP);
		REQUIRE(Fixture.Control().RowHeightDip() == nExpectedHeight);

		const int nRowHeight = Fixture.Control().RowHeightDip();
		const int nViewport = Fixture.Control().ViewportHeightDip();
		const int nMaximum = (std::max)(0, 40 * nRowHeight - nViewport);
		for (const int nOffset : { 0, nMaximum / 2, nMaximum })
		{
			Fixture.Control().ScrollToPixel(nOffset);
			REQUIRE(Fixture.Control().ScrollOffsetDip() == nOffset);
			REQUIRE(Fixture.Control().Render());
			const S_CARD_LIST_FRAME& Frame = Fixture.Control().LastFrame();
			const std::size_t nFirst = static_cast<std::size_t>(nOffset / nRowHeight);
			const std::size_t nLast = (std::min<std::size_t>)(39,
				static_cast<std::size_t>((nOffset + nViewport - 1) / nRowHeight));
			REQUIRE(Frame.nFirstVisibleRow == nFirst);
			REQUIRE(Frame.nLastVisibleRow == nLast);
			REQUIRE(::SendMessageW(Fixture.Control().m_hWnd, LB_GETTOPINDEX, 0, 0) ==
				static_cast<LRESULT>(nFirst));
		}
	}

	// 100줄 경계에서도 present 가 성립한다(비계측 sanity).
	REQUIRE(Fixture.Control().LastFrame().bPresented);
}
