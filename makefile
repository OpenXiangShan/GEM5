#7959313,7964666
#--debug-flags ValueCommit --debug-start 1392200000 --debug-end 1392300000  \
#--xiangshan-system --enable-difftest \
#--difftest-ref-so=../NEMU/build/riscv64-nemu-interpreter-so \
#--bp-type=ITTAGE

gen:
	scons build/RISCV/gem5.opt -j30
test:
	./build/RISCV/gem5.opt --stats-file=RISCV.txt --dump-config=CONFIG.ini configs/example/se.py -c tests/test-progs/hello/bin/riscv/linux/hello --caches --l1i_size=32kB --l1i_assoc=8 --l1d_size=32kB --l1d_assoc=8 --cacheline_size=64 --l2cache --l2_size=2MB --l2_assoc=8 --cpu-clock=1.6GHz --cpu-type=DerivO3CPU  -n 1 --maxinsts=100000000 
run:
	./build/RISCV/gem5.opt \
	--outdir=build/tmp2 ./configs/example/fs.py \
	--bp-type TAGE \
	--generic-rv-cpt \
	/nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gc_o2_50m/take_cpt/perlbench_checkspam_8850000000_0.140878/0/_850001000_.gz \
	--xiangshan-system --enable-difftest \
	--difftest-ref-so=../NEMU/build/riscv64-nemu-interpreter-so \
	--cpu-type=DerivO3CPU  \
	--caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 \
	--l2cache --l2_size=1MB --l2_assoc=8 --l3cache --l3_size=6MB --l3_assoc=6 \
	--mem-type=DDR4_2400_16x4 \
	--mem-size=8GB --num-cpus=1 -I=100000000 
run1:
	./build/RISCV/gem5.opt \
	--outdir=build/tmp1 ./configs/example/fs.py \
	--generic-rv-cpt /nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gc_o2_50m/take_cpt/perlbench_checkspam_697850000000_0.142824/0/_1850001000_.gz \
	--xiangshan-system --enable-difftest \
	--mem-type=DDR4_2400_16x4 \
	--cpu-type=DerivO3CPU  \
	--caches --cacheline_size=64 --l1i_size=128kB --l1i_assoc=8 --l1d_size=128kB --l1d_assoc=8 --l2cache --l2_size=1MB --l2_assoc=8 \
	--mem-size=8GB --num-cpus=1 -I=100000000 --warmup-insts-no-switch=20000000

debug:
	./scripts-gem5/test-perl.sh DecoupleBP 16382916500 debug_dir
kill:
	killall gem5.opt -SIGUSR1 && sleep 2; grep "simTick\|committedInsts\|lastCommit" m5out/stats.txt

#tage 1628324 2099676 1548411 1556750
#ubtb 1324134
#--warmup-insts-no-switch=20000000 


#/nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gc_o2_50m/take_cpt/perlbench_checkspam_697850000000_0.142824/0/_1850001000_.gz

#/nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gc_o2_50m/take_cpt/perlbench_checkspam_8850000000_0.140878/0/_850001000_.gz
#359329 352361 352361 356997 356997 357802 364740 352844
#404794
# /nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gc_o2_50m/take_cpt/omnetpp_172700000000_0.512105/0/_4700001000_.gz
#ittage ittage ittage ittage ittage tage   simple
#573426 571229 571229 569836 569836 364740 561373
#tage  
#36347 
# /nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gc_o2_50m/take_cpt/gobmk_score2_98750000000_0.0859179/0/_2750001000_.gz
#1305904
#1309261
# /nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gc_o2_50m/take_cpt/gobmk_13x13_36150000000_0.110322/0/_4150001000_.gz
#1468418 1469329 1469329 1468418
#1466754 