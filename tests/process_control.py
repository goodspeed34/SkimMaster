"""Cross-platform helpers for subprocesses that need explicit shutdown."""

import os
import signal
import subprocess


def popen(args, **kwargs):
    """Start a process that can receive a targeted Windows console break."""
    if os.name == "nt":
        kwargs["creationflags"] = (
            kwargs.get("creationflags", 0) | subprocess.CREATE_NEW_PROCESS_GROUP
        )
    return subprocess.Popen(args, **kwargs)


def _wait(proc, timeout):
    if proc.stdout is not None or proc.stderr is not None:
        return proc.communicate(timeout=timeout)
    proc.wait(timeout=timeout)
    return None, None


def stop(proc, timeout=5):
    """Try graceful shutdown, then clean up a stuck process and its children."""
    if proc.poll() is not None:
        return (*_wait(proc, timeout), False)

    requested = False
    try:
        if os.name == "nt":
            proc.send_signal(signal.CTRL_BREAK_EVENT)
        else:
            proc.terminate()
        requested = True
    except (OSError, ValueError):
        # A process can exit between poll() and signaling it.
        pass

    try:
        return (*_wait(proc, timeout), requested)
    except subprocess.TimeoutExpired:
        if os.name == "nt":
            # Terminating only the controller can leave its panel and workers
            # holding files in the temporary station directory.
            try:
                subprocess.run(
                    ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                    timeout=3,
                )
            except subprocess.TimeoutExpired:
                proc.kill()
        else:
            proc.terminate()

    try:
        return (*_wait(proc, 3), False)
    except subprocess.TimeoutExpired:
        proc.kill()
        return (*_wait(proc, 3), False)
