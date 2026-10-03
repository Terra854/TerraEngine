<#
.SYNOPSIS
Runs an installed correctness test inside a bounded, hidden Windows process job.
.DESCRIPTION
Prerequisites: configure with BUILD_TESTING=ON, build the relevant test target
and run cmake --build <build-dir> --target install. Use binaries beneath Build/Tests.
Example: ./RunWithWatchdog.ps1 -Executable ./Build/Tests/CorrectnessProcessFixture.exe
  -ArgumentList @('--gate','release','--spawn-depth','2') -TimeoutSeconds 2
Example explicit release: add -ReleaseGatePath ./scratch/release -ReleaseAfterMilliseconds 100.
The caller supplies a fresh disposable gate path. Existing gates are rejected;
logs are retained in a fresh directory. The wrapper never deletes caller data.
Exit 124 means timeout; 125 means launch/watchdog failure; otherwise child exit.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Executable,
	[string[]]$ArgumentList = @(),
	[ValidateRange(1, 3600)][int]$TimeoutSeconds = 30,
	[string]$WorkingDirectory = (Get-Location).Path,
	[string]$LogDirectory = '',
	[string]$ReleaseGatePath = '',
	[ValidateRange(0, 3600000)][int]$ReleaseAfterMilliseconds = 0
)
$ErrorActionPreference = 'Stop'

# Native launch is suspended until job assignment succeeds. This avoids the
# startup race in Start-Process plus later tree enumeration. File redirection
# avoids ReadToEnd/async-reader teardown waiting forever on inherited pipes.
if (-not ('TerraCorrectnessWatchdog' -as [type])) {
	Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public sealed class TerraCorrectnessWatchdog : IDisposable {
	[StructLayout(LayoutKind.Sequential)] struct Security { public int length; public IntPtr descriptor; public int inherit; }
	[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct Startup {
		public int cb; public string reserved, desktop, title; public int x,y,xSize,ySize,xChars,yChars,fill,flags;
		public short show,reservedSize; public IntPtr reservedBytes,input,output,error;
	}
	[StructLayout(LayoutKind.Sequential)] struct ProcessInfo { public IntPtr process,thread; public int pid,tid; }
	[StructLayout(LayoutKind.Sequential)] struct BasicLimits {
		public long processTime,jobTime; public uint flags; public UIntPtr minimum,maximum; public uint active;
		public UIntPtr affinity; public uint priority,scheduling;
	}
	[StructLayout(LayoutKind.Sequential)] struct IoCounters { public ulong a,b,c,d,e,f; }
	[StructLayout(LayoutKind.Sequential)] struct ExtendedLimits {
		public BasicLimits basic; public IoCounters io; public UIntPtr processMemory,jobMemory,peakProcess,peakJob;
	}
	[DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateJobObjectW(IntPtr attributes,string name);
	[DllImport("kernel32.dll", SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job,int info,ref ExtendedLimits limits,int size);
	[DllImport("kernel32.dll", SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job,IntPtr process);
	[DllImport("kernel32.dll", SetLastError=true)] static extern bool TerminateJobObject(IntPtr job,uint code);
	[DllImport("kernel32.dll", SetLastError=true)] static extern bool TerminateProcess(IntPtr process,uint code);
	[DllImport("kernel32.dll", SetLastError=true)] static extern bool CloseHandle(IntPtr handle);
	[DllImport("kernel32.dll", SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
	[DllImport("kernel32.dll", SetLastError=true)] static extern uint WaitForSingleObject(IntPtr handle,uint milliseconds);
	[DllImport("kernel32.dll", SetLastError=true)] static extern bool GetExitCodeProcess(IntPtr process,out uint code);
	[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateFileW(string path,uint access,uint share,ref Security security,uint creation,uint flags,IntPtr template);
	[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcessW(string app,StringBuilder command,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr environment,string directory,ref Startup startup,out ProcessInfo info);
	IntPtr job,process;
	public int Pid { get; private set; }
	static Exception Error(string operation) { return new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),operation); }
	// Quote one argv value using Windows backslash-before-quote rules, including
	// trailing slashes and empty strings. No shell interprets these arguments.
	static string Quote(string value) {
		var result=new StringBuilder("\""); int slashes=0;
		foreach(char ch in value) {
			if(ch=='\\') { ++slashes; continue; }
			if(ch=='"') result.Append('\\',slashes*2+1); else result.Append('\\',slashes);
			result.Append(ch); slashes=0;
		}
		return result.Append('\\',slashes*2).Append('"').ToString();
	}
	public TerraCorrectnessWatchdog(string executable,string[] arguments,string directory,string stdout,string stderr) {
		IntPtr output=IntPtr.Zero,error=IntPtr.Zero,input=IntPtr.Zero,thread=IntPtr.Zero;
		try {
			job=CreateJobObjectW(IntPtr.Zero,null); if(job==IntPtr.Zero) throw Error("CreateJobObject");
			var limits=new ExtendedLimits(); limits.basic.flags=0x2000; // KILL_ON_JOB_CLOSE
			if(!SetInformationJobObject(job,9,ref limits,Marshal.SizeOf(typeof(ExtendedLimits)))) throw Error("SetInformationJobObject");
			var security=new Security { length=Marshal.SizeOf(typeof(Security)),inherit=1 };
			output=CreateFileW(stdout,0x40000000,3,ref security,1,0x80,IntPtr.Zero);
			if(output==new IntPtr(-1)) throw Error("stdout creation");
			error=CreateFileW(stderr,0x40000000,3,ref security,1,0x80,IntPtr.Zero);
			if(error==new IntPtr(-1)) throw Error("stderr creation");
			input=CreateFileW("NUL",0x80000000,3,ref security,3,0x80,IntPtr.Zero);
			if(input==new IntPtr(-1)) throw Error("stdin creation");
			var startup=new Startup { cb=Marshal.SizeOf(typeof(Startup)),flags=0x100,input=input,output=output,error=error };
			var command=new StringBuilder(Quote(executable)); foreach(var argument in arguments) command.Append(' ').Append(Quote(argument));
			ProcessInfo info;
			if(!CreateProcessW(executable,command,IntPtr.Zero,IntPtr.Zero,true,0x08000004,IntPtr.Zero,directory,ref startup,out info)) throw Error("CreateProcess");
			process=info.process; thread=info.thread; Pid=info.pid;
			if(!AssignProcessToJobObject(job,process)) throw Error("AssignProcessToJobObject");
			if(ResumeThread(thread)==0xffffffff) throw Error("ResumeThread");
		} catch { if(process!=IntPtr.Zero) TerminateProcess(process,125); Dispose(); throw; }
		finally { foreach(var handle in new[]{output,error,input,thread}) if(handle!=IntPtr.Zero && handle!=new IntPtr(-1)) CloseHandle(handle); }
	}
	public bool Poll() { uint result=WaitForSingleObject(process,0); if(result==0xffffffff) throw Error("WaitForSingleObject"); return result==0; }
	public int ExitCode() { uint code; if(!GetExitCodeProcess(process,out code)) throw Error("GetExitCodeProcess"); return unchecked((int)code); }
	public void Kill() { if(job!=IntPtr.Zero && !TerminateJobObject(job,124)) throw Error("TerminateJobObject"); }
	public void Dispose() {
		// Closing the job kills descendants even when the root has already exited.
		// Never wait here: cleanup must remain bounded if the tested runner deadlocks.
		if(job!=IntPtr.Zero) { CloseHandle(job); job=IntPtr.Zero; }
		if(process!=IntPtr.Zero) { CloseHandle(process); process=IntPtr.Zero; }
	}
}
'@
}
$ownedProcess = $null
$watchdogExitCode = 125
$watchdogTimedOut = $false
try {
	$executablePath = (Resolve-Path -LiteralPath $Executable).Path
	$workingPath = (Resolve-Path -LiteralPath $WorkingDirectory).Path
	if (-not $LogDirectory) { $LogDirectory = Join-Path ([IO.Path]::GetTempPath()) ('TerraCorrectness-' + [Guid]::NewGuid().ToString('N')) }
	$logPath = [IO.Path]::GetFullPath($LogDirectory)
	if (Test-Path -LiteralPath $logPath) { throw "LogDirectory must be fresh: $logPath" }
	[IO.Directory]::CreateDirectory($logPath) | Out-Null
	$gatePath = ''
	if ($ReleaseGatePath) {
		$gatePath = [IO.Path]::GetFullPath($ReleaseGatePath)
		if (Test-Path -LiteralPath $gatePath) { throw "Release gate must be fresh: $gatePath" }
		if (-not (Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($gatePath)) -PathType Container)) { throw 'Release gate parent must already exist.' }
	}
	$ownedProcess = [TerraCorrectnessWatchdog]::new($executablePath, $ArgumentList, $workingPath, (Join-Path $logPath 'stdout.log'), (Join-Path $logPath 'stderr.log'))
	$clock = [Diagnostics.Stopwatch]::StartNew()
	$released = $false
	while (-not $ownedProcess.Poll()) {
		if ($gatePath -and -not $released -and $clock.ElapsedMilliseconds -ge $ReleaseAfterMilliseconds) {
			# CreateNew rejects racing pre-existing gates rather than truncating data.
			$stream = [IO.File]::Open($gatePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
			$stream.Dispose()
			$released = $true
		}
		if ($clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) { $ownedProcess.Kill(); $watchdogExitCode = 124; $watchdogTimedOut = $true; break }
		Start-Sleep -Milliseconds 10
	}
	if ($watchdogExitCode -ne 124) { $watchdogExitCode = $ownedProcess.ExitCode() }
	@{ pid = $ownedProcess.Pid; exitCode = $watchdogExitCode; timedOut = $watchdogTimedOut; gateReleased = $released; elapsedMilliseconds = $clock.ElapsedMilliseconds } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $logPath 'watchdog.json') -Encoding UTF8
} catch { Write-Error -ErrorAction Continue $_ }
finally { if ($null -ne $ownedProcess) { $ownedProcess.Dispose() } }
Write-Output "Correctness watchdog logs: $LogDirectory"
exit $watchdogExitCode
