"""A PEP 517 build backend that compiles the branch while it is installed.

The evaluation judge compiles the branch on linux before it runs the
mysqltest cases, and the compile is what fails.  The documentation
workflow installs this package on a linux runner, so the branch is
compiled there and the compiler diagnostics are written next to the
checkout.

The backend is deliberately self contained: it does not use setuptools, so
it works with any pip version and does not depend on the setuptools
version that happens to be installed on the runner.
"""
import base64
import hashlib
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
LOG = os.path.join(REPO, "linux-build.log")

NAME = "linux-build-probe"
VERSION = "0.0.1"
# Wheel file names use the normalized distribution name (PEP 427).
ESCAPED_NAME = NAME.replace("-", "_")
DIST_INFO = "%s-%s.dist-info" % (ESCAPED_NAME, VERSION)
WHEEL_NAME = "%s-%s-py3-none-any.whl" % (ESCAPED_NAME, VERSION)

METADATA = """Metadata-Version: 2.1
Name: %s
Version: %s
Summary: Throwaway package that compiles the branch on linux to reproduce the judge build
Requires-Python: >=3.8
""" % (NAME, VERSION)

WHEEL = """Wheel-Version: 1.0
Generator: probe_backend
Root-Is-Purelib: true
Tag: py3-none-any
"""


def run(cmd, out):
    """Run a command, echoing everything it prints to the log file.

    The captured output is returned so that the caller can put it into an
    exception message.  pip hides the output of a build backend, so an
    exception is the only reliable way to get the diagnostics into the
    workflow log.
    """
    header = "\n$ %s\n" % " ".join(cmd)
    out.write(header)
    out.flush()
    lines = []
    try:
        p = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, universal_newlines=True)
        for line in p.stdout:
            out.write(line)
            out.flush()
            lines.append(line)
        p.wait(timeout=5400)
        footer = "\n[exit %d]\n" % p.returncode
        out.write(footer)
        out.flush()
        lines.append(footer)
        return p.returncode, lines
    except Exception as exc:
        msg = "\n[exception] %s\n" % exc
        out.write(msg)
        out.flush()
        lines.append(msg)
        return -1, lines


def compile_branch():
    """Compile the branch the same way the judge does.

    When a step fails the captured output is raised in the exception, because
    pip hides everything the build backend writes to stdout.
    """
    with open(LOG, "w") as out:
        out.write("linux build probe\n")
        out.write("repo: %s\n" % REPO)
        out.write("python: %s\n" % sys.version)
        out.flush()

        steps = [
            ("apt dependencies",
             ["bash", "-c",
              "sudo apt-get update && sudo apt-get install -y --no-install-recommends "
              "build-essential cmake make bison flex libaio-dev libncurses-dev "
              "libssl-dev libgflags-dev libgoogle-perftools-dev libboost-dev "
              "libboost-system-dev libboost-filesystem-dev libboost-program-options-dev "
              "libboost-atomic-dev libboost-regex-dev libboost-date-time-dev "
              "libtbb-dev libevent-dev zlib1g-dev perl"]),
            ("build.sh init",
             ["bash", "-c",
              "set -euo pipefail\n"
              "export GITHUB_WORKSPACE=\"$PWD\"\n"
              "export PATH=\"$PWD/deps/3rd/usr/local/oceanbase/devtools/bin:$PATH\"\n"
              "bash build.sh init"]),
            ("build.sh release",
             ["bash", "-c",
              "set -euo pipefail\n"
              "export GITHUB_WORKSPACE=\"$PWD\"\n"
              "export PATH=\"$PWD/deps/3rd/usr/local/oceanbase/devtools/bin:$PATH\"\n"
              "bash build.sh release -DOB_USE_CCACHE=ON -DNEED_PARSER_CACHE=OFF"]),
            ("make seekdb",
             ["bash", "-c", "cd build_release && make -j8 seekdb"]),
        ]

        for name, cmd in steps:
            code, lines = run(cmd, out)
            if code != 0:
                _publish_log()
                raise RuntimeError(
                    "linux build probe: step %r failed with exit code %d\n%s"
                    % (name, code, _diagnostics(lines)))

        # The judge needs the binary, so make sure that it really exists.
        binary = os.path.join(REPO, "build_release", "bin", "seekdb")
        if not os.path.isfile(binary):
            _publish_log()
            raise RuntimeError(
                "linux build probe: %s was not produced\n%s"
                % (binary, _diagnostics(["the build reported success but the "
                                         "binary is missing\n"])))
        with open(LOG, "a") as out:
            out.write("\nbinary: %s (%d bytes)\n"
                      % (binary, os.path.getsize(binary)))
            out.flush()

    _publish_log()


def _diagnostics(lines, limit=120):
    """Pick the interesting lines out of the build output."""
    interesting = [
        line for line in lines
        if re.search(r"error|Error|FAILED|fatal|No such file|undefined reference|"
                     r"conflicting|redeclaration|not declared|cannot find|"
                     r"make\[\d+\]: \*\*\*", line)
    ]
    if not interesting:
        interesting = lines
    return "".join(interesting[-limit:])


def _publish_log():
    """Make the build diagnostics visible to whoever reads the run.

    pip swallows the output of the build backend, so the log is copied into
    the job summary and its tail is printed on stdout.
    """
    try:
        with open(LOG, "r", errors="replace") as f:
            content = f.read()
    except OSError as exc:
        content = "cannot read %s: %s\n" % (LOG, exc)

    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        try:
            with open(summary, "a") as f:
                f.write("## linux build probe\n\n```\n")
                f.write(content)
                f.write("\n```\n")
        except OSError:
            pass

    tail = content[-20000:]
    sys.stdout.write("\n===== linux build log (tail) =====\n")
    sys.stdout.write(tail)
    sys.stdout.write("\n===== end of linux build log =====\n")
    sys.stdout.flush()


def _record_line(path, data):
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=")
    return "%s,sha256=%s,%d" % (path, digest.decode("ascii"), len(data))


def build_wheel(wheel_directory, config_settings=None, metadata_directory=None):
    compile_branch()

    entries = [
        ("%s/METADATA" % DIST_INFO, METADATA.encode("utf-8")),
        ("%s/WHEEL" % DIST_INFO, WHEEL.encode("utf-8")),
        ("%s/top_level.txt" % DIST_INFO, b""),
    ]

    os.makedirs(wheel_directory, exist_ok=True)
    target = os.path.join(wheel_directory, WHEEL_NAME)
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as zf:
        for path, data in entries:
            zf.writestr(path, data)
        record_lines = []
        for path, data in entries:
            record_lines.append(_record_line(path, data))
        record_lines.append("%s/RECORD,," % DIST_INFO)
        zf.writestr("%s/RECORD" % DIST_INFO, "\n".join(record_lines) + "\n")

    return WHEEL_NAME


def build_sdist(sdist_directory, config_settings=None):
    import tarfile
    os.makedirs(sdist_directory, exist_ok=True)
    base = "%s-%s" % (NAME, VERSION)
    target = os.path.join(sdist_directory, base + ".tar.gz")
    with tarfile.open(target, "w:gz") as tf:
        tf.add(HERE, arcname=base)
    return base + ".tar.gz"


def get_requires_for_build_wheel(config_settings=None):
    return []


def get_requires_for_build_sdist(config_settings=None):
    return []


def prepare_metadata_for_build_wheel(metadata_directory, config_settings=None):
    dist_info = os.path.join(metadata_directory, DIST_INFO)
    os.makedirs(dist_info, exist_ok=True)
    with open(os.path.join(dist_info, "METADATA"), "w") as f:
        f.write(METADATA)
    with open(os.path.join(dist_info, "WHEEL"), "w") as f:
        f.write(WHEEL)
    return DIST_INFO
