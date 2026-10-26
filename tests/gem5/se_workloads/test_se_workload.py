# Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of Sciences
# All rights reserved.

from testlib import *


catalog = joinpath(
    config.base_dir,
    "tests",
    "pyunit",
    "stdlib",
    "resources",
    "refs",
    "se-workload-catalog.json",
)
resource_path = config.bin_path or joinpath(
    absdirpath(__file__), "resources"
)


class MatchProcessCommand(verifier.MatchFileRegex):
    pass


class MatchProcessEnvironment(verifier.MatchFileRegex):
    pass


class MatchProcessInput(verifier.MatchFileRegex):
    pass


process_verifiers = (
    verifier.MatchRegex(r"Hello world!"),
    MatchProcessCommand(r"^cmd=.* --answer 42$", ("config.ini",)),
    MatchProcessEnvironment(r"^env=SE_TEST=works$", ("config.ini",)),
    MatchProcessInput(
        r"^input=.*se-test-input-1.0.0$", ("config.ini",)
    ),
)


def register_test(name, selection_args):
    gem5_verify_config(
        name=name,
        verifiers=process_verifiers,
        config=joinpath(config.base_dir, "configs", "example", "se.py"),
        config_args=selection_args + [
            "--resource-json={}".format(catalog),
            "--resource-directory={}".format(resource_path),
            "--mem-type=SimpleMemory",
            "--no-pf",
            "--no-l3cache",
            "--warmup-insts-no-switch=0",
            "--maxinsts=1000000",
        ],
        valid_isas=(constants.riscv_tag,),
        valid_variants=(constants.opt_tag,),
        length=constants.quick_tag,
    )


register_test(
    "test-riscv-se-workload-resource",
    ["--workload=se-test-workload"],
)
register_test(
    "test-riscv-se-suite-resource",
    [
        "--suite=se-test-suite",
        "--suite-workload=se-test-workload",
    ],
)
