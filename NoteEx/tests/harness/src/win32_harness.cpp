#include <pynote/harness/win32_harness.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace pynote::harness {
namespace {

constexpr wchar_t kWindowClassName[] = L"PyNoteHarnessWindow";

// 클래스 등록은 프로세스 전역이라 인스턴스별로 참조수를 세어 마지막 창이 사라질 때
// 해제한다. 한 프로세스 안에서 생성·파괴를 반복해도 등록이 누적되지 않는다.
// 갱신에 잠금이 없는 것은 위 헤더의 단일 스레드 소유 계약에 기댄 것이다.
std::size_t g_class_refcount = 0;

HINSTANCE module_handle() noexcept
{
    return ::GetModuleHandleW(nullptr);
}

// 윈도우 프로시저는 TestWindow 의 비공개 정적 멤버라 호출부(생성자)에서 넘겨받는다.
void acquire_window_class(WNDPROC window_proc)
{
    if (g_class_refcount == 0) {
        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = module_handle();
        // 커서를 지정하지 않는다 — IDC_ARROW 는 UNICODE 정의 여부로 A/W 가 갈리는
        // 매크로라, 시험 창에 불필요한 의존을 만들지 않는다.
        window_class.hCursor = nullptr;
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kWindowClassName;

        if (::RegisterClassExW(&window_class) == 0) {
            throw std::runtime_error("RegisterClassExW 실패, GetLastError=" +
                                     std::to_string(::GetLastError()));
        }
    }
    ++g_class_refcount;
}

void release_window_class() noexcept
{
    --g_class_refcount;
    if (g_class_refcount == 0) {
        ::UnregisterClassW(kWindowClassName, module_handle());
    }
}

}  // namespace

LRESULT CALLBACK TestWindow::wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    TestWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<TestWindow*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    else {
        self = reinterpret_cast<TestWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    // WM_GETMINMAXINFO 처럼 WM_NCCREATE 보다 먼저 오는 메시지는 인스턴스가 없다.
    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return self->handle(hwnd, message, wparam, lparam);
}

LRESULT TestWindow::handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    messages_.push_back(RecordedMessage{message, wparam, lparam});

    bool handled = false;
    LRESULT result = 0;
    if (handler_) {
        try {
            handled = handler_(message, wparam, lparam, result);
        }
        catch (...) {
            ++handler_exception_count_;
        }
    }

    if (!handled) {
        result = ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    if (message == WM_NCDESTROY) {
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
    }
    return result;
}

TestWindow::TestWindow(const TestWindowOptions& options)
{
    acquire_window_class(&TestWindow::wnd_proc);

    const DWORD style = options.visible ? (WS_OVERLAPPEDWINDOW | WS_VISIBLE) : WS_OVERLAPPEDWINDOW;
    const HWND created = ::CreateWindowExW(0,
                                           kWindowClassName,
                                           options.title,
                                           style,
                                           CW_USEDEFAULT,
                                           CW_USEDEFAULT,
                                           options.width,
                                           options.height,
                                           nullptr,
                                           nullptr,
                                           module_handle(),
                                           this);
    if (created == nullptr) {
        const DWORD error = ::GetLastError();
        release_window_class();
        throw std::runtime_error("CreateWindowExW 실패, GetLastError=" + std::to_string(error));
    }
    hwnd_ = created;

    if (options.visible) {
        // 시험이 개발자의 활성 창을 빼앗지 않도록 활성화 없이 띄운다.
        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
}

TestWindow::~TestWindow()
{
    if (hwnd_ != nullptr) {
        // 같은 스레드의 창이라 WM_DESTROY/WM_NCDESTROY 가 여기서 동기 전달된다.
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    release_window_class();
}

void TestWindow::set_handler(MessageHandler handler)
{
    handler_ = std::move(handler);
}

bool TestWindow::received(UINT message) const noexcept
{
    return count_of(message) != 0;
}

std::size_t TestWindow::count_of(UINT message) const noexcept
{
    return static_cast<std::size_t>(
        std::count_if(messages_.begin(), messages_.end(), [message](const RecordedMessage& recorded) {
            return recorded.message == message;
        }));
}

void TestWindow::clear_messages() noexcept
{
    messages_.clear();
}

std::size_t drain_messages(std::size_t max_messages)
{
    std::size_t dispatched = 0;
    MSG msg = {};

    // WM_QUIT 도 다른 메시지와 같이 배수한다 — 이 하네스는 프로세스를 종료시키지 않는다.
    while (dispatched < max_messages && ::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
        ++dispatched;
    }

    // 상한에 걸렸어도 큐가 마침 비었으면 정상 배수다 — 잔량이 남아 있을 때만 폭주로 본다.
    if (dispatched == max_messages && ::PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE) != FALSE) {
        throw std::runtime_error("drain_messages: 상한 " + std::to_string(max_messages) +
                                 " 도달, 메시지 폭주 의심");
    }
    return dispatched;
}

bool wait_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    if (predicate()) {
        return true;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        drain_messages();
        if (predicate()) {
            return true;
        }

        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            continue;  // 종료 판정은 루프 조건 한 곳만 소유한다.
        }

        // 밀리초 미만 잔량을 절삭하면 예산을 다 쓰기 전에 빠져나온다(요청한 timeout 을
        // 지키지 못한다). 올림해 최소 1ms 는 대기하고, 초과분은 루프 조건이 잘라낸다.
        const auto wait_slice = std::chrono::ceil<std::chrono::milliseconds>(remaining);

        // 메시지 도착 또는 잔여 예산 소진 중 먼저 오는 쪽까지만 잠든다. Sleep 회전과
        // 달리 큐를 굶기지 않으며, 대기 상한은 항상 steady_clock 기준 deadline 이다.
        const DWORD waited = ::MsgWaitForMultipleObjectsEx(
            0, nullptr, static_cast<DWORD>(wait_slice.count()), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waited == WAIT_FAILED) {
            throw std::runtime_error("MsgWaitForMultipleObjectsEx 실패, GetLastError=" +
                                     std::to_string(::GetLastError()));
        }
    }

    // 마지막 펌프와 예산 소진 사이에 조건이 성립했을 수 있으므로 한 번 더 본다.
    return predicate();
}

LRESULT send_message(HWND target, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (::IsWindow(target) == FALSE) {
        throw std::logic_error("send_message: 유효하지 않은 HWND");
    }
    if (::GetWindowThreadProcessId(target, nullptr) != ::GetCurrentThreadId()) {
        // 다른 스레드의 창이면 SendMessage 가 직접 호출이 아니라 큐 경유 블로킹이 되어
        // "반환 시점에 효과가 관측된다"는 계약이 깨진다.
        throw std::logic_error("send_message: 호출 스레드가 소유하지 않은 창");
    }
    return ::SendMessageW(target, message, wparam, lparam);
}

void send_key(HWND target, UINT virtual_key, wchar_t character)
{
    const UINT scan_code = ::MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
    const LPARAM key_down = static_cast<LPARAM>(1) | (static_cast<LPARAM>(scan_code) << 16);
    // 비트 30 = 직전 키 상태, 비트 31 = 전이 상태. 실제 키 해제와 같은 lParam 을 만든다.
    const LPARAM key_up = key_down | (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);

    send_message(target, WM_KEYDOWN, static_cast<WPARAM>(virtual_key), key_down);
    if (character != 0) {
        send_message(target, WM_CHAR, static_cast<WPARAM>(character), key_down);
    }
    send_message(target, WM_KEYUP, static_cast<WPARAM>(virtual_key), key_up);
}

}  // namespace pynote::harness
