"""Check first-frame draw readiness in an installed editor with a disposable project.

Use a cooked scene with distinct, visible meshes and a stationary camera. Set
the minimum draw count from the known scene, or from its settled render stats.
"""

import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
import urllib.request


def check_scene(editor, project, scene_name, view, minimum_draws):
	with tempfile.TemporaryDirectory(prefix="terra-startup-") as temp:
		return observe_launch(editor, project, scene_name, view, minimum_draws, Path(temp))


def observe_launch(editor, project, scene_name, view, minimum_draws, temp):
	with socket.socket() as listener:
		listener.bind(("127.0.0.1", 0))
		port = listener.getsockname()[1]
	prefs = temp / "preferences.json"
	prefs.write_text(json.dumps({
		"recentProjects": [], "autoBuildScripts": False,
		"editorMcpEnabled": True, "editorMcpListenPort": port,
		"editorMcpWriteModeDefault": "read_only",
		"rendererDiagnosticsPreferencesInitialized": True,
		"vulkanValidationLayers": False, "renderDebugDiagnostics": False,
	}), encoding="utf-8")
	environment = dict(os.environ, TERRAENGINE_EDITOR_PREFS_SESSION_PATH=str(prefs),
		TERRAENGINE_SUPPRESS_PROJECT_RECENTS="1")

	def call(name, **arguments):
		body = json.dumps(dict(jsonrpc="2.0", id=1, method="tools/call",
			params=dict(name=name, arguments=arguments))).encode()
		request = urllib.request.Request(f"http://127.0.0.1:{port}/mcp", data=body,
			headers={"Content-Type": "application/json"})
		with urllib.request.urlopen(request, timeout=5) as response:
			return json.loads(json.load(response)["result"]["content"][0]["text"])["data"]

	log = temp / "editor.log"
	with log.open("wb") as output:
		process = subprocess.Popen([str(editor), "--project", str(project)], cwd=editor.parent,
			env=environment, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
		try:
			# Confirm this listener belongs to the disposable project before collecting evidence.
			confirmed_project = False
			samples = []
			deadline = time.monotonic() + 75
			while len(samples) < 20 and time.monotonic() < deadline:
				if process.poll() is not None:
					raise RuntimeError(f"Editor exited during startup with code {process.returncode}")
				try:
					if not confirmed_project:
						state = call("project.state")
						confirmed_project = (state["activeScene"] == scene_name
							and Path(state["projectRoot"]).resolve() == project)
					if confirmed_project:
						draws = call("renderer.virtualGeometry.stats", viewType=view)["drawCalls"]
						if draws > 0:
							samples.append(draws)
				except (OSError, KeyError, TypeError):
					pass
				time.sleep(0.025)
			if len(samples) != 20 or samples[0] < minimum_draws:
				raise RuntimeError(f"Expected at least {minimum_draws} first-frame draws. Observed {samples}.")
			# Send normal close to the process this test launched, keeping other editor sessions untouched.
			close = f"$p = Get-Process -Id {process.pid}; if (-not $p.CloseMainWindow()) {{ exit 1 }}"
			subprocess.run(["powershell.exe", "-NoProfile", "-Command", close], check=True)
			if process.wait(timeout=30) != 0:
				raise RuntimeError(f"Editor exited with code {process.returncode}")
		finally:
			if process.poll() is None:
				process.terminate()
				process.wait(timeout=15)

	workers = set(re.findall(r"Worker thread no\. (\d+) initialized", log.read_text(errors="replace")))
	return {"firstDraws": samples[0], "sampledDraws": samples, "workers": len(workers),
		"logicalProcessors": os.cpu_count()}


if __name__ == "__main__":
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--editor", type=Path, required=True)
	parser.add_argument("--project", type=Path, required=True, help="Disposable cooked editor project")
	parser.add_argument("--scene", required=True, help="Startup scene name")
	parser.add_argument("--view", choices=("Scene", "Game"), default="Scene")
	parser.add_argument("--minimum-draws", required=True, type=int)
	args = parser.parse_args()
	if args.minimum_draws <= 0:
		parser.error("--minimum-draws must be positive")
	print(json.dumps(check_scene(args.editor.resolve(strict=True), args.project.resolve(strict=True),
		args.scene, args.view, args.minimum_draws)))
