"""Exception types used to map failures to exit codes."""


class InputError(ValueError):
    """The request or one of its files is not acceptable (exit code 2)."""


def need(problems, condition, text):
    """Collect text in problems when condition does not hold (used by the design checks)."""
    if not condition:
        problems.append(text)
