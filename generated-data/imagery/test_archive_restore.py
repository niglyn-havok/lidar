"""Pinned archive restoration must not rewrite provenance or extracted originals."""

import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

import acquire_orthophotos as acquisition


class ArchiveRestoreTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.work = self.root / "generated-data" / "imagery"
        (self.work / "provenance").mkdir(parents=True)
        self.manifest = self.work / "source-manifest.json"
        self.bodies = {}
        self.tiles = []
        for tile_id in acquisition.TILES:
            with io.BytesIO() as buffer:
                with zipfile.ZipFile(buffer, "w") as bundle:
                    bundle.writestr(f"{tile_id}.tif", b"fixture")
                    bundle.writestr(f"{tile_id}.tfw", b"0.05\n")
                body = buffer.getvalue()
            url = f"https://archive.nyu.edu/retrieve/1/{tile_id}.zip"
            self.bodies[url] = body
            tile = {"tile": tile_id, "archive_url": url, "head_bytes": len(body),
                    "archive_path": str(Path("generated-data") / "imagery" / "downloads" / f"{tile_id}.zip")}
            self.tiles.append(tile)
            record = {**tile, "actual_archive_bytes": len(body),
                      "archive_sha256": hashlib.sha256(body).hexdigest()}
            (self.work / "provenance" / f"{tile_id}-download.json").write_text(json.dumps(record))
        self.write_manifest()
        patches = [
            patch.object(acquisition, "ROOT", self.root),
            patch.object(acquisition, "WORK", self.work),
            patch.object(acquisition, "MANIFEST", self.manifest),
            patch.object(acquisition, "progress", side_effect=AssertionError("Historical progress write")),
            patch.object(acquisition, "write_json", side_effect=AssertionError("Historical JSON write")),
            patch("sys.stdout", new_callable=io.StringIO),
        ]
        for patched in patches:
            patched.start()
            self.addCleanup(patched.stop)
        self.before = {path: path.read_bytes() for path in self.work.rglob("*.json")}

    def write_manifest(self):
        self.manifest.write_text(json.dumps({
            "tiles": self.tiles, "archive_total_bytes": sum(tile["head_bytes"] for tile in self.tiles),
        }))

    def response(self, url, **kwargs):
        body = self.bodies[url]
        response = Mock()
        response.url = url
        response.headers = {"Content-Length": str(len(body))}
        response.iter_content.return_value = [body]
        response.__enter__ = Mock(return_value=response)
        response.__exit__ = Mock(return_value=False)
        return response

    def target(self, index=0):
        return self.root / self.tiles[index]["archive_path"]

    def test_restore_and_reuse_leave_historical_metadata_unchanged(self):
        unrelated = self.target().with_suffix(".zip.part")
        unrelated.parent.mkdir(parents=True)
        unrelated.write_bytes(b"another transfer")
        with patch.object(acquisition.requests, "get", side_effect=self.response) as get:
            acquisition.restore()
            self.assertEqual(get.call_count, 4)
            acquisition.restore()
            self.assertEqual(get.call_count, 4)
        for index, tile in enumerate(self.tiles):
            self.assertEqual(self.target(index).read_bytes(), self.bodies[tile["archive_url"]])
        self.assertEqual(unrelated.read_bytes(), b"another transfer")
        self.assertEqual({path: path.read_bytes() for path in self.work.rglob("*.json")}, self.before)

    def test_conflicting_cache_is_preserved_without_download(self):
        target = self.target()
        target.parent.mkdir(parents=True)
        existing = b"x" * self.tiles[0]["head_bytes"]
        target.write_bytes(existing)
        with patch.object(acquisition.requests, "get") as get:
            with self.assertRaisesRegex(RuntimeError, "Existing archive"):
                acquisition.restore()
            get.assert_not_called()
        self.assertEqual(target.read_bytes(), existing)

    def test_wrong_hash_cannot_publish_or_rewrite_failure_reports(self):
        url = self.tiles[0]["archive_url"]
        body = bytearray(self.bodies[url])
        body[20] ^= 1
        self.bodies[url] = bytes(body)
        with patch.object(acquisition.requests, "get", side_effect=self.response):
            with patch.object(sys, "argv", ["acquire_orthophotos.py", "restore"]):
                with self.assertRaisesRegex(RuntimeError, "SHA-256"):
                    acquisition.main()
        self.assertFalse(self.target().exists())
        self.assertFalse(list(self.work.rglob("*.part")))
        self.assertEqual({path: path.read_bytes() for path in self.work.rglob("*.json")}, self.before)

    def test_short_and_oversized_streams_cannot_publish(self):
        for change in (-1, 1):
            with self.subTest(change=change):
                response = self.response(self.tiles[0]["archive_url"])
                body = self.bodies[self.tiles[0]["archive_url"]]
                response.iter_content.return_value = [body[:-1] if change < 0 else body + b"x"]
                with patch.object(acquisition.requests, "get", return_value=response):
                    with self.assertRaisesRegex(RuntimeError, "size"):
                        acquisition.restore()
                self.assertFalse(self.target().exists())
                self.assertFalse(list(self.work.rglob("*.part")))

    def test_manifest_path_url_and_budget_changes_fail_before_network(self):
        original = dict(self.tiles[0])
        for key, value in (("archive_path", "..\\outside.zip"),
                           ("archive_url", "https://example.invalid/source.zip"),
                           ("head_bytes", acquisition.BUDGET + 1)):
            with self.subTest(key=key):
                self.tiles[0] = {**original, key: value}
                self.write_manifest()
                with patch.object(acquisition.requests, "get") as get:
                    with self.assertRaises(RuntimeError):
                        acquisition.restore()
                    get.assert_not_called()
        self.tiles[0] = original

    def test_agreeing_records_cannot_redirect_outside_the_cache_or_official_host(self):
        original = dict(self.tiles[0])
        record_path = self.work / "provenance" / f"{original['tile']}-download.json"
        record = json.loads(record_path.read_bytes())
        for key, value in (("archive_path", str(Path("generated-data") / "outside.zip")),
                           ("archive_url", "https://example.invalid/source.zip")):
            with self.subTest(key=key):
                self.tiles[0] = {**original, key: value}
                self.write_manifest()
                record_path.write_text(json.dumps({**record, key: value}))
                with patch.object(acquisition.requests, "get") as get:
                    with self.assertRaises(RuntimeError):
                        acquisition.restore()
                    get.assert_not_called()

    def test_publication_preserves_a_concurrently_created_cache_file(self):
        response = self.response(self.tiles[0]["archive_url"])

        def chunks(*args, **kwargs):
            self.target().write_bytes(b"concurrent owner's file")
            yield self.bodies[self.tiles[0]["archive_url"]]

        response.iter_content.side_effect = chunks
        with patch.object(acquisition.requests, "get", return_value=response):
            with self.assertRaises(FileExistsError):
                acquisition.restore()
        self.assertEqual(self.target().read_bytes(), b"concurrent owner's file")
        self.assertFalse(list(self.work.rglob("*.part")))

    def test_transient_network_failure_retries_without_report_writes(self):
        attempts = 0

        def get(url, **kwargs):
            nonlocal attempts
            attempts += 1
            if attempts == 1:
                raise acquisition.requests.ConnectionError("temporary disconnect")
            return self.response(url, **kwargs)

        with patch.object(acquisition.requests, "get", side_effect=get):
            with patch.object(acquisition.time, "sleep") as sleep:
                acquisition.restore()
                sleep.assert_called_once_with(15)
        self.assertEqual(attempts, 5)
        self.assertEqual({path: path.read_bytes() for path in self.work.rglob("*.json")}, self.before)


if __name__ == "__main__":
    unittest.main()
