# syntax=docker/dockerfile:1

FROM ubuntu:22.04

WORKDIR /workdir
ARG tools_dir="/workdir"
ARG gcpt_dir=${tools_dir}/gcpt
ARG nemu_ref_dir=${tools_dir}/nemu_ref

# install package
ENV DEBIAN_FRONTEND=noninteractive
RUN sed -i 's@//.*archive.ubuntu.com@//mirrors.ustc.edu.cn@g' /etc/apt/sources.list
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    m4 \
    scons \
    zlib1g \
    zlib1g-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libprotoc-dev \
    libgoogle-perftools-dev \
    python3 \
    python3-dev \
    python3-pip \
    libboost-all-dev \
    pkg-config \
    libsqlite3-dev \
    zstd \
    libzstd-dev \
    parallel \
    cmake \
    wget



# clone xs-gem5
ARG xsgem5_url
ARG xsgem5_commit_hash
RUN git clone ${xsgem5_url} && \
    cd GEM5 && \
    git checkout ${xsgem5_commit_hash}
ENV gem5_home=${tools_dir}/GEM5

# creare python env
WORKDIR ${tools_dir}/miniconda3
RUN wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh -O ./miniconda.sh
RUN bash ./miniconda.sh -b -u -p ${tools_dir}/miniconda3
RUN rm -rf ./miniconda.sh

ENV PATH="${tools_dir}/miniconda3/bin:$PATH"

RUN conda create --name py38 --file ${gem5_home}/ext/xs_env/gem5-py38.txt
RUN conda config --set auto_activate_base false

# build dramsim3 for xs-gem5
WORKDIR ${tools_dir}/GEM5/ext/dramsim3
RUN git clone https://github.com/umd-memsys/DRAMsim3.git 
WORKDIR DRAMsim3/build
RUN cmake .. && make -j 4

# build xs-gem5
ARG xsgem5_build_jobs
WORKDIR /workdir/GEM5
ENV LD_LIBRARY_PATH="/workdir/miniconda3/envs/py38/lib:$LD_LIBRARY_PATH"
RUN . ${tools_dir}/miniconda3/etc/profile.d/conda.sh && \
    conda activate py38 && \
    scons build/RISCV/gem5.opt --gold-linker -j ${xsgem5_build_jobs}

# copy the gcpt and nemu ref model
# also set ENV
ARG gcpt
ARG nemu_ref
COPY gcpt ${gcpt_dir}/gcpt
COPY ${nemu_ref} ${nemu_ref_dir}/nemu_ref
ENV GCB_RESTORER=${gcpt_dir}/gcpt
ENV GCBV_RESTORER=${gcpt_dir}/gcpt
ENV GCB_REF_SO=${nemu_ref_dir}/nemu_ref 
ENV GCBV_REF_SO=${nemu_ref_dir}/nemu_ref 

# run xsgem5
WORKDIR /results
RUN chmod -R 777 /results
CMD bash -c " \
    source /workdir/miniconda3/etc/profile.d/conda.sh && \
    conda activate py38 && \
    bash ${gem5_home}/util/xs_scripts/parallel_sim.sh \
    ${gem5_home}/util/xs_scripts/${run_script} \
    /checkpoints/${checkpoints_tag}/${workloads_lst} \
    /checkpoints/${checkpoints_tag} ${run_tag} && \
    chmod -R 777 /results \
" 
