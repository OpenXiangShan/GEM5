#include "mem/ruby/structures/SharingManagerProxy.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"

namespace gem5 {

namespace ruby {


SharingManagerProxy::SharingManagerProxy()
    : Named("EmptySharingManagerProxy"), m_sharing_manager(nullptr)
{

}

SharingManagerProxy::SharingManagerProxy(AbstractController* _parent,
                                   SharingManager* _sm)
    : Named(_parent->name()), m_sharing_manager(_sm)
{
}

Coordinate
SharingManagerProxy::getCoordinate() const
{
    return m_sharing_manager->getCoordinate();
}

}
}
