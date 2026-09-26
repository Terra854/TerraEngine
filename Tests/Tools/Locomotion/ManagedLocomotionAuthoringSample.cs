// Compilable reference script for the managed authoring example in Docs/Locomotion.md.
using TerraEngine;
using TerraEngine.Locomotion;

public sealed class PlayerLocomotionInput : Script
{
	private CharacterLocomotion? _motion;
	private ulong _producer;
	private ulong _sequence;
	private ulong _pendingSequence;
	private ulong _pendingCorrelation;
	private bool _identityResolved;
	private bool _acquirePending;
	private bool _inputPending;
	private bool _hasAuthority;
	private bool _failed;
	public CharacterLocomotionReceipt LastFailure { get; private set; }

	public override void OnStart()
	{
		_motion = GetComponent<CharacterLocomotion>();
	}

	public override void OnUpdate()
	{
		if (_motion == null || _failed) return;
		if (!_identityResolved)
		{
			// Binding can publish after script startup, so retry only this unbound resolve.
			if (!_motion.Identity.IsValid)
			{
				if (_motion.LastStatus == LocomotionStatus.StaleHandle) _failed = true;
				return;
			}
			_identityResolved = true;
			_producer = _motion.ProducerId;
			QueueAcquire();
			return;
		}
		if (_acquirePending || _inputPending)
		{
			CharacterLocomotionReceipt result = _motion.ReadReceipt(
				_producer, _pendingSequence, _pendingCorrelation);
			if (result.State == LocomotionReceiptState.Queued) return;
			if (!result.IsApplied)
			{
				LastFailure = result;
				_acquirePending = false;
				_inputPending = false;
				_failed = true;
				return;
			}
			if (_acquirePending) _hasAuthority = true;
			_acquirePending = false;
			_inputPending = false;
		}
		if (_hasAuthority) QueuePlayerInput();
	}

	private CharacterLocomotionCommand NewCommand(LocomotionCommandKind kind)
	{
		ulong sequence = ++_sequence;
		return new CharacterLocomotionCommand {
			Kind = kind,
			Mode = LocomotionMotionMode.Velocity,
			ExpectedConfigurationRevision = _motion!.ConfigurationRevision,
			ExpectedAuthorityEpoch = _motion.AuthorityEpoch,
			OwnerToken = kind == LocomotionCommandKind.Acquire ? 0UL : _motion.OwnerToken,
			ProducerId = _producer,
			Sequence = sequence,
			CorrelationId = sequence
		};
	}

	private void Track(CharacterLocomotionReceipt receipt, bool acquire)
	{
		if (receipt.State == LocomotionReceiptState.Queued)
		{
			_pendingSequence = receipt.Sequence;
			_pendingCorrelation = receipt.CorrelationId;
			_acquirePending = acquire;
			_inputPending = !acquire;
		}
		else if (receipt.IsApplied && acquire) _hasAuthority = true;
		else if (!receipt.IsApplied)
		{
			LastFailure = receipt;
			_failed = true;
		}
	}

	private void QueueAcquire()
	{
		CharacterLocomotionReceipt receipt = _motion!.Submit(
			NewCommand(LocomotionCommandKind.Acquire));
		Track(receipt, acquire: true);
	}

	private void QueuePlayerInput()
	{
		CharacterLocomotionCommand baseCommand = NewCommand(LocomotionCommandKind.SetIntent);
		var command = new CharacterLocomotionCommand {
			Kind = baseCommand.Kind,
			IntentKind = LocomotionIntentKind.Player,
			DirectionSpace = LocomotionDirectionSpace.World,
			Lifetime = LocomotionIntentLifetime.OneUpdate,
			Mode = baseCommand.Mode,
			DirectionX = 0.0,
			DirectionY = 0.0,
			DirectionZ = 1.0,
			RequestedSpeed = 3.0,
			ExpectedAuthorityEpoch = baseCommand.ExpectedAuthorityEpoch,
			ExpectedConfigurationRevision = baseCommand.ExpectedConfigurationRevision,
			OwnerToken = baseCommand.OwnerToken,
			ProducerId = baseCommand.ProducerId,
			Sequence = baseCommand.Sequence,
			CorrelationId = baseCommand.CorrelationId
		};
		Track(_motion!.Submit(command), acquire: false);
	}
}
