"""Execute actual comparator entry/catch AST with bounded malformed unit input.

No complete GPU receipt is invented. Injected compare work executes exact
production expressions/functions solely to qualify the entrypoint's exception
classification and auditable comparison.json behavior.
"""
import argparse
import ast
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile

# Imported production code is read-only; keep bytecode artifacts out of its tree.
sys.dont_write_bytecode = True


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--source-root", type=Path, required=True)
	parser.add_argument("--evidence-directory", type=Path, required=True)
	parser.add_argument("--pwsh", default="pwsh")
	args = parser.parse_args()
	root = args.source_root.resolve()
	proof = args.evidence_directory.resolve()
	proof.mkdir(parents=True, exist_ok=True)
	comparator = root / "Tests/Tools/RuntimeMemorySafety/Parity/Compare-RuntimeMemorySafetyParity.py"
	tree = ast.parse(comparator.read_text(encoding="utf-8-sig"))
	index_expression = next(node.value for node in ast.walk(tree) if isinstance(node, ast.Assign)
		and any(isinstance(target, ast.Name) and target.id == "metadata" for target in node.targets)
		and isinstance(node.value, ast.Subscript)
		and ast.unparse(node.value).startswith("value['normalCommandMetadata']["))
	cohort_guard = next(node for node in ast.walk(tree) if isinstance(node, ast.Call)
		and isinstance(node.func, ast.Name) and node.func.id == "require"
		and any(isinstance(value, ast.Constant) and value.value == "Selected frame lacks complete actually available lighting payload cohort" for value in node.args))
	index_code = compile(ast.Expression(index_expression), str(comparator), "eval")
	guard_code = compile(ast.Expression(cohort_guard), str(comparator), "eval")
	rows = []

	def record(name, body):
		try:
			body()
			rows.append({"name": name, "status": "passed-unit-contract"})
		except Exception as error:
			rows.append({"name": name, "status": "failed-contract", "errorType": type(error).__name__, "reason": str(error)})

	with tempfile.TemporaryDirectory(prefix="TerraEngine-ComparatorCategories-") as temporary:
		work = Path(temporary).resolve()
		inputs = work / "inputs"
		inputs.mkdir()
		for name in ("baseline.json", "candidate.json", "manifest.json"):
			(inputs / name).write_text("{}\n", encoding="utf-8")

		def run_boundary(name, operation, expected_code, expected_status):
			spec = importlib.util.spec_from_file_location("rms_category_" + name, comparator)
			module = importlib.util.module_from_spec(spec)
			spec.loader.exec_module(module)
			output = work / name
			# Only the compare dependency is fault-injected. main(), catch lists,
			# path checks, comparator pinning and output finalization are production.
			module.compare = lambda unused: operation(module)
			argv = [str(comparator), "--baseline", str(inputs / "baseline.json"), "--candidate", str(inputs / "candidate.json"),
				"--fixture-manifest", str(inputs / "manifest.json"), "--output", str(output), "--scale", "small"]
			original_argv = sys.argv
			returned = None
			escaped = None
			try:
				sys.argv = argv
				try:
					returned = module.main()
				except Exception as error:
					escaped = {"type": type(error).__name__, "reason": str(error)}
			finally:
				sys.argv = original_argv
			artifact = output / "comparison.json"
			document = json.loads(artifact.read_text()) if artifact.exists() else None
			(proof / (name + ".json")).write_text(json.dumps({"mode": "actual imported main with exact-production-AST/function fault injection",
				"argv": argv, "returnedCode": returned, "escaped": escaped, "comparison": document}, indent=2))
			assert escaped is None, "Unexpected malformed input escaped actual main without auditable classification: " + repr(escaped)
			assert returned == expected_code and document and document["status"] == expected_status, (returned, document)
			assert document["nativeAcceptance"] is False and document["performanceAcceptance"] is False
			assert document.get("reason"), "A malformed/different result requires its concrete reason."

		record("ParityCategories.WrongRowTypeIsUnverified", lambda: run_boundary("wrong-row-type",
			lambda module: module.view_semantics(42), 2, "Unverified"))
		record("ParityCategories.MalformedMetadataIndexIsUnverified", lambda: run_boundary("metadata-index",
			lambda module: eval(index_code, vars(module), {"value": {"normalCommandMetadata": []}, "ordinal": 0, "row": {"analysisPurpose": False}}), 2, "Unverified"))
		record("ParityCategories.TruncatedImageIsUnverified", lambda: run_boundary("truncated-image",
			lambda module: module.decode_native_png(b"\x89PNG\r\n\x1a\n\0", 2, 1), 2, "Unverified"))
		record("ParityCategories.UnsupportedImageExtentIsUnverified", lambda: run_boundary("bad-image-extent",
			lambda module: module.png(b"", 1 << 32, 1), 2, "Unverified"))
		record("ParityCategories.AllRejectedCohortCannotCompareEqual", lambda: run_boundary("all-rejected-cohort",
			lambda module: eval(guard_code, vars(module), {"commands": [], "coverage": {"lightingArtifactCommandCount": 1}, "selected_rows": [], "command_serials": []}), 2, "Unverified"))
		record("ParityCategories.ObservedDifferenceRetainsDifferentCategory", lambda: run_boundary("known-difference",
			lambda module: module.equal("unit-reference", "unit-candidate", "Controlled comparison unit difference"), 1, "Different"))

		def empty_cli():
			output = work / "empty-cli"
			command = [sys.executable, str(comparator), "--baseline", str(inputs / "baseline.json"), "--candidate", str(inputs / "candidate.json"),
				"--fixture-manifest", str(inputs / "manifest.json"), "--output", str(output), "--scale", "small"]
			result = subprocess.run(command, capture_output=True, text=True, timeout=30)
			document = json.loads((output / "comparison.json").read_text())
			(proof / "empty-cli.json").write_text(json.dumps({"command": command, "exitCode": result.returncode, "stdout": result.stdout, "stderr": result.stderr, "comparison": document}, indent=2))
			assert result.returncode == 2 and document["status"] == "Unverified"
		record("ParityCategories.EmptyActualCliInputIsAuditableUnverified", empty_cli)

		# PowerShell AST extraction executes the exact final production catch body.
		# Controlled errors are boundary inputs, not full fabricated GPU documents.
		ps_cases = [
			{"name": "RendererCategories.IdentityMismatchIsInvalid", "message": "Hardware identity mismatch", "code": 3, "status": "Invalid"},
			{"name": "RendererCategories.BoundedInputErrorIsInvalid", "message": "Artifact exceeds bounded comparison input", "code": 3, "status": "Invalid"},
			{"name": "RendererCategories.UnverifiedExceedsRemainsUnverified", "message": "UNVERIFIED: input exceeds available observation", "code": 2, "status": "Unverified"},
			{"name": "RendererCategories.KnownNativeDifferenceRemainsFailed", "message": "NATIVE_FAILED: controlled observed renderer difference", "code": 1, "status": "Failed"},
			{"name": "RendererCategories.UnavailableObservationRemainsUnverified", "message": "UNVERIFIED: no actual observation available", "code": 2, "status": "Unverified"},
		]
		case_file = work / "ps-cases.json"
		case_file.write_text(json.dumps(ps_cases))
		driver = work / "actual-catch.ps1"
		driver.write_text(r'''param([string]$Source,[string]$Cases,[string]$Output)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$tokens=$null;$parseErrors=$null
$tree=[Management.Automation.Language.Parser]::ParseFile($Source,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Production comparator parse errors.'}
$topTry=@($tree.EndBlock.Statements|Where-Object {$_ -is [Management.Automation.Language.TryStatementAst]})[-1]
$body=$topTry.CatchClauses[-1].Body.Extent.Text
$handler=[scriptblock]::Create($body.Substring(1,$body.Length-2))
$observed=@()
foreach($case in (Get-Content -Raw -LiteralPath $Cases|ConvertFrom-Json)){
    $summary=[ordered]@{reason='';exitCode=0;status=''}
    try{throw [InvalidOperationException]::new($case.message)}catch{& $handler}
    $observed += [pscustomobject]@{name=$case.name;handlerExitCode=$summary.exitCode;handlerStatus=$summary.status;reason=$summary.reason;expectedCode=$case.code;expectedStatus=$case.status}
}
[IO.File]::WriteAllText($Output,($observed|ConvertTo-Json -Depth 6))
''', encoding="utf-8")
		ps_output = proof / "renderer-catch-observations.json"
		command = [args.pwsh, "-NoProfile", "-NonInteractive", "-File", str(driver), "-Source", str(root / "Tests/Tools/RendererEfficiency/Compare-RendererEfficiencyEvidence.ps1"), "-Cases", str(case_file), "-Output", str(ps_output)]
		result = subprocess.run(command, capture_output=True, text=True, timeout=30)
		(proof / "renderer-catch-command.json").write_text(json.dumps({"command": command, "driverExitCode": result.returncode, "stdout": result.stdout, "stderr": result.stderr}, indent=2))
		if result.returncode != 0:
			raise RuntimeError("Actual production catch AST driver failed: " + result.stderr)
		for observed in json.loads(ps_output.read_text()):
			def assert_category(value=observed):
				assert value["handlerExitCode"] == value["expectedCode"] and value["handlerStatus"] == value["expectedStatus"], value
			record(observed["name"], assert_category)

	failed = sum(row["status"] != "passed-unit-contract" for row in rows)
	result = {"qualification": "malformed-input/parser/catch-boundary unit evidence only; no GPU receipt or native/performance pass",
		"selectedCases": len(rows), "failedCases": failed, "cases": rows}
	(proof / "result.json").write_text(json.dumps(result, indent=2))
	print(json.dumps(result, indent=2))
	return 1 if failed else 0


if __name__ == "__main__":
	raise SystemExit(main())
