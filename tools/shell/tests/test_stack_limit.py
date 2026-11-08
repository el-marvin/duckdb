# fmt: off

import signal
import subprocess
import sys

import pytest
from conftest import ShellTest

resource = pytest.importorskip("resource")

# the stack of a Windows main thread
STACK_LIMIT = 1 << 20


def run_with_stack_limit(shell, sql):
    def limit_stack():
        resource.setrlimit(resource.RLIMIT_STACK, (STACK_LIMIT, resource.getrlimit(resource.RLIMIT_STACK)[1]))

    return subprocess.run(
        ShellTest(shell).arguments,
        input=sql.encode("utf8"),
        capture_output=True,
        preexec_fn=limit_stack,
        env=ShellTest(shell).get_environment(),
        timeout=300,
    )


def nested_views(count):
    # each view first reads a constant and is then replaced from the top down, so no CREATE binds the whole nest
    statements = [f"CREATE VIEW v{i} AS SELECT 42 AS a;" for i in range(count + 1)]
    statements += [f"CREATE OR REPLACE VIEW v{i} AS SELECT * FROM v{i - 1};" for i in range(count, 0, -1)]
    return "\n".join(statements)


# the main thread of a process refuses a nest its stack cannot bind instead of overflowing it
@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="RLIMIT_STACK sizes the main thread stack on Linux")
def test_nested_views_under_a_small_main_thread_stack(shell):
    sql = "SET memory_limit='4GB';\n" + nested_views(300) + "\nSELECT * FROM v3;\nSELECT * FROM v300;\n"
    result = run_with_stack_limit(shell, sql)
    assert result.returncode != -signal.SIGSEGV, result.stderr
    assert result.returncode == 1, result.stderr
    assert b"Max expression depth exceeded: binding the query would overflow the" in result.stderr, result.stderr
    assert b"42" in result.stdout, result.stdout
