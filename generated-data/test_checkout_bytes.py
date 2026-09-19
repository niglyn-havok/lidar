"""Exercise Git checkout conversion without changing the real index or datasets."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
TILES = ("315500_234000", "315500_234500", "316000_234000", "316000_234500")
RAW_PATHS = ("generated-data/sources/osm-hydrology.json",) + tuple(
    f"generated-data/imagery/originals/{tile}/{tile}.tfw" for tile in TILES)


class CheckoutBytesTests(unittest.TestCase):
    def roundtrip(self, autocrlf, fixtures):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            environment = {key: value for key, value in os.environ.items()
                           if key not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR")}
            environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)

            def git(*args):
                subprocess.run(["git", "-C", str(root), "-c", f"core.autocrlf={autocrlf}",
                                "-c", "core.safecrlf=false", *args],
                               env=environment, check=True, capture_output=True)

            git("init", "--quiet", "--template=")
            (root / ".gitattributes").write_bytes((ROOT / ".gitattributes").read_bytes())
            for name, content in fixtures.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            git("add", "--", ".")
            for name in fixtures:
                (root / name).unlink()
            git("checkout-index", "--all", "--force")
            for name, expected in fixtures.items():
                with self.subTest(autocrlf=autocrlf, path=name):
                    self.assertEqual((root / name).read_bytes(), expected)

    def test_raw_sources_preserve_lf_and_crlf(self):
        for autocrlf in ("true", "false", "input"):
            for newline in (b"\n", b"\r\n"):
                self.roundtrip(autocrlf, {name: b'{"source":1}' + newline for name in RAW_PATHS})

    def test_hash_bound_generated_json_has_explicit_original_endings(self):
        fixtures = {
            "DublinFlight/Content/Data/facade-provenance.json": b'{"source":1}\n',
            "DublinFlight/Content/Data/dublin-ortho-georeference.json": b'{"source":1}\r\n',
            "generated-data/imagery/partial-import-contract.json": b'{"source":1}\r\n',
        }
        for autocrlf in ("true", "false", "input"):
            self.roundtrip(autocrlf, fixtures)

    def test_current_source_bytes_match_retained_provenance(self):
        source = ROOT / "generated-data" / "sources" / "osm-hydrology.json"
        record = json.loads(source.with_name("osm-hydrology-provenance.json").read_bytes())
        expected = {source: record["sha256"]}
        inspection = json.loads((ROOT / "generated-data" / "imagery" / "source-inspection.json").read_bytes())
        for tile in inspection["tiles"]:
            path = ROOT / "generated-data" / "imagery" / "originals" / tile["tile"] / f"{tile['tile']}.tfw"
            expected[path] = next(item["sha256"] for item in tile["files"]
                                  if item["path"].endswith(".tfw"))
        for path, digest in expected.items():
            with self.subTest(path=path):
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest)


if __name__ == "__main__":
    unittest.main()
