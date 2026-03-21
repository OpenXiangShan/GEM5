#include "mem/xsCHI/base/CHIPort.hh"

#include <cassert>
#include <utility>

#include "base/trace.hh"
#include "base/types.hh"
#include "debug/CHIPort.hh"
#include "mem/xsCHI/base/flit.hh"
#include "mem/xsCHI/base/params.hh"
#include "params/ClockedObject.hh"
#include "sim/cur_tick.hh"
#include "sim/port.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace xsCHI
{
    namespace
    {
    inline size_t
    channelIndex(Flit::CHI_CHN_TYPE channel)
    {
        return static_cast<size_t>(channel);
    }
    }

    CHIPort::CHIPort(const Params &p)
    : ClockedObject(p),
        //   id(_id),
          _connected(false),
                    blocked(false),
          connected_port(nullptr),
          owner_module(nullptr),
          receive_callback(nullptr),
          req_buffer(),
        //   req_handle_event([this]{OnHandleEventCallback_REQ(); },name()),
          snp_buffer(),
        //   snp_handle_event([this]{OnHandleEventCallback_SNP(); },name()),
          dat_buffer(),
        //   dat_handle_event([this]{OnHandleEventCallback_DAT(); },name()),
          rsp_buffer(),
        //   rsp_handle_event([this]{OnHandleEventCallback_RSP(); },name()),
          global_handle_event([this]{OnHandleEventCallback(); },name()),
          req_credit(0),
          req_last_send_time(0),
          snp_credit(0),
          snp_last_send_time(0),
          dat_credit(0),
          dat_last_send_time(0),
          rsp_credit(0),
          rsp_last_send_time(0),
          BUFFER_SIZE(p.recv_buffer_size)
    {
        //make sure sender_credit==recver_buffer_size
    }
    // CHIPort::CHIPort(const Params &p,SimObject* module, const std::string& _name, int recv_buffer_size)
    //     : ClockedObject(p),
    //       portName(_name),
    //     //   id(_id),
    //       _connected(false),
    //       connected_port(nullptr),
    //       owner_module(module),
    //       receive_callback(nullptr),
    //       req_buffer(),
    //     //   req_handle_event([this]{OnHandleEventCallback_REQ(); },name()),
    //       snp_buffer(),
    //     //   snp_handle_event([this]{OnHandleEventCallback_SNP(); },name()),
    //       dat_buffer(),
    //     //   dat_handle_event([this]{OnHandleEventCallback_DAT(); },name()),
    //       rsp_buffer(),
    //     //   rsp_handle_event([this]{OnHandleEventCallback_RSP(); },name()),
    //       global_handle_event([this]{OnHandleEventCallback(); },name()),
    //       req_credit(0),
    //       req_last_send_time(0),
    //       snp_credit(0),
    //       snp_last_send_time(0),
    //       dat_credit(0),
    //       dat_last_send_time(0),
    //       rsp_credit(0),
    //       rsp_last_send_time(0),
    //       BUFFER_SIZE(recv_buffer_size)
    // {
    //     //make sure sender_credit==recver_buffer_size
    // }
    bool CHIPort::send(FlitPtr& data)
    {
        Flit::CHI_CHN_TYPE channel_type = data->get_Flit_Channel_Type();
        const size_t ch_idx = channelIndex(channel_type);
        //do credit check and check if the port has sent other flit already
        switch (channel_type) {
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
                if (req_credit == 0 || curCycle() <= req_last_send_time) {
                    if (req_credit == 0) {
                        channel_blocked_by_credit[ch_idx] = true;
                    }
                    DPRINTF(CHIPort,"CHIPort %s in module %s has no credit to send request flit :%d\
                        or already sent a request flit in the current cycle\n",
                          owner_module->name(), owner_module->name(), req_credit);
                    return false; // 没有信用，发送失败
                }
                channel_blocked_by_credit[ch_idx] = false;
                req_credit--;
                DPRINTF(CHIPort,"%s:REQ channel send, remain Credit:%d, req_last_send_time:%d, curcycle:%d\n",this->name(),req_credit,req_last_send_time,curCycle());
                req_last_send_time = curCycle();
                break;
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
                if (snp_credit == 0 || curCycle() <= snp_last_send_time) {
                    if (snp_credit == 0) {
                        channel_blocked_by_credit[ch_idx] = true;
                    }
                    DPRINTF(CHIPort,"CHIPort %s in module %s has no credit to send request flit :%d\
                        or already sent a request flit in the current cycle\n",
                          owner_module->name(), owner_module->name(), snp_credit);
                    return false; // 没有信用，发送失败
                }
                channel_blocked_by_credit[ch_idx] = false;
                snp_credit--;
                DPRINTF(CHIPort,"%s:SNP channel send, remain Credit:%d, snp_last_send_time:%d, curcycle:%d\n",this->name(),snp_credit,snp_last_send_time,curCycle());
                snp_last_send_time = curCycle();
                break;
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
                if (dat_credit == 0 || curCycle() <= dat_last_send_time) {
                    if (dat_credit == 0) {
                        channel_blocked_by_credit[ch_idx] = true;
                    }
                    DPRINTF(CHIPort,"CHIPort %s in module %s has no credit to send request flit :%d\
                        or already sent a request flit in the current cycle\n",
                          owner_module->name(), owner_module->name(), dat_credit);
                    return false; // 没有信用，发送失败
                }
                channel_blocked_by_credit[ch_idx] = false;
                dat_credit--;
                DPRINTF(CHIPort,"%s:DAT channel send, remain Credit:%d, dat_last_send_time:%d, curcycle:%d\n",this->name(),dat_credit,dat_last_send_time,curCycle());
                dat_last_send_time = curCycle();
                break;
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
                if (rsp_credit == 0 || curCycle() <= rsp_last_send_time) {
                    if (rsp_credit == 0) {
                        channel_blocked_by_credit[ch_idx] = true;
                    }
                    DPRINTF(CHIPort,"CHIPort %s in module %s has no credit to send request flit :%d\
                        or already sent a request flit in the current cycle\n",
                          owner_module->name(), owner_module->name(), rsp_credit);
                    return false; // 没有信用，发送失败
                }
                channel_blocked_by_credit[ch_idx] = false;
                rsp_credit--;
                DPRINTF(CHIPort,"%s:RSP channel send, remain Credit:%d, rsp_last_send_time:%d, curcycle:%d\n",this->name(),rsp_credit,rsp_last_send_time,curCycle());
                rsp_last_send_time = curCycle();
                break;
            default:
                panic("Invalid channel type for sending flit");
        }
        DPRINTF(CHIPort,"send Flit op:%s, addr: %lx, size:%d,txn_id:%d\n",CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(data->getOpcode()),data->getAddr(),data->getSize(),data->getTxnId());
        // 将数据发送到连接的端口
        if (connected_port != nullptr) {
            connected_port->receive(std::move(data));
            return true;
        } else {
            panic("CHIPort %s in module %s is not connected to any peer port",
                  owner_module->name(), owner_module->name());
            return false; // 连接失败
        }
    }
    void CHIPort::receive(FlitPtr data){
        //todo ::schedule event, call OnEventCallback
        DPRINTF(CHIPort,"Recv Flit op:%s, addr: %lx, size:%d,txn_id:%d\n",CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(data->getOpcode()),data->getAddr(),data->getSize(),data->getTxnId());
        Flit::CHI_CHN_TYPE channel_type = data->get_Flit_Channel_Type();
        switch (channel_type) {
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:{
                assert(req_buffer.size() < BUFFER_SIZE && "Request buffer overflow");
                req_buffer.push(std::move(data));
                if (!global_handle_event.scheduled()) {
                    schedule(global_handle_event, curTick()+clockPeriod()*PortTransferLatency);
                }
                break;
            }
            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:{
                assert(snp_buffer.size() < BUFFER_SIZE && "Snoop buffer overflow");
                snp_buffer.push(std::move(data));
                if (!global_handle_event.scheduled()) {
                    schedule(global_handle_event, curTick()+clockPeriod()*PortTransferLatency);
                }
                break;
            }

            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:{
                assert(dat_buffer.size() < BUFFER_SIZE && "Data buffer overflow");
                dat_buffer.push(std::move(data));
                if (!global_handle_event.scheduled()) {
                    schedule(global_handle_event, curTick()+clockPeriod()*PortTransferLatency);
                }
                break;
            }

            case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:{
                assert(rsp_buffer.size() < BUFFER_SIZE && "Response buffer overflow");
                rsp_buffer.push(std::move(data));
                if (!global_handle_event.scheduled()) {
                    schedule(global_handle_event, curTick()+clockPeriod()*PortTransferLatency);
                }
                break;
            }

            default:
                panic("Invalid channel type for receiving flit");
        }
    }
    void CHIPort::OnHandleEventCallback() {
        // This function is called every cycle to handle events
        // It can be used to process all buffers in a round-robin manner
        // SNP>RSP>DAT>REQ
        if (!snp_buffer.empty()) {
            OnHandleEventCallback_SNP();
        }
        if (!rsp_buffer.empty()) {
            OnHandleEventCallback_RSP();
        }
        if (!dat_buffer.empty()) {
            OnHandleEventCallback_DAT();
        }
        if (!req_buffer.empty()) {
            OnHandleEventCallback_REQ();
        }
        // after handling the head entry, check if the buffer is empty,if not, reschedule the event to next cycle
        if (!snp_buffer.empty()|| !req_buffer.empty()|| !dat_buffer.empty() || !rsp_buffer.empty()) {
            if (!global_handle_event.scheduled()) {
                schedule(global_handle_event, curTick()+clockPeriod());
            }
        }
    }
    void CHIPort::OnHandleEventCallback_REQ() {
        // Handle the event callback logic here
        // call receive_callback to handle the head entry of the buffer
        // if hanle successful, pop the head entry, and return the credit to sender
        assert(!req_buffer.empty() && "Request buffer is empty");
        FlitPtr& head_entry = req_buffer.front();
        if (receive_callback) {
            if (receive_callback(head_entry)) {
                // Handle successful reception
                req_buffer.pop();
                // Return credit to sender
                getPeer().GrantCredit_REQ();
                DPRINTF(CHIPort,"%s:REQ Recv buffer handle success, buffer occupied num:%d, Peer Credit:%d\n",this->name(),req_buffer.size(),getPeer().req_credit);
            } else {
                // Handle failure to process the entry
                //todo, you may want to log or handle this case
                DPRINTF(CHIPort,"%s:REQ Recv buffer handle Failed, buffer occupied num:%d, Peer Credit:%d\n",this->name(),req_buffer.size(),getPeer().req_credit);
            }
        } else {
            panic("No receive callback set for CHIPort %s", owner_module->name());
        }
        // after handling the head entry, check if the buffer is empty,if not, reschedule the event to next cycle
        // if (!req_buffer.empty()) {
        //     if (!req_handle_event.scheduled()) {
        //         schedule(req_handle_event, curTick()+clockPeriod());
        //     }
        // }
    }

    void CHIPort::OnHandleEventCallback_SNP() {
        // Handle the event callback logic here
        // call receive_callback to handle the head entry of the buffer
        // if hanle successful, pop the head entry, and return the credit to sender
        assert(!snp_buffer.empty() && "snoop buffer is empty");
        FlitPtr& head_entry = snp_buffer.front();
        if (receive_callback) {
            if (receive_callback(head_entry)) {
                // Handle successful reception
                snp_buffer.pop();
                // Return credit to sender
                getPeer().GrantCredit_SNP();
                DPRINTF(CHIPort,"%s:SNP Recv buffer handle success, buffer occupied num:%d, Peer Credit:%d\n",this->name(),snp_buffer.size(),getPeer().snp_credit);
            } else {
                // Handle failure to process the entry
                //todo, you may want to log or handle this case
                DPRINTF(CHIPort,"%s:SNP Recv buffer handle failed, buffer occupied num:%d, Peer Credit:%d\n",this->name(),snp_buffer.size(),getPeer().snp_credit);
            }
        } else {
            panic("No receive callback set for CHIPort %s", owner_module->name());
        }
        // // after handling the head entry, check if the buffer is empty,if not, reschedule the event to next cycle
        // if (!snp_buffer.empty()) {
        //     if (!snp_handle_event.scheduled()) {
        //         schedule(snp_handle_event, curTick()+clockPeriod());
        //     }
        // }
    }

    void CHIPort::OnHandleEventCallback_DAT() {
        // Handle the event callback logic here
        // call receive_callback to handle the head entry of the buffer
        // if hanle successful, pop the head entry, and return the credit to sender
        assert(!dat_buffer.empty() && "data buffer is empty");
        FlitPtr& head_entry = dat_buffer.front();
        if (receive_callback) {
            if (receive_callback(head_entry)) {
                // Handle successful reception
                dat_buffer.pop();
                // Return credit to sender
                getPeer().GrantCredit_DAT();
                DPRINTF(CHIPort,"%s:DAT Recv buffer handle success, buffer occupied num:%d, Peer Credit:%d\n",this->name(),dat_buffer.size(),getPeer().dat_credit);
            } else {
                // Handle failure to process the entry
                //todo, you may want to log or handle this case
                DPRINTF(CHIPort,"%s:DAT Recv buffer handle failed, buffer occupied num:%d, Peer Credit:%d\n",this->name(),dat_buffer.size(),getPeer().dat_credit);
            }
        } else {
            panic("No receive callback set for CHIPort %s", owner_module->name());
        }
        // // after handling the head entry, check if the buffer is empty,if not, reschedule the event to next cycle
        // if (!dat_buffer.empty()) {
        //     if (!dat_handle_event.scheduled()) {
        //         schedule(dat_handle_event, curTick()+clockPeriod());
        //     }
        // }
    }

    void CHIPort::OnHandleEventCallback_RSP() {
        // Handle the event callback logic here
        // call receive_callback to handle the head entry of the buffer
        // if hanle successful, pop the head entry, and return the credit to sender
        assert(!rsp_buffer.empty() && "response buffer is empty");
        FlitPtr& head_entry = rsp_buffer.front();
        if (receive_callback) {
            if (receive_callback(head_entry)) {
                // Handle successful reception
                rsp_buffer.pop();
                // Return credit to sender
                getPeer().GrantCredit_RSP();
                DPRINTF(CHIPort,"%s:RSP Recv buffer handle success, buffer occupied num:%d, Peer Credit:%d\n",this->name(),rsp_buffer.size(),getPeer().rsp_credit);
            } else {
                // Handle failure to process the entry
                //todo, you may want to log or handle this case
                DPRINTF(CHIPort,"%s:RSP Recv buffer handle failed, buffer occupied num:%d, Peer Credit:%d\n",this->name(),rsp_buffer.size(),getPeer().rsp_credit);
            }
        } else {
            panic("No receive callback set for CHIPort %s", owner_module->name());
        }
        // // after handling the head entry, check if the buffer is empty,if not, reschedule the event to next cycle
        // if (!rsp_buffer.empty()) {
        //     if (!rsp_handle_event.scheduled()) {
        //         schedule(rsp_handle_event, curTick()+clockPeriod());
        //     }
        // }
    }
    void CHIPort::GrantCredit_REQ() {
        const bool was_blocked = channel_blocked_by_credit[
            channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)];
        req_credit++;
        // If after granting credit, the channel's credit is 1,
        // it means the port cannot use the credit immediately,
        // so we update the last send time to avoid protential use of it.
        if (req_credit==1) {
            req_last_send_time = curCycle();
        }
        if (was_blocked && req_credit > 0) {
            channel_blocked_by_credit[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ)] = false;
            if (credit_unblock_callback) {
                credit_unblock_callback(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ);
            }
        }
    }
    void CHIPort::GrantCredit_SNP() {
        const bool was_blocked = channel_blocked_by_credit[
            channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP)];
        snp_credit++;
        if (snp_credit==1) {
            snp_last_send_time = curCycle();
        }
        if (was_blocked && snp_credit > 0) {
            channel_blocked_by_credit[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP)] = false;
            if (credit_unblock_callback) {
                credit_unblock_callback(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP);
            }
        }
    }
    void CHIPort::GrantCredit_DAT() {
        const bool was_blocked = channel_blocked_by_credit[
            channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA)];
        dat_credit++;
        if (dat_credit==1) {
            dat_last_send_time = curCycle();
        }
        if (was_blocked && dat_credit > 0) {
            channel_blocked_by_credit[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA)] = false;
            if (credit_unblock_callback) {
                credit_unblock_callback(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
            }
        }
    }
    void CHIPort::GrantCredit_RSP() {
        const bool was_blocked = channel_blocked_by_credit[
            channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP)];
        rsp_credit++;
        if (rsp_credit==1) {
            rsp_last_send_time = curCycle();
        }
        if (was_blocked && rsp_credit > 0) {
            channel_blocked_by_credit[
                channelIndex(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP)] = false;
            if (credit_unblock_callback) {
                credit_unblock_callback(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
            }
        }
    }

    void CHIPort::setBlocked()
    {
        blocked = true;
    }

    void CHIPort::setUnblocked()
    {
        blocked = false;
    }

    bool CHIPort::isChannelBlockedByCredit(Flit::CHI_CHN_TYPE channel) const
    {
        const size_t idx = static_cast<size_t>(channel);
        panic_if(idx >= channel_blocked_by_credit.size(),
                 "Invalid CHI channel index %zu", idx);
        return channel_blocked_by_credit[idx];
    }
    void CHIPort::initState()
    {
        // Initialize the port state if needed
    }
    void CHIPort::init()
    {
        // Initialize the port, if needed
        // This can be used to set up initial conditions or configurations
        // DPRINTF(CHIPort, "CHIPort %s initialized with ID %d\n", portName, id);
    }
    void
    CHIPort::connect(CHIPort* peer_port)
    {
        if (!_connected) {
            connected_port = peer_port;
            _connected = true;
            req_credit = peer_port->BUFFER_SIZE;
            snp_credit = peer_port->BUFFER_SIZE;
            dat_credit = peer_port->BUFFER_SIZE;
            rsp_credit = peer_port->BUFFER_SIZE;
            DPRINTF(CHIPort,"Port Connect,%s : credit: %d\n",this->name(),req_credit);
        }else {
            assert(connected_port == peer_port);
        }
        if (!peer_port->_connected) {
            peer_port->connected_port = this;
            peer_port->_connected = true;
            peer_port->req_credit = this->BUFFER_SIZE;
            peer_port->snp_credit = this->BUFFER_SIZE;
            peer_port->dat_credit = this->BUFFER_SIZE;
            peer_port->rsp_credit = this->BUFFER_SIZE;
            DPRINTF(CHIPort,"Port Connect,%s : credit: %d\n",peer_port->name(),peer_port->req_credit);
        }else{
            assert(peer_port->connected_port == this);
        }

    }
    // Port &
    // CHIPort::getPort(const std::string &if_name,
    //               PortID idx=InvalidPortID) {
    //                 return Port();
    //               }
}}
