from __future__ import annotations

import re

from tests.helpers import *


FEATURES_DIR = REPO_ROOT / "docs" / "features"
FEATURES_README = FEATURES_DIR / "README.md"
TRACEABILITY = REPO_ROOT / "docs" / "TRACEABILITY.md"

REQ_RE = re.compile(r"`?(REQ-[A-Z0-9]+-\d{2,})`?")


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _section(text: str, number: int) -> str:
    match = re.search(
        rf"^## {number}\. .*$([\s\S]*?)(?=^## \d+\. |\Z)",
        text,
        flags=re.MULTILINE,
    )
    if not match:
        return ""
    return match.group(1)


def _table_rows(markdown: str) -> list[list[str]]:
    rows: list[list[str]] = []
    for line in markdown.splitlines():
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if not cells:
            continue
        if all(set(cell) <= {"-"} for cell in cells if cell):
            continue
        rows.append(cells)
    return rows


def _req_id(value: str) -> str | None:
    match = REQ_RE.search(value)
    return match.group(1) if match else None


def _req_rows(markdown: str) -> dict[str, list[str]]:
    rows: dict[str, list[str]] = {}
    for cells in _table_rows(markdown):
        if not cells:
            continue
        req = _req_id(cells[0])
        if req:
            rows[req] = cells
    return rows


def _status(text: str) -> str:
    match = re.search(r"^\*\*Status:\*\*\s*(.+?)\s+\*\*Version:\*\*", text, re.MULTILINE)
    if not match:
        return ""
    return match.group(1).strip()


def _status_kind(status: str) -> str:
    return re.split(r"\s|\(", status, maxsplit=1)[0]


def _namespace(text: str) -> str:
    match = re.search(r"^\*\*Namespace:\*\*\s*`([^`]+)`", text, re.MULTILINE)
    if not match:
        return ""
    return match.group(1).strip()


def _feature_id(text: str) -> str:
    match = re.search(r"^# (FEAT-\d{4}):", text, re.MULTILINE)
    if not match:
        return ""
    return match.group(1)


def _feature_specs() -> dict[str, tuple[Path, str]]:
    specs: dict[str, tuple[Path, str]] = {}
    for path in sorted(FEATURES_DIR.glob("FEAT-*.md")):
        text = _read(path)
        specs[_feature_id(text)] = (path, text)
    return specs


def _registry_rows() -> dict[str, dict[str, str]]:
    registry: dict[str, dict[str, str]] = {}
    readme = _read(FEATURES_README)
    for cells in _table_rows(readme):
        if not cells or not cells[0].startswith("[FEAT-"):
            continue
        id_match = re.search(r"\[(FEAT-\d{4})\]\(([^)]+)\)", cells[0])
        namespace_match = re.search(r"`([^`]+)`", cells[2]) if len(cells) > 2 else None
        if not id_match or not namespace_match:
            continue
        registry[id_match.group(1)] = {
            "path": id_match.group(2),
            "namespace": namespace_match.group(1),
            "status": cells[3] if len(cells) > 3 else "",
        }
    return registry


def _traceability_rows() -> dict[str, list[str]]:
    return _req_rows(_read(TRACEABILITY))


class FeatureSpecContractTests(unittest.TestCase):
    def test_feature_registry_matches_feature_specs(self) -> None:
        specs = _feature_specs()
        registry = _registry_rows()

        self.assertEqual(sorted(registry), sorted(specs))
        for feature_id, (path, text) in specs.items():
            row = registry[feature_id]
            self.assertEqual(
                (FEATURES_DIR / row["path"]).resolve(),
                path.resolve(),
                msg=f"{feature_id} registry path drifted",
            )
            self.assertEqual(
                row["namespace"],
                _namespace(text),
                msg=f"{feature_id} namespace drifted",
            )
            self.assertEqual(
                _status_kind(row["status"]),
                _status_kind(_status(text)),
                msg=f"{feature_id} registry status drifted",
            )

    def test_requirement_sections_and_traceability_are_complete(self) -> None:
        all_spec_reqs: set[str] = set()

        for feature_id, (_, text) in _feature_specs().items():
            namespace = _namespace(text)
            self.assertRegex(
                namespace,
                r"^REQ-[A-Z0-9]+-\*$",
                msg=f"{feature_id} namespace should be REQ-AREA-*",
            )
            expected_prefix = namespace[:-1]

            requirement_rows = _req_rows(_section(text, 6))
            acceptance_rows = _req_rows(_section(text, 10))
            verification_rows = _req_rows(_section(text, 14))
            reqs = set(requirement_rows)

            self.assertGreater(len(reqs), 0, msg=f"{feature_id} has no requirements")
            self.assertEqual(
                len(requirement_rows),
                len(reqs),
                msg=f"{feature_id} has duplicate requirement IDs",
            )
            for req in sorted(reqs):
                self.assertTrue(
                    req.startswith(expected_prefix),
                    msg=f"{feature_id} requirement {req} is outside {namespace}",
                )
            self.assertEqual(
                sorted(acceptance_rows),
                sorted(reqs),
                msg=f"{feature_id} acceptance mapping is missing or adding REQ IDs",
            )
            self.assertEqual(
                sorted(verification_rows),
                sorted(reqs),
                msg=f"{feature_id} verification log is missing or adding REQ IDs",
            )

            for req, cells in acceptance_rows.items():
                self.assertGreaterEqual(
                    len(cells),
                    3,
                    msg=f"{feature_id} {req} acceptance row is incomplete",
                )
                verify_codes = [part.strip() for part in cells[1].split(",")]
                self.assertTrue(
                    verify_codes,
                    msg=f"{feature_id} {req} has no verification type",
                )
                for code in verify_codes:
                    self.assertIn(
                        code,
                        {"T", "B", "M", "R"},
                        msg=f"{feature_id} {req} has unknown verify code {code}",
                    )

            all_spec_reqs.update(reqs)

        trace_rows = _traceability_rows()
        self.assertEqual(
            sorted(trace_rows),
            sorted(all_spec_reqs),
            msg="docs/TRACEABILITY.md must mirror every feature requirement exactly",
        )

    def test_lifecycle_status_matches_promotion_gates_and_logs(self) -> None:
        for feature_id, (_, text) in _feature_specs().items():
            status_kind = _status_kind(_status(text))
            gate_section = _section(text, 13)
            gate_marks = re.findall(r"^- \[([ xX])\] \d+\.", gate_section, re.MULTILINE)

            self.assertEqual(
                len(gate_marks),
                7,
                msg=f"{feature_id} should list all seven promotion gates",
            )
            if status_kind in {"Accepted", "Implemented", "Done"}:
                self.assertTrue(
                    all(mark.lower() == "x" for mark in gate_marks),
                    msg=f"{feature_id} is {status_kind} with open promotion gates",
                )

            if status_kind in {"Implemented", "Done"}:
                verification_rows = _req_rows(_section(text, 14))
                for req, cells in verification_rows.items():
                    self.assertGreaterEqual(
                        len(cells),
                        4,
                        msg=f"{feature_id} {req} verification row is incomplete",
                    )
                    self.assertNotEqual(
                        cells[1].strip(),
                        "",
                        msg=f"{feature_id} {req} missing verification result",
                    )
                    self.assertNotEqual(
                        cells[2].strip(),
                        "",
                        msg=f"{feature_id} {req} missing verification evidence",
                    )

    def test_accepted_and_implemented_specs_link_current_decisions(self) -> None:
        for feature_id, (_, text) in _feature_specs().items():
            status_kind = _status_kind(_status(text))
            if status_kind not in {"Accepted", "Implemented", "Done"}:
                continue

            decision_rows = [
                cells
                for cells in _table_rows(_section(text, 9))
                if cells and "Decision doc" not in cells[0]
            ]
            self.assertGreater(
                len(decision_rows),
                0,
                msg=f"{feature_id} is {status_kind} without a decision row",
            )
            for cells in decision_rows:
                self.assertGreaterEqual(
                    len(cells),
                    3,
                    msg=f"{feature_id} decision row is incomplete",
                )
                status = cells[2].lower()
                self.assertTrue(
                    "accepted" in status or "current" in status,
                    msg=f"{feature_id} decision row is not current: {cells[2]}",
                )

                link_match = re.search(r"\]\(([^)]+)\)", cells[0])
                if link_match:
                    decision_path = (FEATURES_DIR / link_match.group(1)).resolve()
                else:
                    path_match = re.search(r"`docs/([^`]+\.md)`", cells[0])
                    self.assertIsNotNone(
                        path_match,
                        msg=f"{feature_id} decision row has no linked doc",
                    )
                    decision_path = (REPO_ROOT / "docs" / path_match.group(1)).resolve()
                self.assertTrue(
                    decision_path.is_file(),
                    msg=f"{feature_id} decision doc does not exist: {decision_path}",
                )

    def test_traceability_results_match_implemented_verification_logs(self) -> None:
        trace_rows = _traceability_rows()
        for feature_id, (_, text) in _feature_specs().items():
            status_kind = _status_kind(_status(text))
            if status_kind not in {"Implemented", "Done"}:
                continue

            verification_rows = _req_rows(_section(text, 14))
            for req, cells in verification_rows.items():
                spec_result = cells[1].strip()
                trace_result = trace_rows[req][3].strip()
                self.assertEqual(
                    trace_result,
                    spec_result,
                    msg=f"{feature_id} {req} traceability result drifted",
                )


if __name__ == "__main__":
    unittest.main()
