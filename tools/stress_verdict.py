"""Completion and verdict checks for the pinned SQLite stress programs."""
from dataclasses import dataclass
import os
import re
import signal
import subprocess


@dataclass(frozen=True)
class Verdict:
    passed: bool
    detail: str
    tests: int = 0


def assess(kind, code, output):
    """Successful exit alone is not evidence that a workload completed."""
    if code == 'timeout':
        return Verdict(False, 'timed out')
    if code != 0:
        return Verdict(False, f'exit {code}')
    summaries = list(re.finditer(
        r'^(?:Summary: )?(\d+) errors? out of (\d+) tests?\s*$',
        output, re.MULTILINE | re.IGNORECASE))
    if len(summaries) != 1:
        return Verdict(False, 'missing or duplicate result summary')
    summary = summaries[0]
    errors, count = map(int, summary.groups())
    if errors:
        return Verdict(False, f'{errors} reported errors', count)
    if count == 0:
        return Verdict(False, 'no tests completed')
    if re.search(r'\bFAILED\b|^Failure\b|^Error:|:(?:ERROR|FATAL):|assertion .*failed',
                 output, re.MULTILINE | re.IGNORECASE):
        return Verdict(False, 'failure diagnostic despite clean summary', count)
    if kind == 'threadtest3':
        started = list(re.finditer(r'^Running \S+ for \d+ seconds\.\.\.\s*$',
                                  output, re.MULTILINE))
        if len(started) != count or any(item.start() > summary.start() for item in started):
            return Verdict(False, 'started/completed test counts differ', count)
    elif kind == 'mptest':
        if not re.search(r'^END: .+', output[summary.end():], re.MULTILINE):
            return Verdict(False, 'missing final END record', count)
    else:
        raise ValueError(f'unknown stress program: {kind}')
    return Verdict(True, f'{count} tests completed', count)


def comparison(native, protected):
    """A broken native control is inconclusive, even if the protected run passes."""
    if not native.passed:
        return 'inconclusive'
    if not protected.passed or protected.tests != native.tests:
        return 'failed'
    return 'passed'


def run(command, directory, environment, limit):
    # mptest starts clients. Kill the owned group on timeout/interruption, not just its
    # parent, or clients can keep pipes/files alive after the test directory closes.
    env = {key: value for key, value in {**os.environ, **environment}.items() if value is not None}
    process = subprocess.Popen(command, cwd=directory, env=env,
                               text=True, errors='replace', stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=True)
    try:
        try:
            output, _ = process.communicate(timeout=limit)
            return process.returncode, output
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            output, _ = process.communicate()
            return 'timeout', output
    except BaseException:
        # The owned session does not receive the caller's Ctrl-C. Reap it before
        # temporary-file cleanup, preserving the original exception if cleanup fails.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            process.wait()
        except BaseException:
            pass
        try:
            process.stdout.close()
        except BaseException:
            pass
        raise
