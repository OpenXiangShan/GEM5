#ifndef __MEM_RUBY_SLICC_INTERFACE_XSPFMETADATA_HH__
#define __MEM_RUBY_SLICC_INTERFACE_XSPFMETADATA_HH__
#include "mem/request.hh"

namespace gem5
{

using XsPFMetaData = Request::XsMetadata;

inline
std::ostream& operator<<(std::ostream& os, const XsPFMetaData& meta)
{
    os << "pfsource[" << meta.prefetchSource << "]";
    return os;
}

}

#endif
