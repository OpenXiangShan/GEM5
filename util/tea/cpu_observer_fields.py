"""Validate named CPU observers without treating sparse output as schema."""


def audit_added_cpu_observers(before_phases, after_phases, observers):
    """Check declared additions and report other emitted CPU key changes.

    Histogram bins and zero-suppressed vectors can appear or disappear when
    behavior changes. Their output membership does not establish a change in
    registered statistics. Same-behavior comparisons still need strict checks
    of every CPU field and value outside this observer-presence audit.
    """
    if len(before_phases) != len(after_phases):
        raise ValueError("Statistics phase counts differ")
    observers = set(observers)
    changes = []
    for phase, (before, after) in enumerate(zip(before_phases, after_phases)):
        before_cpu = {key for key in before if key.startswith("system.cpu.")}
        after_cpu = {key for key in after if key.startswith("system.cpu.")}
        preexisting = observers & before_cpu
        missing = observers - after_cpu
        assert not preexisting and not missing, {
            "phase": phase,
            "preexisting_observers": sorted(preexisting),
            "missing_observers": sorted(missing),
        }
        changes.append({
            "phase": phase,
            "other_added_output_keys": sorted(after_cpu - before_cpu - observers),
            "other_removed_output_keys": sorted(before_cpu - after_cpu),
        })
    return {"passed": True, "added_observers": sorted(observers),
            "phase_output_changes": changes}
