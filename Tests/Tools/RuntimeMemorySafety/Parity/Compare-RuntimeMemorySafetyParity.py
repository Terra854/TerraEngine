"""Strict matching-ABI parity receipt comparator; Python standard library.

Native hosts normalize their own actual completed raw image with the existing
matching-ABI ConvertToRgba8. This tool compares every RGBA8 channel (including
alpha) with integer error <=2, exactly 2/255, with no masks or excluded regions.
It does not launch a runtime or claim private reference camera-array visibility.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import struct
import sys
import zlib


class Unverified(Exception):
	pass


class Mismatch(Exception):
	pass


def require(condition, message):
	if not condition:
		raise Unverified(message)


def equal(left, right, message):
	if left != right:
		raise Mismatch(message)


def pairs(items):
	# Duplicate keys and nonfinite values must not change a receipt's meaning.
	result = {}
	for key, value in items:
		require(key not in result, "Duplicate JSON key: " + key)
		result[key] = value
	return result


def read_json(path):
	return json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=pairs,
		parse_constant=lambda token: (_ for _ in ()).throw(Unverified("Nonfinite JSON: " + token)))


def integer(value, message, positive=False):
	require(type(value) is int and value >= (1 if positive else 0), message)
	return value


def safe_path(text):
	path = Path(text)
	require(path.is_absolute(), "Evidence path must be absolute")
	for item in (path, *path.parents):
		require(not item.is_symlink() and not getattr(os.path, "isjunction", lambda _: False)(item), "Redirected evidence path")
	return path


def sha(data):
	return hashlib.sha256(data).hexdigest()


def pin(path):
	path = safe_path(path)
	require(path.is_file(), "Missing evidence file: " + str(path))
	before = path.stat()
	require(before.st_size <= 512 * 1024 * 1024, "Artifact exceeds bounded comparison input")
	data = path.read_bytes()
	after = path.stat()
	require(before.st_size == after.st_size and before.st_mtime_ns == after.st_mtime_ns, "File changed during read")
	return {"path": str(path), "bytes": len(data), "sha256": sha(data)}, data


def verify_pin(value, root=None):
	path = safe_path(value["path"])
	if root is not None:
		require(path.is_relative_to(root), "Host artifact escapes its evidence directory")
	actual, data = pin(path)
	require(actual["bytes"] == integer(value["bytes"], "Invalid pin size") and actual["sha256"] == value["sha256"], "Pinned evidence changed: " + str(path))
	return data


def png(rgba, width, height):
	# PNG is a derivative review artifact. Pixel comparison uses the complete
	# original normalized bytes, so encoding never changes the comparison input.
	def chunk(kind, body):
		return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)
	rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
	return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


def decode_native_png(data, width, height):
	# Decode every native PNG pixel with bounded zlib output and verify CRCs.
	# The engine encoder produces noninterlaced RGBA8; no image library is needed.
	require(data[:8] == b"\x89PNG\r\n\x1a\n", "Invalid final native PNG signature")
	offset = 8; compressed = bytearray(); header = False; ended = False
	while offset < len(data):
		require(offset + 12 <= len(data), "Truncated native PNG chunk")
		size = struct.unpack_from(">I", data, offset)[0]; kind = data[offset + 4:offset + 8]
		require(size <= len(data) - offset - 12, "Invalid native PNG chunk length")
		body = data[offset + 8:offset + 8 + size]
		require(zlib.crc32(kind + body) & 0xffffffff == struct.unpack_from(">I", data, offset + 8 + size)[0], "Native PNG CRC mismatch")
		if kind == b"IHDR":
			require(not header and offset == 8 and size == 13 and struct.unpack(">IIBBBBB", body) == (width, height, 8, 6, 0, 0, 0), "Unsupported final native PNG layout")
			header = True
		elif kind == b"IDAT":
			require(header and not ended, "Misordered native PNG pixels"); compressed.extend(body)
		elif kind == b"IEND":
			require(size == 0 and offset + 12 == len(data), "Native PNG trailer invalid"); ended = True
		else:
			require(kind[0] & 32, "Unknown required native PNG chunk")
		offset += size + 12
	require(header and ended and compressed, "Incomplete final native PNG")
	stride = width * 4; expected = (stride + 1) * height
	decoder = zlib.decompressobj()
	try: raw = decoder.decompress(compressed, expected + 1)
	except zlib.error as error: raise Unverified("Invalid final native PNG compressed data") from error
	require(len(raw) == expected and decoder.eof and not decoder.unconsumed_tail and not decoder.unused_data, "Invalid final native PNG pixel extent")
	output = bytearray(); previous = bytearray(stride)
	for y in range(height):
		start = y * (stride + 1); mode = raw[start]; require(mode <= 4, "Invalid native PNG filter")
		row = bytearray(raw[start + 1:start + 1 + stride])
		for x in range(stride):
			a = row[x - 4] if x >= 4 else 0; b = previous[x]; c = previous[x - 4] if x >= 4 else 0
			if mode == 1: predictor = a
			elif mode == 2: predictor = b
			elif mode == 3: predictor = (a + b) // 2
			elif mode == 4:
				v = a + b - c; pa, pb, pc = abs(v - a), abs(v - b), abs(v - c)
				predictor = a if pa <= pb and pa <= pc else b if pb <= pc else c
			else: predictor = 0
			row[x] = (row[x] + predictor) & 255
		output.extend(row); previous = row
	return bytes(output)


def normal_output_proof(value, definition, root):
	coverage = value["normalOutputCoverage"]; authored_count = definition["cameraCount"]; count = authored_count
	for field, expected in (("authoredCameraCount", authored_count), ("normalQueuedCameraCommands", authored_count), ("backendSnapshotCapacity", 8),
		("expectedAdmittedCameraCommands", count), ("actualRecordedCameraCommands", count), ("actualNormalPublishedViewCount", 1), ("actualCommandCount", count),
		("lightingPurposeArtifactCount", 2 * integer(coverage["lightingArtifactCommandCount"], "Invalid actual artifact command count", True))):
		require(integer(coverage[field], "Invalid normal output coverage", True) == expected, "Incomplete normal command/output coverage")
	require(coverage["perCameraImageCoverage"] is False and coverage["privateReferenceArraysObserved"] is False
		and coverage["queuePolicy"] == "AllAuthoredCamerasInCapturedOrderSharedGameOutput2", "Unsupported normal output coverage claim")
	require(1 <= coverage["lightingArtifactCommandCount"] <= count and coverage["capacityPolicy"] == "EditorRequiredOutputOverflowSerializesAndReusesEightPhysicalSlices"
		and coverage["capacityLimit"] == "Editor required-output overflow submits and completes a prefix, then reuses the same eight physical snapshot slices", "Unsupported inherited editor capacity policy")
	final = value["finalNormalOutput"]
	require(final["policy"] == "EditorNormalGameViewAfterAllAuthoredCameraSubmissions" and final["frameId"] == value["selectedFrameSerial"]
		and final["ok"] is True and final["backendReadback"] is True and final["deterministicFallback"] is False and final["pendingAsync"] is False
		and final["freshness"] == "latestCompleted" and final["dedicatedRenderTarget"] is False and final["allowBackendFallback"] is False
		and final["overlaysIncluded"] is False and final["size"] == [1280, 720] and final["mimeType"] == "image/png", "Final normal output is missing or substituted")
	present = final["presentation"]
	require(present["submitted"] is True and type(present["nativeResult"]) is int and present["nativeResult"] == 0, "Final normal editor frame was not presented successfully")
	integer(present["submissionSerial"], "Missing final editor presentation receipt", True)
	pixels = decode_native_png(verify_pin(final["png"], root), 1280, 720)
	snapshots = final["snapshotDiagnostics"]
	require(snapshots["valid"] is True and integer(snapshots["maxSnapshotsPerFrame"], "Missing actual snapshot capacity", True) == 8,
		"Final normal output lacks actual snapshot diagnostics")
	for field in ("peakSnapshotCount", "activeSnapshotCount", "completedSnapshotCount"):
		integer(snapshots[field], "Invalid native snapshot counter")
	require(type(snapshots["fallbackEvents"]) is list, "Invalid actual snapshot high-water/fallback evidence")
	serialized = 0
	for event in snapshots["fallbackEvents"]:
		require(event["kind"] == 2 and event["reason"] == 1 and event["result"] == 1 and event["outputId"] == 2 and event["required"] is True,
			"Unexpected normal snapshot fallback")
		serialized += 1
	require(serialized == (count - 1) // 8, "Inherited editor serialization was not actually observed")
	metadata = value["normalCommandMetadata"]; require(type(metadata) is list and len(metadata) == 2 * count, "Incomplete normal camera command metadata")
	semantics = []; serials = []; available_commands = set()
	for index, row in enumerate(metadata):
		ordinal = index // 2; purpose = bool(index % 2)
		integer(row["commandOrdinal"], "Invalid command ordinal"); integer(row["status"], "Invalid native metadata status")
		require(row["commandOrdinal"] == ordinal and row["analysisPurpose"] is purpose and row["frameSerial"] == value["selectedFrameSerial"]
			and row["viewType"] == 0 and row["viewId"] == 2 and row["preparationSettled"] is True, "Misordered native command metadata")
		serial = integer(row["commandSerial"], "Missing native command serial", True)
		if not purpose:
			require(not serials or serial > serials[-1], "Native command order regressed"); serials.append(serial)
		else: require(serial == serials[-1], "Lighting purposes name different native commands")
		for key in ("fixtureControlGeneration", "settingsGeneration", "optionsSessionGeneration"):
			integer(row[key], "Missing native command provenance", True)
		require(row["fixtureControlGeneration"] == value["fixtureControlAdmission"]["controlGeneration"]
			and row["settingsGeneration"] == value["fixtureControlAdmission"]["settingsGeneration"], "Native metadata control/settings join differs")
		if row["status"] == 1:
			available_commands.add(ordinal)
			require(row["reason"] == "" and row["hasArtifacts"] is True, "Valid native lighting command has no artifact")
			integer(row["submitSerial"], "First lighting command not submitted", True); integer(row["completionSerial"], "First lighting command incomplete", True)
		else:
			require(row["status"] == 2 and row["reason"] == "Duplicate native lighting purpose observation." and row["hasArtifacts"] is False
				and row["submitSerial"] == 0 and row["completionSerial"] == 0, "Unexpected unavailable native command metadata")
		semantics.append({key: row[key] for key in ("commandOrdinal", "analysisPurpose", "status", "reason", "viewType", "viewId", "hasArtifacts")})
	for index in range(count):
		require(metadata[2 * index]["status"] == metadata[2 * index + 1]["status"], "Incomplete actual lighting purpose pair")
	require(0 in available_commands and len(available_commands) == coverage["lightingArtifactCommandCount"], "Actual complete lighting artifact command count differs")
	return coverage, final, pixels, semantics, serials


def pixel_compare(left, right):
	equal(len(left), len(right), "Normalized image byte counts differ")
	maximum = 0
	failed = 0
	first = None
	for index, (a, b) in enumerate(zip(left, right)):
		error = abs(a - b)
		maximum = max(maximum, error)
		if error > 2:
			failed += 1
			if first is None:
				first = {"pixel": index // 4, "channel": index % 4, "reference": a, "candidate": b}
	return {"comparedPixels": len(left) // 4, "comparedChannels": len(left), "maximumChannelError": maximum,
		"maximumNormalizedChannelError": maximum / 255, "channelsExceedingTolerance": failed, "firstFailure": first}


def view_semantics(value):
	require(value["valid"] is True and value["dedicatedCaptureOutput"] is False, "Missing normal published view")
	require(value["outputKind"] in (0, 2) and len(value["renderSize"]) == 2, "Unexpected normal output kind/extent")
	for name in ("publicationFrameId", "publicationGeneration", "rendererSessionGeneration"):
		integer(value[name], "Missing actual view publication identity", True)
	ref = value["referenceFrame"]
	require(type(ref) is dict, "Missing published render reference frame")
	integer(ref["frameId"], "Missing reference frame identity", True)
	require(ref["outputId"] == value["outputId"], "Published reference/output identity mismatch")
	for dimension in value["renderSize"]:
		integer(dimension, "Invalid published view extent", True)
	# Frame/session counters are provenance within each independent process.
	# View identity, origin and complete output owner descriptors are semantic.
	return {"outputKind": value["outputKind"], "outputId": value["outputId"], "renderSize": value["renderSize"],
		"referenceViewId": ref["viewId"], "referenceOutputId": ref["outputId"], "canonicalOrigin": ref["canonicalOrigin"],
		"snapshotOwners": value["snapshotOwners"]}


def candidate_scratch(value, authored, selected, root):
	# This proof is mandatory only for Candidate. The baseline is compiled with
	# archived headers and never names these new native types or callbacks.
	proof = value["candidateScratch"]
	require(proof["typedIdentityOrderComponentBitsEqual"] is True, "Candidate did not compare actual typed capture with independent expected inputs")
	verify_pin(proof["expectedInputs"], root); verify_pin(proof["capturedInputs"], root)
	expected = read_json(Path(proof["expectedInputs"]["path"]))
	captured = read_json(Path(proof["capturedInputs"]["path"]))
	require(expected["observationKind"] == "independentProtectedQuery" and captured["observationKind"] == "actualNativeCapturedArrays", "Wrong candidate array evidence provenance")
	for kind in ("cameras", "lights"):
		authored_components = [{key: item for key, item in row.items() if key != "pose"} for row in authored[kind]]
		equal(expected[kind], authored_components, "Independent candidate component oracle differs from protected authored rows")
		equal(captured[kind], expected[kind], "Actual candidate captured component values/UUIDs/order differ")
	capture = proof["capturedEpisode"]
	terminal = proof["terminalEpisode"]
	for episode in (capture, terminal):
		for field in ("lifetimeId", "episodeId", "frameId", "capacityBytes", "usedBytes", "peakBytes", "lifetimeHighWaterBytes", "reuseAllocationCount", "fallbackAllocationCount", "resetCount", "outcomeCode"):
			integer(episode[field], "Missing actual candidate scratch episode field")
		require(episode["lifetimeId"] > 0 and episode["episodeId"] > 0 and episode["frameId"] == selected
			and episode["capacityBytes"] == 16 * 1024 * 1024 and episode["usedBytes"] > 0
			and episode["peakBytes"] >= episode["usedBytes"] and episode["lifetimeHighWaterBytes"] >= episode["peakBytes"], "Candidate scratch episode is not the selected actual frame")
	for field in ("lifetimeId", "episodeId", "frameId", "capacityBytes", "resetCount"):
		equal(capture[field], terminal[field], "Candidate capture/terminal episode identity changed")
	require(capture["outcomeCode"] == 1 and terminal["outcomeCode"] == 2
		and terminal["usedBytes"] >= capture["usedBytes"] and terminal["fallbackAllocationCount"] >= capture["fallbackAllocationCount"], "Candidate selected episode did not complete with preserved capture pressure")
	reclamation = proof["selectedFrameReclamation"]
	retired = proof["retirementReclamation"]
	for counters in (reclamation, retired):
		for field in ("lifetimeId", "totalFallbackAllocationCount", "totalFallbackDeallocationCount", "outstandingFallbackAllocationCount", "outstandingFallbackBytes", "resetCount"):
			integer(counters[field], "Missing actual candidate reclamation field")
		require(counters["lifetimeId"] == terminal["lifetimeId"]
			and counters["totalFallbackAllocationCount"] >= counters["totalFallbackDeallocationCount"]
			and counters["outstandingFallbackAllocationCount"] == counters["totalFallbackAllocationCount"] - counters["totalFallbackDeallocationCount"], "Candidate reclamation identity/accounting inconsistent")
		require((counters["outstandingFallbackAllocationCount"] == 0) == (counters["outstandingFallbackBytes"] == 0), "Candidate outstanding fallback count/bytes disagree")
	require(reclamation["resetCount"] == terminal["resetCount"] and reclamation["totalFallbackAllocationCount"] >= terminal["fallbackAllocationCount"], "Candidate selected-frame reclamation does not join its terminal episode")
	require(retired["resetCount"] >= reclamation["resetCount"] and retired["totalFallbackAllocationCount"] >= reclamation["totalFallbackAllocationCount"]
		and retired["totalFallbackAllocationCount"] == retired["totalFallbackDeallocationCount"] and retired["outstandingFallbackBytes"] == 0, "Candidate actual shutdown did not balance fallback ownership")
	return {"capturedArraysEqual": True, "actualFallbackAllocationCount": terminal["fallbackAllocationCount"],
		"expectedInputs": proof["expectedInputs"], "capturedInputs": proof["capturedInputs"], "capturedEpisode": capture,
		"terminalEpisode": terminal, "selectedFrameReclamation": reclamation, "retirementReclamation": retired}


def load_host(path, variant, definition, manifest_hash):
	path = safe_path(path)
	receipt_pin, _ = pin(path)
	value = read_json(path)
	require(value["schemaVersion"] == "runtime-memory-safety-parity-prototype-v3" and value["variant"] == variant, "Wrong matching-ABI host protocol/variant")
	require(value["status"] == "Captured" and value["normalShutdownCompleted"] is True and value["pausedPlayWithoutExplicitStep"] is True,
		"Host did not capture and close normally in paused play")
	require(value["nativeAcceptance"] is False and value["performanceAcceptance"] is False and value["timedPostImportAssetClosureParityVerified"] is False,
		"Host receipt claims unsupported acceptance/timed closure")
	require(value["fixtureScale"] == definition["id"], "Fixture scale mismatch")
	require(value["imageNormalization"] == "Matching-ABI VulkanReadback::ConvertToRgba8, tightly packed RGBA8; no excluded regions", "Unsupported normalization")
	for field in ("hostExecutable", "archiveReceipt", "supportManifest", "loadedEngine", "expectedEngine", "materialization", "qualification", "rendererSourceProof"):
		verify_pin(value[field])
	equal(value["loadedEngine"]["sha256"], value["expectedEngine"]["sha256"], "Host used a different engine DLL")
	source = read_json(Path(value["rendererSourceProof"]["path"]))
	old_source = verify_pin(source["referenceSource"]); verify_pin(source["candidateSource"])
	capacity_header = verify_pin(source["referenceCompatibilityCapacityHeader"])
	capacity_pattern = rb"static\s+constexpr\s+uint32_t\s+MAX_RENDER_SNAPSHOTS_PER_FRAME\s*=\s*8u\s*;"
	require(source["backendSnapshotCapacity"] == 8 and re.search(capacity_pattern, capacity_header), "Reference compatibility snapshot capacity differs")
	required_sources = {"VulkanContextImpl.h", "VulkanFrameRendering.cpp", "VulkanShadowValidationReadback.cpp", "VulkanReadback.cpp", "VulkanEditorRendering.cpp"}
	require({Path(extra["sourcePath"]).name for extra in source["additionalSources"]} == required_sources, "Missing inherited normal output/capacity source proof")
	for extra in source["additionalSources"]:
		archived = verify_pin(extra["referenceSource"]); verify_pin(extra["candidateSource"])
		if extra["sourcePath"].endswith("/VulkanContextImpl.h"):
			require(re.search(capacity_pattern, archived) and re.search(capacity_pattern, verify_pin(extra["candidateSource"])) and archived.replace(b"\r\n", b"\n") == capacity_header.replace(b"\r\n", b"\n"), "Old/current/compatibility backend snapshot capacity differs")
		require(hashlib.sha1(b"blob " + str(len(archived)).encode() + b"\0" + archived).hexdigest() == extra["referenceGitBlob"], "Archived output/readback source blob differs")
	require(source["sourceRevision"] == read_json(Path(value["archiveReceipt"]["path"]))["sourceRevision"]
		and hashlib.sha1(b"blob " + str(len(old_source)).encode() + b"\0" + old_source).hexdigest() == source["referenceGitBlob"], "Renderer source proof does not join actual archived source bytes")
	# Run identity belongs to the explicit native/archive receipt and generated
	# source proof. Portable source declarations contain no historical run path.
	for field in ("referenceReceipt", "generator", "gitExecutable"):
		verify_pin(source["provenance"][field])
	reference_receipt = source["provenance"]["referenceReceipt"]
	equal({key: reference_receipt[key] for key in ("sha256", "bytes")},
		{key: value["archiveReceipt"][key] for key in ("sha256", "bytes")}, "Generated source proof uses a different explicit archive receipt")
	require(Path(reference_receipt["path"]).samefile(Path(value["archiveReceipt"]["path"])), "Generated source proof archive path differs from native input")
	support_path = Path(value["supportManifest"]["path"])
	support = read_json(support_path)
	require(support["schemaVersion"] == 1 and support["sourceRevision"] == source["sourceRevision"]
		and len(support["support"]) == 2, "Portable common-ABI support source identity differs")
	expected_support = {"RendererEfficiencyArtifacts.h", "RendererEfficiencyFixtureControls.h"}
	seen_support = set()
	for item in support["support"]:
		declared = Path(item["path"])
		# Legacy pinned absolute support manifests remain readable; newly shipped
		# manifests use relative paths contained in their installed package.
		resolved = declared if declared.is_absolute() else support_path.parent / declared
		if not declared.is_absolute():
			require(".." not in declared.parts and resolved.is_relative_to(support_path.parent), "Portable support declaration escapes its package")
		name = resolved.name
		require(name in expected_support and name not in seen_support and item["sourceRevision"] == source["sourceRevision"]
			and item["source"] == "Tests/Support/RendererEfficiency/Renderer/" + name, "Unexpected common support declaration")
		seen_support.add(name)
		data = verify_pin({**item, "path": str(resolved)})
		require(hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest() == item["gitBlob"], "Common ABI support bytes do not match source Git blob")
	closure = value["runtimeClosure"]["matchingClosure"]
	require(len(closure) >= 10, "Missing actual runtime closure inventory")
	for row in closure:
		verify_pin(row["original"]); verify_pin(row["staged"])
		equal(row["original"]["sha256"], row["staged"]["sha256"], "Staged runtime bytes differ")
	materialization = read_json(Path(value["materialization"]["path"]))
	qualification = read_json(Path(value["qualification"]["path"]))
	require(qualification["schemaVersion"] == "runtime-memory-safety-qualified-runtime-v1" and qualification["status"] == "Qualified"
		and qualification["policy"] == "FullActualPostImportReferenceRuntimeClosure" and qualification["scale"] == definition["id"], "Missing approved full post-import qualification")
	for field in ("originalMaterialization", "referenceClosureReceipt", "referenceCapture", "referenceLifecycle", "originalPackageManifest"):
		verify_pin(qualification[field])
	equal(qualification["originalMaterialization"]["sha256"], value["materialization"]["sha256"], "Qualification changed original materialization")
	runtime_paths = {
		'Build/Assets/.terra-animation-manifests/da35202f311bd5ea56e23beb0aafbd9a.json',
		'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_493d9bc3.teanim',
		'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_fad19fd5.teanim',
		'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_9beae902.teskel',
		'Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_b7d2176c.teskel',
		'Build/Assets/Models/ImportedHero_Generated/ImportedHeroBody.temf',
		'Build/Assets/Models/ImportedHero_Generated/ImportedHeroMaterial.tmat',
		'ProjectSettings/RenderSettings.json',
		'Assets/Scenes/Scene.tescene',
	}
	require(len(qualification["files"]) == 9 and {row["relative"] for row in qualification["files"]} == runtime_paths, "Incomplete qualified runtime path set")
	qualification_semantics = {}
	for row in qualification["files"]:
		verify_pin(row["source"]); verify_pin(row["qualified"])
		equal(row["source"]["sha256"], row["qualified"]["sha256"], "Qualified payload differs from observed reference")
		qualification_semantics[row["relative"]] = {key: row["qualified"][key] for key in ("sha256", "bytes")}
	equal(qualification_semantics["Assets/Scenes/Scene.tescene"]["sha256"], materialization["generatedHashes"]["Assets/Scenes/Scene.tescene"], "Qualification changed frozen authored scene")
	geometry = value["qualifiedGeometryValidation"]
	verify_pin(geometry["payload"])
	require(geometry["complete"] is True and geometry["code"] == 0 and geometry["diagnostic"] == "" and geometry["headerBytes"] == 192
		and geometry["formatContractOffset"] == 168 and geometry["formatContractId"] == "TEMFPRD1", "Qualified geometry lacks matching-runtime canonical production validation")
	equal(materialization["fixtureManifestSha256"], manifest_hash, "Host materialization uses a different frozen fixture manifest")
	# Distinct installed bootstrap executables are provenance, like distinct host
	# binaries/import libraries. Their actual emitted input bytes must match;
	# executable identity is retained in each pinned materialization receipt.
	materialization_semantics = {key: materialization[key] for key in ("kind", "fixtureId", "seed", "scale", "generatorSha256", "bootstrapManifestSha256", "inputClosure", "generatedHashes", "expectedCameraCount", "expectedLightCount")}
	assets = value["registeredCookedAssets"]
	require(len(assets) == 7 and len({row["kind"] for row in assets}) == 4 and len({row["id"] for row in assets}) == 7, "Incomplete registered original/canonical GUID closure")
	for row in assets:
		verify_pin(row["payload"])
	asset_semantics = [{"kind": row["kind"], "id": row["id"], "bytes": row["payload"]["bytes"], "sha256": row["payload"]["sha256"]} for row in assets]
	authored_bytes = verify_pin(value["authoredInputs"], path.parent)
	authored = read_json(Path(value["authoredInputs"]["path"]))
	require(authored["observationKind"] == "protectedAuthoredInputProjection", "Authored projection incorrectly labelled")
	require(len(authored["cameras"]) == definition["cameraCount"] and len(authored["lights"]) == definition["lightCount"], "Incomplete authored cohort")
	for kind, prefix in (("cameras", "066c000000000000"), ("lights", "066d000000000000")):
		for index, row in enumerate(authored[kind]):
			require(row["ordinal"] == index and row["objectId"] == prefix + format(index + 1, "016x"), "Authored identity/order differs from frozen fixture")
	light_ids = {row["objectId"] for row in authored["lights"]}
	selected = integer(value["selectedFrameSerial"], "Missing selected completed frame", True)
	scratch = None
	if variant == "Candidate":
		scratch = candidate_scratch(value, authored, selected, path.parent)
	else:
		require("candidateScratch" not in value, "Archived baseline cannot claim new candidate scratch API evidence")
	admission = value["fixtureControlAdmission"]
	require(admission["operationId"] == "runtime-memory-safety-parity", "Wrong native fixture admission")
	for field in ("controlGeneration", "settingsGeneration", "rendererSessionGeneration"):
		integer(admission[field], "Missing native fixture admission identity", True)
	coverage, final_output, final_pixels, command_semantics, command_serials = normal_output_proof(value, definition, path.parent)
	rows = value["actualRows"]
	require(type(rows) is list and 0 < len(rows) <= 8192, "Missing or unbounded actual evidence rows")
	selected_rows = []
	seen = set()
	commands = []
	previous_batch = 0
	for ordinal, row in enumerate(rows):
		integer(row["observedOrdinal"], "Invalid observed row ordinal")
		integer(row["viewType"], "Invalid observed view type")
		integer(row["viewId"], "Invalid observed view identity", True)
		require(row["observedOrdinal"] == ordinal and row["preparationSettled"] is True, "Misordered or unsettled actual row")
		for name in ("batchSequence", "frameSerial", "commandSerial", "submitSerial", "completionSerial", "fixtureControlGeneration"):
			integer(row[name], "Missing actual command completion/control identity", True)
		require(row["batchSequence"] >= previous_batch, "Evidence batch order regressed")
		previous_batch = row["batchSequence"]
		require(row["fixtureOperationId"] == "runtime-memory-safety-parity" and type(row["analysisPurpose"]) is bool, "Wrong operation or purpose")
		require(row["fixtureControlGeneration"] == admission["controlGeneration"], "Row belongs to another native control generation")
		require(row["viewType"] == 0 and row["viewId"] == 2, "Evidence is not a normal authored Game view")
		key = (row["frameSerial"], row["commandSerial"], row["analysisPurpose"])
		require(key not in seen, "Duplicate actual command/purpose")
		seen.add(key)
		count = integer(row["activeRecordCount"], "Invalid active light record count")
		require(0 < count <= 1024 and len(row["payloadLightIds"]) == count and len(set(row["payloadLightIds"])) == count
			and set(row["payloadLightIds"]).issubset(light_ids) and row["fullSourceCandidateCount"] == definition["lightCount"], "Actual GPU light ID membership/count unavailable")
		upload = row["upload"]
		require(upload["available"] is True and upload["succeeded"] is True and type(upload["analysisPurpose"]) is bool
			and upload["analysisPurpose"] == row["analysisPurpose"], "Upload purpose or native success missing")
		for name in ("allocationId", "allocationGeneration", "allocationSizeBytes", "regionSizeBytes"):
			integer(upload[name], "Missing actual upload allocation receipt", True)
		integer(upload["regionOffsetBytes"], "Invalid upload region offset")
		require(upload["regionSizeBytes"] >= 176 * 1024
			and upload["regionOffsetBytes"] + upload["regionSizeBytes"] <= upload["allocationSizeBytes"], "Invalid physical upload bounds")
		payload = verify_pin(row["payload"], path.parent)
		require(len(payload) == 176 * 1024 and not any(payload[count * 176:]), "Incomplete lighting buffer or nonzero inactive tail")
		raw = verify_pin(row["color"], path.parent)
		rgba = verify_pin(row["normalizedRgba8"], path.parent)
		require(len(row["colorExtent"]) == 2, "Missing actual color dimensions")
		width, height = (integer(d, "Invalid color extent", True) for d in row["colorExtent"])
		bpp = {37: 4, 43: 4, 44: 4, 50: 4, 97: 8}.get(row["colorSourceFormat"])
		require(bpp is not None and len(raw) == width * height * bpp and len(rgba) == width * height * 4, "Native/normalized image size or format invalid")
		if row["frameSerial"] == selected:
			if row["commandSerial"] not in commands:
				commands.append(row["commandSerial"])
			selected_rows.append((row, payload, rgba, command_serials.index(row["commandSerial"])))
	require(len(commands) == coverage["lightingArtifactCommandCount"] and len(selected_rows) == 2 * len(commands) and commands[0] == command_serials[0], "Selected frame lacks complete actually available lighting payload cohort")
	expected_artifact_commands = {command_serials[index] for index in range(definition["cameraCount"])
		if value["normalCommandMetadata"][2 * index]["status"] == 1}
	require(set(commands) == expected_artifact_commands, "Artifact-bearing native commands differ from actual metadata availability")
	for row, _, _, ordinal in selected_rows:
		metadata = value["normalCommandMetadata"][2 * ordinal + int(row["analysisPurpose"])]
		for field in ("frameSerial", "commandSerial", "submitSerial", "completionSerial", "viewType", "viewId", "fixtureControlGeneration"):
			require(row[field] == metadata[field], "Actual lighting artifact does not join its native command metadata")
	for command in commands:
		purpose_rows = [row for row, _, _, _ in selected_rows if row["commandSerial"] == command]
		require({row["analysisPurpose"] for row in purpose_rows} == {False, True}, "Selected command lacks both lighting purposes")
		require(len({(row["submitSerial"], row["completionSerial"], row["color"]["sha256"], tuple(row["colorExtent"])) for row in purpose_rows}) == 1,
			"Purposes do not join the same completed command and image")
	return {"pin": receipt_pin, "receipt": value, "materialization": materialization_semantics, "assets": asset_semantics,
		"authored": authored, "authoredSha256": sha(authored_bytes), "rows": selected_rows,
		"before": view_semantics(value["viewBefore"]), "after": view_semantics(value["viewAfter"]), "candidateScratch": scratch,
		"qualification": qualification_semantics, "qualificationSource": qualification["referenceClosureReceipt"],
		"coverage": coverage, "finalOutput": final_output, "finalPixels": final_pixels, "commandSemantics": command_semantics}


def compare(args):
	manifest_path = safe_path(args.fixture_manifest)
	manifest_pin, _ = pin(manifest_path)
	manifest = read_json(manifest_path)
	require(manifest["imageNormalizedChannelError"] == 2 / 255 and manifest["allowExcludedImageRegions"] is False, "Frozen pixel tolerance/region policy differs")
	definitions = [row for row in manifest["scales"] if row["id"] == args.scale]
	require(len(definitions) == 1, "Unknown frozen scale")
	a = load_host(args.baseline, "Baseline", definitions[0], manifest_pin["sha256"])
	b = load_host(args.candidate, "Candidate", definitions[0], manifest_pin["sha256"])
	equal(a["receipt"]["archiveReceipt"]["sha256"], b["receipt"]["archiveReceipt"]["sha256"], "Reference archive join differs")
	equal(a["receipt"]["supportManifest"]["sha256"], b["receipt"]["supportManifest"]["sha256"], "Support headers differ")
	equal(a["qualificationSource"]["sha256"], b["qualificationSource"]["sha256"], "Qualified reference closure receipt differs")
	for key in ("materialization", "qualification", "assets", "authored", "before", "after"):
		equal(a[key], b[key], "Exact parity mismatch: " + key)
	equal(a["receipt"]["rendererSourceProof"]["sha256"], b["receipt"]["rendererSourceProof"]["sha256"], "Renderer source proof differs")
	equal(a["coverage"], b["coverage"], "Normal shared output coverage differs")
	equal(a["finalOutput"]["snapshotDiagnostics"], b["finalOutput"]["snapshotDiagnostics"], "Inherited editor snapshot fallback facts differ")
	equal(a["commandSemantics"], b["commandSemantics"], "Ordered actual normal camera command metadata differs")
	final_image = pixel_compare(a["finalPixels"], b["finalPixels"])
	comparisons = []
	for ordinal, ((ar, ap, ac, ai), (br, bp, bc, bi)) in enumerate(zip(a["rows"], b["rows"])):
		for key in ("viewType", "viewId", "analysisPurpose", "payloadLightIds", "activeRecordCount", "fullSourceCandidateCount", "colorExtent", "colorSourceFormat"):
			equal(ar[key], br[key], "Ordered cohort metadata mismatch: " + key)
		equal(ai, bi, "Command cohort order differs")
		equal(ap, bp, "Actual full lighting payload bytes differ")
		image = pixel_compare(ac, bc)
		width, height = ar["colorExtent"]
		review_images = {}
		for label, rgba in (("baseline", ac), ("candidate", bc)):
			image_path = args.output / (str(ordinal) + "-" + label + ".png")
			image_path.write_bytes(png(rgba, width, height))
			review_images[label] = pin(image_path)[0]
		comparisons.append({"cohortOrdinal": ai, "observedRowOrdinal": ordinal, "analysisPurpose": ar["analysisPurpose"],
			"viewType": ar["viewType"], "viewId": ar["viewId"], "nativeExtent": ar["colorExtent"], "nativeFormat": ar["colorSourceFormat"],
			"baselineRawColor": ar["color"], "candidateRawColor": br["color"], "baselineRgba8": ar["normalizedRgba8"], "candidateRgba8": br["normalizedRgba8"],
			"baselineCommand": {key: ar[key] for key in ("frameSerial", "commandSerial", "submitSerial", "completionSerial")},
			"candidateCommand": {key: br[key] for key in ("frameSerial", "commandSerial", "submitSerial", "completionSerial")}, "image": image, "reviewImages": review_images})
	result = {"schemaVersion": "runtime-memory-safety-parity-comparison-v1", "status": "ComparedEqual", "scale": args.scale,
		"baseline": a["pin"], "candidate": b["pin"], "fixtureManifest": manifest_pin, "pixelTolerance": {"integerChannelError": 2, "denominator": 255, "excludedRegions": []},
		"qualifiedActualPostImportRuntimeClosure": a["qualification"], "qualificationSource": a["qualificationSource"],
		"rendererSourceProof": a["receipt"]["rendererSourceProof"], "normalCommandCohortEqual": True,
		"normalOutputCoverage": a["coverage"], "baselineFinalNormalOutput": a["finalOutput"], "candidateFinalNormalOutput": b["finalOutput"],
		"finalNormalOutputImage": final_image,
		"authoredCameraLightValuesIdentityAndOrderEqual": True, "actualOrderedGpuLightPayloadAndIdsEqual": True,
		"privateReferenceCapturedCameraArraysObserved": False, "candidateCapturedArraysRequireSeparateNativeRegression": True,
		"candidateCapturedArraysObservedAndEqual": b["candidateScratch"]["capturedArraysEqual"],
		"actualCandidateFallbackAllocationCount": b["candidateScratch"]["actualFallbackAllocationCount"], "candidateScratch": b["candidateScratch"],
		"cameraOutputJoin": "All authored camera command metadata in native serial order plus final completed shared Game View; common reference API exposes no private captured arrays or per-command camera UUID",
		"nativeAcceptance": False, "performanceAcceptance": False, "comparisons": comparisons}
	if final_image["channelsExceedingTolerance"] or any(row["image"]["channelsExceedingTolerance"] for row in comparisons):
		result["status"] = "Different"
	return result


def main():
	parser = argparse.ArgumentParser(description=__doc__)
	for name in ("baseline", "candidate", "fixture-manifest", "output"):
		parser.add_argument("--" + name, required=True, type=Path)
	parser.add_argument("--scale", required=True, choices=("small", "normal", "stress"))
	args = parser.parse_args()
	args.output = safe_path(args.output)
	require(not args.output.exists() and args.output.parent.is_dir(), "Comparison output must be a fresh directory")
	for source in (args.baseline, args.candidate, args.fixture_manifest):
		require(not args.output.is_relative_to(safe_path(source).parent), "Comparison output overlaps input evidence")
	args.output.mkdir()
	try:
		result = compare(args)
	except Mismatch as error:
		result = {"status": "Different", "reason": str(error), "nativeAcceptance": False, "performanceAcceptance": False}
	except (Unverified, KeyError, TypeError, ValueError, OSError) as error:
		result = {"status": "Unverified", "reason": str(error), "nativeAcceptance": False, "performanceAcceptance": False}
	result["comparator"] = pin(Path(__file__).absolute())[0]
	(args.output / "comparison.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
	return 0 if result["status"] == "ComparedEqual" else 1 if result["status"] == "Different" else 2


if __name__ == "__main__":
	try:
		sys.exit(main())
	except (Unverified, OSError) as error:
		print(str(error), file=sys.stderr)
		sys.exit(2)
