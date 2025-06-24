# SPEC 06 17 热点块

## b数据来源
编译器及选项：`GCC 14.2.0 (Debian-12) -fpie -pie -march=rv64gcv_zba_zbb_zbc_zbs_zicond -O3 -static`

Workload elf：大机房 `/nfs/home/chenyangyu/spec-workload-characterazation/spec06-bbs-bvzicond` `/nfs/home/chenyangyu/spec-workload-characterazation/spec17-bbs-bvzicond`

**可以对着后面的分析去该路径看对应的汇编和dwarf，知道workload本质在算什么。**

数据来源：大机房 `/nfs/home/chenyangyu/spec-workload-characterazation`，使用 [https://github.com/cyyself/pybinutils/blob/master/src/dump_all_hot_bb.py](https://github.com/cyyself/pybinutils/blob/master/src/dump_all_hot_bb.py) 配合以下脚本结合带 PMU 的 VLEN=256 真机 linux perf生成：

```shell
#!/usr/bin/env bash

SPECINT_R=(500.perlbench_r 502.gcc_r 505.mcf_r 520.omnetpp_r 523.xalancbmk_r 525.x264_r 531.deepsjeng_r 541.leela_r 548.exchange2_r 557.xz_r)
SPECFP_R=(503.bwaves_r 507.cactuBSSN_r 508.namd_r 510.parest_r 511.povray_r 519.lbm_r 521.wrf_r 526.blender_r 527.cam4_r 538.imagick_r 544.nab_r 549.fotonik3d_r 554.roms_r)

for i in "${SPECINT_R[@]}"
do
    ./src/dump_all_hot_bb.py -p /spec_run/CPU2017LiteWrapper/$i/perf-rv64gc-o3-gcc14.2-12-dbg-static.data -e r4d:u -t 0 -l 0.05 -c 0.8 -n 6 -m 4 -o spec17-bbs/$i
done

for i in "${SPECFP_R[@]}"
do
    ./src/dump_all_hot_bb.py -p /spec_run/CPU2017LiteWrapper/$i/perf-rv64gc-o3-gcc14.2-12-dbg-static.data -e r4d:u -t 0 -l 0.05 -c 0.8 -n 6 -m 4 -o spec17-bbs/$i
done
```

然后使用 [https://github.com/cyyself/pybinutils/blob/master/src/export_stat.py](https://github.com/cyyself/pybinutils/blob/master/src/export_stat.py) 对结果进行汇总：

```shell
./src/export_stat.py -d spec06-bbs-bvzicond
```

## SPEC 06
### 原始数据
（建议跳过直接看值得关注的点）

```shell
├── 400.perlbench
│   ├── S_regmatch(f): 35.98%
│   │   ├── 0x8687c(b): 13.65%
│   │   ├── 0x8689c(b): 7.90%
│   │   ├── 0x86996(b): 6.86%
│   │   ├── 0x86820(b): 5.83%
│   ├── Perl_leave_scope(f): 3.91%
│   │   ├── 0x91e0a(b): 15.52%
│   │   ├── 0x91dc8(b): 12.44%
│   │   ├── 0x91e4e(b): 11.95%
│   │   ├── 0x91dde(b): 11.74%
│   ├── Perl_sv_setsv_flags(f): 3.09%
│   │   ├── 0xa1306(b): 13.27%
│   │   ├── 0xa134e(b): 9.84%
│   │   ├── 0xa1498(b): 9.29%
│   │   ├── 0xa16f8(b): 9.04%
│   ├── S_regtry(f): 3.00%
│   │   ├── 0x8a67c(b): 41.04%
│   │   ├── 0x8a646(b): 13.87%
│   │   ├── 0x8a762(b): 11.36%
│   │   ├── 0x8a742(b): 8.13%
│   ├── Perl_runops_standard(f): 2.36%
│   │   ├── 0x8fe98(b): 59.96%
│   │   ├── 0x8fe9c(b): 22.04%
│   │   ├── 0x8fe94(b): 17.73%
│   ├── Perl_save_alloc(f): 2.16%
│   │   ├── 0x91d9a(b): 50.22%
│   │   ├── 0x91d1a(b): 49.78%
├── 401.bzip2
│   ├── mainSort(f): 26.85%
│   │   ├── 0x12d7e(b): 13.57%
│   │   ├── 0x12da8(b): 6.74%
│   │   ├── 0x12372(b): 6.54%
│   │   ├── 0x12160(b): 5.13%
│   ├── BZ2_compressBlock(f): 23.45%
│   │   ├── 0x16be6(b): 40.95%
│   │   ├── 0x17840(b): 26.52%
│   │   ├── 0x186bc(b): 8.30%
│   ├── mainGtU.part.0(f): 17.28%
│   │   ├── 0x11efe(b): 5.32%
│   │   ├── 0x11dd8(b): 5.27%
│   │   ├── 0x11f12(b): 5.14%
│   ├── BZ2_decompress(f): 14.73%
│   │   ├── 0x1d82c(b): 15.24%
│   │   ├── 0x1ceb4(b): 5.06%
├── 403.gcc
│   ├── memset(f): 6.83%
│   │   ├── 0x2543fa(b): 57.92%
│   │   ├── 0x2543c0(b): 16.86%
│   │   ├── 0x254422(b): 7.54%
│   ├── htab_traverse(f): 3.39%
│   │   ├── 0x241684(b): 95.44%
│   ├── for_each_rtx(f): 3.35%
│   │   ├── 0x1e9114(b): 17.02%
│   │   ├── 0x1e916a(b): 13.11%
│   │   ├── 0x1e919a(b): 12.93%
│   │   ├── 0x1e913e(b): 9.83%
│   ├── ggc_set_mark(f): 2.94%
│   │   ├── 0x229a2c(b): 68.00%
│   │   ├── 0x229a0a(b): 19.68%
│   │   ├── 0x229a7e(b): 10.04%
│   ├── mark_set_1(f): 2.91%
│   │   ├── 0xced72(b): 17.30%
│   │   ├── 0xcee3a(b): 8.71%
│   │   ├── 0xcf102(b): 6.28%
│   │   ├── 0xcf1bc(b): 6.08%
│   ├── bitmap_operation(f): 2.85%
│   │   ├── 0x4b0f4(b): 15.67%
│   │   ├── 0x4b08e(b): 13.85%
│   │   ├── 0x4b0aa(b): 10.23%
│   │   ├── 0x4b156(b): 6.47%
├── 429.mcf
│   ├── price_out_impl(f): 31.59%
│   │   ├── 0x1111e(b): 40.87%
│   │   ├── 0x1112a(b): 24.92%
│   │   ├── 0x11138(b): 13.47%
│   │   ├── 0x1113a(b): 7.40%
│   ├── replace_weaker_arc(f): 26.51%
│   │   ├── 0x11026(b): 52.67%
│   │   ├── 0x11066(b): 21.82%
│   │   ├── 0x10ffc(b): 18.69%
│   ├── primal_bea_mpp(f): 20.08%
│   │   ├── 0x11a98(b): 48.92%
│   │   ├── 0x11a9e(b): 27.52%
│   │   ├── 0x11a2e(b): 6.69%
│   │   ├── 0x11abe(b): 6.61%
│   ├── sort_basket(f): 10.20%
│   │   ├── 0x11978(b): 20.27%
│   │   ├── 0x119b8(b): 16.68%
│   │   ├── 0x11984(b): 13.80%
│   │   ├── 0x11990(b): 11.97%
├── 445.gobmk
│   ├── do_play_move(f): 9.72%
│   │   ├── 0x17674(b): 6.97%
│   │   ├── 0x17704(b): 5.05%
│   ├── fastlib(f): 5.56%
│   │   ├── 0x1b7f2(b): 17.13%
│   │   ├── 0x1b782(b): 12.47%
│   │   ├── 0x1b946(b): 7.25%
│   │   ├── 0x1b826(b): 5.69%
│   ├── popgo(f): 4.73%
│   │   ├── 0x198a4(b): 64.58%
│   │   ├── 0x1985a(b): 8.25%
│   │   ├── 0x198da(b): 7.67%
│   │   ├── 0x198b4(b): 7.33%
│   ├── incremental_order_moves(f): 4.71%
│   │   ├── 0x1fe58(b): 9.95%
│   ├── order_moves(f): 4.66%
│   │   ├── 0x51e5a(b): 8.92%
│   │   ├── 0x52084(b): 8.29%
│   │   ├── 0x51d8a(b): 6.20%
│   ├── do_dfa_matchpat(f): 4.29%
│   │   ├── 0x34bc2(b): 37.57%
│   │   ├── 0x34b86(b): 23.65%
├── 456.hmmer
│   ├── P7Viterbi(f): 92.46%
│   │   ├── 0x1cc7a(b): 42.83%
│   │   ├── 0x1cae4(b): 16.90%
│   │   ├── 0x1cc0e(b): 10.71%
│   │   ├── 0x1c578(b): 9.84%
├── 458.sjeng
│   ├── std_eval(f): 24.10%
│   │   ├── 0x1de96(b): 13.87%
│   │   ├── 0x1de68(b): 13.35%
│   │   ├── 0x1ddbe(b): 7.99%
│   │   ├── 0x1dec2(b): 7.50%
│   ├── gen(f): 9.60%
│   │   ├── 0x1c246(b): 8.28%
│   │   ├── 0x1bd4e(b): 7.47%
│   │   ├── 0x1c234(b): 5.61%
│   │   ├── 0x1bd3c(b): 5.11%
│   ├── search(f): 8.32%
│   │   ├── 0x2358c(b): 22.16%
│   │   ├── 0x235ca(b): 10.40%
│   │   ├── 0x23580(b): 7.71%
│   │   ├── 0x23588(b): 6.37%
│   ├── Pawn(f): 6.13%
│   │   ├── 0x1da2a(b): 57.87%
│   │   ├── 0x1da9e(b): 18.56%
│   │   ├── 0x1dab4(b): 12.33%
│   ├── setup_attackers(f): 6.02%
│   │   ├── 0x262cc(b): 10.04%
│   │   ├── 0x264ee(b): 8.53%
│   │   ├── 0x26102(b): 7.00%
│   │   ├── 0x2615c(b): 5.30%
│   ├── make(f): 5.32%
│   │   ├── 0x1ad1e(b): 30.21%
│   │   ├── 0x1adc8(b): 14.80%
│   │   ├── 0x1b190(b): 9.78%
│   │   ├── 0x1b1b8(b): 9.43%
├── 462.libquantum
│   ├── quantum_toffoli(f): 47.37%
│   │   ├── 0x11052(b): 46.03%
│   │   ├── 0x11064(b): 43.68%
│   │   ├── 0x11082(b): 7.26%
│   ├── quantum_sigma_x(f): 29.97%
│   │   ├── 0x111e0(b): 99.97%
│   ├── quantum_cnot(f): 14.54%
│   │   ├── 0x10fa2(b): 54.68%
│   │   ├── 0x10fc4(b): 31.59%
│   │   ├── 0x10fd8(b): 7.15%
│   │   ├── 0x10fb2(b): 6.58%
├── 464.h264ref
│   ├── SetupFastFullPelSearch(f): 48.96%
│   │   ├── 0x5c308(b): 83.90%
│   │   ├── 0x5c4c8(b): 6.71%
│   ├── FastFullPelBlockMotionSearch(f): 9.03%
│   │   ├── 0x5cea8(b): 52.67%
│   │   ├── 0x5ce64(b): 30.22%
│   │   ├── 0x5ce5a(b): 15.64%
│   ├── FastPelY_14(f): 6.50%
│   │   ├── 0x92a88(b): 100.00%
│   ├── SATD.part.0(f): 5.85%
│   │   ├── 0x5436c(b): 100.00%
├── 471.omnetpp
│   ├── _ZN12cMessageHeap8getFirstEv(f): 14.70%
│   │   ├── 0x5bb3e(b): 25.43%
│   │   ├── 0x5bb82(b): 16.44%
│   │   ├── 0x5bb66(b): 10.38%
│   │   ├── 0x5bb1a(b): 7.86%
│   ├── _ZN12cMessageHeap6insertEP8cMessage(f): 5.58%
│   │   ├── 0x5b9b2(b): 30.20%
│   │   ├── 0x5b9a2(b): 16.13%
│   │   ├── 0x5b8e2(b): 15.51%
│   │   ├── 0x5b986(b): 8.78%
│   ├── _ZN7cObject8setOwnerEPS_(f): 5.25%
│   │   ├── 0x647fe(b): 18.55%
│   │   ├── 0x64820(b): 14.98%
│   │   ├── 0x64806(b): 13.18%
│   │   ├── 0x64814(b): 12.68%
│   ├── strcmp(f): 4.50%
│   │   ├── 0x11b974(b): 25.08%
│   │   ├── 0x11bae6(b): 17.05%
│   │   ├── 0x11b982(b): 13.46%
│   │   ├── 0x11b98a(b): 12.61%
│   ├── _int_malloc(f): 3.72%
│   │   ├── 0x118886(b): 11.81%
│   │   ├── 0x1186f4(b): 6.30%
│   ├── _int_free(f): 3.39%
│   │   ├── 0x117a90(b): 18.86%
│   │   ├── 0x117aa4(b): 12.45%
│   │   ├── 0x117a84(b): 11.28%
│   │   ├── 0x117bf6(b): 8.00%
├── 473.astar
│   ├── _ZN6wayobj10makebound2EPiiS0_(f): 41.45%
│   │   ├── 0x16fb2(b): 10.00%
│   │   ├── 0x16e48(b): 8.93%
│   │   ├── 0x16f22(b): 7.54%
│   │   ├── 0x17042(b): 5.02%
│   ├── _ZN7way2obj12releasepointEii(f): 36.29%
│   │   ├── 0x145ec(b): 15.83%
│   │   ├── 0x146f0(b): 14.32%
│   │   ├── 0x1469c(b): 7.73%
│   │   ├── 0x14612(b): 7.03%
│   ├── _ZN9regwayobj10makebound2ER9flexarrayIP6regobjES4_(f): 11.32%
│   │   ├── 0x169f0(b): 31.41%
│   │   ├── 0x169dc(b): 22.03%
│   │   ├── 0x16a30(b): 12.94%
│   │   ├── 0x169c6(b): 8.95%
├── 483.xalancbmk
│   ├── _ZN10xalanc_1_814VariablesStack9findEntryERKNS_10XalanQNameEbb(f): 6.68%
│   │   ├── 0x1dddba(b): 21.31%
│   │   ├── 0x1dde44(b): 10.35%
│   │   ├── 0x1dde8a(b): 9.55%
│   │   ├── 0x1ddeb0(b): 9.24%
│   ├── _ZN10xalanc_1_814XalanDOMString6equalsERKS0_S2_(f): 6.55%
│   │   ├── 0x1f916c(b): 46.81%
│   │   ├── 0x1f9160(b): 15.67%
│   │   ├── 0x1f9132(b): 14.28%
│   │   ├── 0x1f914e(b): 9.28%
│   ├── _ZNK10xalanc_1_85XPath11executeMoreEPNS_9XalanNodeEN9__gnu_cxx17__normal_iteratorIPKiSt6vectorIiSaIiEEEERNS_21XPathExecutionContextE(f): 5.40%
│   │   ├── 0x17e4fe(b): 26.82%
│   │   ├── 0x17e51e(b): 25.87%
│   │   ├── 0x17e548(b): 17.03%
│   │   ├── 0x17e9f8(b): 9.73%
│   ├── _ZN10xalanc_1_819XalanDOMStringCache7releaseERNS_14XalanDOMStringE(f): 4.01%
│   │   ├── 0x1519da(b): 21.22%
│   │   ├── 0x1519c6(b): 18.85%
│   │   ├── 0x1519c0(b): 12.54%
│   │   ├── 0x1519d2(b): 9.78%
│   ├── _ZNK10xalanc_1_811XalanBitmap5isSetEm(f): 3.90%
│   │   ├── 0x151622(b): 78.30%
│   │   ├── 0x15161c(b): 21.70%
│   ├── _ZN10xalanc_1_814VariablesStack11findXObjectERKNS_10XalanQNameERNS_26StylesheetExecutionContextEbbRb(f): 3.57%
│   │   ├── 0x1de080(b): 37.28%
│   │   ├── 0x1de0f8(b): 22.62%
│   │   ├── 0x1de0ac(b): 19.91%
│   │   ├── 0x1de0ca(b): 10.85%
├── 410.bwaves
│   ├── mat_times_vec_(f): 79.89%
│   │   ├── 0x10790(b): 93.19%
│   ├── bi_cgstab_block_(f): 8.96%
│   │   ├── 0x10f1c(b): 19.11%
│   │   ├── 0x11234(b): 14.73%
│   │   ├── 0x10ddc(b): 12.87%
│   │   ├── 0x11114(b): 11.06%
├── 416.gamess
│   ├── forms_(f): 44.93%
│   │   ├── 0x331de8(b): 37.99%
│   │   ├── 0x331f88(b): 28.25%
│   │   ├── 0x331c6c(b): 19.06%
│   │   ├── 0x33215e(b): 6.37%
│   ├── dirfck_(f): 33.55%
│   │   ├── 0x5aba9a(b): 36.03%
│   │   ├── 0x5abf6a(b): 11.59%
│   │   ├── 0x5ab92c(b): 7.61%
│   │   ├── 0x5ab982(b): 7.36%
│   ├── xyzint_(f): 12.05%
│   │   ├── 0x3340fc(b): 9.38%
│   │   ├── 0x33347a(b): 7.87%
│   │   ├── 0x334234(b): 7.43%
│   │   ├── 0x333864(b): 6.58%
├── 433.milc
│   ├── mult_su3_nn(f): 17.63%
│   │   ├── 0x2252a(b): 92.86%
│   ├── mult_su3_na(f): 15.41%
│   │   ├── 0x21f7c(b): 90.71%
│   ├── scalar_mult_add_su3_matrix(f): 12.10%
│   │   ├── 0x23078(b): 94.68%
│   ├── mult_adj_su3_mat_vec(f): 9.76%
│   │   ├── 0x21bda(b): 93.08%
│   │   ├── 0x21bba(b): 5.19%
│   ├── su3_projector(f): 6.46%
│   │   ├── 0x23698(b): 90.16%
│   │   ├── 0x2367a(b): 5.03%
│   ├── mult_su3_an(f): 5.30%
│   │   ├── 0x21e36(b): 99.99%
├── 434.zeusmp
│   ├── momx3_(f): 10.91%
│   │   ├── 0x324f6(b): 15.79%
│   │   ├── 0x31db6(b): 14.37%
│   │   ├── 0x31662(b): 13.16%
│   │   ├── 0x31b70(b): 11.31%
│   ├── momx2_(f): 7.90%
│   │   ├── 0x2f552(b): 18.19%
│   │   ├── 0x2ee8a(b): 16.92%
│   │   ├── 0x2fc76(b): 16.71%
│   │   ├── 0x2f34a(b): 10.79%
│   ├── tranx3_(f): 7.42%
│   │   ├── 0x474e2(b): 37.35%
│   │   ├── 0x477d6(b): 34.71%
│   │   ├── 0x47d14(b): 8.04%
│   │   ├── 0x47eea(b): 5.53%
│   ├── tranx2_(f): 4.98%
│   │   ├── 0x42e6c(b): 41.46%
│   │   ├── 0x42b7c(b): 35.95%
│   │   ├── 0x4341c(b): 13.75%
├── 435.gromacs
│   ├── inl1130_(f): 67.23%
│   │   ├── 0x15e6c(b): 98.37%
│   ├── search_neighbours(f): 10.98%
│   │   ├── 0x742a8(b): 27.44%
│   │   ├── 0x74334(b): 6.87%
│   │   ├── 0x742da(b): 6.84%
│   │   ├── 0x743be(b): 6.69%
│   ├── inl1100_(f): 2.54%
│   │   ├── 0x1510a(b): 97.71%
├── 436.cactusADM
│   ├── bench_staggeredleapfrog2_(f): 99.23%
│   │   ├── 0x18f54(b): 97.62%
├── 437.leslie3d
│   ├── fluxk_(f): 20.22%
│   │   ├── 0x1a784(b): 22.65%
│   │   ├── 0x1a4f4(b): 13.87%
│   │   ├── 0x1aa34(b): 12.38%
│   │   ├── 0x1a0e2(b): 12.14%
│   ├── fluxj_(f): 16.64%
│   │   ├── 0x1d2fc(b): 21.94%
│   │   ├── 0x1d54c(b): 14.98%
│   │   ├── 0x1cf26(b): 12.89%
│   │   ├── 0x1cc76(b): 12.28%
│   ├── fluxi_(f): 16.21%
│   │   ├── 0x1f450(b): 22.12%
│   │   ├── 0x1f5d4(b): 14.94%
│   │   ├── 0x1efae(b): 13.41%
│   │   ├── 0x1eea4(b): 12.72%
│   ├── extrapj_(f): 11.86%
│   │   ├── 0x1b260(b): 51.74%
│   │   ├── 0x1b650(b): 23.52%
│   │   ├── 0x1b40a(b): 7.22%
│   │   ├── 0x1b360(b): 7.20%
│   ├── extrapi_(f): 11.31%
│   │   ├── 0x1ddbe(b): 51.60%
│   │   ├── 0x1e12c(b): 24.12%
│   │   ├── 0x1df34(b): 7.63%
│   │   ├── 0x1dfc4(b): 7.53%
│   ├── setbc_(f): 8.03%
│   │   ├── 0x14baa(b): 23.10%
│   │   ├── 0x16096(b): 22.75%
│   │   ├── 0x151e0(b): 10.66%
│   │   ├── 0x13d78(b): 10.55%
├── 444.namd
│   ├── _ZN20ComputeNonbondedUtil26calc_pair_energy_fullelectEP9nonbonded(f): 13.34%
│   │   ├── 0x147f6(b): 53.08%
│   │   ├── 0x1453e(b): 18.07%
│   │   ├── 0x1468c(b): 9.54%
│   │   ├── 0x14574(b): 9.53%
│   ├── _ZN20ComputeNonbondedUtil19calc_pair_fullelectEP9nonbonded(f): 11.75%
│   │   ├── 0x13792(b): 49.28%
│   │   ├── 0x134ee(b): 20.57%
│   │   ├── 0x13636(b): 10.30%
│   │   ├── 0x13524(b): 8.74%
│   ├── _ZN20ComputeNonbondedUtil16calc_pair_energyEP9nonbonded(f): 10.88%
│   │   ├── 0x1299e(b): 42.90%
│   │   ├── 0x1270c(b): 22.34%
│   │   ├── 0x1274a(b): 11.60%
│   │   ├── 0x12854(b): 11.45%
│   ├── _ZN20ComputeNonbondedUtil32calc_pair_energy_merge_fullelectEP9nonbonded(f): 10.67%
│   │   ├── 0x1661a(b): 43.96%
│   │   ├── 0x16382(b): 23.23%
│   │   ├── 0x164c8(b): 10.89%
│   │   ├── 0x163b8(b): 9.73%
│   ├── _ZN20ComputeNonbondedUtil25calc_pair_merge_fullelectEP9nonbonded(f): 10.15%
│   │   ├── 0x1585c(b): 38.45%
│   │   ├── 0x155d4(b): 24.52%
│   │   ├── 0x1560a(b): 12.12%
│   │   ├── 0x1571c(b): 11.90%
│   ├── _ZN20ComputeNonbondedUtil9calc_pairEP9nonbonded(f): 10.13%
│   │   ├── 0x11d00(b): 38.86%
│   │   ├── 0x11a7e(b): 24.16%
│   │   ├── 0x11bc6(b): 12.34%
│   │   ├── 0x11abc(b): 12.05%
├── 447.dealII
│   ├── _ZNK9MappingQ1ILi3EE12compute_fillERK12TriaIteratorILi3E15DoFCellAccessorILi3EEEjN10QProjectorILi3EE17DataSetDescriptorERNS0_12InternalDataERSt6vectorI5PointILi3EESaISE_EE(f): 33.51%
│   │   ├── 0x1165a6(b): 84.29%
│   ├── _ZN13LaplaceSolver6SolverILi3EE22assemble_linear_systemERNS1_12LinearSystemE(f): 14.13%
│   │   ├── 0x18013e(b): 35.37%
│   │   ├── 0x18018a(b): 28.37%
│   │   ├── 0x1801b4(b): 14.16%
│   │   ├── 0x1801d8(b): 9.34%
│   ├── _ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base(f): 7.14%
│   │   ├── 0x1de02e(b): 28.20%
│   │   ├── 0x1de028(b): 24.47%
│   │   ├── 0x1de036(b): 17.41%
│   │   ├── 0x1de03e(b): 13.41%
│   ├── _ZNK10FullMatrixIdE6TmmultIdEEvRS_IT_ERKS3_b(f): 5.12%
│   │   ├── 0xde6a4(b): 96.56%
│   ├── _ZNK8MappingQILi3EE19transform_covariantEP6TensorILi1ELi3EES3_PKS2_RKN7MappingILi3EE16InternalDataBaseE(f): 4.81%
│   │   ├── 0x11193c(b): 95.99%
│   ├── _ZNK17FiniteElementBaseILi3EE11compute_2ndERK7MappingILi3EERK12TriaIteratorILi3E15DoFCellAccessorILi3EEEjRNS2_16InternalDataBaseERNS0_16InternalDataBaseER12FEValuesDataILi3EE(f): 2.43%
│   │   ├── 0xae8a0(b): 31.41%
│   │   ├── 0xae668(b): 26.36%
│   │   ├── 0xae6ee(b): 9.71%
│   │   ├── 0xae6b2(b): 8.32%
├── 450.soplex
│   ├── _ZN6soplex10SPxSteepPR8entered4ENS_5SPxIdEi(f): 15.11%
│   │   ├── 0x47eb4(b): 53.33%
│   │   ├── 0x47e6a(b): 20.61%
│   │   ├── 0x47ea8(b): 7.14%
│   │   ├── 0x47ecc(b): 6.65%
│   ├── _ZN6soplex8SSVector5setupEv(f): 9.24%
│   │   ├── 0x4d33a(b): 40.93%
│   │   ├── 0x4d330(b): 32.45%
│   │   ├── 0x4d34c(b): 16.18%
│   │   ├── 0x4d354(b): 10.44%
│   ├── _ZN6soplex6SoPlex10updateTestEv(f): 5.60%
│   │   ├── 0x1574e(b): 51.78%
│   │   ├── 0x15812(b): 19.79%
│   │   ├── 0x15784(b): 16.06%
│   │   ├── 0x1577e(b): 5.22%
│   ├── _ZN6soplex10SPxSteepPR11selectEnterEv(f): 5.28%
│   │   ├── 0x47ad6(b): 56.07%
│   │   ├── 0x47ae2(b): 23.25%
│   │   ├── 0x47af2(b): 19.34%
│   ├── _ZN6soplex9CLUFactor16initFactorMatrixEPPNS_7SVectorEd(f): 5.25%
│   │   ├── 0x1cafa(b): 17.40%
│   │   ├── 0x1ca28(b): 14.68%
│   │   ├── 0x1cbf2(b): 11.69%
│   │   ├── 0x1cb96(b): 6.47%
│   ├── _ZN6soplex8SSVector20assign2product4setupERKNS_5SVSetERKS0_(f): 5.00%
│   │   ├── 0x4e0c8(b): 96.87%
├── 453.povray
│   ├── _ZN3povL23All_Plane_IntersectionsEPNS_13Object_StructEPNS_10Ray_StructEPNS_13istack_structE(f): 15.86%
│   │   ├── 0x7b5cc(b): 27.30%
│   │   ├── 0x7b652(b): 16.75%
│   │   ├── 0x7b4da(b): 16.31%
│   │   ├── 0x7b67e(b): 9.88%
│   ├── _ZN3povL24All_Sphere_IntersectionsEPNS_13Object_StructEPNS_10Ray_StructEPNS_13istack_structE(f): 12.99%
│   │   ├── 0x9ec0e(b): 57.80%
│   │   ├── 0x9ecd0(b): 17.50%
│   │   ├── 0x9ec8a(b): 15.32%
│   │   ├── 0x9ec7c(b): 7.60%
│   ├── _ZN3povL31All_CSG_Intersect_IntersectionsEPNS_13Object_StructEPNS_10Ray_StructEPNS_13istack_structE(f): 11.63%
│   │   ├── 0x20f8a(b): 22.06%
│   │   ├── 0x20f80(b): 10.84%
│   │   ├── 0x20f9e(b): 9.44%
│   │   ├── 0x20f3a(b): 9.16%
│   ├── _ZN3pov17Check_And_EnqueueEPNS_21Priority_Queue_StructEPNS_16BBox_Tree_StructEPNS_19Bounding_Box_StructEPNS_14Rayinfo_StructE(f): 9.09%
│   │   ├── 0x13858(b): 13.62%
│   │   ├── 0x13980(b): 8.22%
│   │   ├── 0x13a2c(b): 7.68%
│   │   ├── 0x13a44(b): 6.98%
│   ├── _ZN3povL12Inside_PlaneEPdPNS_13Object_StructE(f): 5.24%
│   │   ├── 0x7b4a6(b): 41.13%
│   │   ├── 0x7b4c0(b): 35.86%
│   │   ├── 0x7b47a(b): 23.01%
│   ├── _ZN3pov13Inside_ObjectEPdPNS_13Object_StructE(f): 4.52%
│   │   ├── 0x5a248(b): 41.08%
│   │   ├── 0x5a23c(b): 30.15%
│   │   ├── 0x5a114(b): 28.78%
├── 454.calculix
│   ├── insert(f): 9.93%
│   │   ├── 0xa4632(b): 54.03%
│   │   ├── 0xa462e(b): 32.83%
│   ├── e_c3d_(f): 9.85%
│   │   ├── 0x3758c(b): 65.98%
│   │   ├── 0x374fe(b): 10.16%
│   │   ├── 0x374e8(b): 8.26%
│   ├── DVdot33(f): 8.60%
│   │   ├── 0x11c6b0(b): 94.49%
│   ├── isortii_(f): 8.24%
│   │   ├── 0x52a8a(b): 17.50%
│   │   ├── 0x52aa0(b): 15.32%
│   │   ├── 0x52abe(b): 14.69%
│   │   ├── 0x52a86(b): 8.67%
│   ├── nident_(f): 3.29%
│   │   ├── 0x5d5a6(b): 61.16%
│   │   ├── 0x5d5d8(b): 10.85%
│   │   ├── 0x5d59a(b): 9.14%
│   │   ├── 0x5d5d0(b): 7.90%
│   ├── IV2DVqsortUp(f): 3.22%
│   │   ├── 0x125892(b): 22.72%
│   │   ├── 0x1258c0(b): 14.46%
│   │   ├── 0x125878(b): 10.72%
│   │   ├── 0x125884(b): 8.70%
├── 459.GemsFDTD
│   ├── __nft_mod_MOD_nft_store(f): 22.70%
│   │   ├── 0x285a6(b): 18.89%
│   │   ├── 0x2824a(b): 18.82%
│   │   ├── 0x27c50(b): 15.72%
│   │   ├── 0x2793c(b): 15.55%
│   ├── __update_mod_MOD_updateh_homo(f): 21.75%
│   │   ├── 0x51710(b): 99.16%
│   ├── __update_mod_MOD_updatee_homo(f): 21.27%
│   │   ├── 0x51356(b): 98.36%
│   ├── __upml_mod_MOD_upmlupdatee(f): 13.12%
│   │   ├── 0x4319e(b): 13.95%
│   │   ├── 0x41d3a(b): 13.91%
│   │   ├── 0x450e6(b): 12.61%
│   │   ├── 0x46634(b): 11.73%
│   ├── __upml_mod_MOD_upmlupdateh(f): 12.69%
│   │   ├── 0x485f0(b): 14.65%
│   │   ├── 0x48e66(b): 14.47%
│   │   ├── 0x49e4e(b): 13.25%
│   │   ├── 0x493e8(b): 13.23%
├── 465.tonto
│   ├── __shell2_module_MOD_make_ft_1(f): 12.22%
│   │   ├── 0x16a240(b): 11.05%
│   │   ├── 0x16ad28(b): 10.42%
│   │   ├── 0x168178(b): 6.97%
│   ├── __sincos(f): 11.20%
│   │   ├── 0x35daf2(b): 21.19%
│   │   ├── 0x35db7a(b): 17.94%
│   │   ├── 0x35d8b6(b): 12.20%
│   │   ├── 0x35dbf0(b): 11.21%
│   ├── __shell1quartet_module_MOD_make_esfs.isra.0(f): 10.08%
│   │   ├── 0x214cc6(b): 37.37%
│   │   ├── 0x2148c4(b): 10.46%
│   │   ├── 0x213eb6(b): 7.16%
│   │   ├── 0x214c8a(b): 7.05%
│   ├── __cexp(f): 6.76%
│   │   ├── 0x3593b0(b): 17.58%
│   │   ├── 0x3593d4(b): 11.92%
│   │   ├── 0x35940c(b): 9.82%
│   │   ├── 0x359466(b): 9.70%
│   ├── __shell1quartet_module_MOD_make_esss.isra.0(f): 6.52%
│   │   ├── 0x221344(b): 16.04%
│   │   ├── 0x221892(b): 11.98%
│   │   ├── 0x22192a(b): 10.45%
│   │   ├── 0x22163a(b): 8.95%
│   ├── __shell1quartet_module_MOD_make_ssfs.isra.0(f): 4.39%
│   │   ├── 0x22408a(b): 15.74%
│   │   ├── 0x2245da(b): 11.72%
│   │   ├── 0x224672(b): 10.32%
│   │   ├── 0x224380(b): 8.90%
├── 470.lbm
│   ├── LBM_performStreamCollide(f): 97.79%
│   │   ├── 0x10f5e(b): 100.00%
├── 481.wrf
│   ├── __ieee754_powf(f): 30.57%
│   │   ├── 0x3d3342(b): 46.50%
│   │   ├── 0x3d33d8(b): 31.18%
│   │   ├── 0x3d32f8(b): 15.07%
│   ├── __module_mp_wsm3_MOD_wsm32d(f): 11.96%
│   │   ├── 0xa3d14(b): 5.45%
│   │   ├── 0xa3cfe(b): 5.08%
│   ├── __module_bl_ysu_MOD_ysu2d(f): 4.14%
│   │   ├── 0x765b2(b): 8.14%
│   │   ├── 0x76ebc(b): 7.40%
│   │   ├── 0x75fe4(b): 6.84%
│   ├── __module_big_step_utilities_em_MOD_calc_cq(f): 3.49%
│   │   ├── 0x19f4a2(b): 26.29%
│   │   ├── 0x19f588(b): 25.34%
│   │   ├── 0x19f3c6(b): 20.67%
│   │   ├── 0x19f3de(b): 7.81%
│   ├── __module_advect_em_MOD_advect_scalar(f): 2.66%
│   │   ├── 0x153890(b): 21.46%
│   │   ├── 0x1564be(b): 20.24%
│   │   ├── 0x155344(b): 17.03%
│   │   ├── 0x15398e(b): 10.52%
│   ├── __module_ra_rrtm_MOD_rtrn(f): 2.31%
│   │   ├── 0xf8784(b): 34.09%
│   │   ├── 0xf89a4(b): 17.93%
│   │   ├── 0xf8dac(b): 16.87%
│   │   ├── 0xf84ce(b): 10.67%
├── 482.sphinx3
│   ├── mgau_eval(f): 37.58%
│   │   ├── 0x1492e(b): 69.71%
│   │   ├── 0x14a1c(b): 12.54%
│   │   ├── 0x148ee(b): 5.90%
│   ├── vector_gautbl_eval_logs3(f): 24.53%
│   │   ├── 0x2a7c8(b): 80.34%
│   │   ├── 0x2a78a(b): 9.49%
│   ├── approx_cont_mgau_frame_eval(f): 5.62%
│   │   ├── 0x11662(b): 14.75%
│   │   ├── 0x1171a(b): 11.29%
│   │   ├── 0x1167e(b): 10.91%
│   │   ├── 0x11656(b): 9.96%
│   ├── mdef_sseq2sen_active(f): 5.45%
│   │   ├── 0x247fc(b): 61.10%
│   │   ├── 0x24808(b): 34.16%
│   ├── subvq_mgau_shortlist(f): 5.44%
│   │   ├── 0x274aa(b): 57.85%
│   │   ├── 0x274e6(b): 13.27%
│   │   ├── 0x273ea(b): 8.24%
│   │   ├── 0x274f6(b): 7.67%
│   ├── __vfscanf_internal(f): 3.93%
│   │   ├── 0x32b4e(b): 7.62%
```

### 值得关注的点：
#### 单热点函数（单个函数指令数占比超过40%）：
`./src/export_stat.py -d spec06-bbs-bvzicond -t 0.4`

+ 436.cactusADM:bench_staggeredleapfrog2_: 99.23% （单热点基本块）
+ 456.hmmer:P7Viterbi: 92.46% （非单热点基本块，但基本都在跑同样的Vector计算）
+ 470.lbm:LBM_performStreamCollide: 97.79% （单热点基本块）
+ 410.bwaves:mat_times_vec_: 79.89% （单热点基本块）
+ 435.gromacs: inl1130_: 67.23% （单热点基本块）
+ 464.h264ref: SetupFastFullPelSearch: 48.96% （单热点基本块）
+ 462.libquantum: quantum_toffoli: 47.37% （两个热点基本块）
+ 416.gamess: forms_: 44.93%（非单热点基本块）
+ 473.astar: _ZN6wayobj10makebound2EPiiS0_ （非单热点基本块）

#### 每个应用最热点的两个函数以及基本块：
`./src/export_stat.py -d spec06-bbs-bvzicond -n 2 -m 1`

每个基本块最后`:`为基本块大小（按指令数统计）

```shell
├── 400.perlbench
│   ├── S_regmatch(f): 35.98%
│   │   ├── 0x8687c(b): 13.65%: 7
│   ├── Perl_leave_scope(f): 3.91%
│   │   ├── 0x91e0a(b): 15.52%: 6
├── 401.bzip2
│   ├── mainSort(f): 26.85%
│   │   ├── 0x12d7e(b): 13.57%: 6
│   ├── BZ2_compressBlock(f): 23.45%
│   │   ├── 0x16be6(b): 40.95%: 5
├── 403.gcc
│   ├── memset(f): 6.83%
│   │   ├── 0x2543fa(b): 57.92%: 10
│   ├── htab_traverse(f): 3.39%
│   │   ├── 0x241684(b): 95.44%: 4
├── 429.mcf
│   ├── price_out_impl(f): 31.59%
│   │   ├── 0x1111e(b): 40.87%: 5 # 2-level data-dep branch
│   ├── replace_weaker_arc(f): 26.51%
│   │   ├── 0x11026(b): 52.67%: 16 # memory copy
├── 445.gobmk
│   ├── do_play_move(f): 9.72%
│   │   ├── 0x17674(b): 6.97%: 25
│   ├── fastlib(f): 5.56%
│   │   ├── 0x1b7f2(b): 17.13%: 13
├── 456.hmmer
│   ├── P7Viterbi(f): 92.46%
│   │   ├── 0x1cc7a(b): 42.83%: 14
├── 458.sjeng
│   ├── std_eval(f): 24.10%
│   │   ├── 0x1de96(b): 13.87%: 15
│   ├── gen(f): 9.60%
│   │   ├── 0x1c246(b): 8.28%: 5
├── 462.libquantum
│   ├── quantum_toffoli(f): 47.37%
│   │   ├── 0x11052(b): 46.03%: 5
│   ├── quantum_sigma_x(f): 29.97%
│   │   ├── 0x111e0(b): 99.97%: 10
├── 464.h264ref
│   ├── SetupFastFullPelSearch(f): 48.96%
│   │   ├── 0x5c308(b): 83.90%: 99
│   ├── FastFullPelBlockMotionSearch(f): 9.03%
│   │   ├── 0x5cea8(b): 52.67%: 4
├── 471.omnetpp
│   ├── _ZN12cMessageHeap8getFirstEv(f): 14.70%
│   │   ├── 0x5bb3e(b): 25.43%: 6
│   ├── _ZN12cMessageHeap6insertEP8cMessage(f): 5.58%
│   │   ├── 0x5b9b2(b): 30.20%: 9
├── 473.astar
│   ├── _ZN6wayobj10makebound2EPiiS0_(f): 41.45%
│   │   ├── 0x16fb2(b): 10.00%: 6
│   ├── _ZN7way2obj12releasepointEii(f): 36.29%
│   │   ├── 0x145ec(b): 15.83%: 13
├── 483.xalancbmk
│   ├── _ZN10xalanc_1_814VariablesStack9findEntryERKNS_10XalanQNameEbb(f): 6.68%
│   │   ├── 0x1dddba(b): 21.31%: 25
│   ├── _ZN10xalanc_1_814XalanDOMString6equalsERKS0_S2_(f): 6.55%
│   │   ├── 0x1f916c(b): 46.81%: 3
├── 410.bwaves
│   ├── mat_times_vec_(f): 79.89%
│   │   ├── 0x10790(b): 93.19%: 38
│   ├── bi_cgstab_block_(f): 8.96%
│   │   ├── 0x10f1c(b): 19.11%: 18
├── 416.gamess
│   ├── forms_(f): 44.93%
│   │   ├── 0x331de8(b): 37.99%: 67
│   ├── dirfck_(f): 33.55%
│   │   ├── 0x5aba9a(b): 36.03%: 62
├── 433.milc
│   ├── mult_su3_nn(f): 17.63%
│   │   ├── 0x2252a(b): 92.86%: 219
│   ├── mult_su3_na(f): 15.41%
│   │   ├── 0x21f7c(b): 90.71%: 195
├── 434.zeusmp
│   ├── momx3_(f): 10.91%
│   │   ├── 0x324f6(b): 15.79%: 126
│   ├── momx2_(f): 7.90%
│   │   ├── 0x2f552(b): 18.19%: 123
├── 435.gromacs
│   ├── inl1130_(f): 67.23%
│   │   ├── 0x15e6c(b): 98.37%: 289
│   ├── search_neighbours(f): 10.98%
│   │   ├── 0x742a8(b): 27.44%: 13
├── 436.cactusADM
│   ├── bench_staggeredleapfrog2_(f): 99.23%
│   │   ├── 0x18f54(b): 97.62%: 3997
├── 437.leslie3d
│   ├── fluxk_(f): 20.22%
│   │   ├── 0x1a784(b): 22.65%: 93
│   ├── fluxj_(f): 16.64%
│   │   ├── 0x1d2fc(b): 21.94%: 91
├── 444.namd
│   ├── _ZN20ComputeNonbondedUtil26calc_pair_energy_fullelectEP9nonbonded(f): 13.34%
│   │   ├── 0x147f6(b): 53.08%: 165
│   ├── _ZN20ComputeNonbondedUtil19calc_pair_fullelectEP9nonbonded(f): 11.75%
│   │   ├── 0x13792(b): 49.28%: 134
├── 447.dealII
│   ├── _ZNK9MappingQ1ILi3EE12compute_fillERK12TriaIteratorILi3E15DoFCellAccessorILi3EEEjN10QProjectorILi3EE17DataSetDescriptorERNS0_12InternalDataERSt6vectorI5PointILi3EESaISE_EE(f): 33.51%
│   │   ├── 0x1165a6(b): 84.29%: 23
│   ├── _ZN13LaplaceSolver6SolverILi3EE22assemble_linear_systemERNS1_12LinearSystemE(f): 14.13%
│   │   ├── 0x18013e(b): 35.37%: 22
├── 450.soplex
│   ├── _ZN6soplex10SPxSteepPR8entered4ENS_5SPxIdEi(f): 15.11%
│   │   ├── 0x47eb4(b): 53.33%: 7
│   ├── _ZN6soplex8SSVector5setupEv(f): 9.24%
│   │   ├── 0x4d33a(b): 40.93%: 6
├── 453.povray
│   ├── _ZN3povL23All_Plane_IntersectionsEPNS_13Object_StructEPNS_10Ray_StructEPNS_13istack_structE(f): 15.86%
│   │   ├── 0x7b5cc(b): 27.30%: 38
│   ├── _ZN3povL24All_Sphere_IntersectionsEPNS_13Object_StructEPNS_10Ray_StructEPNS_13istack_structE(f): 12.99%
│   │   ├── 0x9ec0e(b): 57.80%: 34
├── 454.calculix
│   ├── insert(f): 9.93%
│   │   ├── 0xa4632(b): 54.03%: 6
│   ├── e_c3d_(f): 9.85%
│   │   ├── 0x3758c(b): 65.98%: 57
├── 459.GemsFDTD
│   ├── __nft_mod_MOD_nft_store(f): 22.70%
│   │   ├── 0x285a6(b): 18.89%: 186
│   ├── __update_mod_MOD_updateh_homo(f): 21.75%
│   │   ├── 0x51710(b): 99.16%: 48
├── 465.tonto
│   ├── __shell2_module_MOD_make_ft_1(f): 12.22%
│   │   ├── 0x16a240(b): 11.05%: 61
│   ├── __sincos(f): 11.20%
│   │   ├── 0x35daf2(b): 21.19%: 28
├── 470.lbm
│   ├── LBM_performStreamCollide(f): 97.79%
│   │   ├── 0x10f5e(b): 100.00%: 908
├── 481.wrf
│   ├── __ieee754_powf(f): 30.57%
│   │   ├── 0x3d3342(b): 46.50%: 42
│   ├── __module_mp_wsm3_MOD_wsm32d(f): 11.96%
│   │   ├── 0xa3d14(b): 5.45%: 22
├── 482.sphinx3
│   ├── mgau_eval(f): 37.58%
│   │   ├── 0x1492e(b): 69.71%: 19
│   ├── vector_gautbl_eval_logs3(f): 24.53%
│   │   ├── 0x2a7c8(b): 80.34%: 31
```

（TODO：分析热点在做的事情以及访存和分支特征）

## SPEC 17
### 原始数据
```shell
├── 500.perlbench_r
│   ├── S_regmatch(f): 19.28%
│   │   ├── 0xc61f4(b): 10.14%
│   │   ├── 0xc61ca(b): 7.38%
│   │   ├── 0xc667c(b): 5.75%
│   ├── Perl_sv_setsv_flags(f): 5.22%
│   │   ├── 0xda75c(b): 10.93%
│   │   ├── 0xdac94(b): 7.62%
│   │   ├── 0xda95c(b): 7.48%
│   │   ├── 0xda7aa(b): 7.17%
│   ├── Perl_leave_scope(f): 4.90%
│   │   ├── 0xd2d2e(b): 15.58%
│   │   ├── 0xd2cd0(b): 13.59%
│   │   ├── 0xd2d72(b): 12.20%
│   │   ├── 0xd2d90(b): 10.70%
│   ├── Perl_pp_padsv(f): 3.38%
│   │   ├── 0x8b7d2(b): 42.67%
│   │   ├── 0x8b7f8(b): 40.20%
│   │   ├── 0x8b83a(b): 13.19%
│   ├── Perl_pp_nextstate(f): 2.91%
│   │   ├── 0x8533c(b): 75.81%
│   │   ├── 0x853ba(b): 16.41%
│   │   ├── 0x853d2(b): 7.70%
│   ├── Perl_sv_clear(f): 2.90%
│   │   ├── 0xd51f0(b): 15.58%
│   │   ├── 0xd527e(b): 10.04%
│   │   ├── 0xd562e(b): 9.68%
│   │   ├── 0xd525e(b): 7.31%
├── 502.gcc_r
│   ├── bitmap_set_bit(f): 2.69%
│   │   ├── 0x1b2cc(b): 24.78%
│   │   ├── 0x1b32e(b): 17.81%
│   │   ├── 0x1b402(b): 14.15%
│   ├── memset(f): 2.55%
│   │   ├── 0x7138f0(b): 23.70%
│   │   ├── 0x71392a(b): 22.27%
│   │   ├── 0x713952(b): 15.39%
│   │   ├── 0x7138e8(b): 8.95%
│   ├── bitmap_bit_p(f): 1.98%
│   │   ├── 0x1b548(b): 30.36%
│   │   ├── 0x1b4d8(b): 18.37%
│   │   ├── 0x1b524(b): 9.45%
│   │   ├── 0x1b4e0(b): 8.63%
│   ├── record_reg_classes.constprop.0(f): 1.33%
│   │   ├── 0x426fd0(b): 9.39%
│   │   ├── 0x426fa8(b): 7.04%
│   │   ├── 0x426f6a(b): 6.13%
│   ├── find_base_term(f): 1.31%
│   │   ├── 0x12bbc(b): 23.00%
│   │   ├── 0x12c12(b): 16.79%
│   │   ├── 0x12bdc(b): 13.59%
│   │   ├── 0x12be8(b): 12.60%
│   ├── htab_find_slot_with_hash(f): 1.29%
│   │   ├── 0x21bf02(b): 29.63%
│   │   ├── 0x21c030(b): 15.74%
│   │   ├── 0x21c000(b): 15.50%
│   │   ├── 0x21bf2c(b): 9.46%
├── 505.mcf_r
│   ├── cost_compare(f): 38.41%
│   │   ├── 0x12b2a(b): 67.03%
│   │   ├── 0x12b3c(b): 16.43%
│   │   ├── 0x12b36(b): 9.56%
│   ├── spec_qsort(f): 25.48%
│   │   ├── 0x130ac(b): 12.86%
│   │   ├── 0x130d2(b): 8.53%
│   │   ├── 0x12f3e(b): 7.17%
│   │   ├── 0x130cc(b): 6.33%
│   ├── primal_bea_mpp(f): 13.32%
│   │   ├── 0x12c96(b): 48.74%
│   │   ├── 0x12cb8(b): 36.54%
│   │   ├── 0x12c8e(b): 7.65%
│   ├── price_out_impl(f): 12.32%
│   │   ├── 0x11958(b): 66.46%
│   │   ├── 0x11950(b): 13.74%
│   │   ├── 0x1196c(b): 10.15%
├── 520.omnetpp_r
│   ├── _ZN12cMessageHeap11removeFirstEv(f): 18.27%
│   │   ├── 0x712c6(b): 35.80%
│   │   ├── 0x71300(b): 15.41%
│   │   ├── 0x712ec(b): 9.76%
│   │   ├── 0x712dc(b): 9.73%
│   ├── strcmp(f): 8.45%
│   │   ├── 0x1e464a(b): 41.48%
│   │   ├── 0x1e46ac(b): 13.83%
│   │   ├── 0x1e44d8(b): 8.87%
│   │   ├── 0x1e44ac(b): 6.33%
│   ├── _ZNK10__cxxabiv120__si_class_type_info12__do_dyncastElNS_17__class_type_info10__sub_kindEPKS1_PKvS4_S6_RNS1_16__dyncast_resultE(f): 7.33%
│   │   ├── 0x12ce6a(b): 25.46%
│   │   ├── 0x12ceec(b): 21.83%
│   │   ├── 0x12ce90(b): 13.97%
│   │   ├── 0x12ceb0(b): 8.69%
│   ├── __dynamic_cast(f): 3.56%
│   │   ├── 0x12baec(b): 18.90%
│   │   ├── 0x12bb32(b): 18.00%
│   │   ├── 0x12bb96(b): 9.80%
│   │   ├── 0x12bb1a(b): 9.46%
│   ├── _ZN12cMessageHeap6insertEP8cMessage(f): 3.36%
│   │   ├── 0x71154(b): 42.76%
│   │   ├── 0x71080(b): 15.41%
│   │   ├── 0x7112c(b): 13.52%
│   │   ├── 0x7111a(b): 11.58%
│   ├── _int_free(f): 2.58%
│   │   ├── 0x1e05b6(b): 16.80%
│   │   ├── 0x1e05ca(b): 11.31%
│   │   ├── 0x1e05aa(b): 9.88%
│   │   ├── 0x1e0598(b): 6.16%
├── 523.xalancbmk_r
│   ├── _ZN11xalanc_1_1014XalanDOMString6equalsERKS0_S2_(f): 7.79%
│   │   ├── 0x244d36(b): 48.09%
│   │   ├── 0x244d2a(b): 14.80%
│   │   ├── 0x244cf0(b): 14.71%
│   │   ├── 0x244d18(b): 7.67%
│   ├── _ZN11xalanc_1_1014VariablesStack9findEntryERKNS_10XalanQNameEbb(f): 6.26%
│   │   ├── 0x1a51e8(b): 16.33%
│   │   ├── 0x1a5310(b): 11.25%
│   │   ├── 0x1a52ac(b): 10.91%
│   │   ├── 0x1a5250(b): 9.05%
│   ├── _ZN11xalanc_1_1011XalanVectorItNS_31MemoryManagedConstructionTraitsItEEE6insertEPtPKtS6_(f): 4.87%
│   │   ├── 0x1cbe4(b): 29.95%
│   │   ├── 0x1d128(b): 12.34%
│   │   ├── 0x1caba(b): 10.94%
│   │   ├── 0x1d108(b): 9.42%
│   ├── _ZN11xalanc_1_1014VariablesStack11findXObjectERKNS_10XalanQNameERNS_26StylesheetExecutionContextEbbRb(f): 4.03%
│   │   ├── 0x1a950e(b): 44.41%
│   │   ├── 0x1a956e(b): 26.63%
│   │   ├── 0x1a9544(b): 21.54%
│   ├── _ZN11xalanc_1_1019XalanDOMStringCache7releaseERNS_14XalanDOMStringE(f): 3.81%
│   │   ├── 0x24b3b2(b): 22.13%
│   │   ├── 0x24b39e(b): 17.36%
│   │   ├── 0x24b398(b): 12.19%
│   │   ├── 0x24b3ee(b): 6.72%
│   ├── _ZNK11xalanc_1_105XPath11executeMoreEPNS_9XalanNodeEPKiRNS_21XPathExecutionContextE(f): 3.35%
│   │   ├── 0x1e52aa(b): 34.81%
│   │   ├── 0x1e5290(b): 21.27%
│   │   ├── 0x1e5540(b): 8.07%
│   │   ├── 0x1e5558(b): 7.70%
├── 525.x264_r
│   ├── x264_pixel_satd_8x4(f): 22.84%
│   │   ├── 0x1f4b4(b): 100.00%
│   ├── get_ref(f): 10.89%
│   │   ├── 0x18e78(b): 28.61%
│   │   ├── 0x18e6c(b): 18.84%
│   │   ├── 0x18ea6(b): 18.49%
│   │   ├── 0x18cac(b): 8.32%
│   ├── x264_pixel_sad_x4_16x16(f): 7.05%
│   │   ├── 0x20dee(b): 24.58%
│   │   ├── 0x20ec6(b): 24.54%
│   │   ├── 0x20f9e(b): 24.30%
│   │   ├── 0x21068(b): 24.19%
│   ├── x264_pixel_sad_x4_8x8(f): 4.93%
│   │   ├── 0x26720(b): 100.00%
│   ├── mc_chroma(f): 4.89%
│   │   ├── 0x11e1e(b): 44.66%
│   │   ├── 0x11e02(b): 18.76%
│   │   ├── 0x11e6c(b): 9.71%
│   │   ├── 0x11d76(b): 9.46%
│   ├── x264_pixel_sad_16x16(f): 4.30%
│   │   ├── 0x1db72(b): 99.52%
├── 531.deepsjeng_r
│   ├── _Z5fevalP7state_tiP12t_eval_comps(f): 12.95%
│   │   ├── 0x174ea(b): 7.07%
│   ├── _Z4makeP7state_ti(f): 10.10%
│   │   ├── 0x164b8(b): 13.43%
│   │   ├── 0x160f8(b): 9.52%
│   │   ├── 0x1619c(b): 7.12%
│   │   ├── 0x165ce(b): 6.53%
│   ├── _Z7qsearchP7state_tiiii(f): 9.34%
│   │   ├── 0x1aece(b): 7.32%
│   │   ├── 0x1b20e(b): 7.13%
│   │   ├── 0x1b466(b): 6.25%
│   ├── _Z6searchP7state_tiiiii.part.0(f): 9.31%
│   │   ├── 0x1bcd8(b): 5.83%
│   ├── _ZL11order_movesP7state_tPiS1_ij(f): 7.33%
│   │   ├── 0x1aa9c(b): 25.88%
│   │   ├── 0x1aa4c(b): 13.64%
│   │   ├── 0x1a50a(b): 10.09%
│   │   ├── 0x1a530(b): 10.01%
│   ├── _Z3seeP7state_tiiii(f): 5.59%
│   │   ├── 0x1e188(b): 24.82%
│   │   ├── 0x1e1e8(b): 7.96%
│   │   ├── 0x1e3e4(b): 7.05%
│   │   ├── 0x1e24a(b): 6.43%
├── 541.leela_r
│   ├── _ZN9FastState16play_random_moveEi(f): 14.44%
│   │   ├── 0x2e8aa(b): 9.01%
│   │   ├── 0x2e8be(b): 5.47%
│   │   ├── 0x2e5c2(b): 5.33%
│   ├── _ZN9FastBoard24get_pattern_fast_augmentEi(f): 13.38%
│   │   ├── 0x2379e(b): 22.80%
│   │   ├── 0x237e4(b): 11.59%
│   │   ├── 0x2383a(b): 11.22%
│   │   ├── 0x2371c(b): 9.95%
│   ├── _ZN9FastBoard10self_atariEii(f): 10.64%
│   │   ├── 0x2752a(b): 7.24%
│   │   ├── 0x2754c(b): 6.96%
│   │   ├── 0x27538(b): 6.92%
│   │   ├── 0x27556(b): 5.65%
│   ├── _ZN9FastBoard15nbr_criticalityEii(f): 6.22%
│   │   ├── 0x25690(b): 11.73%
│   │   ├── 0x2563e(b): 10.07%
│   │   ├── 0x25568(b): 9.95%
│   │   ├── 0x255b8(b): 8.31%
│   ├── _ZN6Random7randintEt(f): 5.70%
│   │   ├── 0x33e68(b): 100.00%
│   ├── _ZN9FastBoard11no_eye_fillEi(f): 5.26%
│   │   ├── 0x22b62(b): 14.53%
│   │   ├── 0x22b74(b): 14.44%
│   │   ├── 0x22c32(b): 12.62%
│   │   ├── 0x22b52(b): 10.49%
├── 548.exchange2_r
│   ├── __brute_force_MOD_digits_2.constprop.4.isra.0(f): 42.39%
│   │   ├── 0x23548(b): 8.15%
│   │   ├── 0x23420(b): 5.52%
│   ├── __brute_force_MOD_digits_2.constprop.3.isra.0(f): 16.49%
│   │   ├── 0x2452c(b): 8.17%
│   │   ├── 0x24480(b): 6.16%
│   │   ├── 0x24466(b): 6.16%
│   │   ├── 0x24548(b): 5.99%
│   ├── _gfortran_mminloc0_4_i4(f): 13.21%
│   │   ├── 0x32a06(b): 15.10%
│   │   ├── 0x32a68(b): 13.57%
│   │   ├── 0x32af8(b): 10.62%
│   │   ├── 0x32a94(b): 6.83%
│   ├── specific.4(f): 8.97%
│   │   ├── 0x1b586(b): 5.74%
│   │   ├── 0x18e6e(b): 5.73%
│   │   ├── 0x18dd4(b): 5.45%
│   │   ├── 0x1b38e(b): 5.40%
├── 557.xz_r
│   ├── lzma_lzma_optimum_normal(f): 33.58%
│   │   ├── 0x1f93a(b): 6.06%
│   │   ├── 0x1f132(b): 6.05%
│   │   ├── 0x1f1bc(b): 5.24%
│   ├── bt_find_func(f): 17.66%
│   │   ├── 0x1a56e(b): 29.14%
│   │   ├── 0x1a5e0(b): 22.65%
│   │   ├── 0x1a5d4(b): 8.41%
│   │   ├── 0x1a5fe(b): 7.10%
│   ├── lzma_lzma_encode(f): 16.47%
│   │   ├── 0x1c07a(b): 12.41%
│   │   ├── 0x1bec4(b): 12.07%
│   │   ├── 0x1beb6(b): 11.52%
│   │   ├── 0x1c048(b): 11.08%
│   ├── lzma_mf_bt4_find(f): 7.40%
│   │   ├── 0x1b46a(b): 36.47%
│   │   ├── 0x1b54e(b): 11.05%
│   │   ├── 0x1b554(b): 10.17%
│   │   ├── 0x1b5a2(b): 9.54%
│   ├── bt_skip_func(f): 6.98%
│   │   ├── 0x1a712(b): 43.19%
│   │   ├── 0x1a706(b): 17.59%
│   │   ├── 0x1a6ac(b): 16.81%
│   │   ├── 0x1a6d8(b): 5.35%
├── 503.bwaves_r
│   ├── mat_times_vec_(f): 66.66%
│   │   ├── 0x10774(b): 93.13%
│   ├── shell_(f): 11.34%
│   │   ├── 0x148e8(b): 70.79%
│   │   ├── 0x15060(b): 13.27%
│   │   ├── 0x1479e(b): 5.61%
│   ├── bi_cgstab_block_(f): 7.19%
│   │   ├── 0x11044(b): 18.60%
│   │   ├── 0x111b6(b): 14.58%
│   │   ├── 0x10d1e(b): 12.27%
│   │   ├── 0x10f46(b): 10.89%
├── 507.cactuBSSN_r
│   ├── _ZL16ML_BSSN_RHS_BodyPK4_cGHiiPKdS3_S3_PKiS5_iPKPd(f): 42.84%
│   │   ├── 0x2f218c(b): 72.95%
│   │   ├── 0x2f898e(b): 18.78%
│   ├── _ZL19ML_BSSN_Advect_BodyPK4_cGHiiPKdS3_S3_PKiS5_iPKPd(f): 34.12%
│   │   ├── 0x2b6616(b): 99.96%
│   ├── _ZL24ML_BSSN_constraints_BodyPK4_cGHiiPKdS3_S3_PKiS5_iPKPd(f): 10.07%
│   │   ├── 0x378840(b): 70.59%
│   │   ├── 0x37cf2e(b): 22.04%
│   │   ├── 0x37c982(b): 6.31%
├── 508.namd_r
│   ├── _ZN20ComputeNonbondedUtil26calc_pair_energy_fullelectEP9nonbonded(f): 14.56%
│   │   ├── 0x955b0(b): 72.31%
│   │   ├── 0x953a2(b): 12.52%
│   │   ├── 0x96ee0(b): 5.98%
│   ├── _ZN20ComputeNonbondedUtil19calc_pair_fullelectEP9nonbonded(f): 11.12%
│   │   ├── 0x93590(b): 64.22%
│   │   ├── 0x93390(b): 16.10%
│   │   ├── 0x94ce6(b): 7.89%
│   ├── _ZN20ComputeNonbondedUtil16calc_pair_energyEP9nonbonded(f): 10.80%
│   │   ├── 0x7c764(b): 63.88%
│   │   ├── 0x7c578(b): 16.53%
│   │   ├── 0x7d94a(b): 7.92%
│   ├── _ZN20ComputeNonbondedUtil32calc_pair_energy_merge_fullelectEP9nonbonded(f): 10.55%
│   │   ├── 0x800e4(b): 62.59%
│   │   ├── 0x7fefe(b): 17.11%
│   │   ├── 0x816f4(b): 8.34%
│   ├── _ZN20ComputeNonbondedUtil9calc_pairEP9nonbonded(f): 8.95%
│   │   ├── 0x7ab48(b): 56.25%
│   │   ├── 0x7a96a(b): 20.20%
│   │   ├── 0x7bbda(b): 9.73%
│   ├── _ZN20ComputeNonbondedUtil25calc_pair_merge_fullelectEP9nonbonded(f): 8.94%
│   │   ├── 0x7e3be(b): 56.15%
│   │   ├── 0x7e1cc(b): 20.22%
│   │   ├── 0x7f862(b): 9.72%
├── 510.parest_r
│   ├── _ZNK6dealii12SparseMatrixIdE5vmultINS_6VectorIdEES4_EEvRT_RKT0_(f): 25.22%
│   │   ├── 0x3f601c(b): 94.71%
│   ├── _ZNK6dealii9SparseILUIdE5vmultIdEEvRNS_6VectorIT_EERKS5_(f): 24.91%
│   │   ├── 0x3e3e8c(b): 40.28%
│   │   ├── 0x3e3e02(b): 39.85%
│   │   ├── 0x3e3ec0(b): 5.78%
│   ├── _ZNK6dealii6VectorIdEmlIdEEdRKNS0_IT_EE(f): 7.27%
│   │   ├── 0x434e3c(b): 99.92%
│   ├── _ZN6dealii11SolverGMRESINS_6VectorIdEEE5solveINS_12SparseMatrixIdEENS_9SparseILUIdEEEEvRKT_RS2_RKS2_RKT0_(f): 6.91%
│   │   ├── 0x47306c(b): 46.96%
│   │   ├── 0x473102(b): 45.78%
│   ├── _ZNK6dealii12SparseMatrixIdE17precondition_SSORIdEEvRNS_6VectorIT_EERKS5_dRKSt6vectorIjSaIjEE(f): 6.57%
│   │   ├── 0x3ec096(b): 42.58%
│   │   ├── 0x3ec174(b): 42.33%
│   ├── _ZN12METomography5Slave5SlaveILi3EE12GlobalMatrix15assemble_matrixERKN6dealii18TriaActiveIteratorINS4_15DoFCellAccessorINS4_10DoFHandlerILi3ELi3EEEEEEERNS0_8internal13AssemblerDataILi3EEE(f): 6.17%
│   │   ├── 0x46a3c2(b): 30.01%
│   │   ├── 0x46a400(b): 20.51%
│   │   ├── 0x46a398(b): 17.63%
│   │   ├── 0x46a594(b): 5.99%
├── 511.povray_r
│   ├── _ZN3povL23All_Plane_IntersectionsEPNS_13Object_StructEPNS_10Ray_StructEPNS_13istack_structE(f): 16.20%
│   │   ├── 0x77972(b): 26.96%
│   │   ├── 0x779f8(b): 16.05%
│   │   ├── 0x77880(b): 15.57%
│   │   ├── 0x77a24(b): 11.09%
│   ├── _ZN3povL24All_Sphere_IntersectionsEPNS_13Object_StructEPNS_10Ray_StructEPNS_13istack_structE(f): 12.86%
│   │   ├── 0x8faac(b): 74.44%
│   │   ├── 0x8fb2e(b): 12.31%
│   │   ├── 0x8fb20(b): 5.57%
│   ├── _ZN3povL31All_CSG_Intersect_IntersectionsEPNS_13Object_StructEPNS_10Ray_StructEPNS_13istack_structE(f): 12.26%
│   │   ├── 0x21098(b): 21.84%
│   │   ├── 0x2108e(b): 13.05%
│   │   ├── 0x21048(b): 8.69%
│   │   ├── 0x210ac(b): 8.29%
│   ├── _ZN3pov17Check_And_EnqueueEPNS_21Priority_Queue_StructEPNS_16BBox_Tree_StructEPNS_19Bounding_Box_StructEPNS_14Rayinfo_StructE(f): 8.87%
│   │   ├── 0x13872(b): 12.16%
│   │   ├── 0x13a46(b): 8.16%
│   │   ├── 0x1399a(b): 7.62%
│   │   ├── 0x13a5e(b): 7.50%
│   ├── _ZN3povL12Inside_PlaneEPdPNS_13Object_StructE(f): 5.27%
│   │   ├── 0x7784c(b): 41.12%
│   │   ├── 0x77866(b): 37.29%
│   │   ├── 0x77820(b): 21.59%
│   ├── _ZN3pov13Inside_ObjectEPdPNS_13Object_StructE(f): 4.53%
│   │   ├── 0x566b4(b): 42.49%
│   │   ├── 0x566a8(b): 29.60%
│   │   ├── 0x56580(b): 27.91%
├── 519.lbm_r
│   ├── LBM_performStreamCollideTRT(f): 98.35%
│   │   ├── 0x1277c(b): 100.00%
├── 521.wrf_r
│   ├── __ieee754_powf(f): 19.64%
│   │   ├── 0x10dedd0(b): 46.97%
│   │   ├── 0x10dee66(b): 31.19%
│   │   ├── 0x10ded86(b): 13.30%
│   ├── __ieee754_logf(f): 6.39%
│   │   ├── 0x10decd0(b): 79.42%
│   │   ├── 0x10decc2(b): 9.87%
│   │   ├── 0x10decb2(b): 8.43%
│   ├── __atanf(f): 6.34%
│   │   ├── 0x10df330(b): 53.12%
│   │   ├── 0x10df2c4(b): 14.76%
│   │   ├── 0x10df3ce(b): 11.95%
│   │   ├── 0x10df478(b): 6.39%
│   ├── __module_mp_wsm5_MOD_nislfv_rain_plm(f): 5.04%
│   │   ├── 0xc54f6e(b): 10.83%
│   │   ├── 0xc54f4c(b): 8.42%
│   │   ├── 0xc54fd2(b): 7.56%
│   ├── __module_mp_wsm5_MOD_wsm52d(f): 4.54%
│   │   ├── 0xc58948(b): 7.03%
│   │   ├── 0xc58014(b): 6.34%
│   │   ├── 0xc58d0a(b): 6.26%
│   ├── __module_advect_em_MOD_advect_scalar_pd(f): 4.36%
│   │   ├── 0xc997a(b): 22.93%
│   │   ├── 0xc9aa2(b): 20.24%
│   │   ├── 0xd18e4(b): 9.16%
│   │   ├── 0xcd2de(b): 8.81%
├── 526.blender_r
│   ├── _Z22bvh_node_stack_raycastI8VBVHNodeLi1024ELb0ELb1EEiPT_P5Isect(f): 72.76%
│   │   ├── 0x2dee70(b): 25.72%
│   │   ├── 0x2deed4(b): 13.70%
│   │   ├── 0x2deeaa(b): 13.04%
│   │   ├── 0x2def50(b): 9.55%
│   ├── RE_rayobject_intersect(f): 8.20%
│   │   ├── 0x2d8f8c(b): 24.75%
│   │   ├── 0x2d8d7a(b): 7.91%
│   │   ├── 0x2d9032(b): 7.60%
│   │   ├── 0x2d8f68(b): 7.35%
├── 527.cam4_r
│   ├── __radae_MOD_radabs(f): 15.25%
│   │   ├── 0x19ce5c(b): 23.75%
│   │   ├── 0x19db6e(b): 13.78%
│   │   ├── 0x19d6dc(b): 6.77%
│   │   ├── 0x19e9e8(b): 5.23%
│   ├── __ieee754_log(f): 9.41%
│   │   ├── 0x4da22c(b): 55.84%
│   │   ├── 0x4da2bc(b): 24.32%
│   │   ├── 0x4da200(b): 17.15%
│   ├── __ieee754_pow(f): 6.71%
│   │   ├── 0x4da3ea(b): 51.13%
│   │   ├── 0x4da4e0(b): 31.70%
│   │   ├── 0x4da3a4(b): 9.18%
│   ├── __radae_MOD_trcab.isra.0(f): 5.84%
│   │   ├── 0x194996(b): 7.59%
│   │   ├── 0x1947c2(b): 7.36%
│   │   ├── 0x194a86(b): 6.19%
│   │   ├── 0x194b46(b): 5.69%
│   ├── __radsw_MOD_radcswmx(f): 3.87%
│   │   ├── 0x1ae582(b): 19.52%
│   │   ├── 0x1b4c6e(b): 7.06%
│   │   ├── 0x1ae7c4(b): 6.13%
│   ├── __m_router_MOD_initp_(f): 3.32%
│   │   ├── 0xd8670(b): 53.43%
│   │   ├── 0xd8678(b): 30.31%
│   │   ├── 0xd867e(b): 15.88%
├── 538.imagick_r
│   ├── MorphologyApply(f): 98.57%
│   │   ├── 0xb92a4(b): 89.72%
│   │   ├── 0xb928e(b): 6.68%
├── 544.nab_r
│   ├── mme34(f): 61.18%
│   │   ├── 0x145b2(b): 10.27%
│   │   ├── 0x14a04(b): 8.92%
│   │   ├── 0x1422a(b): 8.30%
│   │   ├── 0x1494a(b): 7.93%
│   ├── nbond.isra.0(f): 9.79%
│   │   ├── 0x1345a(b): 25.85%
│   │   ├── 0x133b8(b): 22.21%
│   │   ├── 0x13370(b): 21.42%
│   │   ├── 0x13414(b): 19.87%
├── 549.fotonik3d_r
│   ├── __material_mod_MOD_mat_updatee(f): 20.94%
│   │   ├── 0x2182e(b): 96.81%
│   ├── __update_mod_MOD_updateh(f): 16.15%
│   │   ├── 0x20b70(b): 95.34%
│   ├── __power_mod_MOD_power_dft(f): 11.87%
│   │   ├── 0x121f6(b): 49.85%
│   │   ├── 0x1216e(b): 49.82%
│   ├── __upml_mod_MOD_upml_updatee_simple(f): 8.99%
│   │   ├── 0x38778(b): 12.62%
│   │   ├── 0x324ce(b): 11.94%
│   │   ├── 0x33ff0(b): 11.78%
│   │   ├── 0x3262a(b): 10.08%
│   ├── _int_malloc(f): 7.47%
│   │   ├── 0xa76c0(b): 25.56%
│   │   ├── 0xa76b6(b): 14.18%
│   ├── __upml_mod_MOD_upml_updateh(f): 7.35%
│   │   ├── 0x4b75c(b): 15.23%
│   │   ├── 0x4c248(b): 14.68%
│   │   ├── 0x4dea8(b): 12.78%
│   │   ├── 0x4e888(b): 12.73%
├── 554.roms_r
│   ├── __step2d_mod_MOD_step2d_tile.isra.0(f): 18.51%
│   │   ├── 0x528ba(b): 7.59%
│   │   ├── 0x5246e(b): 6.94%
│   │   ├── 0x50214(b): 5.06%
│   │   ├── 0x5033e(b): 5.04%
│   ├── __lmd_skpp_mod_MOD_lmd_skpp(f): 9.28%
│   │   ├── 0x44914(b): 44.46%
│   │   ├── 0x4660a(b): 11.35%
│   │   ├── 0x44024(b): 7.19%
│   ├── __ieee754_pow(f): 6.90%
│   │   ├── 0x102830(b): 50.74%
│   │   ├── 0x102926(b): 31.94%
│   │   ├── 0x1027ea(b): 10.30%
│   ├── __pre_step3d_mod_MOD_pre_step3d(f): 6.61%
│   │   ├── 0x68d94(b): 11.10%
│   │   ├── 0x6a688(b): 8.10%
│   │   ├── 0x69d16(b): 6.84%
│   │   ├── 0x6998a(b): 5.66%
│   ├── __step3d_t_mod_MOD_step3d_t(f): 6.14%
│   │   ├── 0x823f8(b): 21.75%
│   │   ├── 0x8109e(b): 8.78%
│   │   ├── 0x82f34(b): 8.42%
│   │   ├── 0x80ed2(b): 7.97%
│   ├── __t3dmix_mod_MOD_t3dmix2(f): 5.56%
│   │   ├── 0x7bdd2(b): 25.40%
│   │   ├── 0x7b962(b): 15.91%
│   │   ├── 0x7bad8(b): 15.63%
│   │   ├── 0x7bc44(b): 12.46%
```

### 值得关注的点
#### 单热点函数（单个函数指令数占比超过40%）：
+ 519.lbm_r: LBM_performStreamCollideTRT (98.35%)
+ 538.imagick_r: MorphologyApply (98.57%)
+ 526.blender_r: _Z22bvh_node_stack_raycastI8VBVHNodeLi1024ELb0ELb1EEiPT_P5Isect (72.76%)
+ 503.bwaves_r: mat_times_vec_ (66.66%)
+ 544.nab_r: mme34 (61.18%)
+ 507.cactuBSSN_r: _ZL16ML_BSSN_RHS_BodyPK4_cGHiiPKdS3_S3_PKiS5_iPKPd (42.84%)
+ 548.exchange2_r: __brute_force_MOD_digits_2.constprop.4.isra.0 (42.39%)

#### 每个应用最热点的两个函数以及基本块：
```shell
├── 500.perlbench_r
│   ├── S_regmatch(f): 19.28%
│   │   ├── 0xc61f4(b): 10.14%
│   ├── Perl_sv_setsv_flags(f): 5.22%
│   │   ├── 0xda75c(b): 10.93%
├── 502.gcc_r
│   ├── bitmap_set_bit(f): 2.69%
│   │   ├── 0x1b2cc(b): 24.78%
│   ├── memset(f): 2.55%
│   │   ├── 0x7138f0(b): 23.70%
├── 505.mcf_r
│   ├── cost_compare(f): 38.41%
│   │   ├── 0x12b2a(b): 67.03% # 2-level data-dep branch
│   ├── spec_qsort(f): 25.48%
│   │   ├── 0x130ac(b): 12.86%
├── 520.omnetpp_r
│   ├── _ZN12cMessageHeap11removeFirstEv(f): 18.27%
│   │   ├── 0x712c6(b): 35.80%
│   ├── strcmp(f): 8.45%
│   │   ├── 0x1e464a(b): 41.48%
├── 523.xalancbmk_r
│   ├── _ZN11xalanc_1_1014XalanDOMString6equalsERKS0_S2_(f): 7.79%
│   │   ├── 0x244d36(b): 48.09%
│   ├── _ZN11xalanc_1_1014VariablesStack9findEntryERKNS_10XalanQNameEbb(f): 6.26%
│   │   ├── 0x1a51e8(b): 16.33%
├── 525.x264_r
│   ├── x264_pixel_satd_8x4(f): 22.84%
│   │   ├── 0x1f4b4(b): 100.00%
│   ├── get_ref(f): 10.89%
│   │   ├── 0x18e78(b): 28.61%
├── 531.deepsjeng_r
│   ├── _Z5fevalP7state_tiP12t_eval_comps(f): 12.95%
│   │   ├── 0x174ea(b): 7.07%
│   ├── _Z4makeP7state_ti(f): 10.10%
│   │   ├── 0x164b8(b): 13.43%
├── 541.leela_r
│   ├── _ZN9FastState16play_random_moveEi(f): 14.44%
│   │   ├── 0x2e8aa(b): 9.01%
│   ├── _ZN9FastBoard24get_pattern_fast_augmentEi(f): 13.38%
│   │   ├── 0x2379e(b): 22.80%
├── 548.exchange2_r
│   ├── __brute_force_MOD_digits_2.constprop.4.isra.0(f): 42.39% # 大量高频基本块
│   │   ├── 0x23548(b): 8.15%
│   ├── __brute_force_MOD_digits_2.constprop.3.isra.0(f): 16.49%
│   │   ├── 0x2452c(b): 8.17%
├── 557.xz_r
│   ├── lzma_lzma_optimum_normal(f): 33.58%
│   │   ├── 0x1f93a(b): 6.06%
│   ├── bt_find_func(f): 17.66%
│   │   ├── 0x1a56e(b): 29.14%
├── 503.bwaves_r
│   ├── mat_times_vec_(f): 66.66%
│   │   ├── 0x10774(b): 93.13%
│   ├── shell_(f): 11.34%
│   │   ├── 0x148e8(b): 70.79%
├── 507.cactuBSSN_r
│   ├── _ZL16ML_BSSN_RHS_BodyPK4_cGHiiPKdS3_S3_PKiS5_iPKPd(f): 42.84%
│   │   ├── 0x2f218c(b): 72.95%
│   ├── _ZL19ML_BSSN_Advect_BodyPK4_cGHiiPKdS3_S3_PKiS5_iPKPd(f): 34.12%
│   │   ├── 0x2b6616(b): 99.96%
├── 508.namd_r
│   ├── _ZN20ComputeNonbondedUtil26calc_pair_energy_fullelectEP9nonbonded(f): 14.56%
│   │   ├── 0x955b0(b): 72.31%
│   ├── _ZN20ComputeNonbondedUtil19calc_pair_fullelectEP9nonbonded(f): 11.12%
│   │   ├── 0x93590(b): 64.22%
├── 510.parest_r
│   ├── _ZNK6dealii12SparseMatrixIdE5vmultINS_6VectorIdEES4_EEvRT_RKT0_(f): 25.22%
│   │   ├── 0x3f601c(b): 94.71%
│   ├── _ZNK6dealii9SparseILUIdE5vmultIdEEvRNS_6VectorIT_EERKS5_(f): 24.91%
│   │   ├── 0x3e3e8c(b): 40.28%
├── 511.povray_r
│   ├── _ZN3povL23All_Plane_IntersectionsEPNS_13Object_StructEPNS_10Ray_StructEPNS_13istack_structE(f): 16.20%
│   │   ├── 0x77972(b): 26.96%
│   ├── _ZN3povL24All_Sphere_IntersectionsEPNS_13Object_StructEPNS_10Ray_StructEPNS_13istack_structE(f): 12.86%
│   │   ├── 0x8faac(b): 74.44%
├── 519.lbm_r
│   ├── LBM_performStreamCollideTRT(f): 98.35%
│   │   ├── 0x1277c(b): 100.00%
├── 521.wrf_r
│   ├── __ieee754_powf(f): 19.64%
│   │   ├── 0x10dedd0(b): 46.97%
│   ├── __ieee754_logf(f): 6.39%
│   │   ├── 0x10decd0(b): 79.42%
├── 526.blender_r
│   ├── _Z22bvh_node_stack_raycastI8VBVHNodeLi1024ELb0ELb1EEiPT_P5Isect(f): 72.76%
│   │   ├── 0x2dee70(b): 25.72%
│   ├── RE_rayobject_intersect(f): 8.20%
│   │   ├── 0x2d8f8c(b): 24.75%
├── 527.cam4_r
│   ├── __radae_MOD_radabs(f): 15.25%
│   │   ├── 0x19ce5c(b): 23.75%
│   ├── __ieee754_log(f): 9.41%
│   │   ├── 0x4da22c(b): 55.84%
├── 538.imagick_r
│   ├── MorphologyApply(f): 98.57%
│   │   ├── 0xb92a4(b): 89.72%
├── 544.nab_r
│   ├── mme34(f): 61.18%
│   │   ├── 0x145b2(b): 10.27%
│   ├── nbond.isra.0(f): 9.79%
│   │   ├── 0x1345a(b): 25.85%
├── 549.fotonik3d_r
│   ├── __material_mod_MOD_mat_updatee(f): 20.94%
│   │   ├── 0x2182e(b): 96.81%
│   ├── __update_mod_MOD_updateh(f): 16.15%
│   │   ├── 0x20b70(b): 95.34%
├── 554.roms_r
│   ├── __step2d_mod_MOD_step2d_tile.isra.0(f): 18.51%
│   │   ├── 0x528ba(b): 7.59%
│   ├── __lmd_skpp_mod_MOD_lmd_skpp(f): 9.28%
│   │   ├── 0x44914(b): 44.46%
```

## 06和17的对比
### MCF
06:

```shell
├── 429.mcf
│   ├── price_out_impl(f): 31.59%
│   │   ├── 0x1111e(b): 40.87%
│   ├── replace_weaker_arc(f): 26.51%
│   │   ├── 0x11026(b): 52.67%
│   ├── primal_bea_mpp(f): 20.08%
│   │   ├── 0x11a98(b): 48.92%
│   ├── sort_basket(f): 10.20%
│   │   ├── 0x11978(b): 20.27%
```

17:

```shell
├── 505.mcf_r
│   ├── cost_compare(f): 38.41%
│   │   ├── 0x12b2a(b): 67.03%
│   ├── spec_qsort(f): 25.48%
│   │   ├── 0x130ac(b): 12.86%
│   ├── primal_bea_mpp(f): 13.32%
│   │   ├── 0x12c96(b): 48.74%
│   ├── price_out_impl(f): 12.32%
│   │   ├── 0x11958(b): 66.46%
```

其中可以发现17的排序占比显著提高，做pointer chasing的primal_bea_mpp显著降低，原因是17中排序追求了更大的精度，见：[https://blog.cyyself.name/spec-cpu-2017-mcf-diff-2006/](https://blog.cyyself.name/spec-cpu-2017-mcf-diff-2006/)

## 一些思考
### Zicond 谓词化
根据统计信息发现，在 GCC 14 编译的情况下， Zicond 在热点基本块中的出现频率非常低，因此对于 SPECCPU 的 Workload 或许暂时不需要考虑 Zicond 的预测等问题。

**TODO：GCC 15有大量的 Zicond优化，该问题可以重新探索。**

SPEC 06:

```shell
➜  spec06-bbs-bvzicond git:(master) ✗ grep -r "czero"              
400.perlbench/S_regmatch_0x8687c.s:czero.eqz    s1,s1,a4
400.perlbench/S_regmatch_0x86996.s:czero.nez    a5,s5,a5
```

SPEC 17:

```shell
➜  spec17-bbs-bvzicond git:(master) ✗ grep -r "czero"
500.perlbench_r/S_regmatch_0xc61ca.s:czero.eqz  s10,s10,a4
505.mcf_r/cost_compare_0x12b3c.s:czero.nez      a0,a5,a0
525.x264_r/get_ref_0x18cac.s:czero.nez  a5,a3,a5
557.xz_r/lzma_mf_bt4_find_0x1b5a2.s:czero.eqz   a4,a4,a1
```

### V 扩展
热点基本块中存在向量扩展的workload包括 (VLEN=256时提交指令数相比rv64gc的减少量）：

#### SPEC 06
+ 456.hmmer (-186.61%)
+ 410.bwaves
+ 416.gamess
+ 433.milc
+ 436.cactusADM (-62.27%)
+ 437.leslie3d (-208.90%)
+ 447.dealII
+ 454.calculix
+ 459.GemsFDTD (-147.72%)
+ 465.tonto
+ 470.lbm
+ 481.wrf (-112.11%)
+ 482.sphinx3 (-76.98%)

#### SPEC 17
+ 523.xalancbmk_r
+ 525.x264_r (-99.45%)
+ 503.bwaves_r
+ 510.parest_r (-85.20%)
+ 519.lbm_r
+ 521.wrf_r (-120.42%)
+ 527.cam4_r
+ 549.fotonik3d_r (-136.22%)
+ 554.roms_r (-144.06%)

考虑到后端实现对性能影响较大，目前只给出指令数的统计信息。

### 统计信息的改进
分析脚本具有分析微架构事件的能力，可考虑将gem5的各事件计数器接入，看基本块粒度的微架构事件情况，留作TODO。

