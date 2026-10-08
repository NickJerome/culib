#include "identity_store_platform.h"

#include <culib/strings.h>

#include <sodium.h>

#include <windows.h>

#include <string>
#include <utility>

namespace mesh {
namespace detail {
namespace {

wavvix::error file_error(const char* operation, DWORD code) {
    const wavvix::error_kind kind =
        (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS)
            ? wavvix::error_kind::already_exists :
        (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            ? wavvix::error_kind::not_found :
        code == ERROR_ACCESS_DENIED ? wavvix::error_kind::permission_denied :
        wavvix::error_kind::io;
    return wavvix::make_error(kind, std::string(operation) + ": Windows error " +
                                   std::to_string(code));
}

std::wstring temporary_path(const std::wstring& path) {
    unsigned char random[8];
    randombytes_buf(random, sizeof(random));
    static const wchar_t hex[] = L"0123456789abcdef";
    std::wstring result = path + L".tmp-";
    for (std::size_t i = 0; i < sizeof(random); ++i) {
        result += hex[random[i] >> 4];
        result += hex[random[i] & 15];
    }
    return result;
}

}  // namespace

wavvix::result<void> read_identity_bytes(
    wavvix::string_view path, unsigned char* output, std::size_t size) {
    wavvix::result<std::wstring> wide = wavvix::utf8_to_wide(path);
    if (wide.is_err()) return std::move(wide).error();
    HANDLE file = ::CreateFileW(wide.value().c_str(), GENERIC_READ, FILE_SHARE_READ,
                                0, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, 0);
    if (file == INVALID_HANDLE_VALUE) return file_error("open identity file", ::GetLastError());
    BY_HANDLE_FILE_INFORMATION info;
    LARGE_INTEGER length;
    if (!::GetFileInformationByHandle(file, &info) || !::GetFileSizeEx(file, &length)) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(file);
        return file_error("inspect identity file", code);
    }
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        length.QuadPart != static_cast<LONGLONG>(size)) {
        ::CloseHandle(file);
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "identity file must be a regular file of the expected size");
    }
    std::size_t offset = 0;
    while (offset < size) {
        DWORD got = 0;
        if (!::ReadFile(file, output + offset, static_cast<DWORD>(size - offset), &got, 0)) {
            const DWORD code = ::GetLastError();
            ::CloseHandle(file);
            return file_error("read identity file", code);
        }
        if (got == 0) {
            ::CloseHandle(file);
            return wavvix::make_error(wavvix::error_kind::io,
                                      "identity file ended while reading");
        }
        offset += got;
    }
    unsigned char trailing = 0;
    DWORD extra = 0;
    const BOOL read_extra = ::ReadFile(file, &trailing, 1, &extra, 0);
    ::CloseHandle(file);
    if (!read_extra || extra != 0)
        return wavvix::make_error(wavvix::error_kind::invalid_argument,
                                  "identity file changed while reading");
    return wavvix::result<void>();
}

wavvix::result<void> publish_identity_bytes(
    wavvix::string_view path, const unsigned char* input, std::size_t size) {
    wavvix::result<std::wstring> wide = wavvix::utf8_to_wide(path);
    if (wide.is_err()) return std::move(wide).error();
    const std::wstring final_path = wide.value();
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 4; ++attempt) {
        temporary = temporary_path(final_path);
        file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, 0, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, 0);
        if (file != INVALID_HANDLE_VALUE) break;
        const DWORD code = ::GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS)
            return file_error("create identity temp file", code);
    }
    if (file == INVALID_HANDLE_VALUE)
        return file_error("create identity temp file", ERROR_FILE_EXISTS);
    std::size_t offset = 0;
    while (offset < size) {
        DWORD wrote = 0;
        const BOOL succeeded = ::WriteFile(file, input + offset,
                                           static_cast<DWORD>(size - offset), &wrote, 0);
        if (!succeeded || wrote == 0) {
            const DWORD code = succeeded ? ERROR_WRITE_FAULT : ::GetLastError();
            ::CloseHandle(file);
            ::DeleteFileW(temporary.c_str());
            return file_error("write identity temp file", code);
        }
        offset += wrote;
    }
    if (!::FlushFileBuffers(file)) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(file);
        ::DeleteFileW(temporary.c_str());
        return file_error("sync identity temp file", code);
    }
    if (!::CloseHandle(file)) {
        const DWORD code = ::GetLastError();
        ::DeleteFileW(temporary.c_str());
        return file_error("close identity temp file", code);
    }
    if (!::MoveFileW(temporary.c_str(), final_path.c_str())) {
        const DWORD code = ::GetLastError();
        ::DeleteFileW(temporary.c_str());
        return file_error("publish identity file", code);
    }
    return wavvix::result<void>();
}

}  // namespace detail
}  // namespace mesh
