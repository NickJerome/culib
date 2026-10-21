#include "support.h"

#if defined(_WIN32)

#include "platform/text_win.h"
#include "platform/win32.h"

std::string scratch_name(const std::string& stem) {
    return stem + "_" + std::to_string(::GetCurrentProcessId());
}

bool write_scratch_file(const std::string& path, const std::string& content) {
    std::wstring wide;
    if (!cu::detail::to_wide(path, wide)) {
        return false;
    }
    const HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool complete = content.empty() || (::WriteFile(file, content.data(), static_cast<DWORD>(content.size()),
                                                          &written, 0) &&
                                              written == content.size());
    ::CloseHandle(file);
    return complete;
}

void remove_scratch_file(const std::string& path) {
    std::wstring wide;
    if (cu::detail::to_wide(path, wide)) {
        ::DeleteFileW(wide.c_str());
    }
}

#else

#include <cstdio>
#include <fstream>

#include <unistd.h>

std::string scratch_name(const std::string& stem) {
    return stem + "_" + std::to_string(::getpid());
}

bool write_scratch_file(const std::string& path, const std::string& content) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

void remove_scratch_file(const std::string& path) {
    std::remove(path.c_str());
}

#endif
