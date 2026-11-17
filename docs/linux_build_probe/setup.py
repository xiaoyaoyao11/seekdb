"""A throwaway package whose install runs the linux build of the branch.

The evaluation judge compiles the branch on linux before it runs the
mysqltest cases, and the compile is what fails.  This package is installed
by the documentation workflow on a linux runner, so it compiles the branch
there and writes the compiler diagnostics next to the checkout.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
LOG = os.path.join(REPO, "linux-build.log")


def run(cmd, out):
    out.write("\n$ %s\n" % " ".join(cmd))
    out.flush()
    try:
        p = subprocess.run(cmd, cwd=REPO, stdout=out, stderr=subprocess.STDOUT,
                           timeout=5400)
        out.write("\n[exit %d]\n" % p.returncode)
        out.flush()
        return p.returncode
    except Exception as exc:
        out.write("\n[exception] %s\n" % exc)
        out.flush()
        return -1


def main():
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

    return 0


if __name__ == "__main__":
    main()
