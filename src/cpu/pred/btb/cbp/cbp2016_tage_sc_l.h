#ifndef _TAGE_PREDICTOR_H_
#define _TAGE_PREDICTOR_H_

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <unordered_map>
#include <vector>
#include <array>
#include <iostream>

namespace gem5 {
namespace branch_prediction {
namespace btb_pred {
namespace cbp_pred {

//parameters of the loop predictor
#define LOGL 5
#define WIDTHNBITERLOOP 10  // we predict only loops with less than 1K iterations
#define LOOPTAG 10      //tag width in the loop predictor


#define UINT64 uint64_t

#define BORNTICK  1024     // U计数器重置周期
//To get the predictor storage budget on stderr  uncomment the next line
#define PRINTSIZE

// #define SC          // 8.2 % if TAGE alone
// #define IMLI            // 0.2 %
// #define LOCALH

#ifdef LOCALH           // 2.7 %
#define LOOPPREDICTOR   //loop predictor enable
#define LOCALS          //enable the 2nd local history
#define LOCALT          //enables the 3rd local history
#endif



//The statistical corrector components

#define PERCWIDTH 6     //Statistical corrector  counter width 5 -> 6 : 0.6 %
//The three BIAS tables in the SC component
//We play with the TAGE  confidence here, with the number of the hitting bank
#define LOGBIAS 8
int8_t Bias[(1 << LOGBIAS)];
int8_t BiasSK[(1 << LOGBIAS)];
int8_t BiasBank[(1 << LOGBIAS)];

//In all th GEHL components, the two tables with the shortest history lengths have only half of the entries.

// IMLI-SIC -> Micro 2015  paper: a big disappointment on  CBP2016 traces
#ifdef IMLI
#define LOGINB 8        // 128-entry
#define INB 1
int Im[INB] = { 8 };
int8_t IGEHLA[INB][(1 << LOGINB)] = { {0} };

int8_t *IGEHL[INB];

#define LOGIMNB 9       // 2 * 256-entry
#define IMNB 2

int IMm[IMNB] = { 10, 4 };
int8_t IMGEHLA[IMNB][(1 << LOGIMNB)] = { {0} };

int8_t *IMGEHL[IMNB];

#endif

//global branch GEHL
#define LOGGNB 10       // 1 1K + 2 * 512-entry tables
#define GNB 3
int Gm[GNB] = { 40, 24, 10 };
int8_t GGEHLA[GNB][(1 << LOGGNB)] = { {0} };

int8_t *GGEHL[GNB];

//variation on global branch history
#define PNB 3
#define LOGPNB 9        // 1 512 + 2 * 256-entry tables
int Pm[PNB] = { 25, 16, 9 };
int8_t PGEHLA[PNB][(1 << LOGPNB)] = { {0} };

int8_t *PGEHL[PNB];

//first local history
#define LOGLNB  10      // 1 1K + 2 * 512-entry tables
#define LNB 3
int Lm[LNB] = { 11, 6, 3 };
int8_t LGEHLA[LNB][(1 << LOGLNB)] = { {0} };

int8_t *LGEHL[LNB];
#define  LOGLOCAL 8
#define NLOCAL (1<<LOGLOCAL)

// second local history
#define LOGSNB 9        // 1 512 + 2 * 256-entry tables
#define SNB 3
int Sm[SNB] = { 16, 11, 6 };
int8_t SGEHLA[SNB][(1 << LOGSNB)] = { {0} };

int8_t *SGEHL[SNB];
#define LOGSECLOCAL 4
#define NSECLOCAL (1<<LOGSECLOCAL)  //Number of second local histories

//third local history
#define LOGTNB 10       // 2 * 512-entry tables
#define TNB 2
int Tm[TNB] = { 9, 4 };
int8_t TGEHLA[TNB][(1 << LOGTNB)] = { {0} };

int8_t *TGEHL[TNB];
#define NTLOCAL 16


// playing with putting more weights (x2)  on some of the SC components
// playing on using different update thresholds on SC
//update threshold for the statistical corrector
#define VARTHRES
#define WIDTHRES 12
#define WIDTHRESP 8
#ifdef VARTHRES
#define LOGSIZEUP 6     //not worth increasing
#else
#define LOGSIZEUP 0
#endif
#define LOGSIZEUPS  (LOGSIZEUP/2)
int updatethreshold;
int Pupdatethreshold[(1 << LOGSIZEUP)]; //size is fixed by LOGSIZEUP
#define INDUPD (PC ^ (PC >>2)) & ((1 << LOGSIZEUP) - 1)
#define INDUPDS ((PC ^ (PC >>2)) & ((1 << (LOGSIZEUPS)) - 1))
int8_t WG[(1 << LOGSIZEUPS)];
int8_t WL[(1 << LOGSIZEUPS)];
int8_t WS[(1 << LOGSIZEUPS)];
int8_t WT[(1 << LOGSIZEUPS)];
int8_t WP[(1 << LOGSIZEUPS)];
int8_t WI[(1 << LOGSIZEUPS)];
int8_t WIM[(1 << LOGSIZEUPS)];
int8_t WB[(1 << LOGSIZEUPS)];
#define EWIDTH 6
int LSUM;

// The two counters used to choose between TAGE and SC on Low Conf SC
int8_t FirstH, SecondH;
bool MedConf;           // is the TAGE prediction medium confidence


#define CONFWIDTH 7     //for the counters in the choser
#define HISTBUFFERLENGTH 4096   // we use a 4K entries history buffer to store the branch history (this allows us to explore using history length up to 4K)

// utility class for index computation
// this is the cyclic shift register for folding 
// a long global history into a smaller number of bits; see P. Michaud's PPM-like predictor at CBP-1

class bentry            // TAGE bimodal table entry
{
    public:
        int8_t hyst;    // 滞后计数器，用于提供2位预测能力
        int8_t pred;    // 预测计数器


        bentry ()
        {
            pred = 0;

            hyst = 1;
        }

};

class gentry            // TAGE global table entry
{
    public:
        int8_t ctr;     // 预测计数器 (3位: -4~+3)，>=0预测taken，<0预测not taken
        uint tag;       // 标签，用于验证条目是否匹配当前分支
        int8_t u;       // 使用计数器，标记条目是否被使用过，用于替换策略

        gentry ()
        {
            ctr = 0;
            u = 0;
            tag = 0;


        }
};

#define  POWER
//use geometric history length

#define NHIST 36        // twice the number of different histories  // 不同历史长度的两倍数量
// 历史长度分配示例:
// i=1,2: 历史长度6     i=3,4: 历史长度9     i=5,6: 历史长度13
// i=7,8: 历史长度19    i=9,10: 历史长度28   i=11,12: 历史长度40
// i=13,14: 历史长度58  i=15,16: 历史长度83  i=17,18: 历史长度119
// i=19,20: 历史长度171 i=21,22: 历史长度244 i=23,24: 历史长度349
// i=25,26: 历史长度499 i=27,28: 历史长度713 i=29,30: 历史长度1019
// i=31,32: 历史长度1456 i=33,34: 历史长度2081 i=35,36: 历史长度3000
// 每对(i,i+1)使用相同的历史长度，但有不同的索引扰动

#define NBANKLOW 10     // number of banks in the shared bank-interleaved for the low history lengths  // 低历史长度表的bank数量
#define NBANKHIGH 20        // number of banks in the shared bank-interleaved for the  history lengths // 高历史长度表的bank数量

int SizeTable[NHIST + 1];    // 各表的大小数组


#define BORN 13         // below BORN in the table for low history lengths, >= BORN in the table for high history lengths, // 低历史长度表的分界点

// we use 2-way associativity for the medium history lengths
#define BORNINFASSOC 9      //2 -way assoc for those banks 0.4 %     // 2路关联起始点
#define BORNSUPASSOC 23                                                    // 2路关联结束点

/*in practice 2 bits or 3 bits par branch: around 1200 cond. branchs*/

#define MINHIST 6       //not optimized so far                          // 最小历史长度
#define MAXHIST 3000                                                       // 最大历史长度


#define LOGG 10         /* logsize of the  banks in the  tagged TAGE tables */    // TAGE表bank大小的对数， 1024 项
#define TBITS 8         //minimum width of the tags  (low history lengths), +4 for high history lengths  // 标签最小宽度


bool NOSKIP[NHIST + 1];     // to manage the associativity for different history lengths  // 管理不同历史长度的关联性



#define NNN 1           // number of extra entries allocated on a TAGE misprediction (1+NNN)  // TAGE误预测时分配的额外条目数
#define HYSTSHIFT 2     // bimodal hysteresis shared by 4 entries                      // 双模态滞后计数器共享
#define LOGB 13         // log of number of entries in bimodal predictor               // 双模态预测器条目数的对数


#define PHISTWIDTH 27       // width of the path history used in TAGE                      // TAGE中使用的路径历史宽度
#define UWIDTH 1        // u counter width on TAGE (2 bits not worth the effort for a 512 Kbits predictor 0.2 %)  // TAGE中U计数器宽度
#define CWIDTH 3        // predictor counter width on the TAGE tagged tables             // TAGE带标签表中的预测计数器宽度


//the counter(s) to chose between longest match and alternate prediction on TAGE when weak counters
#define LOGSIZEUSEALT 4                                                                // 使用备选预测表大小的对数
bool AltConf;           // Confidence on the alternate prediction                       // 备选预测的置信度
#define ALTWIDTH 5                                                                     // 备选预测宽度
#define SIZEUSEALT  (1<<(LOGSIZEUSEALT))                                              // 使用备选预测表大小
#define INDUSEALT (((((HitBank-1)/8)<<1)+AltConf) % (SIZEUSEALT-1))                    // 使用备选预测索引
int8_t use_alt_on_na[SIZEUSEALT];                                                     // 新分配条目使用备选预测表
//very marginal benefit
int8_t BIM;                                                                           // 双模态预测器计数器

int TICK;           // for the reset of the u counter                                  // U计数器重置计数器
//uint8_t ghist[HISTBUFFERLENGTH];
//int ptghist;
//uint64_t phist;      //path history
//folded_history ch_i[NHIST + 1];   //utility for computing TAGE indices
//folded_history ch_t[2][NHIST + 1];    //utility for computing TAGE tags

class lentry            //loop predictor entry
{
    public:
        uint16_t NbIter;        //10 bits
        uint8_t confid;     // 4bits
        uint16_t CurrentIter;       // 10 bits

        uint16_t TAG;           // 10 bits
        uint8_t age;            // 4 bits
        bool dir;           // 1 bit

        //39 bits per entry    
        lentry ()
        {
            confid = 0;
            CurrentIter = 0;
            NbIter = 0;
            TAG = 0;
            age = 0;
            dir = false;
        }
};

//For the TAGE predictor
bentry *btable;         //bimodal TAGE table
gentry *gtable[NHIST + 1];  // tagged TAGE tables
//lentry *ltable;
int m[NHIST + 1];       // 各bank的历史长度数组：几何分布从6到3000
int TB[NHIST + 1];      // 各bank的标签宽度数组：低历史8位，高历史12位
int logg[NHIST + 1];    // 各bank的大小对数数组：都等于LOGG=10，即1024项

uint64_t Seed;           // for the pseudo-random number generator


class folded_history    // 历史折叠类：将长历史压缩为短索引
{
    public:
        unsigned comp;  // 压缩后的历史值
        int CLENGTH;    // 压缩长度（目标长度）
        int OLENGTH;    // 原始历史长度
        int OUTPOINT;   // 折叠点：OLENGTH % CLENGTH

        folded_history ()
        {
        }

        void init (int original_length, int compressed_length)
        {
            comp = 0;
            OLENGTH = original_length;
            CLENGTH = compressed_length;
            OUTPOINT = OLENGTH % CLENGTH;

        }

        void update (std::array<uint8_t, HISTBUFFERLENGTH>&h, int PT)
        {
            comp = (comp << 1) ^ h[PT & (HISTBUFFERLENGTH - 1)];
            comp ^= h[(PT + OLENGTH) & (HISTBUFFERLENGTH - 1)] << OUTPOINT;
            comp ^= (comp >> CLENGTH);
            comp = (comp) & ((1 << CLENGTH) - 1);
        }
};
using tage_index_t = std::array<folded_history, NHIST+1>;
using tage_tag_t = std::array<folded_history, NHIST+1>;


struct cbp_hist_t    // 分支预测器历史状态结构体
{
      // Begin Conventional Histories
      uint64_t GHIST;                                    // 全局分支历史 (64位)
      std::array<uint8_t, HISTBUFFERLENGTH> ghist;       // 全局历史缓冲区
      uint64_t phist;      //path history               // 路径历史
      int ptghist;                                       // 全局历史指针
      tage_index_t ch_i;                                 // TAGE索引折叠历史数组
      std::array<tage_tag_t, 2> ch_t;                   // TAGE标签折叠历史数组

      std::array<uint64_t, NLOCAL> L_shist;        // 本地历史数组
      std::array<uint64_t, NSECLOCAL> S_slhist;     // 二级本地历史数组
      std::array<uint64_t, NTLOCAL> T_slhist;       // 三级本地历史数组

      std::array<uint64_t, 256> IMHIST;             // IMLI历史缓冲区
      uint64_t IMLIcount;      // use to monitor the iteration number // 用于监控迭代次数
#ifdef LOOPPREDICTOR
      std::vector<lentry> ltable;   // 循环预测器表
      int8_t WITHLOOP;              // 循环预测器使用计数器
#endif
      cbp_hist_t()
      {
#ifdef LOOPPREDICTOR
          ltable.resize(1 << (LOGL));
          WITHLOOP = -1;
#endif
      }
};



int predictorsize ()
{
    int STORAGESIZE = 0;
    int inter = 0;



    STORAGESIZE +=
        NBANKHIGH * (1 << (logg[BORN])) * (CWIDTH + UWIDTH + TB[BORN]);
    STORAGESIZE += NBANKLOW * (1 << (logg[1])) * (CWIDTH + UWIDTH + TB[1]);

    STORAGESIZE += (SIZEUSEALT) * ALTWIDTH;
    STORAGESIZE += (1 << LOGB) + (1 << (LOGB - HYSTSHIFT));
    STORAGESIZE += m[NHIST];
    STORAGESIZE += PHISTWIDTH;
    STORAGESIZE += 10;      //the TICK counter

    fprintf (stderr, " (TAGE %d) ", STORAGESIZE);
#ifdef SC
#ifdef LOOPPREDICTOR

    inter = (1 << LOGL) * (2 * WIDTHNBITERLOOP + LOOPTAG + 4 + 4 + 1);
    fprintf (stderr, " (LOOP %d) ", inter);
    STORAGESIZE += inter;
#endif

    inter += WIDTHRES;
    inter += WIDTHRESP * ((1 << LOGSIZEUP)); //the update threshold counters
    inter += 3 * EWIDTH * (1 << LOGSIZEUPS);    // the extra weight of the partial sums
    inter += (PERCWIDTH) * 3 * (1 << (LOGBIAS));

    inter +=
        (GNB - 2) * (1 << (LOGGNB)) * (PERCWIDTH) +
        (1 << (LOGGNB - 1)) * (2 * PERCWIDTH);
    inter += Gm[0];     //global histories for SC
    inter += (PNB - 2) * (1 << (LOGPNB)) * (PERCWIDTH) +
        (1 << (LOGPNB - 1)) * (2 * PERCWIDTH);
    //we use phist already counted for these tables

#ifdef LOCALH
    inter +=
        (LNB - 2) * (1 << (LOGLNB)) * (PERCWIDTH) +
        (1 << (LOGLNB - 1)) * (2 * PERCWIDTH);
    inter += NLOCAL * Lm[0];
    inter += EWIDTH * (1 << LOGSIZEUPS);
#ifdef LOCALS
    inter +=
        (SNB - 2) * (1 << (LOGSNB)) * (PERCWIDTH) +
        (1 << (LOGSNB - 1)) * (2 * PERCWIDTH);
    inter += NSECLOCAL * (Sm[0]);
    inter += EWIDTH * (1 << LOGSIZEUPS);

#endif
#ifdef LOCALT
    inter +=
        (TNB - 2) * (1 << (LOGTNB)) * (PERCWIDTH) +
        (1 << (LOGTNB - 1)) * (2 * PERCWIDTH);
    inter += NTLOCAL * Tm[0];
    inter += EWIDTH * (1 << LOGSIZEUPS);
#endif









#endif



#ifdef IMLI

    inter += (1 << (LOGINB - 1)) * PERCWIDTH;
    inter += Im[0];

    inter += IMNB * (1 << (LOGIMNB - 1)) * PERCWIDTH;
    inter += 2 * EWIDTH * (1 << LOGSIZEUPS);    // the extra weight of the partial sums
    inter += 256 * IMm[0];
#endif
    inter += 2 * CONFWIDTH; //the 2 counters in the choser
    STORAGESIZE += inter;


    fprintf (stderr, " (SC %d) ", inter);
#endif
#ifdef PRINTSIZE
    fprintf (stderr, " (TOTAL %d bits %d Kbits) ", STORAGESIZE,
            STORAGESIZE / 1024);
    fprintf (stdout, " (TOTAL %d bits %d Kbits) ", STORAGESIZE,
            STORAGESIZE / 1024);
#endif


    return (STORAGESIZE);
}

// The interface to the simulator is defined in cond_branch_predictor_interface.cc
// This predictor is a modified version of CBP2016 Tage.
// The CBP Tage predicted and updated the predictor right away.
// The simulator here provides 3 major hooks for the predictor:
// * get_cond_dir_prediction -> lookup the predictor and return the prediction.  This is invoked only for conditional branches.
// * spec_update -> This is used for updating the history. It provides the actual direction of the branch. This is invoked for all branches.
// * notify_instr_execute_resolve -> This hook is used to update the predictor. This is invoked for all the instructions and provides all information available at execute.
//    * Note: The history at update is different than history at predict. To ensure that the predictor is getting trained correctly, 
//    at predict, we checkpoint the history in an unordered_map(pred_time_histories) using unique identifying id of the instruction. 
//    When updating the predicor, we recover the prediction time history.
// There are a couple of other hooks that aren't used in the current implementation, but are available to exploit:
// * notify_instr_decode 
// * notify_instr_commit
class CBP2016_TAGE_SC_L
{
    public:
        //state set by predict
        int GI[NHIST + 1];      // indexes to the different tables are computed only once   // 各表的索引，只计算一次
        uint GTAG[NHIST + 1];   // tags for the different tables are computed only once   // 各表的标签，只计算一次
        int BI;             // index of the bimodal table                                // 双模态表的索引

        //
        int THRES;       // 统计修正器阈值
        //
        // State set in predict and used in update
        // Begin LOOPPREDICTOR State
        bool predloop;  // loop predictor prediction                           // 循环预测器预测结果
        int LIB;                                                         // 循环预测器bank索引
        int LI;                                                          // 循环预测器索引
        int LHIT;           //hitting way in the loop predictor              // 循环预测器命中way
        int LTAG;           //tag on the loop predictor                      // 循环预测器标签
        bool LVALID;        // validity of the loop predictor prediction     // 循环预测器预测有效性
        // End LOOPPREDICTOR State

        bool tage_pred;         // TAGE prediction                            // TAGE预测结果
        bool alttaken;          // alternate  TAGEprediction                  // 备选TAGE预测结果
        bool LongestMatchPred;                                               // 最长匹配预测结果
        int HitBank;            // longest matching bank                      // 最长匹配bank
        int AltBank;            // alternate matching bank                    // 备选匹配bank
        bool pred_inter;                                                     // 中间预测结果

        bool LowConf;                                                       // 低置信度标志
        bool HighConf;                                                      // 高置信度标志

        // checkpointed in history
        //int8_t WITHLOOP;    // counter to monitor whether or not loop prediction is beneficial

        cbp_hist_t active_hist; // running history always updated accurately     // 运行时历史，始终准确更新
        // checkpointed history. Can be accesed using the inst-id(seq_no/piece)
        std::unordered_map<uint64_t/*key*/, cbp_hist_t/*val*/> pred_time_histories;  // 预测时历史检查点，可通过指令ID访问

        CBP2016_TAGE_SC_L (void)
        {
            init_histories (active_hist);
#ifdef PRINTSIZE
            predictorsize ();
#endif
        }

        void setup()
        {
        }

        void terminate()
        {
        }

        uint64_t get_unique_inst_id(uint64_t seq_no, uint8_t piece) const
        {
            assert(piece < 16);
            return (seq_no << 4) | (piece & 0x000F);
        }

        void init_histories (cbp_hist_t& current_hist)
        {
            m[1] = MINHIST;
            m[NHIST / 2] = MAXHIST;
            for (int i = 2; i <= NHIST / 2; i++)
            {
                m[i] =
                    (int) (((double) MINHIST *
                                pow ((double) (MAXHIST) / (double) MINHIST,
                                    (double) (i - 1) / (double) (((NHIST / 2) - 1)))) +
                            0.5);
                //      fprintf(stderr, "(%d %d)", m[i],i);

            }
            for (int i = 1; i <= NHIST; i++)
            {
                NOSKIP[i] = ((i - 1) & 1)
                    || ((i >= BORNINFASSOC) & (i < BORNSUPASSOC));

            }

            NOSKIP[4] = 0;
            NOSKIP[NHIST - 2] = 0;
            NOSKIP[8] = 0;
            NOSKIP[NHIST - 6] = 0;
            // just eliminate some extra tables (very very marginal)

            for (int i = NHIST; i > 1; i--)
            {
                m[i] = m[(i + 1) / 2];


            }
            for (int i = 1; i <= NHIST; i++)
            {
                TB[i] = TBITS + 4 * (i >= BORN);
                logg[i] = LOGG;

            }


//#ifdef LOOPPREDICTOR
//            ltable = new lentry[1 << (LOGL)];
//#endif

            gtable[1] = new gentry[NBANKLOW * (1 << LOGG)];     // 低历史长度表，10个bank，每bank 1024项
            SizeTable[1] = NBANKLOW * (1 << LOGG);

            gtable[BORN] = new gentry[NBANKHIGH * (1 << LOGG)];     // 高历史长度表，20个bank，每bank 1024项
            SizeTable[BORN] = NBANKHIGH * (1 << LOGG);

            for (int i = BORN + 1; i <= NHIST; i++)     // 高历史表(14-36)共享gtable[BORN/13]
                gtable[i] = gtable[BORN];
            for (int i = 2; i <= BORN - 1; i++)         // 低历史表(2-12)共享gtable[1]
                gtable[i] = gtable[1];
            btable = new bentry[1 << LOGB];

            for (int i = 1; i <= NHIST; i++)
            {
                current_hist.ch_i[i].init (m[i], (logg[i]));
                current_hist.ch_t[0][i].init (current_hist.ch_i[i].OLENGTH, TB[i]);
                current_hist.ch_t[1][i].init (current_hist.ch_i[i].OLENGTH, TB[i] - 1);

            }

// LOOPPREDICTOR state
            LVALID = false;
            //WITHLOOP = -1;
            Seed = 0;

            TICK = 0;
            current_hist.phist = 0;
            Seed = 0;

            for (int i = 0; i < HISTBUFFERLENGTH; i++)
                current_hist.ghist[i] = 0;
            current_hist.ptghist = 0;
            updatethreshold=35<<3;

            for (int i = 0; i < (1 << LOGSIZEUP); i++)
                Pupdatethreshold[i] = 0;
            for (int i = 0; i < GNB; i++)
                GGEHL[i] = &GGEHLA[i][0];
            for (int i = 0; i < LNB; i++)
                LGEHL[i] = &LGEHLA[i][0];

            for (int i = 0; i < GNB; i++)
                for (int j = 0; j < ((1 << LOGGNB) - 1); j++)
                {
                    if (!(j & 1))
                    {
                        GGEHL[i][j] = -1;

                    }
                }
            for (int i = 0; i < LNB; i++)
                for (int j = 0; j < ((1 << LOGLNB) - 1); j++)
                {
                    if (!(j & 1))
                    {
                        LGEHL[i][j] = -1;

                    }
                }

            for (int i = 0; i < SNB; i++)
                SGEHL[i] = &SGEHLA[i][0];
            for (int i = 0; i < TNB; i++)
                TGEHL[i] = &TGEHLA[i][0];
            for (int i = 0; i < PNB; i++)
                PGEHL[i] = &PGEHLA[i][0];
#ifdef IMLI
#ifdef IMLIOH
            for (int i = 0; i < FNB; i++)
                FGEHL[i] = &FGEHLA[i][0];

            for (int i = 0; i < FNB; i++)
                for (int j = 0; j < ((1 << LOGFNB) - 1); j++)
                {
                    if (!(j & 1))
                    {
                        FGEHL[i][j] = -1;

                    }
                }
#endif
            for (int i = 0; i < INB; i++)
                IGEHL[i] = &IGEHLA[i][0];
            for (int i = 0; i < INB; i++)
                for (int j = 0; j < ((1 << LOGINB) - 1); j++)
                {
                    if (!(j & 1))
                    {
                        IGEHL[i][j] = -1;

                    }
                }
            for (int i = 0; i < IMNB; i++)
                IMGEHL[i] = &IMGEHLA[i][0];
            for (int i = 0; i < IMNB; i++)
                for (int j = 0; j < ((1 << LOGIMNB) - 1); j++)
                {
                    if (!(j & 1))
                    {
                        IMGEHL[i][j] = -1;

                    }
                }

#endif
            for (int i = 0; i < SNB; i++)
                for (int j = 0; j < ((1 << LOGSNB) - 1); j++)
                {
                    if (!(j & 1))
                    {
                        SGEHL[i][j] = -1;

                    }
                }
            for (int i = 0; i < TNB; i++)
                for (int j = 0; j < ((1 << LOGTNB) - 1); j++)
                {
                    if (!(j & 1))
                    {
                        TGEHL[i][j] = -1;

                    }
                }
            for (int i = 0; i < PNB; i++)
                for (int j = 0; j < ((1 << LOGPNB) - 1); j++)
                {
                    if (!(j & 1))
                    {
                        PGEHL[i][j] = -1;

                    }
                }


            for (int i = 0; i < (1 << LOGB); i++)
            {
                btable[i].pred = 0;
                btable[i].hyst = 1;
            }




            for (int j = 0; j < (1 << LOGBIAS); j++)
            {
                switch (j & 3)
                {
                    case 0:
                        BiasSK[j] = -8;
                        break;
                    case 1:
                        BiasSK[j] = 7;
                        break;
                    case 2:
                        BiasSK[j] = -32;

                        break;
                    case 3:
                        BiasSK[j] = 31;
                        break;
                }
            }
            for (int j = 0; j < (1 << LOGBIAS); j++)
            {
                switch (j & 3)
                {
                    case 0:
                        Bias[j] = -32;

                        break;
                    case 1:
                        Bias[j] = 31;
                        break;
                    case 2:
                        Bias[j] = -1;
                        break;
                    case 3:
                        Bias[j] = 0;
                        break;
                }
            }
            for (int j = 0; j < (1 << LOGBIAS); j++)
            {
                switch (j & 3)
                {
                    case 0:
                        BiasBank[j] = -32;

                        break;
                    case 1:
                        BiasBank[j] = 31;
                        break;
                    case 2:
                        BiasBank[j] = -1;
                        break;
                    case 3:
                        BiasBank[j] = 0;
                        break;
                }
            }
            for (int i = 0; i < SIZEUSEALT; i++)
            {
                use_alt_on_na[i] = 0;

            }
            for (int i = 0; i < (1 << LOGSIZEUPS); i++)
            {
                WG[i] = 7;
                WL[i] = 7;
                WS[i] = 7;
                WT[i] = 7;
                WP[i] = 7;
                WI[i] = 7;
                WB[i] = 4;
            }
            TICK = 0;
            for (int i = 0; i < NLOCAL; i++)
            {
                current_hist.L_shist[i] = 0;
            }
            for (int i = 0; i < NSECLOCAL; i++)
            {
                current_hist.S_slhist[i] = 3;

            }
            current_hist.GHIST = 0;
            current_hist.ptghist = 0;
            current_hist.phist = 0;
        }// end init_histories




        // index function for the bimodal table
        int bindex (UINT64 PC) const
        {
            return ((PC ^ (PC >> LOGB)) & ((1 << (LOGB)) - 1));
        }


        // =================================================================
        // F: 路径历史折叠函数 - 将长路径历史压缩为短索引
        // 功能：使用循环移位将路径历史折叠到bank大小范围内
        // 输入：A - 路径历史, size - 历史长度, bank - bank编号
        // 输出：折叠后的索引值
        // =================================================================
        // the index functions for the tagged tables uses path history as in the OGEHL predictor
        //F serves to mix path history: not very important impact
        int F (uint64_t A, int size, int bank) const
        {
            int   A1, A2;
            A = A & ((1 << size) - 1);                                         // 截断到指定长度
            A1 = (A & ((1 << logg[bank]) - 1));                               // 低位部分
            A2 = (A >> logg[bank]);                                           // 高位部分

            // 对高位部分进行循环移位，避免冲突
            if (bank < logg[bank])
                A2 = ((A2 << bank) & ((1 << logg[bank]) - 1)) +
                     (A2 >> (logg[bank] - bank));
            A = A1 ^ A2;                                                      // 异或组合低位和高位

            // 最终循环移位，确保均匀分布
            if (bank < logg[bank])
                A = ((A << bank) & ((1 << logg[bank]) - 1)) +
                    (A >> (logg[bank] - bank));
            return (A);
        }

        // =================================================================
        // gindex: TAGE索引计算函数 - 计算给定bank的表索引
        // 输入：PC - 程序计数器, bank - bank编号, hist - 路径历史, ch_i - 折叠历史索引
        // 输出：计算出的索引值
        // =================================================================
        // gindex computes a full hash of PC, ghist and phist
        //int gindex (unsigned int PC, int bank, uint64_t hist, const folded_history * ch_i) const
        int gindex (unsigned int PC, int bank, uint64_t hist, const tage_index_t& ch_i) const
        {
            int index;
            int M = (m[bank] > PHISTWIDTH) ? PHISTWIDTH : m[bank];              // 限制路径历史长度不超过PHISTWIDTH = 27
            index = PC ^                                                         // PC散列
                    (PC >> (abs (logg[bank] - bank) + 1)) ^                     // PC移位散列
                    ch_i[bank].comp ^                                           // 折叠分支历史
                    F (hist, M, bank);                                          // 路径历史折叠

            return (index & ((1 << (logg[bank])) - 1));                        // 截断到bank大小范围
        }

        // =================================================================
        // gtag: TAGE标签计算函数 - 计算给定bank的标签用于条目匹配
        // 输入：PC - 程序计数器, bank - bank编号, tag_0_array/tag_1_array - 折叠历史标签数组
        // 输出：计算出的标签值
        // =================================================================
        //  tag computation
        uint16_t gtag (unsigned int PC, int bank, const tage_tag_t& tag_0_array, const tage_tag_t& tag_1_array) const
        {
            int tag = (PC) ^                                                      // PC散列
                      tag_0_array[bank].comp ^                                    // 第一折叠历史
                      (tag_1_array[bank].comp << 1);                             // 第二折叠历史(左移1位)
            return (tag & ((1 << (TB[bank])) - 1));                             // 截断到标签宽度
        }

        // up-down saturating counter
        void ctrupdate (int8_t & ctr, bool taken, int nbits)
        {
            if (taken)
            {
                if (ctr < ((1 << (nbits - 1)) - 1))
                    ctr++;
            }
            else
            {
                if (ctr > -(1 << (nbits - 1)))
                    ctr--;
            }
        }


        // =================================================================
        // getbim: 双模态预测器获取函数
        // 功能：计算双模态预测器的预测结果和置信度
        // 输出：预测结果(true=taken, false=not taken)
        // 副作用：更新HighConf, LowConf, AltConf等置信度标志
        // =================================================================
        bool getbim ()
        {
            BIM = (btable[BI].pred << 1) + (btable[BI >> HYSTSHIFT].hyst);     // 组合pred和hyst为2位计数器
            HighConf = (BIM == 0) || (BIM == 3);                              // 高置信度：强taken(BIM=3)或强not taken(BIM=0)
            LowConf = !HighConf;                                              // 低置信度：其他情况
            AltConf = HighConf;                                               // 备选置信度等于双模态置信度
            MedConf = false;                                                  // 双模态没有中等置信度
            return (btable[BI].pred > 0);                                     // 返回预测结果
        }

        // =================================================================
        // baseupdate: 双模态预测器更新函数
        // 功能：根据实际分支方向更新双模态预测器
        // 输入：Taken - 实际分支方向
        // =================================================================
        void baseupdate (bool Taken)
        {
            int inter = BIM;                                                   // 获取当前2位计数器值
            if (Taken)                                                         // 分支实际taken
            {
                if (inter < 3)                                                 // 计数器未饱和
                    inter += 1;                                                // 增加计数器
            }
            else if (inter > 0)                                                // 分支实际not taken且计数器>0
                inter--;                                                       // 减少计数器
            btable[BI].pred = inter >> 1;                                      // 更新pred位(高位)
            btable[BI >> HYSTSHIFT].hyst = (inter & 1);                        // 更新hyst位(低位)
        };

        //just a simple pseudo random number generator: use available information
        // to allocate entries  in the loop predictor
        int MYRANDOM ()
        {
            Seed++;
            Seed ^= active_hist.phist;
            Seed = (Seed >> 21) + (Seed << 11);
            Seed ^= (int64_t)active_hist.ptghist;
            Seed = (Seed >> 10) + (Seed << 22);
            return (Seed & 0xFFFFFFFF);
        };


        // ============================================================================
        // TAGE PREDICTION: TAGE分支预测核心函数
        // 功能：通过PC和历史信息进行分支预测
        // 输入：PC - 程序计数器, hist_to_use - 历史状态
        // 输出：更新HitBank, AltBank, tage_pred等预测结果
        // ============================================================================
        void Tagepred (UINT64 PC, const cbp_hist_t& hist_to_use)
        {
            HitBank = 0;      // 最长匹配bank初始化
            AltBank = 0;      // 备选匹配bank初始化

            // =================================================================
            // 阶段1：为所有历史长度计算索引和标签 (几何分布历史长度1-36)
            // =================================================================
            // =================================================================
            // 核心机制：i和i+1构成"伪2路组相连"
            // 相同标签但不同索引，类似于2路组相连但更灵活
            // =================================================================
            for (int i = 1; i <= NHIST; i += 2)
            {
                GI[i] = gindex (PC, i, hist_to_use.phist, hist_to_use.ch_i);          // 计算bank i的索引
                GTAG[i] = gtag (PC, i, hist_to_use.ch_t[0], hist_to_use.ch_t[1]);     // 计算bank i的标签
                GTAG[i + 1] = GTAG[i];                                                // 相邻bank使用相同标签 (关键!)
                GI[i + 1] = GI[i] ^ (GTAG[i] & ((1 << LOGG) - 1));                   // 索引扰动: 避免与i完全相同
            }
            // =================================================================
            // 阶段2：Bank交织分配 - 核心作用：将大物理存储分割成多个逻辑bank
            // 原理：多个逻辑表共享同一个物理存储，通过偏移量访问不同区域
            // 优势：1) 减少存储开销 2) 分散索引冲突 3) 提高硬件利用率
            // 实现：高历史长度表(13-36)共享20*1024的物理存储，低表(1-12)共享10*1024
            // =================================================================
            int T = (PC ^ (hist_to_use.phist & ((1ULL << m[BORN]) - 1))) % NBANKHIGH;  // 计算起始bank
            //int T = (PC ^ phist) % NBANKHIGH;
            for (int i = BORN; i <= NHIST; i++)
                if (NOSKIP[i])                                                          // 跳过被禁用的表
                {
                    GI[i] += (T << LOGG);                                              // 添加bank偏移量
                    T++;
                    T = T % NBANKHIGH;                                                 // 循环分配到不同bank
                }

            // 低历史长度bank (1-12) 使用10个bank
            T = (PC ^ (hist_to_use.phist & ((1 << m[1]) - 1))) % NBANKLOW;            // 重新计算起始bank
            for (int i = 1; i <= BORN - 1; i++)
                if (NOSKIP[i])
                {
                    GI[i] += (T << LOGG);                                              // 添加bank偏移量
                    T++;
                    T = T % NBANKLOW;                                                  // 循环分配到不同bank
                }
            // =================================================================
            // 阶段3：双模态预测作为备用预测源
            // =================================================================
            //just do not forget most address are aligned on 4 bytes
            BI = (PC ^ (PC >> 2)) & ((1 << LOGB) - 1);                         // 双模态表索引计算

            {
                alttaken = getbim ();                                           // 获取双模态预测结果
                tage_pred = alttaken;                                           // 初始化TAGE预测为双模态结果
                LongestMatchPred = alttaken;                                    // 初始化最长匹配预测为双模态结果
            }

            // =================================================================
            // 阶段4：最长匹配查找 - 从最长历史bank开始查找标签匹配的条目
            // =================================================================
            //Look for the bank with longest matching history
            for (int i = NHIST; i > 0; i--)                                    // 从历史长度最长的bank开始
            {
                if (NOSKIP[i])                                                // 跳过被禁用的表
                    if (gtable[i][GI[i]].tag == GTAG[i])                      // 检查标签是否匹配
                    {
                        HitBank = i;                                          // 记录命中bank
                        LongestMatchPred = (gtable[HitBank][GI[HitBank]].ctr >= 0); // 根据计数器预测
                        break;                                                 // 找到最长匹配后立即停止
                    }
            }

            // =================================================================
            // 阶段5：备选预测查找 - 在HitBank之前查找次长匹配作为备选
            // =================================================================
            //Look for the alternate bank
            for (int i = HitBank - 1; i > 0; i--)                             // 从HitBank-1开始向下查找
            {
                if (NOSKIP[i])
                    if (gtable[i][GI[i]].tag == GTAG[i])                      // 检查标签是否匹配
                    {
                        AltBank = i;                                          // 记录备选bank
                        break;                                                 // 找到备选后立即停止
                    }
            }
            // =================================================================
            // 阶段6：最终预测决策 - 基于命中情况选择预测结果
            // =================================================================
            //computes the prediction and the alternate prediction

            if (HitBank > 0)                                                     // 如果有命中
            {
                // 获取备选预测结果
                if (AltBank > 0)
                {
                    alttaken = (gtable[AltBank][GI[AltBank]].ctr >= 0);           // 备选bank的预测结果
                    AltConf = (abs (2 * gtable[AltBank][GI[AltBank]].ctr + 1) > 1); // 备选预测置信度
                }
                else
                    alttaken = getbim ();                                        // 无备选则使用双模态预测

                // =================================================================
                // 预测选择逻辑：新分配条目使用备选预测，否则使用最长匹配
                // =================================================================
                //if the entry is recognized as a newly allocated entry and
                //USE_ALT_ON_NA is positive  use the alternate prediction

                bool Huse_alt_on_na = (use_alt_on_na[INDUSEALT] >= 0);           // 检查是否启用新分配条目使用备选预测
                if ((!Huse_alt_on_na)                                            // 不启用新分配条目备选预测 或者
                        || (abs (2 * gtable[HitBank][GI[HitBank]].ctr + 1) > 1)) // 最长匹配计数器不是弱计数器
                    tage_pred = LongestMatchPred;                                // 使用最长匹配预测
                else
                    tage_pred = alttaken;                                        // 使用备选预测

                // =================================================================
                // 计算预测置信度 (基于最长匹配计数器的饱和程度)
                // =================================================================
                HighConf =                                                         // 高置信度：计数器接近饱和
                    (abs (2 * gtable[HitBank][GI[HitBank]].ctr + 1) >=
                     (1 << CWIDTH) - 1);                                          // CWIDTH=3, 饱和值为7
                LowConf = (abs (2 * gtable[HitBank][GI[HitBank]].ctr + 1) == 1);  // 低置信度：计数器为1
                MedConf = (abs (2 * gtable[HitBank][GI[HitBank]].ctr + 1) == 5);  // 中置信度：计数器为5
            }
        }

        uint64_t get_local_index(uint64_t PC) const
        {
            return ((PC ^ (PC >>2)) & (NLOCAL-1));
        }

        uint64_t get_second_local_index(uint64_t PC) const
        {
            return (((PC ^ (PC >>5))) & (NSECLOCAL-1));
        }

        uint64_t get_third_local_index(uint64_t PC) const
        {
            return  (((PC ^ (PC >>(LOGTNB)))) & (NTLOCAL-1)); // different hash for the history
        }

        uint64_t get_bias_index(uint64_t PC) const
        {
            return (((((PC ^(PC >>2))<<1)  ^  (LowConf &(LongestMatchPred!=alttaken))) <<1) +  pred_inter) & ((1<<LOGBIAS) -1);
        }

        uint64_t get_biassk_index(uint64_t PC) const
        {
            return (((((PC^(PC>>(LOGBIAS-2)))<<1) ^ (HighConf))<<1) +  pred_inter) & ((1<<LOGBIAS) -1);
        }

        uint64_t get_biasbank_index(uint64_t PC) const
        {
            return (pred_inter + (((HitBank+1)/4)<<4) + (HighConf<<1) + (LowConf <<2) +((AltBank!=0)<<3)+ ((PC^(PC>>2))<<7)) & ((1<<LOGBIAS) -1);
        }

        bool predict (uint64_t seq_no, uint8_t piece, UINT64 PC)
        {
            // =================================================================
            // 预测阶段：保存历史状态 + 执行预测 + 保存索引信息
            // 在真实硬件中，这些索引信息需要作为分支的元数据保存
            // =================================================================
            // checkpoint current hist
            pred_time_histories.emplace(get_unique_inst_id(seq_no, piece), active_hist);
            const bool pred_taken = predict_using_given_hist(seq_no, piece, PC, active_hist, true/*pred_time_predict*/);

            // 在真实硬件中，这里应该保存：
            // - GI[37] (各表的索引)
            // - GTAG[37] (各表的标签)
            // - HitBank, AltBank (命中信息)
            // - tage_pred, alttaken (预测结果)
            // 然后在update时直接使用这些保存的值
            return pred_taken;
        }

        bool predict_using_given_hist (uint64_t seq_no, uint8_t piece, UINT64 PC, const cbp_hist_t& hist_to_use, const bool pred_time_predict)
        {
            // computes the TAGE table addresses and the partial tags
            Tagepred (PC, hist_to_use);
            bool pred_taken = tage_pred;
#ifndef SC
            return (tage_pred);
#endif

#ifdef LOOPPREDICTOR
            predloop = getloop (PC, hist_to_use);   // loop prediction
            pred_taken = ((hist_to_use.WITHLOOP >= 0) && (LVALID)) ? predloop : pred_taken;
#endif
            pred_inter = pred_taken;

            //Compute the SC prediction

            LSUM = 0;

            //integrate BIAS prediction   
            int8_t ctr = Bias[get_bias_index(PC)];

            LSUM += (2 * ctr + 1);
            ctr = BiasSK[get_biassk_index(PC)];
            LSUM += (2 * ctr + 1);
            ctr = BiasBank[get_biasbank_index(PC)];
            LSUM += (2 * ctr + 1);
#ifdef VARTHRES
            LSUM = (1 + (WB[INDUPDS] >= 0)) * LSUM;
#endif
            //integrate the GEHL predictions
            LSUM += Gpredict ((PC << 1) + pred_inter, hist_to_use.GHIST, Gm, GGEHL, GNB, LOGGNB, WG);
            LSUM += Gpredict (PC, hist_to_use.phist, Pm, PGEHL, PNB, LOGPNB, WP);
#ifdef LOCALH
            LSUM += Gpredict (PC, hist_to_use.L_shist[get_local_index(PC)], Lm, LGEHL, LNB, LOGLNB, WL);
#ifdef LOCALS
            LSUM += Gpredict (PC, hist_to_use.S_slhist[get_second_local_index(PC)], Sm, SGEHL, SNB, LOGSNB, WS);
#endif
#ifdef LOCALT
            LSUM += Gpredict (PC, hist_to_use.T_slhist[get_third_local_index(PC)], Tm, TGEHL, TNB, LOGTNB, WT);
#endif
#endif

#ifdef IMLI
            LSUM += Gpredict (PC, hist_to_use.IMHIST[(hist_to_use.IMLIcount)], IMm, IMGEHL, IMNB, LOGIMNB, WIM);
            LSUM += Gpredict (PC, hist_to_use.IMLIcount, Im, IGEHL, INB, LOGINB, WI);
#endif
            bool SCPRED = (LSUM >= 0);
            //just  an heuristic if the respective contribution of component groups can be multiplied by 2 or not
            THRES = (updatethreshold>>3)+Pupdatethreshold[INDUPD]
#ifdef VARTHRES
                + 12 * ((WB[INDUPDS] >= 0) + (WP[INDUPDS] >= 0)
#ifdef LOCALH
                        + (WS[INDUPDS] >= 0) + (WT[INDUPDS] >= 0) + (WL[INDUPDS] >= 0)
#endif
                        + (WG[INDUPDS] >= 0)
#ifdef IMLI
                        + (WI[INDUPDS] >= 0)
#endif
                       )
#endif
                ;

            //Minimal benefit in trying to avoid accuracy loss on low confidence SC prediction and  high/medium confidence on TAGE
            // but just uses 2 counters 0.3 % MPKI reduction
            if (pred_inter != SCPRED)
            {
                //Choser uses TAGE confidence and |LSUM|
                pred_taken = SCPRED;
                if (HighConf)
                {
                    if ((abs (LSUM) < THRES / 4))
                    {
                        pred_taken = pred_inter;
                    }

                    else if ((abs (LSUM) < THRES / 2))
                    {
                        pred_taken = (SecondH < 0) ? SCPRED : pred_inter;
                    }
                }

                if (MedConf)
                    if ((abs (LSUM) < THRES / 4))
                    {
                        pred_taken = (FirstH < 0) ? SCPRED : pred_inter;
                    }

            }

            return pred_taken;
        }


        void history_update (uint64_t seq_no, uint8_t piece, UINT64 PC, int brtype, bool pred_taken, bool taken, UINT64 nextPC)
        {
            //HistoryUpdate (PC, brtype, taken, nextPC, active_hist.phist, active_hist.ptghist, active_hist.ch_i, active_hist.ch_t[0], active_hist.ch_t[1]);
            HistoryUpdate (PC, brtype, pred_taken, taken, nextPC);
        }

        void TrackOtherInst (UINT64 PC, int brtype, bool pred_taken, bool taken, UINT64 nextPC)
        {
            HistoryUpdate (PC, brtype, pred_taken, taken, nextPC);
        }

        void HistoryUpdate (UINT64 PC, int brtype, bool pred_taken, bool taken, UINT64 nextPC)
        {

            auto& X = active_hist.phist;
            auto& Y = active_hist.ptghist;

            auto& H = active_hist.ch_i;
            auto& G = active_hist.ch_t[0];
            auto& J = active_hist.ch_t[1];

            //special treatment for indirect  branchs;
            int maxt = 2;
            if (brtype & 1)   // conditional
                maxt = 2;
            else if ((brtype & 2) )
                maxt = 3;

#ifdef IMLI
            if (brtype & 1)   // conditional
            {
#ifdef IMLI
                active_hist.IMHIST[active_hist.IMLIcount] = (active_hist.IMHIST[active_hist.IMLIcount] << 1) + taken;
#endif

#ifdef LOOPPREDICTOR
                // only for conditional branch
                if (LVALID)
                {
                    if (pred_taken != predloop)
                        ctrupdate (active_hist.WITHLOOP, (predloop == pred_taken), 7);
                }

                loopupdate(PC, pred_taken, false/*alloc*/, active_hist.ltable);
#endif
                if (nextPC < PC)

                {
                    //This branch corresponds to a loop
                    if (!taken)
                    {
                        //exit of the "loop"
                        active_hist.IMLIcount = 0;

                    }
                    if (taken)
                    {

                        if (active_hist.IMLIcount < ((1ULL << Im[0]) - 1))
                            active_hist.IMLIcount++;
                    }
                }
            }


#endif

            if (brtype & 1)
            {
                active_hist.GHIST = (active_hist.GHIST << 1) + (taken & (nextPC < PC));
                active_hist.L_shist[get_local_index(PC)] = (active_hist.L_shist[get_local_index(PC)] << 1) + (taken);
                active_hist.S_slhist[get_second_local_index(PC)] = ((active_hist.S_slhist[get_second_local_index(PC)] << 1) + taken) ^ (PC & 15);
                active_hist.T_slhist[get_third_local_index(PC)] = (active_hist.T_slhist[get_third_local_index(PC)] << 1) + taken;
            }


            int T = ((PC ^ (PC >> 2))) ^ taken;
            int PATH = PC ^ (PC >> 2) ^ (PC >> 4);
            if ((brtype == 3) & taken)
            {
                T = (T ^ (nextPC >> 2));
                PATH = PATH ^ (nextPC >> 2) ^ (nextPC >> 4);
            }

            for (int t = 0; t < maxt; t++)
            {
                bool DIR = (T & 1);
                T >>= 1;
                int PATHBIT = (PATH & 127);
                PATH >>= 1;
                //update  history
                Y--;  //ptghist
                active_hist.ghist[Y & (HISTBUFFERLENGTH - 1)] = DIR;
                X = (X << 1) ^ PATHBIT; //phist


                // updates to folded histories
                for (int i = 1; i <= NHIST; i++)
                {
                    H[i].update (active_hist.ghist, Y);
                    G[i].update (active_hist.ghist, Y);
                    J[i].update (active_hist.ghist, Y);
                }
            }

            X = (X & ((1<<PHISTWIDTH)-1));

        }//END UPDATE  HISTORIES

        // PREDICTOR UPDATE

        //void update (UINT64 PC, int brtype, bool resolveDir, bool predDir, UINT64 nextPC)
        // =================================================================
        // 更新接口函数：处理历史检查点和索引重新计算
        // 设计权衡：重新预测 vs 保存元数据
        // =================================================================
        void update (uint64_t seq_no, uint8_t piece, UINT64 PC, bool resolveDir, bool predDir, UINT64 nextPC)
        {
            const auto pred_hist_key = get_unique_inst_id(seq_no, piece);
            const auto& pred_time_history = pred_time_histories.at(pred_hist_key);

            // =================================================================
            // 关键操作：使用预测时的历史重新执行预测
            // 原因：获取当时的索引信息(GI[], GTAG[], HitBank等)
            // 在真实硬件中：应该从分支元数据中直接读取这些信息
            // =================================================================
            const bool pred_taken = predict_using_given_hist(seq_no, piece, PC, pred_time_history, false/*pred_time_predict*/);

            // 调试检查：确保重新预测结果与原始预测一致
            //if(pred_taken != predDir)
            //{
            //    std::cout<<"id:"<<seq_no<<" PC:0x"<<std::hex<<PC<<std::dec<<" resolveDir:"<<resolveDir<<" pred_dir_at_pred:"<<predDir<<" pred_dir_at_update:"<<pred_taken<<std::endl;
            //    assert(false);
            //}

            // 执行实际更新
            update(PC, resolveDir, pred_taken, nextPC, pred_time_history);

            // 清理历史检查点
            pred_time_histories.erase(pred_hist_key);
            // erase pred_hist_key before 
        }

        void update (UINT64 PC, bool resolveDir, bool pred_taken, UINT64 nextPC, const cbp_hist_t& hist_to_use)
        {
#ifdef SC
#ifdef LOOPPREDICTOR
            if(pred_taken != resolveDir)  // incorrect loophhist updates in spec_update
            {
                // fix active hist.ltable and active_hist.WITHLOOP
                active_hist.ltable = hist_to_use.ltable;
                active_hist.WITHLOOP = hist_to_use.WITHLOOP;
                if (LVALID)
                {
                    if (pred_taken != predloop)
                        ctrupdate (active_hist.WITHLOOP, (predloop == resolveDir), 7);
                }
                loopupdate (PC, resolveDir, (pred_taken != resolveDir), active_hist.ltable);
            }
#endif

            bool SCPRED = (LSUM >= 0);
            if (pred_inter != SCPRED)
            {
                if ((abs (LSUM) < THRES))
                    if ((HighConf))
                    {


                        if ((abs (LSUM) < THRES / 2))
                            if ((abs (LSUM) >= THRES / 4))
                                ctrupdate (SecondH, (pred_inter == resolveDir), CONFWIDTH);
                    }
                if ((MedConf))
                    if ((abs (LSUM) < THRES / 4))
                    {
                        ctrupdate (FirstH, (pred_inter == resolveDir), CONFWIDTH);
                    }
            }

            if ((SCPRED != resolveDir) || ((abs (LSUM) < THRES)))
            {
                {
                    if (SCPRED != resolveDir)
                    {
                        Pupdatethreshold[INDUPD] += 1;
                        updatethreshold+=1;
                    }

                    else
                    {
                        Pupdatethreshold[INDUPD] -= 1;
                        updatethreshold -= 1;
                    }


                    if (Pupdatethreshold[INDUPD] >= (1 << (WIDTHRESP - 1)))
                        Pupdatethreshold[INDUPD] = (1 << (WIDTHRESP - 1)) - 1;
                    //Pupdatethreshold[INDUPD] could be negative
                    if (Pupdatethreshold[INDUPD] < -(1 << (WIDTHRESP - 1)))
                        Pupdatethreshold[INDUPD] = -(1 << (WIDTHRESP - 1));
                    if (updatethreshold >= (1 << (WIDTHRES - 1)))
                    {
                        updatethreshold = (1 << (WIDTHRES - 1)) - 1;
                    }
                    //updatethreshold could be negative
                    if (updatethreshold < -(1 << (WIDTHRES - 1)))
                    {
                        updatethreshold = -(1 << (WIDTHRES - 1));
                    }
                }
#ifdef VARTHRES
                {
                    int XSUM =
                        LSUM - ((WB[INDUPDS] >= 0) * ((2 * Bias[get_bias_index(PC)] + 1) +
                                    (2 * BiasSK[get_biassk_index(PC)] + 1) +
                                    (2 * BiasBank[get_biasbank_index(PC)] + 1)));
                    if ((XSUM +
                                ((2 * Bias[get_bias_index(PC)] + 1) + (2 * BiasSK[get_biassk_index(PC)] + 1) +
                                 (2 * BiasBank[get_biasbank_index(PC)] + 1)) >= 0) != (XSUM >= 0))
                        ctrupdate (WB[INDUPDS],
                                (((2 * Bias[get_bias_index(PC)] + 1) +
                                  (2 * BiasSK[get_biassk_index(PC)] + 1) +
                                  (2 * BiasBank[get_biasbank_index(PC)] + 1) >= 0) == resolveDir),
                                EWIDTH);
                }
#endif
                ctrupdate (Bias[get_bias_index(PC)], resolveDir, PERCWIDTH);
                ctrupdate (BiasSK[get_biassk_index(PC)], resolveDir, PERCWIDTH);
                ctrupdate (BiasBank[get_biasbank_index(PC)], resolveDir, PERCWIDTH);
                Gupdate ((PC << 1) + pred_inter, resolveDir,
                        hist_to_use.GHIST, Gm, GGEHL, GNB, LOGGNB, WG);
                Gupdate (PC, resolveDir, hist_to_use.phist, Pm, PGEHL, PNB, LOGPNB, WP);
#ifdef LOCALH
                Gupdate (PC, resolveDir, hist_to_use.L_shist[get_local_index(PC)], Lm, LGEHL, LNB, LOGLNB,
                        WL);
#ifdef LOCALS
                Gupdate (PC, resolveDir, hist_to_use.S_slhist[get_second_local_index(PC)], Sm,
                        SGEHL, SNB, LOGSNB, WS);
#endif
#ifdef LOCALT

                Gupdate (PC, resolveDir, hist_to_use.T_slhist[get_third_local_index(PC)], Tm, TGEHL, TNB, LOGTNB,
                        WT);
#endif
#endif


#ifdef IMLI
                Gupdate (PC, resolveDir, hist_to_use.IMHIST[(hist_to_use.IMLIcount)], IMm, IMGEHL, IMNB,
                        LOGIMNB, WIM);
                Gupdate (PC, resolveDir, hist_to_use.IMLIcount, Im, IGEHL, INB, LOGINB, WI);
#endif



            }
#endif

            // =================================================================
            // TAGE UPDATE: 分支预测器学习和适应阶段
            // =================================================================

            // =================================================================
            // 阶段1: 判断是否需要分配新条目
            // 条件: 预测错误 且 HitBank不是最长历史表(NHIST=36)
            // =================================================================
            bool ALLOC = ((tage_pred != resolveDir) & (HitBank < NHIST));


            // =================================================================
            // 阶段2: 新分配条目优化 - 避免过度分配
            // =================================================================
            //do not allocate too often if the overall prediction is correct

            if (HitBank > 0)
            {
                // =================================================================
                // 新分配条目检测: 计数器弱(±1)认为是新分配的
                // =================================================================
                // Manage the selection between longest matching and alternate matching
                // for "pseudo"-newly allocated longest matching entry
                // this is extremely important for TAGE only, not that important when the overall predictor is implemented
                bool PseudoNewAlloc =
                    (abs (2 * gtable[HitBank][GI[HitBank]].ctr + 1) <= 1);
                // an entry is considered as newly allocated if its prediction counter is weak

                if (PseudoNewAlloc)
                {
                    // =================================================================
                    // 如果最长匹配预测正确，不需要分配新条目
                    // =================================================================
                    if (LongestMatchPred == resolveDir)
                        ALLOC = false;
                    // if it was delivering the correct prediction, no need to allocate a new entry
                    //even if the overall prediction was false

                    // =================================================================
                    // 更新备选预测选择器: 根据新分配条目的表现调整选择策略
                    // =================================================================
                    if (LongestMatchPred != alttaken)
                    {
                        ctrupdate (use_alt_on_na[INDUSEALT], (alttaken == resolveDir),
                                ALTWIDTH);
                    }
                }
            }

            // =================================================================
            // 阶段3: 预测正确时减少分配 - 避免在预测正确时过度分配
            // =================================================================
            if (pred_taken == resolveDir)
                if ((MYRANDOM () & 31) != 0)  // 1/32概率仍然分配
                    ALLOC = false;

            // =================================================================
            // 阶段4: 新条目分配 - 在合适位置分配新的预测条目
            // =================================================================
            if (ALLOC)
            {
                // =================================================================
                // 分配策略设置
                // =================================================================
                int T = NNN;                    // NNN=1，通常分配1个条目

                int A = 1;                      // 起始bank偏移
                if ((MYRANDOM () & 127) < 32)   // 32/128概率
                    A = 2;                      // 使用更大的偏移
                int Penalty = 0;                // 遍历惩罚计数
                int NA = 0;                     // 已分配条目数
                int DEP = ((((HitBank - 1 + 2 * A) & 0xffe)) ^ (MYRANDOM () & 1));
                // just a complex formula to chose between X and X+1, when X is odd: sorry

                // =================================================================
                // 核心：体现"组相连"结构的分配策略
                // TAGE的组相连体现在i和i+1的配对关系上
                // =================================================================
                for (int I = DEP; I < NHIST; I += 2)
                {
                    int i = I + 1;              // 选择bank i (主位置)
                    bool Done = false;          // 是否完成分配标志
                    if (NOSKIP[i])              // 跳过被禁用的表
                    {
                        // =================================================================
                        // 第一步：尝试主位置 i
                        // 如果U=0且计数器不饱和，则可以替换
                        // =================================================================
                        if (gtable[i][GI[i]].u == 0)  // U计数器为0，可替换
                        {
                            // =================================================================
                            // 优化替换策略: 优先替换"不重要"的条目
                            // =================================================================
#define OPTREMP
                            // the replacement is optimized with a single u bit: 0.2 %
#ifdef OPTREMP
                            if (abs (2 * gtable[i][GI[i]].ctr + 1) <= 3)  // 计数器不饱和=不重要
#endif
                            {
                                // =================================================================
                                // 在主位置i成功分配
                                // =================================================================
                                gtable[i][GI[i]].tag = GTAG[i];           // 设置标签
                                gtable[i][GI[i]].ctr = (resolveDir) ? 0 : -1; // 初始化计数器
                                NA++;                                     // 已分配数量+1
                                if (T <= 0)                              // 分配完成
                                {
                                    break;
                                }
                                I += 2;                                  // 跳到下一个候选bank对
                                Done = true;                             // 标记已完成
                                T -= 1;                                 // 还需要分配的数量-1
                            }
#ifdef OPTREMP
                            // =================================================================
                            // 如果计数器饱和，执行年龄惩罚: 向错误方向移动计数器
                            // =================================================================
                            else
                            {
                                if (gtable[i][GI[i]].ctr > 0)
                                    gtable[i][GI[i]].ctr--;              // 饱和正计数器减1
                                else
                                    gtable[i][GI[i]].ctr++;              // 饱和负计数器加1
                            }
#endif
                        }
                        else
                        {
                            // =================================================================
                            // U计数器不为0，记录惩罚用于后续U计数器重置
                            // =================================================================
                            Penalty++;                                  // 惩罚计数+1
                        }
                    }

                        // =================================================================
                        // 第二步：如果主位置失败，尝试备选位置 i^1
                        // 体现"组相连"的本质：主位置不行就试备选位置
                        // =================================================================
                        if (!Done)  // 主位置i分配失败
                        {
                            i = (I ^ 1) + 1;  // 计算备选位置: I^1 (在i和i+1之间切换)
                            if (NOSKIP[i])
                            {
                                // =================================================================
                                // 在备选位置尝试相同的分配逻辑
                                // =================================================================
                                if (gtable[i][GI[i]].u == 0)  // 备选位置U计数器为0
                                {
    #ifdef OPTREMP
                                    if (abs (2 * gtable[i][GI[i]].ctr + 1) <= 3)  // 计数器不饱和
    #endif
                                    {
                                        // =================================================================
                                        // 在备选位置i成功分配 - 这是"组相连"的体现！
                                        // =================================================================
                                        gtable[i][GI[i]].tag = GTAG[i];           // 设置标签 (注意用GTAG[i])
                                        gtable[i][GI[i]].ctr = (resolveDir) ? 0 : -1; // 初始化计数器
                                        NA++;                                     // 已分配数量+1
                                        if (T <= 0)
                                        {
                                            break;
                                        }
                                        I += 2;                                  // 跳到下一个候选bank对
                                        T -= 1;                                 // 还需要分配的数量-1
                                    }
    #ifdef OPTREMP
                                    else
                                    {
                                        // 年龄惩罚：计数器饱和则向错误方向移动
                                        if (gtable[i][GI[i]].ctr > 0)
                                            gtable[i][GI[i]].ctr--;
                                        else
                                            gtable[i][GI[i]].ctr++;
                                    }
    #endif
                                }
                                else
                                {
                                    Penalty++;  // 备选位置也不可用，记录惩罚
                                }
                            }
                        }

                }
                // =================================================================
                // 更新TICK计数器: Penalty - 2*NA
                // Penalty: 遍历过程中遇到的U!=0条目数
                // NA: 成功分配的新条目数
                // =================================================================
                TICK += (Penalty - 2 * NA);

                // =================================================================
                // U计数器周期性重置机制
                // =================================================================
                //just the best formula for the Championship:
                //In practice when one out of two entries are useful
                if (TICK < 0)
                    TICK = 0;
                if (TICK >= BORNTICK)                           // BORNTICK=1024
                {
                    // =================================================================
                    // 重置所有U计数器: 右移1位(相当于减半)
                    // =================================================================
                    for (int i = 1; i <= BORN; i += BORN - 1)  // i=1和i=13
                        for (int j = 0; j < SizeTable[i]; j++)
                            gtable[i][j].u >>= 1;               // U计数器减半
                    TICK = 0;                                  // 重置TICK
                }
            }

            // =================================================================
            // 阶段5: 更新预测计数器 - TAGE学习的核心
            // =================================================================
            //update predictions
            if (HitBank > 0)
            {
                // =================================================================
                // 弱计数器保护机制: 当计数器为±1时，避免错误更新
                // =================================================================
                if (abs (2 * gtable[HitBank][GI[HitBank]].ctr + 1) == 1)
                    if (LongestMatchPred != resolveDir)
                    {           // acts as a protection
                        // =================================================================
                        // 更新备选预测而不是命中预测
                        // =================================================================
                        if (AltBank > 0)
                        {
                            ctrupdate (gtable[AltBank][GI[AltBank]].ctr,
                                    resolveDir, CWIDTH);
                        }
                        if (AltBank == 0)
                            baseupdate (resolveDir);              // 更新双模态预测器
                    }

                // =================================================================
                // 更新命中条目的计数器
                // =================================================================
                ctrupdate (gtable[HitBank][GI[HitBank]].ctr, resolveDir, CWIDTH);

                // =================================================================
                // 如果更新后计数器变为±1，重置U计数器
                // =================================================================
                //sign changes: no way it can have been useful
                if (abs (2 * gtable[HitBank][GI[HitBank]].ctr + 1) == 1)
                    gtable[HitBank][GI[HitBank]].u = 0;

                // =================================================================
                // 备选预测正确且饱和时的U计数器惩罚
                // =================================================================
                if (alttaken == resolveDir)
                    if (AltBank > 0)
                        if (abs (2 * gtable[AltBank][GI[AltBank]].ctr + 1) == 7)  // 备选计数器饱和
                            if (gtable[HitBank][GI[HitBank]].u == 1)             // 命中条目U=1
                            {
                                if (LongestMatchPred == resolveDir)             // 但命中预测正确
                                {
                                    gtable[HitBank][GI[HitBank]].u = 0;          // 重置U计数器
                                }
                            }
            }
            else
                // =================================================================
                // 没有命中时，更新双模态预测器
                // =================================================================
                baseupdate (resolveDir);

            // =================================================================
            // 更新U计数器: 最长匹配预测正确时增加U计数器
            // =================================================================
            if (LongestMatchPred != alttaken)                      // 最长匹配与备选不同
                if (LongestMatchPred == resolveDir)               // 最长匹配预测正确
                {
                    if (gtable[HitBank][GI[HitBank]].u < (1 << UWIDTH) - 1)  // U计数器未饱和
                        gtable[HitBank][GI[HitBank]].u++;          // 增加U计数器
                }
            //END TAGE UPDATE
            //HistoryUpdate (PC, brtype, resolveDir, nextPC, phist, ptghist, ch_i, ch_t[0], ch_t[1]);

        }//END PREDICTOR UPDATE

#define GINDEX (((uint64_t) PC) ^ bhist ^ (bhist >> (8 - i)) ^ (bhist >> (16 - 2 * i)) ^ (bhist >> (24 - 3 * i)) ^ (bhist >> (32 - 3 * i)) ^ (bhist >> (40 - 4 * i))) & ((1 << (logs - (i >= (NBR - 2)))) - 1)
        int Gpredict (UINT64 PC, uint64_t BHIST, int *length, int8_t ** tab, int NBR, int logs, int8_t * W)
        {
            int PERCSUM = 0;
            for (int i = 0; i < NBR; i++)
            {
                uint64_t bhist = BHIST & ((uint64_t) ((1ULL << length[i]) - 1));
                uint64_t index = GINDEX;

                int8_t ctr = tab[i][index];

                PERCSUM += (2 * ctr + 1);
            }
#ifdef VARTHRES
            PERCSUM = (1 + (W[INDUPDS] >= 0)) * PERCSUM;
#endif
            return ((PERCSUM));
        }
        void Gupdate (UINT64 PC, bool taken, uint64_t BHIST, int *length,
                int8_t ** tab, int NBR, int logs, int8_t * W)
        {

            int PERCSUM = 0;

            for (int i = 0; i < NBR; i++)
            {
                uint64_t bhist = BHIST & ((uint64_t) ((1ULL << length[i]) - 1));
                uint64_t index = GINDEX;

                PERCSUM += (2 * tab[i][index] + 1);
                ctrupdate (tab[i][index], taken, PERCWIDTH);
            }
#ifdef VARTHRES
            {
                int XSUM = LSUM - ((W[INDUPDS] >= 0)) * PERCSUM;
                if ((XSUM + PERCSUM >= 0) != (XSUM >= 0))
                    ctrupdate (W[INDUPDS], ((PERCSUM >= 0) == taken), EWIDTH);
            }
#endif
        }



#ifdef LOOPPREDICTOR
        int lindex (UINT64 PC)
        {
            return (((PC ^ (PC >> 2)) & ((1 << (LOGL - 2)) - 1)) << 2);
        }


        //loop prediction: only used if high confidence
        //skewed associative 4-way
        //At fetch time: speculative
#define CONFLOOP 15
        bool getloop (UINT64 PC, const cbp_hist_t& hist_to_use)
        {
            const auto& ltable = hist_to_use.ltable;
            LHIT = -1;

            LI = lindex (PC);
            LIB = ((PC >> (LOGL - 2)) & ((1 << (LOGL - 2)) - 1));
            LTAG = (PC >> (LOGL - 2)) & ((1 << 2 * LOOPTAG) - 1);
            LTAG ^= (LTAG >> LOOPTAG);
            LTAG = (LTAG & ((1 << LOOPTAG) - 1));

            for (int i = 0; i < 4; i++)
            {
                int index = (LI ^ ((LIB >> i) << 2)) + i;

                if (ltable[index].TAG == LTAG)
                {
                    LHIT = i;
                    LVALID = ((ltable[index].confid == CONFLOOP)
                            || (ltable[index].confid * ltable[index].NbIter > 128));


                    if (ltable[index].CurrentIter + 1 == ltable[index].NbIter)
                        return (!(ltable[index].dir));
                    return ((ltable[index].dir));

                }
            }

            LVALID = false;
            return (false);
        }



        void loopupdate (UINT64 PC, bool Taken, bool ALLOC, std::vector<lentry>& ltable)
        {
            if (LHIT >= 0)
            {
                int index = (LI ^ ((LIB >> LHIT) << 2)) + LHIT;
                //already a hit 
                if (LVALID)
                {
                    if (Taken != predloop)
                    {
                        // free the entry
                        ltable[index].NbIter = 0;
                        ltable[index].age = 0;
                        ltable[index].confid = 0;
                        ltable[index].CurrentIter = 0;
                        return;

                    }
                    else if ((predloop != tage_pred) || ((MYRANDOM () & 7) == 0))
                        if (ltable[index].age < CONFLOOP)
                            ltable[index].age++;
                }

                ltable[index].CurrentIter++;
                ltable[index].CurrentIter &= ((1 << WIDTHNBITERLOOP) - 1);
                //loop with more than 2** WIDTHNBITERLOOP iterations are not treated correctly; but who cares :-)
                if (ltable[index].CurrentIter > ltable[index].NbIter)
                {
                    ltable[index].confid = 0;
                    ltable[index].NbIter = 0;
                    //treat like the 1st encounter of the loop 
                }
                if (Taken != ltable[index].dir)
                {
                    if (ltable[index].CurrentIter == ltable[index].NbIter)
                    {
                        if (ltable[index].confid < CONFLOOP)
                            ltable[index].confid++;
                        if (ltable[index].NbIter < 3)
                            //just do not predict when the loop count is 1 or 2     
                        {
                            // free the entry
                            ltable[index].dir = Taken;
                            ltable[index].NbIter = 0;
                            ltable[index].age = 0;
                            ltable[index].confid = 0;
                        }
                    }
                    else
                    {
                        if (ltable[index].NbIter == 0)
                        {
                            // first complete nest;
                            ltable[index].confid = 0;
                            ltable[index].NbIter = ltable[index].CurrentIter;
                        }
                        else
                        {
                            //not the same number of iterations as last time: free the entry
                            ltable[index].NbIter = 0;
                            ltable[index].confid = 0;
                        }
                    }
                    ltable[index].CurrentIter = 0;
                }

            }
            else if (ALLOC)

            {
                UINT64 X = MYRANDOM () & 3;

                if ((MYRANDOM () & 3) == 0)
                    for (int i = 0; i < 4; i++)
                    {
                        int loop_hit_way_loc = (X + i) & 3;
                        int index = (LI ^ ((LIB >> loop_hit_way_loc) << 2)) + loop_hit_way_loc;
                        if (ltable[index].age == 0)
                        {
                            ltable[index].dir = !Taken;
                            // most of mispredictions are on last iterations
                            ltable[index].TAG = LTAG;
                            ltable[index].NbIter = 0;
                            ltable[index].age = 7;
                            ltable[index].confid = 0;
                            ltable[index].CurrentIter = 0;
                            break;

                        }
                        else
                            ltable[index].age--;
                        break;
                    }
            }
        }
#endif    // LOOPPREDICTOR
};
// =================
// Predictor End
// =================


#undef UINT64
} // namespace cbp_pred
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5


#endif  // _TAGE_PREDICTOR_H_



