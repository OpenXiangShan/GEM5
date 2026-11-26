# Copyright (c) 2013, 2015-2017 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2011 Advanced Micro Devices, Inc.
# Copyright (c) 2009 The Hewlett-Packard Development Company
# Copyright (c) 2004-2005 The Regents of The University of Michigan
# All rights reserved.
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

import os
import sys

import gem5_scons.util
import SCons.Script

git_style_message = """
Install the gem5 pre-commit hook to check code style before each commit.
Press enter to continue, or ctrl-c to abort: """


def install_style_hooks(env):
    try:
        gitdir = env.Dir(
            gem5_scons.util.readCommand(
                ["git", "rev-parse", "--git-dir"]
            ).strip()
        )
    except Exception as e:
        print("Warning: Failed to find git repo directory: %s" % e)
        return

    git_hooks = gitdir.Dir("hooks")
    hook = git_hooks.File("pre-commit")
    if hook.exists():
        return

    print(git_style_message, end=" ")
    if SCons.Script.GetOption("install_hooks"):
        print("Installing the pre-commit hook automatically.")
    else:
        try:
            input()
        except:
            print("Input exception, exiting scons.\n")
            sys.exit(1)

    if hook.islink():
        print("Warning: Removing broken symlink for hook pre-commit.")
        os.unlink(hook.get_abspath())
    if not git_hooks.exists():
        os.mkdir(git_hooks.get_abspath())
        git_hooks.clear()

    script_path = env.Dir("#util").File("git-pre-commit.py").get_abspath()
    # Keep the link relative when the hooks directory moves with the checkout.
    if hook.is_under(env.Dir("#")) and not (
        git_hooks.islink()
        and os.path.isabs(os.readlink(git_hooks.get_abspath()))
    ):
        script_path = os.path.relpath(
            os.path.realpath(script_path),
            os.path.realpath(git_hooks.get_abspath()),
        )
    os.symlink(script_path, hook.get_abspath())


def generate(env):
    if exists(env) and not gem5_scons.util.ignore_style():
        install_style_hooks(env)


def exists(env):
    return env.Entry("#.git").exists()
