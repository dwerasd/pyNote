#pragma once

// C군 테스트 하네스 — 순수 로직으로 환원되지 않는 동작(실제 포커스, IME 조합,
// 메시지 전파, DPI, 페인팅)을 테스트 프로세스 안의 진짜 창으로 관측하기 위한
// 원시 도구. 창·펌프·합성 입력·유계 대기 네 가지만 제공하며 컨트롤 골격은 없다.
//
// 스레드 계약: TestWindow 는 생성한 스레드가 소유하고 그 스레드에서만 펌프·전송한다.
// SendMessage 가 윈도우 프로시저 직접 호출이 되는 것(반환 시점에 효과 관측)이
// 이 하네스의 전제이며, 다른 스레드의 창에 보내면 그 전제가 깨진다.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <vector>

namespace pynote::harness {

// 윈도우 프로시저가 실제로 받은 메시지의 기록.
struct RecordedMessage {
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
};

// 인스턴스별 메시지 처리기. 처리했으면 result 를 채우고 true, 아니면 false 를
// 반환한다(false = DefWindowProc 위임).
using MessageHandler = std::function<bool(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result)>;

struct TestWindowOptions {
    const wchar_t* title = L"pynote harness";
    int width = 400;
    int height = 300;
    // 페인팅·활성화가 필요한 시험만 true. 기본값은 개발자 데스크톱의 포커스를
    // 빼앗지 않기 위해 숨김이며, 보일 때도 SW_SHOWNOACTIVATE 로 띄운다.
    bool visible = false;
};

// 테스트 프로세스 안에 만드는 실제 최상위 창. 소멸자에서 HWND 를 파괴하고
// 마지막 인스턴스가 사라질 때 윈도우 클래스 등록도 해제한다.
class TestWindow {
public:
    explicit TestWindow(const TestWindowOptions& options = {});
    ~TestWindow();

    // 윈도우 프로시저가 GWLP_USERDATA 에 this 를 들고 있으므로 복사·이동 불가.
    TestWindow(const TestWindow&) = delete;
    TestWindow& operator=(const TestWindow&) = delete;

    HWND hwnd() const noexcept { return hwnd_; }

    void set_handler(MessageHandler handler);

    const std::vector<RecordedMessage>& messages() const noexcept { return messages_; }
    bool received(UINT message) const noexcept;
    std::size_t count_of(UINT message) const noexcept;
    void clear_messages() noexcept;

    // 핸들러에서 빠져나온 예외의 누적 횟수. 윈도우 프로시저 경계를 넘는 예외는
    // 정의되지 않은 동작이라 삼켜야 하지만, 삼킨 사실을 관측 가능하게 남긴다.
    std::size_t handler_exception_count() const noexcept { return handler_exception_count_; }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    HWND hwnd_ = nullptr;
    MessageHandler handler_;
    std::vector<RecordedMessage> messages_;
    std::size_t handler_exception_count_ = 0;
};

// 스레드 큐에 이미 들어 있는 메시지만 비운다(대기하지 않는다). 반환값은 디스패치한
// 개수. max_messages 에 도달하면 메시지 폭주로 보고 예외를 던진다 — 큐를 다 비우지
// 못한 채 "비웠다"고 반환하면 이후 단언 전부가 조용히 거짓이 된다.
std::size_t drain_messages(std::size_t max_messages = 512);

// 술어가 참이 될 때까지 메시지를 펌프하며 기다린다. steady_clock 기준 timeout 을
// 넘기면 매달리거나 단언하지 않고 false 를 반환한다.
bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout);

// 합성 입력의 단일 경로. 호출 스레드가 소유한 창이므로 SendMessage 는 윈도우
// 프로시저 직접 호출이고, 반환 시점에 효과가 이미 관측 가능하다.
// IME 조합 시험(WM_IME_STARTCOMPOSITION/COMPOSITION/ENDCOMPOSITION)도 이 함수를
// 그대로 쓴다.
LRESULT send_message(HWND target, UINT message, WPARAM wparam, LPARAM lparam);

// 키 입력 합성. character 가 0 이 아니면 WM_KEYDOWN 과 WM_KEYUP 사이에 WM_CHAR 를
// 끼워 넣는다(0 = 문자 발생 없는 키). 내부는 전부 send_message 경유다.
void send_key(HWND target, UINT virtual_key, wchar_t character = 0);

}  // namespace pynote::harness
