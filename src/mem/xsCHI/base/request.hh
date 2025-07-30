#pragma once
#include <bitset>
#include <cstdint>
#include <memory>
#include <vector>

#include "mem/packet.hh"
#include "mem/xsCHI/base/FlitOpType.hh"

// #include "mem/request.hh"
#include "mem/xsCHI/base/params.hh"

namespace gem5 {
    namespace xsCHI {
    class Flit;
    using FlitPtr = std::unique_ptr<Flit>;
    class Request;
    using ReqPtr = std::shared_ptr<Request>;
} } // namespace gem5::xsCHI

namespace gem5
{
namespace xsCHI
{

class Request : public std::enable_shared_from_this<Request>
{
protected:
    // // 事务状态跟踪
    // enum class TransactionState
    // {
    //     Pending, Snooping, Responded, Completed };

    // 合并后的成员变量
    uint32_t qos_priority;            // QoS: 服务质量优先级 (0~15, 越大优先级越高)
    uint32_t target_id;               // TgtID: 目标节点ID
    uint32_t source_id;               // SrcID: 请求源节点ID
    uint64_t transaction_id;          // TxnID: 唯一事务标识符
    uint32_t logicalProcessor_id;     // LPID: 逻辑处理器ID
    uint32_t pgroup_id;               // PGroupID: 持久性组ID
    bool deep;                        // Deep: 深持久性
    uint32_t return_nid;              // ReturnNID: 返回节点ID
    uint64_t return_txnid;            // ReturnTxnID: 返回事务ID
    uint32_t stash_nid;               // StashNID: Stash目标节点ID
    bool stash_nid_valid;             // StashNIDValid: Stash节点ID有效
    uint32_t stash_lpid;              // StashLPID: Stash目标逻辑处理器ID
    bool stash_lpid_valid;            // StashLPIDValid: Stash逻辑处理器ID有效
    uint32_t stash_group_id;          // StashGroupID: Stash组ID
    CHI_OP_TYPE opcode;               // Opcode: 请求/数据/响应/Snoop操作码
    uint64_t addr;                    // Addr: 访问地址
    bool ns;                          // NS: 非安全
    uint32_t size;                    // Size: 数据大小
    bool allow_retry;                 // AllowRetry: 允许重试
    uint8_t pcrd_type;                // PCrdType: 协议信用类型
    bool exp_comp_ack;                // ExpCompAck: 期望完成确认
    uint8_t mem_attr;                 // MemAttr: 内存属性
    uint8_t snp_attr;                 // SnpAttr: Snoop属性
    bool do_dwt;                      // DoDWT: 直接写传输
    bool snoop_me;                    // SnoopMe: Snoop Me
    bool likely_shared;               // LikelyShared: 可能共享
    uint8_t slc_rep_hint;             // SLCRepHint: SLC替换提示
    bool excl;                        // Excl: 独占访问
    bool order;                       // Order: 顺序要求
    bool endian;                      // Endian: 大小端
    uint8_t tag_op;                   // TagOp: 标签操作
    uint32_t tag_group_id;            // TagGroupID: 标签组ID
    uint64_t trace_tag;               // TraceTag: 跟踪标签
    uint32_t mpam;                    // MPAM: 内存系统性能资源分区与监控

    // SnoopRequestFields
    uint32_t fwd_nid;                 // FwdNID: 原始请求者节点ID
    uint64_t fwd_txnid;               // FwdTxnID: 原始请求者事务ID
    uint16_t vmid_ext;                // VMIDExt: 虚拟机ID扩展
    bool do_not_go_to_sd;             // DoNotGoToSD: 不进入SD状态
    bool ret_to_src;                  // RetToSrc: 返回源

    // DataFileds
    uint32_t home_nid;                // HomeNID: CompAck响应目标节点ID
    uint8_t cbusy;                    // CBusy: Completer Busy
    uint64_t dbid;                    // DBID: 数据缓冲区ID
    uint8_t resp_err;                 // RespErr: 响应错误状态
    uint8_t resp;                     // Resp: 响应状态
    uint8_t fwd_state;                // FwdState: 转发状态
    bool data_pull;                   // DataPull: 数据拉取
    uint8_t data_source;              // DataSource: 数据来源
    uint16_t ccid;                    // CCID: 关键块标识符
    uint16_t data_id;                 // DataID: 数据标识符
    uint32_t be;                      // BE: 字节使能
    uint8_t* data;        // Data: 数据载荷
    uint32_t data_check;              // DataCheck: 数据校验
    bool poison;                      // Poison: 数据损坏标记
    std::vector<uint8_t> tag;         // Tag: 内存标签
    uint32_t tu;                      // TU: 标签更新
    uint32_t rsvdc;                   // RSVDC: 用户自定义

    // state tracking for dataTransfer

    std::bitset<DATAFLITS_PER_TRANSACTION> dataFlitsTransferred; // 已接收的数据Flit位图



    bool IsRecvSepData; // if this transaction is receiving separated data , true if we received a CHI_RSP_RESPSEPDATA.

    // end of state tracking for dataTransfer
public:
    bool dataTransferFinished() {
        return dataFlitsTransferred.all();
    } // 数据传输是否完成
    bool dataTransferStarted() {
        return dataFlitsTransferred.any();
    }
    void setRecvSepData() { IsRecvSepData = true; }
    bool isRecvSepData() { return IsRecvSepData; } // 是否接收了分离数据

    void gatherDataFlit(FlitPtr &flit);
    uint32_t generateWriteDataID();
    void finishTransferdata(int data_id);
    // getter/setter
    uint32_t getQosPriority()  { return qos_priority; }
    void setQosPriority(uint32_t v) { qos_priority = v; }

    uint32_t getTargetId()  { return target_id; }
    void setTargetId(uint32_t v) { target_id = v; }

    uint32_t getSourceId()  { return source_id; }
    void setSourceId(uint32_t v) { source_id = v; }

    uint64_t getTransactionId()  { return transaction_id; }
    void setTransactionId(uint64_t v) { transaction_id = v; }

    uint32_t getLogicalProcessorId()  { return logicalProcessor_id; }
    void setLogicalProcessorId(uint32_t v) { logicalProcessor_id = v; }

    uint32_t getPgroupId()  { return pgroup_id; }
    void setPgroupId(uint32_t v) { pgroup_id = v; }

    bool getDeep()  { return deep; }
    void setDeep(bool v) { deep = v; }

    uint32_t getReturnNid()  { return return_nid; }
    void setReturnNid(uint32_t v) { return_nid = v; }

    uint64_t getReturnTxnid()  { return return_txnid; }
    void setReturnTxnid(uint64_t v) { return_txnid = v; }

    uint32_t getStashNid()  { return stash_nid; }
    void setStashNid(uint32_t v) { stash_nid = v; }

    bool getStashNidValid()  { return stash_nid_valid; }
    void setStashNidValid(bool v) { stash_nid_valid = v; }

    uint32_t getStashLpid()  { return stash_lpid; }
    void setStashLpid(uint32_t v) { stash_lpid = v; }

    bool getStashLpidValid()  { return stash_lpid_valid; }
    void setStashLpidValid(bool v) { stash_lpid_valid = v; }

    uint32_t getStashGroupId()  { return stash_group_id; }
    void setStashGroupId(uint32_t v) { stash_group_id = v; }

    CHI_OP_TYPE getOpcode()  { return opcode; }
    void setOpcode(CHI_OP_TYPE v) { opcode = v; }

    uint64_t getAddr()  { return addr; }
    void setAddr(uint64_t v) { addr = v; }

    bool getNs()  { return ns; }
    void setNs(bool v) { ns = v; }

    uint32_t getSize()  { return size; }
    void setSize(uint32_t v) { size = v; }

    bool getAllowRetry()  { return allow_retry; }
    void setAllowRetry(bool v) { allow_retry = v; }

    uint8_t getPcrdType()  { return pcrd_type; }
    void setPcrdType(uint8_t v) { pcrd_type = v; }

    bool getExpCompAck()  { return exp_comp_ack; }
    void setExpCompAck(bool v) { exp_comp_ack = v; }

    uint8_t getMemAttr()  { return mem_attr; }
    void setMemAttr(uint8_t v) { mem_attr = v; }

    uint8_t getSnpAttr()  { return snp_attr; }
    void setSnpAttr(uint8_t v) { snp_attr = v; }

    bool getDoDwt()  { return do_dwt; }
    void setDoDwt(bool v) { do_dwt = v; }

    bool getSnoopMe()  { return snoop_me; }
    void setSnoopMe(bool v) { snoop_me = v; }

    bool getLikelyShared()  { return likely_shared; }
    void setLikelyShared(bool v) { likely_shared = v; }

    uint8_t getSlcRepHint()  { return slc_rep_hint; }
    void setSlcRepHint(uint8_t v) { slc_rep_hint = v; }

    bool getExcl()  { return excl; }
    void setExcl(bool v) { excl = v; }

    bool getOrder()  { return order; }
    void setOrder(bool v) { order = v; }

    bool getEndian()  { return endian; }
    void setEndian(bool v) { endian = v; }

    uint8_t getTagOp()  { return tag_op; }
    void setTagOp(uint8_t v) { tag_op = v; }

    uint32_t getTagGroupId()  { return tag_group_id; }
    void setTagGroupId(uint32_t v) { tag_group_id = v; }

    uint64_t getTraceTag()  { return trace_tag; }
    void setTraceTag(uint64_t v) { trace_tag = v; }

    uint32_t getMpam()  { return mpam; }
    void setMpam(uint32_t v) { mpam = v; }

    uint32_t getFwdNid()  { return fwd_nid; }
    void setFwdNid(uint32_t v) { fwd_nid = v; }

    uint64_t getFwdTxnid()  { return fwd_txnid; }
    void setFwdTxnid(uint64_t v) { fwd_txnid = v; }

    uint16_t getVmidExt()  { return vmid_ext; }
    void setVmidExt(uint16_t v) { vmid_ext = v; }

    bool getDoNotGoToSd()  { return do_not_go_to_sd; }
    void setDoNotGoToSd(bool v) { do_not_go_to_sd = v; }

    bool getRetToSrc()  { return ret_to_src; }
    void setRetToSrc(bool v) { ret_to_src = v; }

    uint32_t getHomeNid()  { return home_nid; }
    void setHomeNid(uint32_t v) { home_nid = v; }

    uint8_t getCbusy()  { return cbusy; }
    void setCbusy(uint8_t v) { cbusy = v; }

    uint64_t getDbid()  { return dbid; }
    void setDbid(uint64_t v) { dbid = v; }

    uint8_t getRespErr()  { return resp_err; }
    void setRespErr(uint8_t v) { resp_err = v; }

    uint8_t getResp()  { return resp; }
    void setResp(uint8_t v) { resp = v; }

    uint8_t getFwdState()  { return fwd_state; }
    void setFwdState(uint8_t v) { fwd_state = v; }

    bool getDataPull()  { return data_pull; }
    void setDataPull(bool v) { data_pull = v; }

    uint8_t getDataSource()  { return data_source; }
    void setDataSource(uint8_t v) { data_source = v; }

    uint16_t getCcid()  { return ccid; }
    void setCcid(uint16_t v) { ccid = v; }

    uint16_t getDataId()  { return data_id; }
    void setDataId(uint16_t v) { data_id = v; }

    uint32_t getBe()  { return be; }
    void setBe(uint32_t v) { be = v; }

    bool DataValid()
    {
        return (data != nullptr);
    }
    void
    getData(uint8_t* p)
    {
        // assert(isData() && "Flit must be a data flit to set data");
        assert(getSize() > 0 && "Flit data size must be greater than 0");
        assert(p != nullptr && "Destination pointer for data must not be null");
        assert(data != nullptr && "Flit data must not be null");
        std::memcpy(p, data, getSize());
    }

    void
    setData(const uint8_t *p)
    {
        // assert(isData() && "Flit must be a data flit to set data");
        assert(p != nullptr && "Source pointer for data must not be null");
        assert(getSize() > 0 && "Flit data size must be greater than 0");
        if (!DataValid()){
            // allocate memory for data
            data = new uint8_t[getSize()];
        }
        // we should never be copying data onto itself, which means we
        // must idenfity packets with static data, as they carry the
        // same pointer from source to destination and back
        assert(p != data);

        if (p != data) {
            // for packet with allocated dynamic data, we copy data from
            // one to the other, e.g. a forwarded response to a response
            std::memcpy(data, p, getSize());
        }
    }
    void
    setData(PacketPtr pkt)
    {
        // assert(isData() && "Flit must be a data flit to set data");
        assert(getSize() > 0 && getSize()==pkt->getSize() && "Flit data size must be greater than 0");
        if (!DataValid()){
            // allocate memory for data
            data = new uint8_t[getSize()];
        }
        pkt->getData(data);
    }

    void
    setData(ReqPtr Req)
    {
        // assert(isData() && "Flit must be a data flit to set data");
        assert(getSize() > 0 && getSize()==Req->getSize() && "Flit data size must be greater than 0");
        if (!DataValid()){
            // allocate memory for data
            data = new uint8_t[getSize()];
        }
        Req->getData(data);
    }

    //use for deconstruct
    void deleteData()
    {
        if (data != nullptr) {
            delete [] data;
            data = nullptr;
        }
    }

    uint32_t getDataCheck()  { return data_check; }
    void setDataCheck(uint32_t v) { data_check = v; }

    bool getPoison()  { return poison; }
    void setPoison(bool v) { poison = v; }

     std::vector<uint8_t>& getTag()  { return tag; }
    void setTag( std::vector<uint8_t>& v) { tag = v; }

    uint32_t getTu()  { return tu; }
    void setTu(uint32_t v) { tu = v; }

    uint32_t getRsvdc()  { return rsvdc; }
    void setRsvdc(uint32_t v) { rsvdc = v; }

    // 构造函数
    Request() = default;
    Request(const Request& other);
    Request(CHI_OP_TYPE op, uint64_t addr, uint32_t size);
    ~Request(){
        deleteData(); // 确保在析构时释放数据内存
    }

    ReqPtr createReadResponse();

    bool isResponse()
    {
        return (opcode >= CHI_OP_TYPE::CHI_RSP_OP_START &&
                opcode <= CHI_OP_TYPE::CHI_RSP_OP_END);
    }
    bool isRequest()
    {
        return (opcode >= CHI_OP_TYPE::CHI_REQ_OP_START &&
                opcode <= CHI_OP_TYPE::CHI_REQ_OP_END);
    }
    bool isSnoop()
    {
        return (opcode >= CHI_OP_TYPE::CHI_SNP_OP_START &&
                opcode <= CHI_OP_TYPE::CHI_SNP_OP_END);
    }
    bool isData()
    {
        return (opcode >= CHI_OP_TYPE::CHI_DAT_OP_START &&
                opcode <= CHI_OP_TYPE::CHI_DAT_OP_END);
    }
    //     A Snoop transaction includes a Snoop response.
    // A Snoop response can be with or without data. The forms of Snoop
    // response are:
    //  Snoop response without data
    // • This Snoop response is used when no data transfer is required.
    //  • It is sent on the SRSP channel and uses the SnpResp opcode.
    //  • It can include a Data Pull request for stash snoops.
    //  • Snoop response without data is always used for the response to a SnpDVMOp transaction.
    //  Snoop response without data to Home and Direct Cache Transfer (DCT)
    // • This Snoop response is used when the Snoopee sends Data to the Requester and a data
    // transfer to the Home is not required.
    //  • It is sent on the SRSP channel and uses the SnpRespFwded opcode.
    //  Snoop response with data
    // • This Snoop response is used when a full cache line of data is transferred to Home.
    //  • It is sent on the WDAT channel and uses the SnpRespData opcode.
    //  • It can include a Data Pull request for stash snoops.
    //  Snoop response with partial data
    // • This Snoop response is used when a partial cache line of data is transferred to the Home.
    //  • It is sent on the WDAT channel and uses the SnpRespDataPtl opcode.
    //  • It can include a Data Pull request for stash snoops.
    //  • It is sent when the combination of the Snoop request and cache line state is:
    //  — Any Snoop request except SnpMakeInvalid, and the cache line state is UDP.
    //  Snoop response with data to Home and DCT
    // • This Snoop response is used when the Snoopee sends Data to the Requester and a data
    // transfer to the Home is also required.
    //  • It is sent on the DAT channel and uses the SnpRespDataFwded opcode.
    //  The Snoop response includes the Resp field, which indicates the following:
    //  Cache state
    // Pass Dirty
    // The final state of the cache line at the snooped node after sending the Snoop response.
    //  Indicates that the responsibility for updating memory is passed to the Requester or ICN.
    //  Pass Dirty must only be asserted for a Snoop response with data. The assertion of the Pass Dirty bit
    // is shown by _PD in the response name
    bool isSnoopResponse()
    {
        return (opcode == CHI_OP_TYPE::CHI_DAT_SNPRESPDATA ||
                opcode == CHI_OP_TYPE::CHI_DAT_SNPRESPDATAPTL ||
                opcode == CHI_OP_TYPE::CHI_DAT_SNPRESPDATAFWDED ||
                opcode == CHI_OP_TYPE::CHI_RSP_SNPRESP ||
                opcode == CHI_OP_TYPE::CHI_RSP_SNPRESPFWDED);
    }
};

} // namespace xsCHI
} // namespace gem5
