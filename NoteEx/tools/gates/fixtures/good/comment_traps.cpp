// 주석 안의 금지 include 는 위반이 아니다. 스캐너가 주석을 걷어내는지 본다.

/*
 * 블록 주석 안의 줄머리 include:
#include <windows.h>
 */

// 줄 주석 안의 include: #include <windowsx.h>

// 백슬래시로 다음 줄까지 이어지는 줄 주석 \
#include <d3d11.h>

/* 한 줄짜리 블록 주석 뒤의 include 는 진짜 지시문이다(표준상 주석은 공백) */ #include <vector>

namespace pynote::core {

int trap_count() {
    return 3; /* 꼬리 주석에도 #include <atlbase.h> 를 넣어 둔다 */
}

}  // namespace pynote::core
