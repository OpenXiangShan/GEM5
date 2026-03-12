#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

#include "mem/xsCHI/base/FlitOpType.hh"

#include "vector"
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
/**
 * @file
 * Declares a flit for the xsCHI protocol.
 */

    class Flit
    {
    public:
        Flit() { init(); }
        Flit(CHI_OP_TYPE op,uint64_t addr,uint32_t size);
        Flit(Flit& other);
        ~Flit(){
            deleteData();
        }

        // Initialize all member variables to 0
        void init();

        // getter/setter
        uint32_t getTgtId()  { return tgt_id; }
        void setTgtId(uint32_t v) { tgt_id = v; }

        uint32_t getSrcId()  { return src_id; }
        void setSrcId(uint32_t v) { src_id = v; }

        uint32_t getHomeNid()  { return home_nid; }
        void setHomeNid(uint32_t v) { home_nid = v; }

        uint32_t getReturnNid()  { return return_nid; }
        void setReturnNid(uint32_t v) { return_nid = v; }

        uint32_t getFwdNid()  { return fwd_nid; }
        void setFwdNid(uint32_t v) { fwd_nid = v; }

        uint32_t getLpid()  { return lpid; }
        void setLpid(uint32_t v) { lpid = v; }

        uint32_t getPgroupId()  { return pgroup_id; }
        void setPgroupId(uint32_t v) { pgroup_id = v; }

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

        uint64_t getTxnId()  { return txn_id; }
        void setTxnId(uint64_t v) { txn_id = v; }

        uint64_t getReturnTxnid()  { return return_txnid; }
        void setReturnTxnid(uint64_t v) { return_txnid = v; }

        uint64_t getFwdTxnid()  { return fwd_txnid; }
        void setFwdTxnid(uint64_t v) { fwd_txnid = v; }

        uint64_t getDbid()  { return dbid; }
        void setDbid(uint64_t v) { dbid = v; }

        CHI_OP_TYPE getOpcode()  { return opcode; }
        void setOpcode(CHI_OP_TYPE v) { opcode = v; }

        // bool getDeep()  { return deep; }
        // void setDeep(bool v) { deep = v; }

        uint64_t getAddr()  { return addr; }
        void setAddr(uint64_t v) { addr = v; }

        // bool getNs()  { return ns; }
        // void setNs(bool v) { ns = v; }

        // Size in bytes
        uint32_t getSize()  { return size; }
        void setSize(uint32_t v) { size = v; }

        // uint8_t getMemAttr()  { return mem_attr; }
        // void setMemAttr(uint8_t v) { mem_attr = v; }

        // uint8_t getSnpAttr()  { return snp_attr; }
        // void setSnpAttr(uint8_t v) { snp_attr = v; }

        // bool getDoDwt()  { return do_dwt; }
        // void setDoDwt(bool v) { do_dwt = v; }

        // bool getLikelyShared()  { return likely_shared; }
        // void setLikelyShared(bool v) { likely_shared = v; }

        // bool getOrder()  { return order; }
        // void setOrder(bool v) { order = v; }

        // bool getExcl()  { return excl; }
        // void setExcl(bool v) { excl = v; }

        // bool getEndian()  { return endian; }
        // void setEndian(bool v) { endian = v; }

        // bool getAllowRetry()  { return allow_retry; }
        // void setAllowRetry(bool v) { allow_retry = v; }

        // bool getExpCompAck()  { return exp_comp_ack; }
        // void setExpCompAck(bool v) { exp_comp_ack = v; }

        // bool getSnoopMe()  { return snoop_me; }
        // void setSnoopMe(bool v) { snoop_me = v; }

        // bool getRetToSrc()  { return ret_to_src; }
        // void setRetToSrc(bool v) { ret_to_src = v; }

        // bool getDataPull()  { return data_pull; }
        // void setDataPull(bool v) { data_pull = v; }

        // bool getDoNotGoToSd()  { return do_not_go_to_sd; }
        // void setDoNotGoToSd(bool v) { do_not_go_to_sd = v; }

        // uint32_t getQos()  { return qos; }
        // void setQos(uint32_t v) { qos = v; }

        // uint8_t getPcrdType()  { return pcrd_type; }
        // void setPcrdType(uint8_t v) { pcrd_type = v; }

        // uint8_t getTagOp()  { return tag_op; }
        // void setTagOp(uint8_t v) { tag_op = v; }

        //  std::vector<uint8_t>& getTag()  { return tag; }
        // void setTag( std::vector<uint8_t>& v) { tag = v; }

        // uint32_t getTu()  { return tu; }
        // void setTu(uint32_t v) { tu = v; }

        // uint32_t getTagGroupId()  { return tag_group_id; }
        // void setTagGroupId(uint32_t v) { tag_group_id = v; }

        // uint64_t getTraceTag()  { return trace_tag; }
        // void setTraceTag(uint64_t v) { trace_tag = v; }

        // uint32_t getMpam()  { return mpam; }
        // void setMpam(uint32_t v) { mpam = v; }

        // uint16_t getVmidExt()  { return vmid_ext; }
        // void setVmidExt(uint16_t v) { vmid_ext = v; }

        // uint8_t getResp()  { return resp; }
        // void setResp(uint8_t v) { resp = v; }

        // uint8_t getFwdState()  { return fwd_state; }
        // void setFwdState(uint8_t v) { fwd_state = v; }

        // uint8_t getCbusy()  { return cbusy; }
        // void setCbusy(uint8_t v) { cbusy = v; }

        // uint8_t getRespErr()  { return resp_err; }
        // void setRespErr(uint8_t v) { resp_err = v; }

        bool DataValid()
        {
            return (data != nullptr);
        }
        void
        getData(uint8_t* p)
        {
            assert(opcode >= CHI_OP_TYPE::CHI_DAT_OP_START &&
                   opcode <= CHI_OP_TYPE::CHI_DAT_OP_END &&
                   "Flit must be a data flit to set data");
            assert(getSize() > 0 && "Flit data size must be greater than 0");
            assert(p != nullptr && "Destination pointer for data must not be null");
            assert(data != nullptr && "Flit data must not be null");
            std::memcpy(p, data, getSize());
        }
        void
        setData(ReqPtr req);

        void
        setData(const uint8_t *p)
        {
            assert(opcode >= CHI_OP_TYPE::CHI_DAT_OP_START &&
                   opcode <= CHI_OP_TYPE::CHI_DAT_OP_END &&
                   "Flit must be a data flit to set data");
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

        //use for deconstruct
        void deleteData()
        {
            if (data != nullptr) {
                delete [] data;
                data = nullptr;
            }
        }

        uint16_t getCcid()  { return ccid; }
        void setCcid(uint16_t v) { ccid = v; }

        uint16_t getDataId()  { return data_id; }
        void setDataId(uint16_t v) { data_id = v; }

        bool getCacheResponding() { return cache_responding; }
        void setCacheResponding(bool v) { cache_responding = v; }

        bool getResponderHadWritable() { return responder_had_writable; }
        void setResponderHadWritable(bool v) { responder_had_writable = v; }
        uint16_t getMeshHopCount() { return mesh_hop_count; }
        void setMeshHopCount(uint16_t v) { mesh_hop_count = v; }

        uint64_t getMeshInjectTick() { return mesh_inject_tick; }
        void setMeshInjectTick(uint64_t v) { mesh_inject_tick = v; }

        bool getMeshStatsValid() { return mesh_stats_valid; }
        void setMeshStatsValid(bool v) { mesh_stats_valid = v; }

        // uint32_t getBe()  { return be; }
        // void setBe(uint32_t v) { be = v; }

        // uint32_t getDataCheck()  { return data_check; }
        // void setDataCheck(uint32_t v) { data_check = v; }

        // bool getPoison()  { return poison; }
        // void setPoison(bool v) { poison = v; }

        // uint8_t getDataSource()  { return data_source; }
        // void setDataSource(uint8_t v) { data_source = v; }

        // uint8_t getSlcRepHint()  { return slc_rep_hint; }
        // void setSlcRepHint(uint8_t v) { slc_rep_hint = v; }

        // uint32_t getRsvdc()  { return rsvdc; }
        // void setRsvdc(uint32_t v) { rsvdc = v; }

        // RequestPtr getRequest()  { return request; }
        // void setRequest( RequestPtr& v) { request = v; }

    protected:
        // 协议字段成员变量
        uint32_t tgt_id;            // Target Identifier, TgtID
        uint32_t src_id;            // Source Identifier, SrcID
        uint32_t home_nid;          // Home Node Identifier, HomeNID
        uint32_t return_nid;        // Return Node Identifier, ReturnNID
        uint32_t fwd_nid;           // Forward Node Identifier, FwdNID
        uint32_t lpid;              // Logical Processor Identifier, LPID
        uint32_t pgroup_id;         // Persistence Group Identifier, PGroupID
        uint32_t stash_nid;         // Stash Node Identifier, StashNID
        bool stash_nid_valid;       // Stash Node Identifier Valid, StashNIDValid
        uint32_t stash_lpid;        // Stash Logical Processor Identifier, StashLPID
        bool stash_lpid_valid;      // Stash Logical Processor Identifier Valid, StashLPIDValid
        uint32_t stash_group_id;    // Stash Group Identifier, StashGroupID
        uint64_t txn_id;            // Transaction Identifier, TxnID
        uint64_t return_txnid;      // Return Transaction Identifier, ReturnTxnID
        uint64_t fwd_txnid;         // Forwarding Transaction Identifier, FwdTxnID
        uint64_t dbid;              // Data Buffer Identifier, DBID
        CHI_OP_TYPE opcode;             // Channel opcodes, Opcode
        // bool deep;                  // Deep persistence, Deep
        uint64_t addr;              // Address, Addr
        // bool ns;                    // Non-secure, NS
        uint32_t size;              // Size of transaction data, Size
        // uint8_t mem_attr;           // Memory Attribute, MemAttr
        // uint8_t snp_attr;           // Snoop Attribute, SnpAttr
        // bool do_dwt;                // Do Direct Write Transfer, DoDWT
        // bool likely_shared;         // Likely Shared, LikelyShared
        // bool order;                 // Ordering requirements, Order
        // bool excl;                  // Exclusive, Excl
        // bool endian;                // Endian
        // bool allow_retry;           // Allow Retry, AllowRetry
        // bool exp_comp_ack;          // Expect Completion Acknowledge, ExpCompAck
        // bool snoop_me;              // SnoopMe
        // bool ret_to_src;            // Return to Source, RetToSrc
        // bool data_pull;             // Data Pull, DataPull
        // bool do_not_go_to_sd;       // Do not transition to SD state, DoNotGoToSD
        // uint32_t qos;               // Quality of Service priority level, QoS
        // uint8_t pcrd_type;          // Protocol Credit Type, PCrdType
        // uint8_t tag_op;             // Tag Operation, TagOp
        // std::vector<uint8_t> tag;   // Tag
        // uint32_t tu;                // Tag Update, TU
        // uint32_t tag_group_id;      // Tag Group Identifier, TagGroupID
        // uint64_t trace_tag;         // Trace Tag, TraceTag
        // uint32_t mpam;              // Memory System Performance Resource Partitioning and Monitoring, MPAM
        // uint16_t vmid_ext;          // Virtual Machine Identifier Extension, VMIDExt
        // uint8_t resp;               // Response status, Resp
        // uint8_t fwd_state;          // Forward State, FwdState
        // uint8_t cbusy;              // Completer Busy, CBusy
        // uint8_t resp_err;           // Response Error, RespErr

        uint8_t* data;     // Data payload, Data actually has all 64 bits, but data_id
                            // indicates which part of the data this flit carries.
                            // For example, if data_id is 0,
                            // this flit carries the first DATA_TRANSFER_WIDTH_BYTE bytes of data.
        uint16_t ccid;              // Critical Chunk Identifier, CCID
        uint16_t data_id;           // Data Identifier, DataID
        bool cache_responding;
        bool responder_had_writable;
        uint16_t mesh_hop_count;    // Number of directional mesh hops traversed.
        uint64_t mesh_inject_tick;  // Tick when entering mesh from a local ingress.
        bool mesh_stats_valid;      // Whether mesh hop/latency metadata is valid.

        // uint32_t be;                // Byte Enable, BE
        // uint32_t data_check;        // Data check, DataCheck
        // bool poison;                // Poison
        // uint8_t data_source;        // Data source, DataSource
        // uint8_t slc_rep_hint;       // System Level Caches Replacement Hint, SLCRepHint
        // uint32_t rsvdc;             // Reserved for Customer Use, RSVDC

        // RequestPtr request; // 请求指针，指向与此Flit相关的请求

        public:
        enum class CHI_CHN_TYPE
        {
            CHI_CHN_TYPE_SNP,
            CHI_CHN_TYPE_REQ,
            CHI_CHN_TYPE_RSP,
            CHI_CHN_TYPE_DATA,
            CHI_CHN_TYPE_NUM
        };

        CHI_CHN_TYPE get_Flit_Channel_Type(){
            assert(opcode != CHI_OP_TYPE::CHI_REQ_OP_START &&
                   opcode != CHI_OP_TYPE::CHI_RSP_OP_START &&
                   opcode != CHI_OP_TYPE::CHI_SNP_OP_START &&
                   opcode != CHI_OP_TYPE::CHI_DAT_OP_START);
            assert(opcode != CHI_OP_TYPE::CHI_REQ_OP_END &&
                   opcode != CHI_OP_TYPE::CHI_RSP_OP_END &&
                   opcode != CHI_OP_TYPE::CHI_SNP_OP_END &&
                   opcode != CHI_OP_TYPE::CHI_DAT_OP_END);
            if (opcode >= CHI_OP_TYPE::CHI_SNP_OP_START && opcode <= CHI_OP_TYPE::CHI_SNP_OP_END) {
                return CHI_CHN_TYPE::CHI_CHN_TYPE_SNP;
            } else if (opcode >= CHI_OP_TYPE::CHI_REQ_OP_START && opcode <= CHI_OP_TYPE::CHI_REQ_OP_END) {
                return CHI_CHN_TYPE::CHI_CHN_TYPE_REQ;
            } else if (opcode >= CHI_OP_TYPE::CHI_RSP_OP_START && opcode <= CHI_OP_TYPE::CHI_RSP_OP_END) {
                return CHI_CHN_TYPE::CHI_CHN_TYPE_RSP;
            } else if (opcode >= CHI_OP_TYPE::CHI_DAT_OP_START && opcode <= CHI_OP_TYPE::CHI_DAT_OP_END) {
                return CHI_CHN_TYPE::CHI_CHN_TYPE_DATA;
            }
            assert(false && "Invalid opcode for CHI_CHN_TYPE");

        }


    };

    // class SnpFlit : public Flit
    // {
    // public:

    //     // ...SnpFlit特有成员...
    // };

    // class ReqFlit : public Flit
    // {
    // public:

    //     // ...ReqFlit特有成员...
    // };

    // class RespFlit : public Flit
    // {
    // public:

    //     // ...RespFlit特有成员...
    // };

    // class DataFlit : public Flit
    // {
    // public:

    //     // ...DataFlit特有成员...
    // };
} // namespace xsCHI

} // namespace gem5
