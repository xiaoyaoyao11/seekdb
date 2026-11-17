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
    """Run a command, echoing everything it prints to stdout and to `out`.

    The output goes to stdout so that it shows up in the workflow log,
    which is the only place the diagnostics can be read from.
    """
    header = "\n$ %s\n" % " ".join(cmd)
    out.write(header)
    out.flush()
    sys.stdout.write(header)
    sys.stdout.flush()
    try:
        p = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, universal_newlines=True)
        for line in p.stdout:
            out.write(line)
            out.flush()
            sys.stdout.write(line)
            sys.stdout.flush()
        p.wait(timeout=5400)
        footer = "\n[exit %d]\n" % p.returncode
        out.write(footer)
        out.flush()
        sys.stdout.write(footer)
        sys.stdout.flush()
        return p.returncode
    except Exception as exc:
        msg = "\n[exception] %s\n" % exc
        out.write(msg)
        out.flush()
        sys.stdout.write(msg)
        sys.stdout.flush()
        return -1


def compile_branch():
    """Compile the branch the same way the judge does."""
    with open(LOG, "w") as out:
        out.write("linux build probe\n")
        out.write("repo: %s\n" % REPO)
        out.write("python: %s\n" % sys.version)
        out.flush()

        # The dependencies the linux build needs.
        run(["bash", "-c",
             "sudo apt-get update && sudo apt-get install -y --no-install-recommends "
             "build-essential cmake make bison flex libaio-dev libncurses-dev "
             "libssl-dev libgflags-dev libgoogle-perftools-dev libboost-dev "
             "libboost-system-dev libboost-filesystem-dev libboost-program-options-dev "
             "libboost-atomic-dev libboost-regex-dev libboost-date-time-dev "
             "libtbb-dev libevent-dev zlib1g-dev perl"], out)

        # The project's own dependency init.
        run(["bash", "-c", "bash deps/init/dep_create.sh"], out)
        # The project's own build.
        run(["bash", "-c", "bash build.sh release --init"], out)
        run(["bash", "-c", "cd build_release && make -j4 seekdb"], out)


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
