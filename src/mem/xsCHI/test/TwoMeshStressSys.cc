#include "mem/xsCHI/test/TwoMeshStressSys.hh"

#include "base/logging.hh"

namespace gem5
{
namespace xsCHI
{

TwoMeshStressSys::TwoMeshStressSys(const Params &p)
    : ClockedObject(p),
      sender(p.sender),
      receiver(p.receiver),
      mesh0(p.mesh0),
      mesh1(p.mesh1)
{
    panic_if(sender == nullptr || receiver == nullptr,
             "TwoMeshStressSys requires sender and receiver endpoints");
    panic_if(mesh0 == nullptr || mesh1 == nullptr,
             "TwoMeshStressSys requires mesh0 and mesh1");

    CHIPort *senderPort = sender->getNetworkPort();
    CHIPort *receiverPort = receiver->getNetworkPort();
    CHIPort *mesh0Local0 = mesh0->getLocal0Port();
    CHIPort *mesh0East = mesh0->getEastPort();
    CHIPort *mesh1West = mesh1->getWestPort();
    CHIPort *mesh1Local0 = mesh1->getLocal0Port();

    panic_if(senderPort == nullptr || receiverPort == nullptr,
             "TwoMeshStressSys got null endpoint networkPort");
    panic_if(mesh0Local0 == nullptr || mesh0East == nullptr ||
                 mesh1West == nullptr || mesh1Local0 == nullptr,
             "TwoMeshStressSys requires mesh0.local0/east and mesh1.west/local0");

    senderPort->connect(mesh0Local0);
    mesh0East->connect(mesh1West);
    mesh1Local0->connect(receiverPort);
}

void
TwoMeshStressSys::init()
{
    ClockedObject::init();
}

} // namespace xsCHI
} // namespace gem5
