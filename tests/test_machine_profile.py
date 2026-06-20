from __future__ import annotations

from tests.helpers import *


class MachineProfileTests(WindowsExeTestCase):
    """FEAT-0023 startup profile resolution end-to-end (slice 4).

    A profile placed in a cwd-relative ``config/profiles/<name>.json`` is loaded
    when selected by ``--profile`` (or ``SVG_MB_PROFILE``), and ``--show-config``
    reports the resolved profile. The default (no profile selected) is unchanged.
    """

    def _write_profile(self, root: Path, name: str, poll_ms: int) -> None:
        base = json.loads((REPO_ROOT / "config" / "control.example.json").read_text())
        base["poll_ms"] = poll_ms  # distinctive marker proving this file loaded
        profiles = root / "config" / "profiles"
        profiles.mkdir(parents=True, exist_ok=True)
        (profiles / f"{name}.json").write_text(json.dumps(base))

    def test_profile_flag_selects_catalog_profile(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            self._write_profile(td, "myprofile", poll_ms=1234)
            result = _run_control(
                "--show-config", "--json", "--profile", "myprofile", cwd=td
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(
                data["poll_ms"], 1234,
                msg="--profile must load the catalog profile config",
            )

    def test_profile_env_selects_catalog_profile(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            self._write_profile(td, "viaenv", poll_ms=2345)
            result = _run_control(
                "--show-config", "--json", cwd=td, env={"SVG_MB_PROFILE": "viaenv"}
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data["poll_ms"], 2345)

    def test_unknown_profile_errors(self) -> None:
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            (td / "config" / "profiles").mkdir(parents=True, exist_ok=True)
            result = _run_control(
                "--show-config", "--json", "--profile", "nope", cwd=td
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("nope", result.stderr)

    def test_explicit_config_beats_profile(self) -> None:
        config_path = REPO_ROOT / "config" / "control.example.json"
        with tempfile.TemporaryDirectory() as td_str:
            td = Path(td_str)
            self._write_profile(td, "myprofile", poll_ms=9999)
            result = _run_control(
                "--show-config", "--json",
                "--config", str(config_path), "--profile", "myprofile", cwd=td,
            )
            self.assertEqual(result.returncode, 0, msg=result.stderr)
            data = json.loads(result.stdout)
            # control.example.json ships poll_ms=500; explicit --config wins.
            self.assertEqual(data["poll_ms"], 500)
