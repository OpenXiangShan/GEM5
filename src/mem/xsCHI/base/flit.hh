#pragma once
#include <memory>

#include "transactions.hh"

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
        int optype; // 通用操作类型（可由子类具体化）
        // ...可扩展的通用Flit成员...
    };

    class SnpFlit : public Flit
    {
    public:
        SnpFlit(CHI_SNP_type t) : snp_type(t) { optype = static_cast<int>(t); }
        CHI_SNP_type getType() const { return snp_type; }
        void setType(CHI_SNP_type t) { snp_type = t; optype = static_cast<int>(t); }
    private:
        CHI_SNP_type snp_type;
        // ...SnpFlit特有成员...
    };

    class ReqFlit : public Flit
    {
    public:
        ReqFlit(CHI_REQ_type t) : req_type(t) { optype = static_cast<int>(t); }
        CHI_REQ_type getType() const { return req_type; }
        void setType(CHI_REQ_type t) { req_type = t; optype = static_cast<int>(t); }
    private:
        CHI_REQ_type req_type;
        // ...ReqFlit特有成员...
    };

    class RespFlit : public Flit
    {
    public:
        RespFlit(CHI_RSP_type t) : rsp_type(t) { optype = static_cast<int>(t); }
        CHI_RSP_type getType() const { return rsp_type; }
        void setType(CHI_RSP_type t) { rsp_type = t; optype = static_cast<int>(t); }
    private:
        CHI_RSP_type rsp_type;
        // ...RespFlit特有成员...
    };

    class DataFlit : public Flit
    {
    public:
        DataFlit(CHI_DATA_type t) : data_type(t) { optype = static_cast<int>(t); }
        CHI_DATA_type getType() const { return data_type; }
        void setType(CHI_DATA_type t) { data_type = t; optype = static_cast<int>(t); }
    private:
        CHI_DATA_type data_type;
        // ...DataFlit特有成员...
    };
    using FlitPtr = std::unique_ptr<Flit>;
} // namespace xsCHI

} // namespace gem5
