#!/usr/bin/env python3
#
# Copyright (c) 2019 Inria
# All rights reserved
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import re
import sys
from pathlib import Path


# Git prefixes the target subject when preparing commits for autosquash.
subject_pattern = re.compile(
    r"(?:(?:fixup|squash|amend)! )?"
    r"(?P<subject>(?:feat|fix|perf|refactor|ci|test|build|docs|chore|style)"
    r"\([\w][\w.-]*\)!?: \S(?:.*\S)?)"
)


def main(message_file):
    lines = Path(message_file).read_text(encoding="utf-8").splitlines()
    match = subject_pattern.fullmatch(lines[0]) if lines else None
    if not match:
        sys.exit(
            "Expected type(scope): description, e.g. "
            "fix(cpu-o3): Correct integer divide latency (see AGENTS.md)"
        )
    if len(match["subject"]) > 65:
        sys.exit("Keep the commit subject within 65 characters.")
    if len(lines) > 1 and lines[1]:
        sys.exit("Leave a blank line between the subject and body.")


if __name__ == "__main__":
    main(sys.argv[1])
