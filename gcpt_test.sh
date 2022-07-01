./build/RISCV/gem5.opt \
    --debug-flags=AddrRanges,Faults \
    ./configs/example/fs.py --mem-size=256MB \
	--l2cache --caches \
	--mem-type=DDR4_2400_16x4 \
	--cacheline_size=64 --l1i_size=32kB --l1i_assoc=8 \
	--l1d_size=32kB --l1d_assoc=8 --l2_size=4MB --l2_assoc=8 \
	--cpu-type=DerivO3CPU  \
	--mem-size=16GB --num-cpus=1 \
    --generic-rv-cpt /nfs-nvme/home/share/checkpoints_profiles/spec06_rv64gc_o2_50m/take_cpt/gamess_gradient_1319250000000_0.134337/0/_7250001000_.gz \
    --maxinsts=300000