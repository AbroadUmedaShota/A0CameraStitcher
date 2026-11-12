"""Exception types used to map failures to exit codes."""


class InputError(ValueError):
    """The request or one of its files is not acceptable (exit code 2)."""
