# Local paper downloads

PDFs are downloaded for this investigation and kept locally, outside the code
commit. To download the same copies:

```sh
curl -L -o seshadri-taco2015-icp.pdf https://users.ece.cmu.edu/~omutlu/pub/informed-caching-for-prefetching_taco15.pdf
curl -L -o srinath-hpca2007-fdp.pdf https://users.ece.cmu.edu/~omutlu/pub/srinath_hpca07.pdf
```

SHA256:

- `seshadri-taco2015-icp.pdf`: `3bfbe5dd86e4951eb4d86621a29cd3f56269e04f0095b36032c0284d6d9f155e`
- `srinath-hpca2007-fdp.pdf`: `cb1c60bbe98b88c499bc9cf4282868737508cc86f77558150eedf755660ac1ce`

ICP author code: https://github.com/CMU-SAFARI/memsim,
commit `45e57250c9b60f29243c9c826ec7d9bf9bd75051`.
The author's project page (https://users.ece.cmu.edu/~omutlu/projects.htm)
and SAFARI tools page (https://users.ece.cmu.edu/~safari/tools.html) both link
this implementation.
`Simulator/CmpDCP.h` contains ICP; `Simulator/CmpFDPAP.h` contains the FDP
accuracy-prediction comparison.
