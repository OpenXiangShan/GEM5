//
//created by zhenh on 2024/04/19
//

#ifndef GEM5_NEMUPLIC_HH
#define GEM5_NEMUPLIC_HH

#include "dev/io_device.hh"
#include "params/NemuPlic.hh"

namespace gem5
{

class NemuPlic : public BasicPioDevice
{
  public:
    typedef NemuPlicParams Params;
    NemuPlic(const Params &p);

    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
};

}
#endif