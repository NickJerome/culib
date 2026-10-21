#include "text_win.h"

#include "win32.h"

#include <climits>

namespace cu {
namespace detail {

bool to_wide(string_view text, std::wstring& wide) {
    wide.clear();
    if (text.empty()) {
        return true;
    }
    if (text.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    const int length = static_cast<int>(text.size());
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, 0, 0);
    if (needed <= 0) {
        return false;
    }
    wide.resize(static_cast<std::size_t>(needed));
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, &wide[0], needed) == needed;
}

std::string to_utf8(const wchar_t* text, std::size_t size) {
    std::string utf8;
    if (size == 0 || size > static_cast<std::size_t>(INT_MAX)) {
        return utf8;
    }
    const int length = static_cast<int>(size);
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, length, 0, 0, 0, 0);
    if (needed <= 0) {
        return utf8;
    }
    utf8.resize(static_cast<std::size_t>(needed));
    ::WideCharToMultiByte(CP_UTF8, 0, text, length, &utf8[0], needed, 0, 0);
    return utf8;
}

}  // namespace detail
}  // namespace cu
