#pragma once
#include <cassert>
#include <memory>

#include "FlitOpType.hh"
#include "request.hh"

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
        virtual ~Flit() = default;

        // getter/setter
        uint32_t getTgtId() const { return tgt_id; }
        void setTgtId(uint32_t v) { tgt_id = v; }

        uint32_t getSrcId() const { return src_id; }
        void setSrcId(uint32_t v) { src_id = v; }

        uint32_t getHomeNid() const { return home_nid; }
        void setHomeNid(uint32_t v) { home_nid = v; }

        uint32_t getReturnNid() const { return return_nid; }
        void setReturnNid(uint32_t v) { return_nid = v; }

        uint32_t getFwdNid() const { return fwd_nid; }
        void setFwdNid(uint32_t v) { fwd_nid = v; }

        uint32_t getLpid() const { return lpid; }
        void setLpid(uint32_t v) { lpid = v; }

        uint32_t getPgroupId() const { return pgroup_id; }
        void setPgroupId(uint32_t v) { pgroup_id = v; }

        uint32_t getStashNid() const { return stash_nid; }
        void setStashNid(uint32_t v) { stash_nid = v; }

        bool getStashNidValid() const { return stash_nid_valid; }
        void setStashNidValid(bool v) { stash_nid_valid = v; }

        uint32_t getStashLpid() const { return stash_lpid; }
        void setStashLpid(uint32_t v) { stash_lpid = v; }

        bool getStashLpidValid() const { return stash_lpid_valid; }
        void setStashLpidValid(bool v) { stash_lpid_valid = v; }

        uint32_t getStashGroupId() const { return stash_group_id; }
        void setStashGroupId(uint32_t v) { stash_group_id = v; }

        uint64_t getTxnId() const { return txn_id; }
        void setTxnId(uint64_t v) { txn_id = v; }

        uint64_t getReturnTxnid() const { return return_txnid; }
        void setReturnTxnid(uint64_t v) { return_txnid = v; }

        uint64_t getFwdTxnid() const { return fwd_txnid; }
        void setFwdTxnid(uint64_t v) { fwd_txnid = v; }

        uint64_t getDbid() const { return dbid; }
        void setDbid(uint64_t v) { dbid = v; }

        CHI_OP_TYPE getOpcode() const { return opcode; }
        void setOpcode(CHI_OP_TYPE v) { opcode = v; }

        bool getDeep() const { return deep; }
        void setDeep(bool v) { deep = v; }

        uint64_t getAddr() const { return addr; }
        void setAddr(uint64_t v) { addr = v; }

        bool getNs() const { return ns; }
        void setNs(bool v) { ns = v; }

        uint32_t getSize() const { return size; }
        void setSize(uint32_t v) { size = v; }

        uint8_t getMemAttr() const { return mem_attr; }
        void setMemAttr(uint8_t v) { mem_attr = v; }

        uint8_t getSnpAttr() const { return snp_attr; }
        void setSnpAttr(uint8_t v) { snp_attr = v; }

        bool getDoDwt() const { return do_dwt; }
        void setDoDwt(bool v) { do_dwt = v; }

        bool getLikelyShared() const { return likely_shared; }
        void setLikelyShared(bool v) { likely_shared = v; }

        bool getOrder() const { return order; }
        void setOrder(bool v) { order = v; }

        bool getExcl() const { return excl; }
        void setExcl(bool v) { excl = v; }

        bool getEndian() const { return endian; }
        void setEndian(bool v) { endian = v; }

        bool getAllowRetry() const { return allow_retry; }
        void setAllowRetry(bool v) { allow_retry = v; }

        bool getExpCompAck() const { return exp_comp_ack; }
        void setExpCompAck(bool v) { exp_comp_ack = v; }

        bool getSnoopMe() const { return snoop_me; }
        void setSnoopMe(bool v) { snoop_me = v; }

        bool getRetToSrc() const { return ret_to_src; }
        void setRetToSrc(bool v) { ret_to_src = v; }

        bool getDataPull() const { return data_pull; }
        void setDataPull(bool v) { data_pull = v; }

        bool getDoNotGoToSd() const { return do_not_go_to_sd; }
        void setDoNotGoToSd(bool v) { do_not_go_to_sd = v; }

        uint32_t getQos() const { return qos; }
        void setQos(uint32_t v) { qos = v; }

        uint8_t getPcrdType() const { return pcrd_type; }
        void setPcrdType(uint8_t v) { pcrd_type = v; }

        uint8_t getTagOp() const { return tag_op; }
        void setTagOp(uint8_t v) { tag_op = v; }

        const std::vector<uint8_t>& getTag() const { return tag; }
        void setTag(const std::vector<uint8_t>& v) { tag = v; }

        uint32_t getTu() const { return tu; }
        void setTu(uint32_t v) { tu = v; }

        uint32_t getTagGroupId() const { return tag_group_id; }
        void setTagGroupId(uint32_t v) { tag_group_id = v; }

        uint64_t getTraceTag() const { return trace_tag; }
        void setTraceTag(uint64_t v) { trace_tag = v; }

        uint32_t getMpam() const { return mpam; }
        void setMpam(uint32_t v) { mpam = v; }

        uint16_t getVmidExt() const { return vmid_ext; }
        void setVmidExt(uint16_t v) { vmid_ext = v; }

        uint8_t getResp() const { return resp; }
        void setResp(uint8_t v) { resp = v; }

        uint8_t getFwdState() const { return fwd_state; }
        void setFwdState(uint8_t v) { fwd_state = v; }

        uint8_t getCbusy() const { return cbusy; }
        void setCbusy(uint8_t v) { cbusy = v; }

        uint8_t getRespErr() const { return resp_err; }
        void setRespErr(uint8_t v) { resp_err = v; }

        const std::vector<uint8_t>& getData() const { return data; }
        void setData(const std::vector<uint8_t>& v) { data = v; }

        uint16_t getCcid() const { return ccid; }
        void setCcid(uint16_t v) { ccid = v; }

        uint16_t getDataId() const { return data_id; }
        void setDataId(uint16_t v) { data_id = v; }

        uint32_t getBe() const { return be; }
        void setBe(uint32_t v) { be = v; }

        uint32_t getDataCheck() const { return data_check; }
        void setDataCheck(uint32_t v) { data_check = v; }

        bool getPoison() const { return poison; }
        void setPoison(bool v) { poison = v; }

        uint8_t getDataSource() const { return data_source; }
        void setDataSource(uint8_t v) { data_source = v; }

        uint8_t getSlcRepHint() const { return slc_rep_hint; }
        void setSlcRepHint(uint8_t v) { slc_rep_hint = v; }

        uint32_t getRsvdc() const { return rsvdc; }
        void setRsvdc(uint32_t v) { rsvdc = v; }

        RequestPtr getRequest() const { return request; }
        void setRequest(const RequestPtr& v) { request = v; }

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
        bool deep;                  // Deep persistence, Deep
        uint64_t addr;              // Address, Addr
        bool ns;                    // Non-secure, NS
        uint32_t size;              // Size of transaction data, Size
        uint8_t mem_attr;           // Memory Attribute, MemAttr
        uint8_t snp_attr;           // Snoop Attribute, SnpAttr
        bool do_dwt;                // Do Direct Write Transfer, DoDWT
        bool likely_shared;         // Likely Shared, LikelyShared
        bool order;                 // Ordering requirements, Order
        bool excl;                  // Exclusive, Excl
        bool endian;                // Endian
        bool allow_retry;           // Allow Retry, AllowRetry
        bool exp_comp_ack;          // Expect Completion Acknowledge, ExpCompAck
        bool snoop_me;              // SnoopMe
        bool ret_to_src;            // Return to Source, RetToSrc
        bool data_pull;             // Data Pull, DataPull
        bool do_not_go_to_sd;       // Do not transition to SD state, DoNotGoToSD
        uint32_t qos;               // Quality of Service priority level, QoS
        uint8_t pcrd_type;          // Protocol Credit Type, PCrdType
        uint8_t tag_op;             // Tag Operation, TagOp
        std::vector<uint8_t> tag;   // Tag
        uint32_t tu;                // Tag Update, TU
        uint32_t tag_group_id;      // Tag Group Identifier, TagGroupID
        uint64_t trace_tag;         // Trace Tag, TraceTag
        uint32_t mpam;              // Memory System Performance Resource Partitioning and Monitoring, MPAM
        uint16_t vmid_ext;          // Virtual Machine Identifier Extension, VMIDExt
        uint8_t resp;               // Response status, Resp
        uint8_t fwd_state;          // Forward State, FwdState
        uint8_t cbusy;              // Completer Busy, CBusy
        uint8_t resp_err;           // Response Error, RespErr
        std::vector<uint8_t> data;  // Data payload, Data
        uint16_t ccid;              // Critical Chunk Identifier, CCID
        uint16_t data_id;           // Data Identifier, DataID
        uint32_t be;                // Byte Enable, BE
        uint32_t data_check;        // Data check, DataCheck
        bool poison;                // Poison
        uint8_t data_source;        // Data source, DataSource
        uint8_t slc_rep_hint;       // System Level Caches Replacement Hint, SLCRepHint
        uint32_t rsvdc;             // Reserved for Customer Use, RSVDC

        RequestPtr request; // 请求指针，指向与此Flit相关的请求

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

    class SnpFlit : public Flit
    {
    public:

        // ...SnpFlit特有成员...
    };

    class ReqFlit : public Flit
    {
    public:

        // ...ReqFlit特有成员...
    };

    class RespFlit : public Flit
    {
    public:

        // ...RespFlit特有成员...
    };

    class DataFlit : public Flit
    {
    public:

        // ...DataFlit特有成员...
    };
    using FlitPtr = std::unique_ptr<Flit>;
} // namespace xsCHI

} // namespace gem5
