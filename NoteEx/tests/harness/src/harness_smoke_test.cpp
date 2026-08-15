#include <catch_amalgamated.hpp>

#include <pynote/harness/win32_harness.h>

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <vector>

// C군 하네스의 연기 시험. 매 빌드마다 도는 시험이므로 통과 경로의 벽시계 비용은
// 유계 대기 음성 사례의 50ms 예산이 전부다.
namespace harness = pynote::harness;

TEST_CASE("하네스 창은 테스트 프로세스 안의 진짜 창이다", "[harness][win32]")
{
    HWND first = nullptr;
    {
        harness::TestWindow window;
        first = window.hwnd();

        REQUIRE(first != nullptr);
        // 널이 아닌 것만으로는 실창의 증거가 아니므로 Win32 에 되묻는다.
        REQUIRE(::IsWindow(first) != FALSE);
        REQUIRE(::GetParent(first) == nullptr);

        DWORD process_id = 0;
        const DWORD thread_id = ::GetWindowThreadProcessId(first, &process_id);
        REQUIRE(thread_id == ::GetCurrentThreadId());
        REQUIRE(process_id == ::GetCurrentProcessId());
    }

    // 소멸자가 HWND 를 확정적으로 회수했는가.
    REQUIRE(::IsWindow(first) == FALSE);

    // 클래스 등록이 새면 두 번째 생성이 ERROR_CLASS_ALREADY_EXISTS 로 던진다.
    {
        harness::TestWindow window;
        REQUIRE(::IsWindow(window.hwnd()) != FALSE);
        REQUIRE(window.hwnd() != first);
    }
}

TEST_CASE("합성 메시지가 인스턴스 핸들러까지 도달한다", "[harness][win32]")
{
    constexpr UINT kProbeMessage = WM_APP + 1;

    harness::TestWindow window;
    WPARAM observed = 0;
    int handler_calls = 0;
    window.set_handler([&](UINT message, WPARAM wparam, LPARAM, LRESULT& result) {
        if (message != kProbeMessage) {
            return false;
        }
        observed = wparam;
        ++handler_calls;
        result = 0x5A5A;
        return true;
    });

    const LRESULT reply = harness::send_message(window.hwnd(), kProbeMessage, WPARAM{0x1234}, 0);

    // 핸들러만이 세울 수 있는 상태로 판정한다 — 반환값만 보면 DefWindowProc 와
    // 구분되지 않는다.
    REQUIRE(handler_calls == 1);
    REQUIRE(observed == WPARAM{0x1234});
    REQUIRE(reply == 0x5A5A);
    REQUIRE(window.count_of(kProbeMessage) == 1u);
    REQUIRE(window.handler_exception_count() == 0u);

    window.clear_messages();
    harness::send_key(window.hwnd(), 'A', L'A');

    const std::vector<harness::RecordedMessage>& log = window.messages();
    REQUIRE(log.size() == 3u);
    REQUIRE(log[0].message == WM_KEYDOWN);
    REQUIRE(log[1].message == WM_CHAR);
    REQUIRE(log[2].message == WM_KEYUP);
    REQUIRE(log[1].wparam == static_cast<WPARAM>(L'A'));
    // 스캔코드 필드(16~23비트)와 해제 전이 비트(31비트)가 실제 키 입력처럼 채워진다.
    REQUIRE(((log[0].lparam >> 16) & 0xFF) != 0);
    REQUIRE((log[2].lparam & (static_cast<LPARAM>(1) << 31)) != 0);
}

TEST_CASE("핸들러에서 빠져나온 예외는 삼켜지되 셈으로 남는다", "[harness][win32]")
{
    constexpr UINT kThrowingMessage = WM_APP + 3;

    harness::TestWindow window;
    window.set_handler([](UINT message, WPARAM, LPARAM, LRESULT&) -> bool {
        if (message == kThrowingMessage) {
            throw std::runtime_error("handler probe");
        }
        return false;
    });

    // 예외가 윈도우 프로시저 경계를 넘지 않는다.
    REQUIRE_NOTHROW(harness::send_message(window.hwnd(), kThrowingMessage, 0, 0));
    // 삼킨 사실이 관측 가능해야 나중 파도가 거짓 초록을 받지 않는다.
    REQUIRE(window.handler_exception_count() == 1u);
}

TEST_CASE("게시된 메시지는 펌프가 돌아야만 전달된다", "[harness][win32]")
{
    constexpr UINT kPostedMessage = WM_APP + 2;

    harness::TestWindow window;
    REQUIRE(::PostMessageW(window.hwnd(), kPostedMessage, 7, 0) != FALSE);

    // 게시 직후에는 큐에만 있고 윈도우 프로시저에 닿지 않았다. 이 단언이 아래
    // wait_until 을 비어 있지 않게 만든다 — 펌프를 빼면 여기는 통과하고 대기가
    // 타임아웃으로 실패한다.
    REQUIRE_FALSE(window.received(kPostedMessage));

    REQUIRE(harness::wait_until([&] { return window.received(kPostedMessage); },
                                std::chrono::milliseconds(500)));
    REQUIRE(window.count_of(kPostedMessage) == 1u);

    window.clear_messages();
    for (WPARAM index = 0; index < 3; ++index) {
        REQUIRE(::PostMessageW(window.hwnd(), kPostedMessage, index, 0) != FALSE);
    }
    REQUIRE_FALSE(window.received(kPostedMessage));

    const std::size_t dispatched = harness::drain_messages();
    REQUIRE(dispatched >= 3u);
    REQUIRE(window.count_of(kPostedMessage) == 3u);
}

TEST_CASE("유계 대기는 술어가 참이 되지 않아도 예산 안에서 끝난다", "[harness][win32]")
{
    const std::chrono::milliseconds budget(50);

    const auto started = std::chrono::steady_clock::now();
    const bool satisfied = harness::wait_until([] { return false; }, budget);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(satisfied);
    REQUIRE(elapsed >= budget);
    REQUIRE(elapsed < std::chrono::seconds(2));

    // 대기 중 메시지가 오가면 내부 대기가 중도에 깨어난다. 그때도 예산을 다 써야 한다
    // — 잔여 시간을 밀리초로 절삭하면 여기서 예산보다 일찍 포기한다.
    {
        harness::TestWindow window;
        const auto started_busy = std::chrono::steady_clock::now();
        const bool busy_satisfied = harness::wait_until(
            [&] {
                ::PostMessageW(window.hwnd(), WM_APP + 4, 0, 0);
                return false;
            },
            budget);
        const auto busy_elapsed = std::chrono::steady_clock::now() - started_busy;

        REQUIRE_FALSE(busy_satisfied);
        REQUIRE(busy_elapsed >= budget);
    }

    // 이미 참인 술어는 예산을 태우지 않는다(30초 예산이 즉시 반환돼야 한다).
    const auto started_true = std::chrono::steady_clock::now();
    REQUIRE(harness::wait_until([] { return true; }, std::chrono::seconds(30)));
    REQUIRE(std::chrono::steady_clock::now() - started_true < std::chrono::seconds(1));
}
