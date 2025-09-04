# args_template.py
from __future__ import annotations
from typing import Dict, Any, List, Optional
import shlex

class CliffArgsTemplate:
    """
    A reusable template for simulator CLI arguments.
    - Only supports inline brace placeholders now, e.g. '--arch-xlen={XLEN}'
      which will be replaced by params['XLEN'].
    """

    # Optional program (first token). If None, you can prepend externally.
    program: Optional[str] = None

    # Base argument tokens. Keep them as a flat list.
    base_arguments: List[str] = [
        #"configs/example/cliff.py",
        #"--no-l3cache",
        #"--arch-xlen={XLEN}",   # inline placeholder
        "cpu.fetchWidth={FetchWidth}\n",
        "cpu.fetchQueueSize={FtqSize}\n",
        "cpu.decodeWidth={DecodeWidth}\n",   # inline placeholder
        "cpu.renameWidth={RenameWidth}\n",
        "cpu.commitWidth={CommitWidth}\n",
        "cpu.LQEntries={VirtualLoadQueueSize}\n",
        "cpu.SQEntries={StoreQueueSize}\n",
        "cpu.SbufferEntries={StoreBufferSize}\n",
        "cpu.SbufferEvictThreshold={StoreBufferThreshold}\n",
        "cpu.RARQEntries={LoadQueueRARSize}\n",
        "cpu.RAWQEntries={LoadQueueRAWSize}\n",
        "cpu.numROBEntries={RobSize}\n",
        "cpu.numPhysIntRegs={intPreg}\n",
        "cpu.numPhysFloatRegs={fpPreg}\n",
        "cpu.numPhysVecRegs={vfPreg}\n",
        # more options...
    ]

    def __init__(
        self,
        program: Optional[str] = None,
        base_arguments: Optional[List[str]] = None,
    ) -> None:
        if program is not None:
            self.program = program
        if base_arguments is not None:
            self.base_arguments = list(base_arguments)

    @staticmethod
    def _format_inline_placeholders(token: str, params: Dict[str, Any]) -> str:
        """Replace {KEY} with params[KEY] (as str)."""
        out = token
        for k, v in params.items():
            placeholder = "{" + str(k) + "}"
            if placeholder in out:
                out = out.replace(placeholder, str(v))
        return out

    def render(
        self,
        params: Dict[str, Any],
        extra_overrides: Optional[Dict[str, Any]] = None,
        prepend_program: Optional[str] = None,
    ) -> List[str]:
        """
        Render final CLI list.
        - params: parameters dict, e.g. {"XLEN": 64, "VLEN": 128, ...}
        - extra_overrides: ad-hoc runtime overrides applied on top of 'params'
        - prepend_program: if provided, put it as the first token
        """
        effective_params = dict(params)
        if extra_overrides:
            effective_params.update(extra_overrides)

        final_args: List[str] = []
        if prepend_program:
            final_args.append(prepend_program)
        elif self.program:
            final_args.append(self.program)

        for tok in self.base_arguments:
            t = self._format_inline_placeholders(tok, effective_params)
            final_args.append(t)

        return final_args

    @staticmethod
    def join_cli(tokens: List[str]) -> str:
        """Safe shell-quoted command line string."""
        return " ".join(shlex.quote(x) for x in tokens)


# ---- minimal-change helper for existing scripts ----
def get_base_arguments(
    params: Optional[Dict[str, Any]] = None,
    program: Optional[str] = None,
    extra_overrides: Optional[Dict[str, Any]] = None,
    as_string: bool = False,
) -> List[str] | str:
    """
    Minimal-change accessor for existing scripts.

    - If params is None: return the raw template list (placeholders intact).
    - If params is provided: return the rendered list with placeholders resolved.
    - program: prepend a program/binary (e.g., 'python' or your simulator).
    - extra_overrides: ad-hoc updates applied on top of 'params'.
    - as_string: when True, return a shell-quoted command line string.
    """
    tmpl = CliffArgsTemplate()
    if params is None:
        argv = []
        if program:
            argv.append(program)
        argv.extend(tmpl.base_arguments)
    else:
        argv = tmpl.render(
            params=params,
            extra_overrides=extra_overrides,
            prepend_program=program,
        )

    if as_string:
        return CliffArgsTemplate.join_cli(argv)
    return argv

