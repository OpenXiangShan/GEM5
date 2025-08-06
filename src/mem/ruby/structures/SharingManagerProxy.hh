#ifndef __MEM_RUBY_STRUCTURES_SHARING_MANAGER_PROXY_HH__
#define __MEM_RUBY_STRUCTURES_SHARING_MANAGER_PROXY_HH__

#include "base/named.hh"
#include "mem/ruby/structures/SharingManager.hh"

namespace gem5
{

namespace ruby
{

class AbstractController;

class SharingManagerProxy : public Named
{
public:
    SharingManagerProxy();
    SharingManagerProxy(AbstractController* _parent,
                        SharingManager* _sm);
    Coordinate getCoordinate() const;


private:
    SharingManager* m_sharing_manager;
};

}

}
#endif // __MEM_RUBY_STRUCTURES_SHARING_MANAGER_PROXY_HH__
