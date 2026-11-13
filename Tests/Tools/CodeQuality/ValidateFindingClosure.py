#!/usr/bin/env python3
"""Python3.9+ standard library: validate179 closures, never infer native proof.

Normal use (all roots and normative inputs are explicit):
	python ValidateFindingClosure.py --register <findings.json> --closure <closure.json>
		--proof-requirements <reviewed-projection.json> --evidence-root <receipts>
		--repository-root <checkout> [--acceptance-errata <errata.json>] [--report <json>]
A missing proof projection or required proof is BLOCKED (exit1). Schema/scope/hash
errors are INVALID (exit2). Complete normal evidence returns0. No commands execute.

--self-test <FindingClosureCases.json> --register <findings.json> [--work-root <fresh>]
runs synthetic parser contracts only, always acceptanceGranted=false. Portable
installed tests may replace --register with --fixture-register ONLY with --self-test;
that mode derives artificial acceptance text from fixture scope and validates no
normative finding acceptance. Normal mode still requires an explicit real register.
The adjacent
fixture JSON contains portable record/projection examples. Normal projection rows
pin the register acceptance (and effective erratum when supplied), required kinds,
platform/configuration, observed counts/results and prerequisites independently of
closure claims. Reviewers still judge whether pinned observations prove semantics;
this checker cannot turn a human assertion or arbitrary log into native observation.

Preferred wrapper: {"findings":[{"findingId":"CR-001","status":"accepted",
"closure":{...required fields...}}]}. A flat list with the same explicit status and
fields is also accepted; mixed/ambiguous finding IDs are rejected. Artifact byte lengths may be zero (for example empty stderr); hashes alone never
prove execution. All artifacts use
{id,root:"evidence"|"repository",path:<slash-relative>,sha256,bytes}. Change references
are {type:"source",artifactReference,description} or {type:"commit",commit,description}.
countUnit defaults to cases: actual nonzero selected/executed case floors and
zero required skips remain mandatory. Explicit procedures are allowed only for
compile/build/install/source/source-inspection/artifact-inspection/documentation:
requirements use minimumExecutedProcedures, observations use positive actually
observed executedProcedures, and testcase fields must be absent/null. No count is
automatically invented; native/test kinds cannot relabel proof into procedures.
Remaining limitations are {description,required}; a required limitation blocks.
Only fixed/superseded-with-proof apply to confirmed findings. All five dispositions
are permitted for qualified partial findings. Synthetic fixtures certify no runtime.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path, PureWindowsPath
import re
import sys
import tempfile

EXCLUDED = frozenset((98,107,117,123,141,142,146,151,153,154,159,160,161,162,164,165,166,167,168,169,170,173,174,175,176,178,197))
PARTIAL = frozenset((4,9,34,66,102,106,108,112,114,133,135,143,150,152,155,157,163,185,186,187,200))
INCLUDED = frozenset(f"CR-{n:03d}" for n in range(1,207) if n not in EXCLUDED)
DISPOSITIONS = frozenset(("fixed","superseded-with-proof","bounded-hardening","configuration-or-documentation-corrected","supported-path-claim-narrowed"))
KINDS = frozenset(("native","gpu-native","worker-native","compile","build","install","source-inspection","artifact-inspection","source","documentation","protocol","synthetic"))
PROCEDURE_KINDS = frozenset(("compile","build","install","source","source-inspection","artifact-inspection","documentation"))
SHA = re.compile(r"^[0-9a-f]{64}$")
MAX_JSON = 16 * 1024 * 1024

class Invalid(ValueError):
	"""Malformed or contradictory input, distinct from missing required proof."""

def unique_object(pairs):
	value = {}
	for key, item in pairs:
		if key in value:
			raise Invalid(f"Duplicate JSON object key: {key}")
		value[key] = item
	return value

def bounded_integer(value):
	# Bound conversion itself, including Python hosts without a default digit cap.
	if len(value.lstrip("-")) > 20:
		raise Invalid("JSON integer exceeds the unsigned64-bit input bound")
	result = int(value)
	if abs(result) > (1<<64)-1:
		raise Invalid("JSON integer exceeds the unsigned64-bit input bound")
	return result

def bounded_float(value):
	if len(value)>128 or not math.isfinite(float(value)):
		raise Invalid("Nonfinite or oversized JSON number")
	return float(value)

def invalid_constant(value):
	raise Invalid(f"Nonstandard JSON constant: {value}")

def read_json(path):
	path = Path(path)
	if path.stat().st_size > MAX_JSON:
		raise Invalid("JSON input exceeds the sixteen MiB parser bound")
	with path.open("r", encoding="utf-8-sig") as stream:
		return json.load(stream, object_pairs_hook=unique_object, parse_int=bounded_integer, parse_float=bounded_float, parse_constant=invalid_constant)

def text(value):
	return isinstance(value, str) and bool(value.strip()) and len(value) <= 16384

def integer(value):
	return type(value) is int

def acceptance_hash(value):
	return hashlib.sha256(value.encode("utf-8")).hexdigest()

def keyed(rows, field, description):
	if not isinstance(rows, list) or len(rows) > 256:
		raise Invalid(f"{description} must be a bounded array")
	result = {}
	for row in rows:
		if not isinstance(row, dict) or not text(row.get(field)):
			raise Invalid(f"{description} needs a nonempty {field}")
		key = row[field]
		if key in result:
			raise Invalid(f"Duplicate {description} ID: {key}")
		result[key] = row
	return result

def load_register(document):
	if not isinstance(document, dict) or type(document.get("schemaVersion")) is not int or document["schemaVersion"] != 1:
		raise Invalid("Register schemaVersion must be1")
	rows = keyed(document.get("findings"), "key", "register")
	excluded = document.get("excludedFalsePositiveIds", [])
	if not isinstance(excluded,list) or any(not integer(n) for n in excluded) or len(excluded)!=27 or set(rows) != INCLUDED or set(excluded) != EXCLUDED:
		raise Invalid("Register must preserve the exact179 included/27 excluded IDs")
	for key, row in rows.items():
		number = int(key[3:])
		expected = "Partially valid" if number in PARTIAL else "Confirmed"
		if not integer(row.get("id")) or row["id"] != number or row.get("verdict") != expected:
			raise Invalid(f"Register ID/verdict drift: {key}")
		if not text(row.get("acceptance")) or not text(row.get("package")):
			raise Invalid(f"Register lacks normative acceptance/package: {key}")
	if document.get("counts") != {"Confirmed":158,"Partially valid":21}:
		raise Invalid("Register counts do not preserve158 confirmed/21 partial")
	return rows

class Checker:
	"""Bounded, read-only evidence checks. Requirements never come from a row's flags."""
	def __init__(self, register, evidence_root, repository_root=None, projection=None,
			errata=None, expected_revision=None, synthetic=False, max_artifact=1<<30,
			max_total=8<<30):
		self.register = register
		self.roots = {"evidence":Path(evidence_root).resolve(strict=True)}
		if repository_root is not None:
			self.roots["repository"] = Path(repository_root).resolve(strict=True)
		if any(not root.is_dir() for root in self.roots.values()):
			raise Invalid("Evidence/repository roots must be existing directories")
		self.synthetic = synthetic
		self.expected_revision = expected_revision
		self.max_artifact, self.max_total = max_artifact, max_total
		self.projection = projection
		self.effective = {key:row["acceptance"] for key,row in register.items()}
		if errata is not None:
			if not isinstance(errata, dict):
				raise Invalid("Errata must be an explicit object")
			for key, item in keyed(errata.get("items"), "findingId", "errata").items():
				if key not in register or not text(item.get("currentRequiredOutcome")):
					raise Invalid("Erratum cannot change scope or omit its current outcome")
				self.effective[key] = item["currentRequiredOutcome"]
		self.invalid, self.blocked = [], []
		self.cache, self.read_paths = {}, set()
		self.hashed_bytes = 0

	def note(self, severity, code, detail, finding=None):
		items = self.invalid if severity == "invalid" else self.blocked
		if len(items) < 4096:
			items.append({"findingId":finding,"code":code,"detail":str(detail)[:2048]})

	def required_text(self, value, field, finding):
		if value is None or value == "":
			self.note("blocked", "missing-proof-field", field, finding)
			return False
		if not text(value):
			self.note("invalid", "invalid-text", field, finding)
			return False
		return True

	def references(self, references, artifacts, field, finding):
		if not isinstance(references, list) or not references:
			self.note("blocked", "missing-artifact-reference", field, finding)
			return False
		if len(references) > 256 or any(not text(ref) for ref in references) or len(set(references)) != len(references):
			self.note("invalid", "invalid-artifact-reference", field, finding)
			return False
		unknown = set(references) - set(artifacts)
		if unknown:
			self.note("invalid", "unknown-artifact-reference", sorted(unknown), finding)
			return False
		return all(artifacts[ref] for ref in references)

	def artifact(self, item, finding):
		try:
			root = self.roots.get(item.get("root"))
			value, digest, size = item.get("path"), item.get("sha256"), item.get("bytes")
			if root is None or not isinstance(value, str) or not value or len(value) > 4096:
				raise Invalid("Artifact needs an available explicit root and relative path")
			if "\\" in value or ":" in value or "\x00" in value or value.startswith("/") or PureWindowsPath(value).drive or ".." in value.split("/"):
				raise Invalid("Artifact path must be portable slash-relative without parent/drive/root escapes")
			if not isinstance(digest, str) or not SHA.fullmatch(digest) or not integer(size) or size < 0 or size > self.max_artifact:
				raise Invalid("Artifact SHA256/nonnegative bounded byte length is invalid")
			candidate = (root / value).resolve(strict=True)
			if not candidate.is_relative_to(root) or not candidate.is_file():
				raise Invalid("Resolved artifact escapes its root or is not a regular file")
			self.read_paths.add(candidate)
			before = candidate.stat()
			if before.st_size != size:
				raise Invalid("Artifact byte length mismatch")
			cache_key = (candidate, digest, size, before.st_mtime_ns)
			if cache_key not in self.cache:
				if self.hashed_bytes + size > self.max_total:
					raise Invalid("Evidence hashing exceeds the explicit total byte budget")
				observed = hashlib.sha256()
				with candidate.open("rb") as stream:
					for block in iter(lambda:stream.read(1024*1024), b""):
						observed.update(block)
				after = candidate.stat()
				if (before.st_size,before.st_mtime_ns,before.st_ino) != (after.st_size,after.st_mtime_ns,after.st_ino):
					raise Invalid("Artifact changed while hashing")
				if observed.hexdigest() != digest:
					raise Invalid("Artifact SHA256 mismatch")
				self.hashed_bytes += size
				self.cache[cache_key] = True
			return True
		except (Invalid, OSError, ValueError) as error:
			self.note("invalid", "invalid-artifact", error, finding)
			return False

	def requirements(self):
		if self.projection is None:
			self.note("blocked", "missing-proof-projection", "Explicit reviewed per-finding proof requirements are absent")
			return {}
		p = self.projection
		if not isinstance(p, dict) or type(p.get("schemaVersion")) is not int or p["schemaVersion"] != 1:
			raise Invalid("Proof projection schemaVersion must be1")
		if set(p) - {"schemaVersion","syntheticFixture","findings","description","registerRevision"}:
			raise Invalid("Unknown proof projection semantics; do not silently ignore requirements")
		if p.get("syntheticFixture") and not self.synthetic:
			self.note("blocked", "synthetic-not-native", "Synthetic proof projection cannot authorize acceptance")
		rows = keyed(p.get("findings"), "findingId", "proof projection")
		if set(rows) - INCLUDED:
			raise Invalid("Proof projection contains an excluded/unknown ID")
		for key, item in rows.items():
			if set(item) - {"findingId","acceptanceSha256","effectiveAcceptanceSha256","requiredValidations","requiredPrerequisites","description"}:
				raise Invalid("Unknown per-finding proof requirement semantics")
			if item.get("acceptanceSha256") != acceptance_hash(self.register[key]["acceptance"]):
				raise Invalid(f"Proof projection register acceptance pin mismatch: {key}")
			effective = acceptance_hash(self.effective[key])
			if item.get("effectiveAcceptanceSha256", item["acceptanceSha256"]) != effective:
				raise Invalid(f"Proof projection effective acceptance/erratum mismatch: {key}")
			proofs = keyed(item.get("requiredValidations"), "requirementId", "required validations")
			if not proofs:
				self.note("blocked", "empty-proof-requirements", key, key)
			for proof in proofs.values():
				if set(proof) - {"requirementId","kind","platform","configuration","minimumSelected","minimumExecuted","countUnit","minimumExecutedProcedures","expectedExitStatus","expectedResult","description"}:
					raise Invalid("Unknown required validation semantics")
				if proof.get("kind") not in KINDS or not text(proof.get("platform")) or not text(proof.get("configuration")):
					raise Invalid(f"Proof requirement kind/platform/configuration invalid: {key}")
				unit=proof.get("countUnit","cases")
				if unit not in ("cases","procedures"):
					raise Invalid("Unknown proof countUnit")
				if unit=="procedures":
					if proof["kind"] not in PROCEDURE_KINDS:
						raise Invalid("Native/test proof cannot evade case floors through procedures")
					if not integer(proof.get("minimumExecutedProcedures")) or proof["minimumExecutedProcedures"]<1:
						raise Invalid("Procedure requirements need a positive observed-procedure floor")
					if any(proof.get(field) is not None for field in ("minimumSelected","minimumExecuted")):
						raise Invalid("Procedure requirements must not invent test-case floors")
				else:
					if proof.get("minimumExecutedProcedures") is not None:
						raise Invalid("Case requirements must not mix procedure floors")
					for count in ("minimumSelected","minimumExecuted"):
						if not integer(proof.get(count)) or proof[count] < 1:
							raise Invalid("Required proof needs positive selected/executed case floors")
				if not integer(proof.get("expectedExitStatus")) or not text(proof.get("expectedResult")):
						raise Invalid("Required proof needs expected exit/result")
			prerequisites = item.get("requiredPrerequisites", [])
			if not isinstance(prerequisites, list) or len(prerequisites)>256 or any(not text(x) for x in prerequisites) or len(set(prerequisites)) != len(prerequisites):
				raise Invalid("Required prerequisite names are invalid")
		return rows

	def validation_counts(self,value,proof,finding):
		# Case totals describe selected/executed tests, never invented build or
		# inspection counts. Procedure attempts have their own observed counter.
		unit=value.get("countUnit","cases")
		if value.get("kind") not in KINDS or unit not in ("cases","procedures"):
			self.note("invalid","invalid-kind-or-count-unit","Unknown observation kind/countUnit",finding)
			return
		if unit=="procedures" and value["kind"] not in PROCEDURE_KINDS:
			self.note("invalid","native-or-test-procedure-unit","Native/test observations require actual case counts",finding)
			return
		if unit!=proof.get("countUnit","cases"):
			self.note("blocked","count-unit-mismatch","Observation does not meet projected count semantics",finding)
			return
		if unit=="procedures":
			if any(value.get(field) is not None for field in ("selectedCases","executedCases","skippedCases")):
				self.note("invalid","fabricated-case-counters","Procedure test counters must be absent/null",finding)
			observed=value.get("executedProcedures")
			if observed is None:
				self.note("blocked","missing-observed-procedures","No actual procedure attempt count was observed",finding)
			elif not integer(observed) or observed<0:
				self.note("invalid","invalid-procedure-count","Observed procedure attempts must be a nonnegative integer",finding)
			elif observed<proof["minimumExecutedProcedures"]:
				self.note("blocked","procedure-floor-unproven","Observed procedure attempts do not meet the required floor",finding)
			if value.get("skippedProcedures") is not None:
				if not integer(value["skippedProcedures"]) or value["skippedProcedures"]<0:
					self.note("invalid","invalid-procedure-count","Skipped procedure count is invalid",finding)
				elif value["skippedProcedures"]:
					self.note("blocked","required-procedure-skipped","A required procedure did not execute",finding)
			return
		if value.get("executedProcedures") is not None or value.get("skippedProcedures") is not None:
			self.note("invalid","mixed-count-semantics","Case observations must not fabricate procedure counters",finding)
		for count, minimum in (("selectedCases",proof["minimumSelected"]),("executedCases",proof["minimumExecuted"]),("skippedCases",0)):
			if not integer(value.get(count)) or value[count] < 0:
				self.note("invalid", "invalid-count", count, finding)
			elif value[count] < minimum or (count == "skippedCases" and value[count] != 0):
				self.note("blocked", "required-cases-not-executed", count, finding)
		if integer(value.get("executedCases")) and integer(value.get("selectedCases")) and value["executedCases"] > value["selectedCases"]:
			self.note("invalid", "inconsistent-counts", "Executed exceeds selected", finding)
		elif integer(value.get("executedCases")) and integer(value.get("selectedCases")) and value["executedCases"] != value["selectedCases"]:
			self.note("blocked", "selected-cases-incomplete", "Every selected required case must actually execute", finding)

	def validation(self, value, proof, artifacts, finding):
		# A matching label or registered count is insufficient. Require the exact
		# projected kind/platform/config, observed execution and pinned raw evidence.
		for field in ("kind","platform","configuration"):
			if value.get(field) != proof[field]:
				self.note("blocked", "proof-kind-or-platform-mismatch", field, finding)
		if value.get("kind") == "synthetic" or value.get("synthetic"):
			if not self.synthetic:
				self.note("blocked", "synthetic-not-native", "Synthetic observation is not acceptance", finding)
		command, procedure = value.get("command"), value.get("procedure")
		if command is not None:
			if not isinstance(command, list) or not command or len(command)>256 or any(not text(arg) for arg in command):
				self.note("invalid", "invalid-command", "Use an exact bounded argv array", finding)
		elif not text(procedure):
			self.note("blocked", "missing-exact-procedure", proof["requirementId"], finding)
		self.validation_counts(value,proof,finding)
		for field in ("observed","completed"):
			if value.get(field) is not True:
				self.note("blocked", "unobserved-or-incomplete", field, finding)
		if not integer(value.get("exitStatus")) or value.get("exitStatus") != proof["expectedExitStatus"] or value.get("expectedResult") != proof["expectedResult"] or value.get("actualResult") != proof["expectedResult"]:
			self.note("blocked", "required-result-unproven", proof["requirementId"], finding)
		self.references(value.get("artifactReferences"), artifacts, "validation raw observations", finding)

	def row(self, key, outer, proof):
		before = len(self.invalid)+len(self.blocked)
		status = outer.get("status")
		if status is not None and status not in ("planned","qualification-required","qualified","in-progress","ready-for-validation","accepted","blocked","reopened"):
			self.note("invalid", "unsupported-row-status", status, key)
		if status != "accepted":
			self.note("blocked", "row-not-accepted", status, key)
		closure = outer.get("closure") if "closure" in outer else outer
		if closure is None:
			self.note("blocked", "missing-closure", "Required closure evidence remains pending", key)
			return False
		if not isinstance(closure, dict):
			raise Invalid(f"Closure must be an object or pending null: {key}")
		if "findingId" in closure and closure["findingId"] != key:
			raise Invalid(f"Ambiguous nested finding ID: {key}")
		artifact_rows = keyed(closure.get("artifacts",[]),"id","artifacts")
		artifacts = {name:self.artifact(item,key) for name,item in artifact_rows.items()}
		disposition = closure.get("disposition")
		if disposition is None:
			self.note("blocked", "missing-disposition", key, key)
		elif disposition not in DISPOSITIONS or (int(key[3:]) not in PARTIAL and disposition not in ("fixed","superseded-with-proof")):
			self.note("invalid", "unsupported-disposition", disposition, key)
		self.required_text(closure.get("currentRevision"), "currentRevision", key)
		if self.expected_revision is not None and closure.get("currentRevision") != self.expected_revision:
			self.note("blocked", "revision-not-current", "Record is not on required reviewed revision", key)
		changes = closure.get("changeReferences")
		if not isinstance(changes,list) or not changes:
			self.note("blocked", "missing-change-reference", key, key)
		elif len(changes)>256:
			raise Invalid("Change references exceed bound")
		else:
			for ref in changes:
				if not isinstance(ref,dict) or not text(ref.get("description")):
					self.note("invalid", "invalid-change-reference", key, key)
				elif ref.get("type") == "source":
					self.references([ref.get("artifactReference")],artifacts,"source change",key)
					if artifact_rows.get(ref.get("artifactReference"),{}).get("root") != "repository":
						self.note("invalid", "source-reference-not-repository", key, key)
				elif ref.get("type") != "commit" or not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}",str(ref.get("commit",""))):
					self.note("invalid", "invalid-change-reference", "Concrete source/commit needed", key)
		if int(key[3:]) in PARTIAL:
			q = closure.get("qualification")
			if not isinstance(q,dict):
				self.note("blocked", "partial-needs-qualification", key, key)
			else:
				for field in ("condition","decision","currentSourceProof","supportedPathObservations","boundedDecision"):
					self.required_text(q.get(field),"qualification."+field,key)
				if q.get("disposition") != disposition:
					self.note("invalid", "qualification-disposition-mismatch", key, key)
				if not isinstance(q.get("unsupportedClaims"),list) or not q["unsupportedClaims"] or any(not text(x) for x in q["unsupportedClaims"]):
					self.note("blocked", "partial-needs-bounded-claim", key, key)
				self.references(q.get("artifactReferences"),artifacts,"partial qualification",key)
		limitations = closure.get("remainingLimitations")
		if not isinstance(limitations,list) or len(limitations)>256:
			self.note("invalid", "invalid-limitations", "Use explicit {description,required} rows", key)
		else:
			for limitation in limitations:
				if not isinstance(limitation,dict) or not text(limitation.get("description")) or type(limitation.get("required")) is not bool:
					self.note("invalid", "invalid-limitations", key, key)
				elif limitation["required"]:
					self.note("blocked", "required-proof-limited", limitation["description"], key)
		if proof is None:
			self.note("blocked", "missing-proof-row", key, key)
			return False
		validations = keyed(closure.get("validations",[]),"id","validations")
		for required in proof["requiredValidations"]:
			matched = [v for v in validations.values() if v.get("requirementId") == required["requirementId"]]
			if len(matched) != 1:
				self.note("blocked", "required-proof-missing", required["requirementId"], key)
			else:
				self.validation(matched[0],required,artifacts,key)
		prerequisites = closure.get("prerequisites",{})
		if not isinstance(prerequisites,dict):
			raise Invalid("Prerequisites must be a named object")
		for name in proof.get("requiredPrerequisites",[]):
			p = prerequisites.get(name)
			if not isinstance(p,dict) or p.get("status") != "satisfied":
				self.note("blocked", "required-prerequisite-blocked", name, key)
			else:
				self.references(p.get("artifactReferences"),artifacts,"prerequisite "+name,key)
		review = closure.get("acceptanceReview")
		if not isinstance(review,dict):
			self.note("blocked", "missing-acceptance-review", key, key)
		else:
			self.required_text(review.get("reviewer"),"acceptanceReview.reviewer",key)
			if review.get("reviewedRevision") != closure.get("currentRevision") or review.get("acceptanceSha256") != acceptance_hash(self.effective[key]):
				self.note("invalid", "review-revision-or-acceptance-mismatch", key, key)
			if review.get("outcomeSatisfied") is not True or review.get("proofMethodSatisfied") is not True:
				self.note("blocked", "acceptance-not-reviewed", key, key)
			expected = {p["requirementId"] for p in proof["requiredValidations"]}
			covered = review.get("coveredRequirementIds")
			if not isinstance(covered,list) or any(not text(x) for x in covered) or set(covered) != expected:
				self.note("blocked", "review-proof-coverage-missing", key, key)
			self.references(review.get("artifactReferences"),artifacts,"acceptance review",key)
		return len(self.invalid)+len(self.blocked) == before

	def check(self, document):
		if isinstance(document,list):
			rows = document
		elif isinstance(document,dict) and type(document.get("schemaVersion",1)) is int and document.get("schemaVersion",1) == 1:
			rows = document.get("findings")
			if document.get("syntheticFixture") and not self.synthetic:
				self.note("blocked", "synthetic-not-native", "Synthetic closure cannot authorize acceptance")
		else:
			raise Invalid("Closure is a findings wrapper/schema1 or flat array")
		rows = keyed(rows,"findingId","closure")
		if set(rows) != INCLUDED:
			raise Invalid("Closure must contain every exact179 included IDs once; excluded/unknown IDs forbidden")
		requirements = self.requirements()
		complete = sum(self.row(key,rows[key],requirements.get(key)) for key in sorted(rows))
		result = "invalid" if self.invalid else "blocked" if self.blocked else "complete"
		return {"schemaVersion":1,"result":result,"acceptanceGranted":result=="complete" and not self.synthetic,
			"syntheticParserOnly":self.synthetic,"scopeRows":len(rows),"partialRows":21,
			"structurallyCompleteRows":complete,"uniqueArtifactsVerified":len(self.cache),"hashedBytes":self.hashed_bytes,
			"invalid":self.invalid,"blocked":self.blocked}


def fixture_scope(definition):
	"""Only IDs/verdict partitions are portable fixture facts, never real claims."""
	if not isinstance(definition,dict) or type(definition.get("schemaVersion")) is not int or definition["schemaVersion"]!=1:
		raise Invalid("Fixture schemaVersion must be1")
	scope=definition.get("scope")
	if not isinstance(scope,dict):
		raise Invalid("Fixture requires explicit mechanical scope")
	for name,expected in (("includedIds",INCLUDED),("partialIds",{f"CR-{n:03d}" for n in PARTIAL}),("excludedIds",{f"CR-{n:03d}" for n in EXCLUDED})):
		values=scope.get(name)
		if not isinstance(values,list) or any(not text(value) for value in values) or len(values)!=len(expected) or set(values)!=expected:
			raise Invalid("Fixture scope must preserve exact179/21/27 mechanical ID partitions")
	return scope

def fixture_register(definition):
	"""Produce artificial acceptance only for the explicitly synthetic self-test."""
	scope=fixture_scope(definition)
	policy=definition.get("fixtureRegister")
	if not isinstance(policy,dict) or policy.get("isNormativeRegister") is not False or not text(policy.get("acceptanceTemplate")) or not text(policy.get("package")):
		raise Invalid("Fixture register needs an explicitly nonnormative artificial template")
	rows=[]
	for key in scope["includedIds"]:
		number=int(key[3:])
		rows.append({"id":number,"key":key,"verdict":"Partially valid" if number in PARTIAL else "Confirmed",
			"package":policy["package"],"acceptance":policy["acceptanceTemplate"].replace("{findingId}",key)})
	# Reuse the strict mechanical register validator. No real title/claim/source
	# hashes, research conclusions or acceptance prose are copied into fixtures.
	return load_register({"schemaVersion":1,"counts":{"Confirmed":158,"Partially valid":21},
		"excludedFalsePositiveIds":sorted(EXCLUDED),"findings":rows})


def fixtures(register, definition, work):
	"""Materialize all179 rows from the explicit normative register, not a cache."""
	fixture_scope(definition)
	work.mkdir(parents=True,exist_ok=False)
	evidence, repository = work/"evidence",work/"repository"
	evidence.mkdir();repository.mkdir()
	(evidence/"proof.txt").write_text("Synthetic parser observation only; NOT native acceptance.\n",encoding="utf-8")
	(repository/"source.cpp").write_text("// Synthetic source reference only.\n",encoding="utf-8")
	(evidence/"empty-stderr.log").write_bytes(b"")
	artifacts = []
	for name,root,path in (("proof","evidence",evidence/"proof.txt"),("source","repository",repository/"source.cpp")):
		data=path.read_bytes();artifacts.append({"id":name,"root":root,"path":path.name,"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest()})
	rows, requirements = [],[]
	for key,item in sorted(register.items()):
		q = None
		if int(key[3:]) in PARTIAL:
			q={field:"Synthetic parser fixture; not supported-path/native evidence." for field in ("condition","decision","currentSourceProof","supportedPathObservations","boundedDecision")}
			q.update(disposition="fixed",unsupportedClaims=["Synthetic evidence proves no actual finding"],artifactReferences=["proof"])
		closure={"disposition":"fixed","currentRevision":"synthetic-reviewed-revision","changeReferences":[{"type":"source","artifactReference":"source","description":"Synthetic source pin"}],"qualification":q,
			"validations":[{"id":"v1","requirementId":"fixture-proof","kind":"synthetic","command":["synthetic-parser-fixture"],"platform":"Synthetic","configuration":"ParserOnly","selectedCases":1,"executedCases":1,"skippedCases":0,"observed":True,"completed":True,"expectedResult":"Passed","actualResult":"Passed","exitStatus":0,"artifactReferences":["proof"]}],
			"artifacts":copy.deepcopy(artifacts),"prerequisites":{"fixture-only":{"status":"satisfied","artifactReferences":["proof"]}},"remainingLimitations":[],
			"acceptanceReview":{"reviewer":"synthetic-parser-review","reviewedRevision":"synthetic-reviewed-revision","acceptanceSha256":acceptance_hash(item["acceptance"]),"outcomeSatisfied":True,"proofMethodSatisfied":True,"coveredRequirementIds":["fixture-proof"],"artifactReferences":["proof"]}}
		rows.append({"findingId":key,"status":"accepted","closure":closure})
		requirements.append({"findingId":key,"acceptanceSha256":acceptance_hash(item["acceptance"]),"requiredValidations":[{"requirementId":"fixture-proof","kind":"synthetic","platform":"Synthetic","configuration":"ParserOnly","minimumSelected":1,"minimumExecuted":1,"expectedExitStatus":0,"expectedResult":"Passed"}],"requiredPrerequisites":["fixture-only"]})
	return {"schemaVersion":1,"syntheticFixture":True,"findings":rows},{"schemaVersion":1,"syntheticFixture":True,"findings":requirements},evidence,repository


def self_test(register, definition, work, register_mode="explicit-normative-register"):
	cases = definition.get("cases")
	if not isinstance(cases,list) or not cases or len(cases)>256:
		raise Invalid("Nonempty bounded fixture cases required")
	names = set()
	for case in cases:
		if not isinstance(case,dict) or not isinstance(case.get("name"),str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}",case["name"]) or case["name"] in names or case.get("expected") not in ("complete","blocked","invalid","crashed"):
			raise Invalid("Fixture names must be unique bounded filenames with known expected results")
		names.add(case["name"])
	base, projection, evidence, repository = fixtures(register,definition,work)
	results=[]
	for case in cases:
		document, requirements = copy.deepcopy(base),copy.deepcopy(projection)
		rows=document["findings"];c=rows[0]["closure"];v=c["validations"][0];a=c["artifacts"][0]
		partial=next(row["closure"] for row in rows if int(row["findingId"][3:]) in PARTIAL)
		mutation=case["mutation"];synthetic=True;errata=None
		procedure_cases={"observed-procedure-kind","zero-procedures","absent-procedures","unobserved-procedures","fake-procedure-cases","invalid-procedure-floor","skipped-procedures","unknown-count-unit","unknown-observation-kind","native-procedure-requirement","native-procedure-observation","native-relabelled-procedure","case-procedure-counter"}
		if mutation in procedure_cases:
			p=requirements["findings"][0]["requiredValidations"][0]
			if mutation=="case-procedure-counter":
				v["executedProcedures"]=1
			elif mutation in ("native-procedure-observation","native-relabelled-procedure"):
				p["kind"]="gpu-native";v["kind"]="gpu-native" if mutation=="native-procedure-observation" else "compile"
				v["countUnit"]="procedures";v["executedProcedures"]=2
				for field in ("selectedCases","executedCases","skippedCases"):v.pop(field)
			else:
				p.pop("minimumSelected");p.pop("minimumExecuted")
				kind=case.get("value","compile")
				p.update(kind=kind,countUnit="procedures",minimumExecutedProcedures=2)
				v.update(kind=kind,countUnit="procedures",executedProcedures=2)
				for field in ("selectedCases","executedCases","skippedCases"):v.pop(field)
				if mutation=="zero-procedures":v["executedProcedures"]=0
				elif mutation=="absent-procedures":v.pop("executedProcedures")
				elif mutation=="unobserved-procedures":v["observed"]=False
				elif mutation=="fake-procedure-cases":v.update(selectedCases=1,executedCases=1)
				elif mutation=="invalid-procedure-floor":p["minimumExecutedProcedures"]=0
				elif mutation=="skipped-procedures":v["skippedProcedures"]=1
				elif mutation=="unknown-count-unit":p["countUnit"]="assumed-success";v["countUnit"]="assumed-success"
				elif mutation=="unknown-observation-kind":p["kind"]="assumed-build";v["kind"]="assumed-build"
				elif mutation=="native-procedure-requirement":p["kind"]="gpu-native";v["kind"]="gpu-native"
		elif mutation=="partial-disposition":
			for row in rows:
				if row["closure"]["qualification"] is not None:
					row["closure"]["disposition"]=case["value"];row["closure"]["qualification"]["disposition"]=case["value"]
		elif mutation=="confirmed-superseded":c["disposition"]="superseded-with-proof"
		elif mutation=="missing":rows.pop()
		elif mutation=="duplicate":rows.append(copy.deepcopy(rows[0]))
		elif mutation=="excluded":rows[0]["findingId"]="CR-098"
		elif mutation=="unknown":rows[0]["findingId"]="CR-999"
		elif mutation=="bad-disposition":c["disposition"]="looks-fixed"
		elif mutation=="confirmed-narrow":c["disposition"]="supported-path-claim-narrowed"
		elif mutation=="confirmed-bounded":c["disposition"]="bounded-hardening"
		elif mutation=="confirmed-config":c["disposition"]="configuration-or-documentation-corrected"
		elif mutation=="unknown-status":rows[0]["status"]="seems-finished"
		elif mutation=="no-qualification":partial["qualification"]=None
		elif mutation=="no-observation":partial["qualification"].pop("supportedPathObservations")
		elif mutation=="no-projection":requirements=None
		elif mutation=="missing-proof-row":requirements["findings"].pop()
		elif mutation=="duplicate-proof-row":requirements["findings"].append(copy.deepcopy(requirements["findings"][0]))
		elif mutation=="acceptance-drift":requirements["findings"][0]["acceptanceSha256"]="0"*64
		elif mutation=="ready":rows[0]["status"]="ready-for-validation";rows[0]["closure"]=None
		elif mutation=="blocked-prerequisite":c["prerequisites"]["fixture-only"]["status"]="blocked"
		elif mutation=="required-limit":c["remainingLimitations"]=[{"description":"Required proof unavailable","required":True}]
		elif mutation=="unproven-native":requirements["findings"][0]["requiredValidations"][0]["kind"]="gpu-native"
		elif mutation=="zero-selected":v["selectedCases"]=0;v["executedCases"]=0
		elif mutation=="zero-executed":v["executedCases"]=0
		elif mutation=="partial-execution":v["selectedCases"]=2;v["executedCases"]=1
		elif mutation=="skipped":v["skippedCases"]=1;v["executedCases"]=0
		elif mutation=="registered-only":v["executedCases"]=0;v["actualResult"]="Registered"
		elif mutation=="not-observed":v["observed"]=False
		elif mutation=="failed-exit":v["exitStatus"]=1
		elif mutation=="unreviewed":c["acceptanceReview"]["proofMethodSatisfied"]=False
		elif mutation=="missing-artifact":a["path"]="does-not-exist"
		elif mutation=="wrong-hash":a["sha256"]="0"*64
		elif mutation=="wrong-length":a["bytes"]+=1
		elif mutation=="parent-path":a["path"]="../repository/source.cpp"
		elif mutation=="absolute-path":a["path"]="/outside/proof.txt"
		elif mutation=="drive-path":a["path"]="C:/outside/proof.txt"
		elif mutation=="alternate-stream":a["path"]="proof.txt:hidden"
		elif mutation=="oversize-artifact":a["bytes"]=(1<<30)+1
		elif mutation=="unknown-artifact-ref":v["artifactReferences"]=["not-pinned"]
		elif mutation in ("empty-artifact","empty-wrong-size","empty-wrong-hash"):
			# Empty stderr is a valid artifact, alongside the existing nonempty raw
			# observation/source controls. Its hash never implies native execution.
			empty={"id":"empty-stderr","root":"evidence","path":"empty-stderr.log","bytes":0,"sha256":hashlib.sha256(b"").hexdigest()}
			if mutation=="empty-wrong-size":empty["bytes"]=1
			if mutation=="empty-wrong-hash":empty["sha256"]="0"*64
			c["artifacts"].append(empty);v["artifactReferences"].append("empty-stderr")
		elif mutation=="normal-synthetic":synthetic=False
		elif mutation=="unexpected-checker-crash":pass
		elif mutation=="unknown-proof-semantics":requirements["findings"][0]["requiredValidations"][0]["unimplementedMandatoryGpuProof"]=True
		elif mutation=="source-ref-not-repository":c["changeReferences"][0]["artifactReference"]="proof"
		elif mutation in ("explicit-erratum","erratum-drift"):
			outcome="Synthetic corrected acceptance; parser fixture only."
			errata={"items":[{"findingId":rows[0]["findingId"],"currentRequiredOutcome":outcome}]}
			c["acceptanceReview"]["acceptanceSha256"]=acceptance_hash(outcome)
			if mutation=="explicit-erratum":requirements["findings"][0]["effectiveAcceptanceSha256"]=acceptance_hash(outcome)
		else:raise Invalid(f"Unknown fixture mutation: {mutation}")
		try:
			if mutation=="unexpected-checker-crash":raise KeyError("controlled checker crash")
			checker=Checker(register,evidence,repository,requirements,errata=errata,synthetic=synthetic)
			report=checker.check(document)
		except Invalid as error:
			report={"result":"invalid","acceptanceGranted":False,"invalid":[{"detail":str(error)}]}
		except Exception as error:
			# Unexpected exceptions must fail a negative fixture, not impersonate Invalid.
			report={"result":"crashed","acceptanceGranted":False,"crash":type(error).__name__+": "+str(error)}
		# Fixture results are parser observations even when the normal synthetic
		# rejection branch was exercised. Never grant runtime acceptance here.
		report["acceptanceGranted"]=False
		report["registerMode"]=register_mode
		report["normativeAcceptanceValidated"]=False
		results.append({"name":case["name"],"expected":case["expected"],"actual":report["result"],"passed":report["result"]==case["expected"],"acceptanceGranted":False})
		(work/(case["name"]+".report.json")).write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
	return {"schemaVersion":1,"result":"parser-fixtures-passed" if all(r["passed"] for r in results) else "parser-fixtures-failed","acceptanceGranted":False,"syntheticParserOnly":True,"registerMode":register_mode,"normativeAcceptanceValidated":False,"scopeRowsPerPositiveCase":179,"partialRowsPerPositiveCase":21,"selectedCases":len(results),"executedCases":len(results),"failedCases":sum(not r["passed"] for r in results),"cases":results}


def main(argv=None):
	parser=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
	register_input=parser.add_mutually_exclusive_group(required=True)
	register_input.add_argument("--register",type=Path)
	register_input.add_argument("--fixture-register",action="store_true",help="Artificial scope register; valid ONLY with --self-test, never native acceptance")
	parser.add_argument("--closure",type=Path)
	parser.add_argument("--proof-requirements",type=Path)
	parser.add_argument("--acceptance-errata",type=Path)
	parser.add_argument("--evidence-root",type=Path)
	parser.add_argument("--repository-root",type=Path)
	parser.add_argument("--expected-revision")
	parser.add_argument("--report",type=Path)
	parser.add_argument("--self-test",type=Path)
	parser.add_argument("--work-root",type=Path)
	parser.add_argument("--max-artifact-bytes",type=int,default=1<<30)
	parser.add_argument("--max-total-bytes",type=int,default=8<<30)
	args=parser.parse_args(argv);checker=None
	if args.max_artifact_bytes<=0 or args.max_total_bytes<=0:
		parser.error("Evidence byte budgets must be positive")
	if args.fixture_register and not args.self_test:
		parser.error("--fixture-register is valid ONLY with --self-test; normal validation requires --register")
	if args.self_test and (args.closure or args.proof_requirements or args.acceptance_errata):
		parser.error("Self-test is separate from actual closure/proof/errata validation")
	if not args.self_test and (args.closure is None or args.evidence_root is None):
		parser.error("Normal validation requires --closure and --evidence-root")
	protected={Path(__file__).resolve()} | {p.resolve() for p in (args.register,args.closure,args.proof_requirements,args.acceptance_errata,args.self_test) if p is not None}
	try:
		definition=read_json(args.self_test) if args.self_test else None
		register=fixture_register(definition) if args.fixture_register else load_register(read_json(args.register))
		if args.self_test:
			work=args.work_root or Path(tempfile.gettempdir())/("terra-finding-closure-"+os.urandom(8).hex())
			report=self_test(register,definition,work,"synthetic-fixture-scope" if args.fixture_register else "explicit-normative-register")
			exit_status=0 if report["failedCases"]==0 else 2
		else:
			checker=Checker(register,args.evidence_root,args.repository_root,
				read_json(args.proof_requirements) if args.proof_requirements else None,
				read_json(args.acceptance_errata) if args.acceptance_errata else None,
				args.expected_revision,max_artifact=args.max_artifact_bytes,max_total=args.max_total_bytes)
			report=checker.check(read_json(args.closure));exit_status={"complete":0,"blocked":1,"invalid":2}[report["result"]]
	except (Invalid,OSError,ValueError,TypeError,KeyError,RecursionError) as error:
		report={"schemaVersion":1,"result":"invalid","acceptanceGranted":False,"invalid":[{"code":"invalid-input","detail":str(error)[:2048]}]};exit_status=2
	if checker:
		protected.update(checker.read_paths)
	if args.report:
		if args.report.resolve() in protected:
			print("Report would overwrite a normative input or verified artifact",file=sys.stderr);return 2
		args.report.parent.mkdir(parents=True,exist_ok=True)
		args.report.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
	print(json.dumps(report,indent=2))
	return exit_status

if __name__=="__main__":
	raise SystemExit(main())
