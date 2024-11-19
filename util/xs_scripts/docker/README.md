# run xs-gem5 in docker

In order to be able to run scores on servers without root access, we provide a simple docker script to run xs-gem5. 

## prerequisites

- Generate checkpoints by detercheckpoints.
- Build gcpt and nemu ref model in host.
- Be able to use docker and docker compose.

## use

modify prepare-env.sh and docker-compose.yml to config:

```yml
services:
  para_xsgem5:
    build:
      context: .
      dockerfile: Dockerfile
      args:
        # build args
        xsgem5_url: "" # gem5 repo url
        xsgem5_commit_hash: "" # commit hash
        xsgem5_build_jobs: 64 # build threads
        gcpt: "gcpt" # do not change
        nemu_ref: "nemu_ref" # do not change
    image: xsgem5:latest 
    volumes:
      # dir map
      - ./checkpoints:/checkpoints # do not change
      - ./results:/results # run result dir, do not change
    environment:
      # runtime env
      - checkpoints_dir="/checkpoints"
      - checkpoints_tag="spec06_gc" # tag of checkpoint 
      - run_script="kmh_6wide.sh" # must in util/xs_scripts
      - workloads_lst="checkpoints.lst"
      - run_tag="any-tag" # any tag
      - xsgem5_para_jobs=40 # para-run threads
```

to build:
```bash
./prepare-env.sh
docker compose build
```

to para-run xs-gem5:
```bash
docker compose up
```

## announcement

If you have any questions please raise an issue and @zybzzz, or email yb_zhang@mail.ustc.edu.cn.
