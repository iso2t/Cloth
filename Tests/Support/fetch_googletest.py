# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import hashlib
import io
import os
from pathlib import Path
import sys
import tarfile
import tempfile
from urllib.request import urlopen


def fetch(cache, version, digest):
    release = f"googletest-{version}"
    destination = cache / release
    archive = os.environ.get("GTEST_ARCHIVE")
    if archive:
        data = Path(archive).read_bytes()
    else:
        url = f"https://github.com/google/googletest/releases/download/v{version}/{release}.tar.gz"
        print(f"Fetching GoogleTest {version}", flush=True)
        with urlopen(url, timeout=60) as response:
            data = response.read()

    if hashlib.sha256(data).hexdigest() != digest:
        sys.exit(f"SHA-256 mismatch for {release}.tar.gz")

    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=cache) as staging:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as package:
            # The digest pins the contents on Python versions predating tar filters.
            filters = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
            package.extractall(staging, **filters)
        try:
            (Path(staging) / release).rename(destination)
        except OSError:
            # Another Make invocation can finish installing the same release first.
            if not (destination / "googletest/src/gtest-all.cc").is_file():
                raise


if __name__ == "__main__":
    try:
        fetch(Path(sys.argv[1]), sys.argv[2], sys.argv[3])
    except (OSError, tarfile.TarError) as error:
        sys.exit(f"Could not prepare GoogleTest: {error}\nSet GTEST_ARCHIVE to a local copy of the release tarball for an offline build.")
