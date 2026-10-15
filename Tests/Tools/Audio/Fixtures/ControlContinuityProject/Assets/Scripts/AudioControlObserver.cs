using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using TerraEngine;
using TerraEngine.Audio;

// This is real gameplay: every observation comes from OnUpdate and the public
// AudioSource facade. The runner supplies only an owned evidence directory and
// optional stage commands; it never fabricates callback or playback receipts.
public sealed partial class AudioControlObserver : Script
{
	private readonly string _instanceId = Guid.NewGuid().ToString("N");
	private readonly string? _root = Environment.GetEnvironmentVariable("TERRAENGINE_AUDIO_CONTROL_OBSERVATION_ROOT");
	private readonly List<object> _stages = new();
	private AudioSource? _source;
	private AudioSource? _oldSource;
	private ulong _updates;
	private ulong _positiveDeltaFrames;
	private ulong _stageStarted;
	private ulong _cursor;
	private ulong _playback;
	private int _stage;
	private string _failure = string.Empty;
	private AudioCommandReceipt? _command;

	private AudioSource? FreshSource() => TerraEngine.Object.Find("Audio Continuity Source")?.GetComponent<AudioSource>();
	private static bool Accepted(AudioCommandReceipt receipt) => receipt.Status is AudioStatus.Success or AudioStatus.Queued or AudioStatus.Accepted;

	private void Record(string name, AudioSourceState state)
	{
		_stages.Add(new { name, updates = _updates, frame = Time.FrameCount, delta = Time.DeltaTime,
			positiveDeltaFrames = _positiveDeltaFrames, state, command = _command });
		_stageStarted = _updates;
	}

	private void Submit(string name, AudioCommandReceipt receipt, AudioSourceState state)
	{
		_command = receipt;
		Record(name, state);
		if (!Accepted(receipt)) _failure = name + ": " + receipt.Status + " " + receipt.Diagnostic;
	}

	public override void OnUpdate()
	{
		++_updates;
		if (Time.DeltaTime > 0) ++_positiveDeltaFrames;
		if (string.IsNullOrWhiteSpace(_root)) return;
		try
		{
			_source ??= FreshSource();
			if (_source == null)
			{
				// Missing facade acquisition is a bounded failed observation, even
				// when the native provider has independently started a real voice.
				if (_updates > 600) _failure = "Audio source facade was not acquired within 600 actual callbacks.";
				Persist(); return;
			}
			AudioSourceState state = _source.ReadState();
			if (_failure.Length == 0) Advance(state);
			if (_stage < 9 && _updates - _stageStarted > 600)
				_failure = "Timed out waiting for observed audio stage " + _stage;
			Persist();
		}
		catch (Exception error)
		{
			_failure = error.GetType().Name + ": " + error.Message;
			Persist();
		}
	}

	private void Advance(AudioSourceState state)
	{
		// Waiting for audible autoplay ensures missing provider/clip is never
		// mistaken for a successful logical-only managed acceptance run.
		switch (_stage)
		{
		case 0 when state.Status == AudioStatus.Success && state.State == AudioPlaybackState.Playing && state.Audible:
			_playback = state.PlaybackGeneration; _cursor = state.LogicalFrame;
			Record("autoplay-observed", state); _stage = 1; break;
		case 1 when state.LogicalFrame > _cursor && _positiveDeltaFrames >= 3:
			Record("autoplay-advanced", state);
			// Object.Find constructs another facade. This wrapper has never called
			// Play: v1 reproduces the original zero-generation stale-target defect.
			_source = FreshSource() ?? throw new InvalidOperationException("Fresh facade missing.");
			Submit("fresh-facade-pause", _source.Pause(), state); _stage = 2; break;
		case 2 when state.State == AudioPlaybackState.Paused && state.PlaybackGeneration == _playback:
			_cursor = state.LogicalFrame; Record("pause-observed", state); _stage = 3; break;
		case 3 when _updates - _stageStarted >= 4:
			if (state.State != AudioPlaybackState.Paused || state.LogicalFrame != _cursor)
				throw new InvalidOperationException("Paused cursor advanced.");
			Submit("fresh-facade-resume", _source!.Resume(), state); _stage = 4; break;
		case 4 when state.State == AudioPlaybackState.Playing && state.LogicalFrame > _cursor:
			Record("resume-advanced", state); Submit("fresh-facade-stop", _source!.Stop(), state); _stage = 5; break;
		case 5 when state.State == AudioPlaybackState.Stopped && state.LogicalFrame == 0:
			Record("stop-observed", state);
			Submit("pending-play", _source!.Play(), state);
			Submit("pending-pause", _source.Pause(), state); _stage = 6; break;
		case 6 when state.State == AudioPlaybackState.Paused && state.PlaybackGeneration != _playback:
			_playback = state.PlaybackGeneration; Record("pending-play-pause-observed", state); _stage = 7; break;
		case 7:
			// The runner submits another producer's Play and then lets normal
			// positive-delta frames publish it before this callback controls it.
			if (!File.Exists(Path.Combine(_root!, "other-producer.ready")) || state.PlaybackGeneration == _playback) break;
			_playback = state.PlaybackGeneration; _source = FreshSource()!;
			Submit("other-producer-pause", _source.Pause(), state); _stage = 8; break;
		case 8 when state.State == AudioPlaybackState.Paused && state.PlaybackGeneration == _playback:
			Record("other-producer-controlled", state); _oldSource = _source; _stage = 9; break;
		case 9:
			if (!File.Exists(Path.Combine(_root!, "replacement.ready"))) break;
			_source = FreshSource();
			if (_source == null) break;
			AudioSourceState replacement = _source.ReadState();
			if (replacement.Status != AudioStatus.Success || replacement.PlaybackGeneration == _playback || !replacement.Audible) break;
			AudioCommandReceipt stale = _oldSource!.Pause();
			if (Accepted(stale)) throw new InvalidOperationException("Old facade controlled the replacement source.");
			_command = stale; Record("stale-facade-rejected", replacement);
			Submit("replacement-pause", _source.Pause(), replacement); _playback = replacement.PlaybackGeneration; _stage = 10; break;
		case 10 when state.State == AudioPlaybackState.Paused && state.PlaybackGeneration == _playback:
			Record("replacement-controlled", state); _stage = 11; break;
		}
	}

	private void Persist()
	{
		// Atomic replacement plus delete-sharing in the reader prevents partial
		// JSON and avoids turning observation into an access-denied script fault.
		string pending = Path.Combine(_root!, "managed.pending");
		string text = JsonSerializer.Serialize(new { schemaVersion = 1, instanceId = _instanceId,
			updates = _updates, frame = Time.FrameCount, positiveDeltaFrames = _positiveDeltaFrames,
			stage = _stage, passed = _stage == 11 && _failure.Length == 0, failure = _failure, stages = _stages });
		try { File.WriteAllText(pending, text); File.Move(pending, Path.Combine(_root!, "managed.json"), true); }
		catch (IOException) { /* A later callback retries; the runner still enforces its deadline. */ }
		catch (UnauthorizedAccessException) { /* Persistent evidence failure never becomes a passing receipt. */ }
	}
}
