// SPDX-License-Identifier: Apache-2.0
#include "common/file.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace RayoPdf::File {

InputFile::~InputFile() {
    Close();
}

void InputFile::Close() {
    if (mapped_ && data_) munmap(const_cast<char*>(data_), size_);
    data_ = nullptr;
    size_ = 0;
    mapped_ = false;
    owned_.clear();
}

ReadStatus InputFile::Open(const std::string& path, uint64_t maximumBytes) {
    Close();
    // Non-blocking, so a FIFO cannot stall the open; fstat then decides before any read.
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) return ReadStatus::NotFound;
    struct stat info;
    if (fstat(fd, &info) != 0) {
        close(fd);
        return ReadStatus::Failed;
    }
    if (!S_ISREG(info.st_mode)) {
        close(fd);
        return ReadStatus::NotRegular;
    }
    const uint64_t size = static_cast<uint64_t>(info.st_size);
    if (size > maximumBytes || size > SIZE_MAX / 2) {
        close(fd);
        return ReadStatus::TooLarge;
    }
    if (size == 0) {
        close(fd);
        data_ = owned_.data();
        return ReadStatus::Ok;
    }
    void* map = mmap(nullptr, static_cast<size_t>(size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
        close(fd);
        data_ = static_cast<const char*>(map);
        size_ = static_cast<size_t>(size);
        mapped_ = true;
        return ReadStatus::Ok;
    }
    // Some file systems cannot be mapped: read the file instead.
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    owned_.resize(static_cast<size_t>(size));
    size_t done = 0;
    while (done < owned_.size()) {
        const ssize_t got = read(fd, owned_.data() + done, owned_.size() - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        done += static_cast<size_t>(got);
    }
    close(fd);
    if (done != owned_.size()) {
        owned_.clear();
        return ReadStatus::Failed;
    }
    data_ = owned_.data();
    size_ = owned_.size();
    return ReadStatus::Ok;
}

namespace {

bool WriteFd(int fd, std::string_view content) {
    size_t done = 0;
    while (done < content.size()) {
        const ssize_t put = write(fd, content.data() + done, content.size() - done);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        done += static_cast<size_t>(put);
    }
    return true;
}

} // namespace

bool WriteAll(const std::string& path, std::string_view content) {
    if (path == "-") return WriteFd(STDOUT_FILENO, content);
    std::string temp = path + ".tmp-XXXXXX";
    const int fd = mkostemp(temp.data(), O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = WriteFd(fd, content);
    // mkostemp creates 0600; give the result the usual permissions under the umask.
    const mode_t mask = umask(0);
    umask(mask);
    ok = fchmod(fd, 0666 & ~mask) == 0 && ok;
    ok = close(fd) == 0 && ok;
    if (ok && rename(temp.c_str(), path.c_str()) == 0) return true;
    unlink(temp.c_str());
    return false;
}

} // namespace RayoPdf::File
