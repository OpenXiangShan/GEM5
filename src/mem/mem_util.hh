#ifndef __MEM_MEM_UTIL_HH_
#define __MEM_MEM_UTIL_HH_

#include <cstddef>
#include <cstdint>
#include <vector>

#include <base/types.hh>

namespace gem5
{
namespace mem_util
{
    // a 4k zero page
    bool isPageZero(uint8_t *page, std::size_t size);

    class DedupMemory
    {
      public:
        DedupMemory();
        void releaseResources();
        ~DedupMemory();

        int shmFd{-1};
        std::string backedFilePath;
        bool fromExistingFile{false};
        size_t memSize;
        uint8_t *rootPMem = nullptr;
        std::vector<uint8_t *> branchPMems;

        uint8_t* createSharedReadOnlyRoot(std::size_t size);

        uint8_t* createCopyOnWriteBranch(std::size_t size);

        uint8_t *createCopyOnWriteBranch() { return createCopyOnWriteBranch(memSize); }

        void syncRootUpdates();

        void initRootFromExistingFile(int fd, const char* file_path);
    };
}  // namespace mem_util
}  // namespace gem5

#endif // __MEM_MEM_UTIL_HH_
