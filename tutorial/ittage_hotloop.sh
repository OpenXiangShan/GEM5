#! /bin/bash

# set env
python3 util/perfcct.py tutorial/results/enable_ittage/trace.db --zoom 1.5 -p 333 | gzip > tutorial/results/enable_ittage/trace.gz
python3 util/hotloop.py tutorial/results/enable_ittage/trace.gz > tutorial/results/enable_ittage/hotloop.txt

cat tutorial/results/enable_ittage/hotloop.txt