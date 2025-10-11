#include "mem/cache/tags/dbi_set_assoc.hh"
#include "debug/DBIAssoc.hh"
namespace gem5
{

DBISetAssoc::DBISetAssoc(const DBISetAssocParams &p)
    : BaseSetAssoc(p),
      rowMask(p.row_mask),
      colMask(~p.row_mask),
      lblkSize(log2i(blkSize)),
      granularity(p.granularity),
      meta(p.dbi_assoc, p.dbi_entries, p.dbi_indexing_policy,
          p.dbi_replacement_policy, DBIEntry(p.granularity))
{
}

void
DBISetAssoc::invalidate(CacheBlk *blk)
{
    if (blk) {
        Addr addr = regenerateBlkAddr(blk);
        Addr row_addr = rowAddress(addr);
        Addr col_addr = colAddress(addr);
        DBIEntry* dbi_entry = meta.findEntry(row_addr, blk->isSecure());
        if (dbi_entry) {
            dbi_entry->eraseEntry(col_addr);
            DPRINTF(DBIAssoc, "%s -> Invalidate DBI entry: paddr=0x%lx, row_addr=0x%lx, col_addr=0x%lx\n",
                     __func__, addr, row_addr, col_addr);
        }
    }
    
    BaseSetAssoc::invalidate(blk);
}

CacheBlk* 
DBISetAssoc::accessBlock(const PacketPtr pkt, Cycles &lat)
{
    CacheBlk* blk = BaseSetAssoc::accessBlock(pkt, lat);
    
    if (pkt->cmd == MemCmd::WritebackDirty || pkt->cmd == MemCmd::WriteClean) {
        if (blk) {
            Addr addr = pkt->getBlockAddr(blkSize);
            Addr row_addr = rowAddress(addr);
            Addr col_addr = colAddress(addr);
            DBIEntry* dbi_entry = meta.findEntry(row_addr, blk->isSecure());
            if (dbi_entry) {
                meta.accessEntry(dbi_entry);
                if (!dbi_entry->containEntry(col_addr)) {
                    dbi_entry->insertEntry(col_addr, blk);
                    DPRINTF(DBIAssoc, "%s -> Insert DBI entry(exist row): paddr=0x%lx, row_addr=0x%lx, col_addr=0x%lx\n",
                             __func__, addr, row_addr, col_addr);
                }
            }
        }
    }

    return blk;
}

void
DBISetAssoc::insertBlock(const PacketPtr pkt, CacheBlk *blk)
{
    BaseSetAssoc::insertBlock(pkt, blk);

    if (blk && (pkt->cmd == MemCmd::WritebackDirty || pkt->cmd == MemCmd::WriteClean)) {
        Addr addr = pkt->getBlockAddr(blkSize);
        Addr row_addr = rowAddress(addr);
        Addr col_addr = colAddress(addr);
        DBIEntry* dbi_entry = meta.findEntry(row_addr, blk->isSecure());
        if (dbi_entry) {
            meta.accessEntry(dbi_entry);
        }
        bool need_insert = false;
        if (!dbi_entry) {
            dbi_entry = meta.findVictim(row_addr);
            DPRINTF(DBIAssoc, "%s -> Replace DBI entry: row_addr=0x%lx\n", __func__, dbi_entry->row);
            dbi_entry->invalidateAll();
            dbi_entry->row = row_addr;
            need_insert = true;
        }
        if (!dbi_entry->containEntry(col_addr)) {
            dbi_entry->insertEntry(col_addr, blk);
            DPRINTF(DBIAssoc, "%s -> Insert DBI entry(%s): paddr=0x%lx, row_addr=0x%lx, col_addr=0x%lx\n",
                        __func__, (need_insert ? "new row" : "exist row"), addr, row_addr, col_addr);
        }
        dbi_entry->touchEntry(col_addr);
        if (need_insert) {
            meta.insertEntry(row_addr, blk->isSecure(), dbi_entry, true);
        }
    }
}


CacheBlk*
DBISetAssoc::findVictim(PacketPtr pkt, const bool is_secure,
                        const std::size_t size,
                        std::vector<CacheBlk*>& evict_blks,
                        std::vector<CacheBlk*>& clean_blks,
                        const std::size_t& num_wb_entries)
{
    CacheBlk* blk = BaseSetAssoc::findVictim(pkt->getAddr(), is_secure, size, evict_blks);

    if (blk && blk->isSet(CacheBlk::DirtyBit)) {
        Addr addr = regenerateBlkAddr(blk);
        Addr row_addr = rowAddress(addr);
        Addr col_addr = colAddress(addr);
        DPRINTF(DBIAssoc, "%s -> Victim: paddr=0x%lx, row_addr=0x%lx, col_addr=0x%lx\n",
                 __func__, addr, row_addr, col_addr);
        DBIEntry* dbi_entry = meta.findEntry(row_addr, blk->isSecure());
        if (dbi_entry) {
            if (num_wb_entries < dbi_entry->dirty_blks.size()) {
                DPRINTF(DBIAssoc, "%s -> Skip DBI entry: row_addr=0x%lx, num_wb_entries=%lu, num_dirty_blks=%lu\n",
                            __func__, dbi_entry->row, num_wb_entries, dbi_entry->dirty_blks.size());
                return blk;
            }
            for (auto dirty_blk : dbi_entry->dirty_blks) {
                if (dirty_blk.blk != blk) {
                    DPRINTF(DBIAssoc, "%s -> Corrlated Clean: paddr=0x%lx, row_addr=0x%lx, col_addr=0x%lx\n",
                                __func__, regenerateBlkAddr(dirty_blk.blk), dbi_entry->row, dirty_blk.col);
                    assert(dirty_blk.blk->isSet(CacheBlk::DirtyBit));
                    clean_blks.push_back(dirty_blk.blk);
                }
            }
            dbi_entry->invalidateAll();
        }
    }

    DPRINTF(DBIAssoc, "%s -> Victim: num_evicted_blks=%lu, num_clean_blks=%lu, num_wb_entries=%lu\n",
             __func__, evict_blks.size(), clean_blks.size(), num_wb_entries);

    return blk;
}

} // namespace gem5
