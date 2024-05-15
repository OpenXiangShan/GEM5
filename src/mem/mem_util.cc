#include "mem/mem_util.hh"

#include <fcntl.h>
#include <linux/mman.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <string>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>            // uuid class
#include <boost/uuid/uuid_generators.hpp> // generators
#include <boost/uuid/uuid_io.hpp>

#include "base/logging.hh"
#include "sim/core.hh"
#include "sim/root.hh"

namespace gem5
{
namespace mem_util
{
const unsigned pageSize = 4096;
uint8_t zeroPage[pageSize] = {0};
bool
isPageZero(uint8_t *page, std::size_t size)
{
    assert (size == pageSize);
    return !memcmp(page, zeroPage, size);
}

DedupMemory::DedupMemory()
{
    initDedupMemory();
}

void
DedupMemory::initDedupMemory()
{
    registerExitCallback([this]() { this->releaseResources(); });
}

uint8_t*
DedupMemory::createSharedReadOnlyRoot(size_t size)
{
    // currently we assume only one chunk of memory and initiate it only once
    if (rootPMem != nullptr) {
        panic("Shared memory object already created at %#lx\n", (uint64_t)rootPMem);
    }

    boost::uuids::uuid uuid = boost::uuids::random_generator()();
    backedFilePath = boost::uuids::to_string(uuid);
    shmFd = memfd_create(backedFilePath.c_str(), 0);
    // backedFilePath = "/gem5-" + boost::uuids::to_string(uuid);
    // shmFd = shm_open(backedFilePath.c_str(), O_RDWR | O_CREAT, 0644);
    memSize = size;

    if (shmFd == -1) {
        panic("Failed to create shared memory object: %s\n", backedFilePath.c_str());
    }
    if (ftruncate64(shmFd, size) == -1) {
        panic("Failed to resize shared memory object: %s to %lu\n", backedFilePath.c_str(), size);
    }
    rootPMem = (uint8_t *)mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_HUGE_2MB, shmFd, 0);
    // close(shmFd);
    warn("Created shared memory object: %s\n", backedFilePath.c_str());
    assert(rootPMem != MAP_FAILED);
    return rootPMem;
}

void
DedupMemory::initRootFromExistingFile(int fd, const char* file_path)
{
    panic("This fucntion has not been tested yet\n");
    assert(backedFilePath.empty() && rootPMem == nullptr);
    // tell file size
    auto file_size = lseek(fd, 0, SEEK_END);
    memSize = file_size;
    warn("Memory size set to the same as file size = %lu\n", file_size);

    rootPMem = (uint8_t *)mmap(nullptr,file_size, PROT_WRITE, MAP_SHARED, fd, 0);

    assert(rootPMem != MAP_FAILED);
    backedFilePath = file_path;
    fromExistingFile = true;
}

uint8_t*
DedupMemory::createCopyOnWriteBranch(size_t size)
{
    if (rootPMem) {
        munmap(rootPMem, memSize);
        rootPMem = nullptr;
    }
    assert(size == memSize);
    // create a private mapping of the shared memory object, which is expected to copy-on-write
    if (!backedFilePath.empty() && shmFd == -1) {
        warn("Reopen shared memory object: %s\n", backedFilePath.c_str());
        shmFd = open(backedFilePath.c_str(), O_RDWR);
    }
    uint8_t *pmem =
        (uint8_t *)mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, shmFd, 0);
    if (pmem == MAP_FAILED) {
        panic("Failed to create copy-on-write branch, errno=%i\n", errno);
    }
    branchPMems.push_back(pmem);
    warn("Create new COW mirror %#lx, current branch count = %lu\n", (uint64_t)pmem, branchPMems.size());
    return pmem;
}

DedupMemory::~DedupMemory()
{
    releaseResources();
}

void
DedupMemory::releaseResources()
{
    for (auto pmem : branchPMems) {
        munmap(pmem, memSize);
    }
    branchPMems.clear();
    if (shmFd != -1) {
        close(shmFd);
        shmFd = -1;
    }
}

void
DedupMemory::syncRootUpdates()
{
    msync(rootPMem, memSize, MS_SYNC | MS_INVALIDATE);
}

} // namespace mem_util
} // namespace gem5
