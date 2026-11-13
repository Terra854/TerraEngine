<#
Build/install producer protocols. Requires PowerShell7, CMake>=3.24, existing Ninja.
Usage: pwsh -File TestBuildInstallContracts.ps1 -RepositoryRoot <checkout>
 -MakeProgram <existing-ninja> [-WorkRoot <fresh-path>]. Retains every owned fixture.
No real .NET restore, shader compile, platform build, source cache or install edit.
The configure verifier/compiler are explicit protocol substitutes where noted.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RepositoryRoot,[Parameter(Mandatory)][string]$MakeProgram,[string]$CMakeCommand='cmake',[string]$WorkRoot='')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function Need([bool]$Ok,[string]$Message){if(!$Ok){throw $Message}}
function WriteText([string]$Path,[string]$Text){$null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Path));[IO.File]::WriteAllText($Path,$Text,[Text.UTF8Encoding]::new($false))}
function Q([string]$Value){return '"'+$Value.Replace('\','/').Replace('"','\"').Replace('$','\$')+'"'}
function Pin([string]$Path){return @{path=[IO.Path]::GetFullPath($Path);sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $Path).Length}}
# Balanced CMake commands, including escaped quotes, comments and bracket strings.
# Spans are provenance only: test outcomes come from actual CMake execution.
# CR053 permits the support source's explicit portable unavailable implementation;
# its real Linux compile/unchanged-output probe is separate from source selection.
function Commands([string]$Text){
	$at=0;$regex=[regex]'(?m)^[\t ]*([A-Za-z_][A-Za-z0-9_]*)[\t ]*\('
	while($at -lt $Text.Length){$m=$regex.Match($Text,$at);if(!$m.Success){break};$i=$Text.IndexOf('(',$m.Index)+1;$depth=1;$quoted=$false
		while($i -lt $Text.Length -and $depth -gt 0){$c=$Text[$i]
			if($c -eq '\'){$i+=2;continue};if($c -eq '"'){$quoted=!$quoted;$i++;continue}
			if(!$quoted){
				if($c -eq '#'){$end=$Text.IndexOf("`n",$i);$i=if($end -lt 0){$Text.Length}else{$end+1};continue}
				if($c -eq '['){$b=[regex]::Match($Text.Substring($i),'^\[(=*)\[');if($b.Success){$close=']'+$b.Groups[1].Value+']';$end=$Text.IndexOf($close,$i+$b.Length);Need ($end -ge 0) 'Unterminated CMake bracket string.';$i=$end+$close.Length;continue}}
				if($c -eq '('){$depth++};if($c -eq ')'){$depth--}
			};$i++
		};Need ($depth -eq 0) 'Unbalanced CMake producer.'
		[pscustomobject]@{name=$m.Groups[1].Value.ToLowerInvariant();text=$Text.Substring($m.Index,$i-$m.Index);start=1+([regex]::Matches($Text.Substring(0,$m.Index),"`n")).Count;end=1+([regex]::Matches($Text.Substring(0,$i),"`n")).Count};$at=$i
	}
}
function CMake([string[]]$Arguments,[string]$Directory,[string]$Log){
	$si=[Diagnostics.ProcessStartInfo]::new();$si.FileName=$script:cmakeExe;$si.WorkingDirectory=$Directory;$si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
	foreach($a in $Arguments){$si.ArgumentList.Add($a)};$p=[Diagnostics.Process]::new();$p.StartInfo=$si;Need ($p.Start()) 'Cannot start owned CMake.';$o=$p.StandardOutput.ReadToEndAsync();$e=$p.StandardError.ReadToEndAsync()
	if(!$p.WaitForExit(60000)){$p.Kill($true);$p.WaitForExit();throw 'Owned protocol exceeded60second bound.'};$text=$o.GetAwaiter().GetResult()+$e.GetAwaiter().GetResult();WriteText $Log $text;return @{exit=$p.ExitCode;text=$text;arguments=$Arguments}
}
$repo=[IO.Path]::GetFullPath($RepositoryRoot);$module=Join-Path $repo 'cmake/TerraEngineOfflineNuGet.cmake';$rootFile=Join-Path $repo 'CMakeLists.txt';$editorFile=Join-Path $repo 'Tests/Editor/CMakeLists.txt'
Need (Test-Path -LiteralPath $module) 'Invalid RepositoryRoot.';$script:cmakeExe=(Get-Command $CMakeCommand -CommandType Application).Source;$make=[IO.Path]::GetFullPath($MakeProgram);Need (Test-Path -LiteralPath $make) 'Existing Ninja required; never install tools.'
$work=if($WorkRoot){[IO.Path]::GetFullPath($WorkRoot)}else{Join-Path ([IO.Path]::GetTempPath()) ('terra-build-contract-'+[guid]::NewGuid().ToString('N'))};Need (!(Test-Path -LiteralPath $work)) 'Fresh exclusive WorkRoot required.';$null=[IO.Directory]::CreateDirectory($work)
$results=[Collections.Generic.List[object]]::new();$receipts=[Collections.Generic.List[object]]::new();$spans=[Collections.Generic.List[object]]::new()
function Case([string]$Name,[scriptblock]$Body){try{&$Body;$results.Add(@{name=$Name;passed=$true});Write-Host "PASS $Name"}catch{$results.Add(@{name=$Name;passed=$false;message=$_.Exception.Message});Write-Host "FAIL $Name : $($_.Exception.Message)"}}
function Fixture([string]$Name){$f=Join-Path $work $Name;$null=[IO.Directory]::CreateDirectory($f);Copy-Item -LiteralPath $module -Destination (Join-Path $f 'Module.cmake');return $f}
# Execute the actual configure guard with controlled toolchain values. The
# positive uses CMake as a runnable protocol tool, not a claimed Linux ELF proof.
foreach($mode in @('missing','directory','cannot-run','runnable-control')) { Case "CorrectnessReadelf.$mode" {
	$f=Fixture ('readelf-'+$mode)
	$readelf = switch($mode) { 'missing' { Join-Path $f 'absent-readelf' }; 'directory' { $f }; 'cannot-run' { Join-Path $f 'invalid-readelf.bin' }; default { $script:cmakeExe } }
	if($mode -eq 'cannot-run') { WriteText $readelf 'not executable' }
	$body='set(BUILD_TESTING ON)'+"`n"+'set(CMAKE_SYSTEM_NAME Linux)'+"`n"+'set(CMAKE_READELF '+(Q $readelf)+')'+"`n"+'include('+(Q (Join-Path $repo 'cmake/TerraEngineCorrectnessTools.cmake'))+')'+"`n"+'terraengine_validate_correctness_tools()'
	$input=Join-Path $f 'guard.cmake';WriteText $input $body
	$r=CMake @('-P',$input) $f (Join-Path $f 'guard.log')
	Need (($r.exit -eq 0) -eq ($mode -eq 'runnable-control')) ('Configure prerequisite guard result: '+$r.text)
} }
function Manifest([string]$f){
	$packages=@();foreach($id in @('Review.Shared','Review.Runtime.win-x64','Review.Runtime.linux-x64')){$relative='feed/'+$id.ToLowerInvariant()+'.1.0.0.nupkg';$file=Join-Path (Join-Path $f '.nuget') $relative;WriteText $file ('owned-protocol-'+$id);$packages+=@{id=$id;version='1.0.0';file=$relative;sha256=(Pin $file).sha256;sha512=(Get-FileHash -LiteralPath $file -Algorithm SHA512).Hash.ToLowerInvariant();sourceUrl='file:///NETWORK_FORBIDDEN_PROTOCOL_TRIPWIRE'}}
	$json=@{schemaVersion=1;dotnetSdkVersion='0.0.0';packages=$packages}|ConvertTo-Json -Depth 20;WriteText (Join-Path $f '.nuget/manifest.json') $json;WriteText (Join-Path $f 'cmake/TerraEngineNuGet.lock.json') $json
	foreach($name in @('NuGet.Config','.nuget/NativeAotNuGet.Config','global.json','Engine/Managed/TerraEngine.csproj','Editor/Managed/TerraEngine.Export.Generator/TerraEngine.Export.Generator.csproj','External/Hexa.NET.ImGui/Hexa.NET.ImGui/Hexa.NET.ImGui.csproj','Editor/Managed/TerraEngine.Editor/TerraEngine.Editor.csproj')){WriteText (Join-Path $f $name) 'owned-protocol-prerequisite'}
}
foreach($rid in @('win-x64','linux-x64')){Case "HostOutputs.$rid" {
	$f=Fixture ('outputs-'+$rid);Manifest $f;$captured=Join-Path $f 'outputs.txt'
	$body=@'
set(CMAKE_SOURCE_DIR @SOURCE@)
set(CMAKE_BINARY_DIR @BINARY@)
include(@MODULE@)
# Only configure feed verification is replaced for declaration capture. Package
# selection invokes the real producer; no real legal/restore verification claimed.
function(terraengine_verify_offline_nuget_feed runtime_id)
endfunction()
macro(add_custom_command)
 set(capturing FALSE)
 foreach(argument IN ITEMS ${ARGV})
  if(argument STREQUAL "OUTPUT")
   set(capturing TRUE)
  elseif(argument STREQUAL "COMMAND")
   break()
  elseif(capturing)
   file(APPEND @OUTPUT@ "${argument}\n")
  endif()
 endforeach()
endmacro()
macro(add_custom_target)
endmacro()
set(TERRAENGINE_DOTNET_EXECUTABLE "${CMAKE_COMMAND}")
set(TERRAENGINE_DOTNET_SDK_ROOT "${CMAKE_SOURCE_DIR}/protocol-sdk")
set(TERRAENGINE_RUNTIME_ID @RID@)
terraengine_add_offline_nuget_restore_target(ProtocolRestore @RID@)
'@
	$body=$body.Replace('@SOURCE@',(Q $f)).Replace('@BINARY@',(Q (Join-Path $f 'binary'))).Replace('@MODULE@',(Q (Join-Path $f 'Module.cmake'))).Replace('@OUTPUT@',(Q $captured)).Replace('@RID@',$rid);$path=Join-Path $f 'capture.cmake';WriteText $path $body
	$r=CMake @('-P',$path) $f (Join-Path $f 'capture.log');Need ($r.exit -eq 0) ('Producer capture failed: '+$r.text)
	$outputs=@(Get-Content -LiteralPath $captured|Where-Object{$_ -like '*/.nupkg.metadata'});$wrong=if($rid -eq 'win-x64'){'linux-x64'}else{'win-x64'};$receipts.Add(@{contract='actual-host-output-declarations';rid=$rid;outputs=$outputs})
	Need ($outputs.Count -eq 2 -and !($outputs|Where-Object{$_ -match [regex]::Escape($wrong)})) 'Restore OUTPUT includes metadata not restored on the selected host.'
}}
Case 'OffModeDamagedFeedPreservesLkg' {
	$f=Fixture 'off-mode';Manifest $f
	$body=@'
cmake_minimum_required(VERSION 3.24)
project(OwnedOffModeProtocol NONE)
include("${CMAKE_SOURCE_DIR}/Module.cmake")
# Configure bypass only. The GENERATED build-time module subprocess is actual.
function(terraengine_verify_offline_nuget_feed runtime_id)
endfunction()
set(TERRAENGINE_DOTNET_EXECUTABLE "${CMAKE_COMMAND}")
set(TERRAENGINE_DOTNET_SDK_ROOT "${CMAKE_SOURCE_DIR}/protocol-sdk")
terraengine_add_offline_nuget_restore_target(ProtocolRestore win-x64)
'@
	WriteText (Join-Path $f 'CMakeLists.txt') $body;$binary=Join-Path $f 'binary';$r=CMake @('-S',$f,'-B',$binary,'-G','Ninja',('-DCMAKE_MAKE_PROGRAM='+$make),'-DTERRAENGINE_NUGET_CACHE_MODE=OFF') $f (Join-Path $f 'configure.log');Need ($r.exit -eq 0) ('Tiny configure failed: '+$r.text)
	$lkg=Join-Path $binary 'NuGetPackages/last-good.bin';WriteText $lkg 'LAST-GOOD';$pin=Pin $lkg
	# Non-HTTPS URL is a no-network tripwire: accidental AUTO must fail before
	# file(DOWNLOAD). Only this disposable feed is damaged, after configuration.
	WriteText (Join-Path $f '.nuget/feed/review.shared.1.0.0.nupkg') 'DAMAGED'
	$r=CMake @('--build',$binary,'--target','ProtocolRestore') $f (Join-Path $f 'build.log');$preserved=(Test-Path -LiteralPath $lkg) -and ((Pin $lkg).sha256 -eq $pin.sha256)
	$receipts.Add(@{contract='actual-generated-build-verifier';exit=$r.exit;lkgPreserved=$preserved;diagnostic=$r.text;network='producer rejects non-HTTPS before download if AUTO escapes'})
	Need ($preserved) 'Failed verifier damaged last-good cache.'
	Need ($r.exit -ne 0 -and $r.text -match 'NuGet cache entry is missing or damaged' -and $r.text -notmatch 'NuGet lock source must use HTTPS|Regenerating NuGet cache entry') 'OFF mode was not preserved in actual generated build verification.'
}
Case 'LiveShaderRetirementAndCurrentPreservation' {
	$f=Fixture 'shaders';$commands=@(Commands ([IO.File]::ReadAllText($rootFile)));$block=@($commands|Where-Object{$_.name -eq 'file' -and $_.text -match '^\s*file\(GENERATE OUTPUT "\$\{SHADER_INSTALL_SCRIPT\}" CONTENT '});Need ($block.Count -eq 1) 'Shader producer entry moved; revise exact extraction.';$spans.Add(@{producer='shader-file-GENERATE';start=$block[0].start;end=$block[0].end})
	$binary=Join-Path $f 'binary';$live=Join-Path $f 'live/Editor/Shaders';$generated=Join-Path $binary 'CMakeFiles/install_default_shaders_Protocol.cmake'
	$prefix="cmake_minimum_required(VERSION 3.24)`nproject(OwnedShaderProtocol NONE)`nadd_executable(TerraEngineShaderCompiler IMPORTED)`nset_target_properties(TerraEngineShaderCompiler PROPERTIES IMPORTED_LOCATION "+(Q $script:cmakeExe)+")`nset(ENGINE_INSTALL_DESTINATION "+(Q (Join-Path $f 'live'))+")`nset(SHADER_INSTALL_SCRIPT "+(Q (Join-Path $binary 'CMakeFiles/install_default_shaders_$<CONFIG>.cmake'))+")`n"
	WriteText (Join-Path $f 'CMakeLists.txt') ($prefix+$block[0].text);$r=CMake @('-S',$f,'-B',$binary,'-G','Ninja',('-DCMAKE_MAKE_PROGRAM='+$make),'-DCMAKE_BUILD_TYPE=Protocol') $f (Join-Path $f 'configure.log');Need ($r.exit -eq 0) ('Shader generation configure failed: '+$r.text)
	$current=Join-Path $f 'current.txt';$removed=Join-Path $f 'removed.txt'
	$spy=@'
# Synthetic compiler protocol only: actual remove/copy publication code executes.
macro(execute_process)
 set(output_path "")
 set(result_name "")
 set(previous "")
 foreach(argument IN ITEMS ${ARGV})
  if(previous STREQUAL "-o" OR previous STREQUAL "-Fo")
   set(output_path "${argument}")
  endif()
  if(previous STREQUAL "RESULT_VARIABLE")
   set(result_name "${argument}")
  endif()
  set(previous "${argument}")
 endforeach()
 if(output_path STREQUAL "" OR result_name STREQUAL "")
  message(FATAL_ERROR "Unsupported compiler protocol; never silently accept")
 endif()
 get_filename_component(directory "${output_path}" DIRECTORY)
 file(MAKE_DIRECTORY "${directory}")
 # Different sizes make publication independent of subsecond timestamp granularity.
 string(REPEAT "x" ${generation} generation_suffix)
 file(WRITE "${output_path}" "synthetic-generation-${generation}-${generation_suffix}")
 file(APPEND @CURRENT@ "${output_path}\n")
 set(${result_name} 0)
endmacro()
macro(file)
 if("${ARGV0}" STREQUAL "REMOVE")
  foreach(path IN ITEMS ${ARGV})
   if(NOT path STREQUAL "REMOVE")
    _file(APPEND @REMOVED@ "${path}\n")
   endif()
  endforeach()
 endif()
 _file(${ARGV})
endmacro()
'@
	$spy=$spy.Replace('@CURRENT@',(Q $current)).Replace('@REMOVED@',(Q $removed));$runner=Join-Path $f 'run.cmake';WriteText $runner ("set(generation 1)`n"+$spy+"`ninclude("+(Q $generated)+")`n");$r=CMake @('-P',$runner) $f (Join-Path $f 'first.log');Need ($r.exit -eq 0) ('First publication failed: '+$r.text)
	$stage=Join-Path $binary 'Build/Intermediate/InstalledShaders/Protocol';$currentNames=@(Get-Content -LiteralPath $current|ForEach-Object{[IO.Path]::GetRelativePath($stage,$_).Replace('\','/')}|Sort-Object -Unique)
	Need ($currentNames.Count -gt 0) 'No actual producer outputs.';foreach($sky in @('Sky/SkyBackground.task.tspf','Sky/SkyBackground.mesh.tspf','Sky/SkyBackground.frag.tspf')){Need ($sky -in $currentNames) ('Current sky output missing: '+$sky)}
	# Subtract every dynamically produced current output, not just a fixed sky
	# exception. Never copy the stage cleanup list verbatim into live retirement.
	$retired=@(Get-Content -LiteralPath $removed|Where-Object{$_.StartsWith($stage.Replace('\','/'))}|ForEach-Object{[IO.Path]::GetRelativePath($stage,$_).Replace('\','/')}|Where-Object{$_ -notin $currentNames}|Sort-Object -Unique);Need ($retired.Count -gt 0) 'No retired-name candidates.'
	foreach($name in $retired){WriteText (Join-Path $live $name) 'STALE'}
	# Make distinct install generations explicit without sleeping. CMake COPY
	# compares file timestamps, whose granularity can hide rapid protocol writes.
	foreach($name in $currentNames){[IO.File]::SetLastWriteTimeUtc((Join-Path $live $name),[DateTime]::UtcNow.AddMinutes(-1))}
	$unrelated=Join-Path $live 'UserOwned/keep.tspf';WriteText $unrelated 'KEEP'
	WriteText $runner ("set(generation 2)`n"+$spy+"`ninclude("+(Q $generated)+")`n");$r=CMake @('-P',$runner) $f (Join-Path $f 'second.log');Need ($r.exit -eq 0) ('Second publication failed: '+$r.text)
	$preserved=$true;foreach($name in $currentNames){$file=Join-Path $live $name;if(!(Test-Path -LiteralPath $file) -or [IO.File]::ReadAllText($file) -ne 'synthetic-generation-2-xx'){$preserved=$false}}
	$remaining=@($retired|Where-Object{Test-Path -LiteralPath (Join-Path $live $_)});$receipts.Add(@{contract='actual-two-shader-publications';current=$currentNames;retired=$retired;remaining=$remaining;allCurrentPreserved=$preserved;compiler='synthetic protocol bytes'})
	Need ($preserved -and [IO.File]::ReadAllText($unrelated) -eq 'KEEP') 'Current/global sky or unrelated output was damaged.';Need ($remaining.Count -eq 0) ('Retired LIVE names survived: '+($remaining -join ','))
}
Case 'ElfComparisonScratchOwnershipAndCleanup' {
	$f=Fixture 'elf-comparison'
	$helper=Join-Path $repo 'cmake/CompareInstalledElf.cmake'
	# Real bundled ELF bytes exercise CMake's real ELF editor on every host.
	# Only readelf/RUNPATH inspection is substituted: this is a cleanup protocol,
	# not acceptance of a Linux executable or its installed loader closure.
	$fixture=Join-Path $repo 'External/Hexa.NET.ImGui/Hexa.NET.ImGui.Backends.SDL3/native/linux-x64/ImGuiImplSDL3.so'
	$built=Join-Path $f 'built.so';$installed=Join-Path $f 'installed.so'
	Copy-Item -LiteralPath $fixture -Destination $built;Copy-Item -LiteralPath $fixture -Destination $installed
	$bytes=[IO.File]::ReadAllBytes($built)
	Need ($bytes.Length -gt 64 -and $bytes[0] -eq 127 -and $bytes[1] -eq 69 -and $bytes[2] -eq 76 -and $bytes[3] -eq 70) 'Cleanup protocol requires genuine bundled ELF bytes.'
	$original=(Pin $built).sha256
	$body=@'
include(@HELPER@)
# Explicit protocol substitution; the actual copy/RPATH_REMOVE/hash/cleanup
# implementation remains the maintained producer, without a simulated editor.
function(correctness_verify_elf_runpath installed expected readelf)
endfunction()
correctness_compare_elf("${BUILT}" "${INSTALLED}" "" "protocol-unused" "${PRIVATE}")
'@
	$script=Join-Path $f 'compare.cmake';WriteText $script ($body.Replace('@HELPER@',(Q $helper)))
	foreach($kind in @('success','mismatch','preexisting')) {
		$private=Join-Path $f ('scratch-'+$kind)
		if($kind -eq 'mismatch'){$changed=$bytes.Clone();$changed[$changed.Length-1]=$changed[$changed.Length-1] -bxor 1;[IO.File]::WriteAllBytes($installed,$changed)}else{[IO.File]::WriteAllBytes($installed,$bytes)}
		if($kind -eq 'preexisting'){WriteText (Join-Path $private 'caller-marker') 'CALLER OWNED'}
		$before=(Pin $installed).sha256
		$r=CMake @(('-DBUILT='+$built),('-DINSTALLED='+$installed),('-DPRIVATE='+$private),'-P',$script) $f (Join-Path $f ($kind+'.log'))
		if($kind -eq 'success'){Need ($r.exit -eq 0 -and !(Test-Path -LiteralPath $private)) ('Successful comparison must release its owned copies: '+$r.text)}
		elseif($kind -eq 'mismatch'){Need ($r.exit -ne 0 -and (Test-Path -LiteralPath (Join-Path $private 'built'))) 'Failed comparison must retain diagnostic copies.'}
		else{Need ($r.exit -ne 0 -and [IO.File]::ReadAllText((Join-Path $private 'caller-marker')) -ceq 'CALLER OWNED') 'Pre-existing caller directory must be rejected without deletion.'}
		Need ((Pin $built).sha256 -ceq $original -and (Pin $installed).sha256 -ceq $before) 'Comparison cannot edit either original ELF.'
	}
	$receipts.Add(@{contract='Actual ELF editor/copy/compare/cleanup';producer=(Pin $helper);input=(Pin $fixture);limit='RUNPATH inspection explicitly substituted; NOT native Linux acceptance'})
}
Case 'NonWindowsEditorSourceSelectionQualification' {
	$f=Fixture 'editor-projection';$selected=Join-Path $f 'selected.txt';$projection=[Collections.Generic.List[string]]::new()
	foreach($c in @(Commands ([IO.File]::ReadAllText($editorFile)))){if($c.name -in @('if','elseif','else','endif','foreach','endforeach','set','list','add_executable','target_sources') -or ($c.name -eq 'file' -and $c.text -match '^\s*file\(GLOB')){$projection.Add($c.text)}}
	$prefix=@'
cmake_minimum_required(VERSION 3.24)
project(OwnedEditorSourceProjection NONE)
set(WIN32 FALSE)
set(MSVC FALSE)
set(CMAKE_SOURCE_DIR @REPO@)
set(CMAKE_CURRENT_SOURCE_DIR @EDITOR@)
macro(add_executable target)
 if("${target}" STREQUAL "TerraEngine-EditorTests")
  foreach(source IN ITEMS ${ARGN})
   file(APPEND @SELECTED@ "${source}\n")
  endforeach()
 endif()
endmacro()
macro(target_sources target)
 if("${target}" STREQUAL "TerraEngine-EditorTests")
  foreach(source IN ITEMS ${ARGN})
   if(NOT source MATCHES "^(PRIVATE|PUBLIC|INTERFACE)$")
    file(APPEND @SELECTED@ "${source}\n")
   endif()
  endforeach()
 endif()
endmacro()
'@
	$prefix=$prefix.Replace('@REPO@',(Q $repo)).Replace('@EDITOR@',(Q (Join-Path $repo 'Tests/Editor'))).Replace('@SELECTED@',(Q $selected));WriteText (Join-Path $f 'CMakeLists.txt') ($prefix+"`n"+($projection -join "`n"));$r=CMake @('-S',$f,'-B',(Join-Path $f 'binary'),'-G','Ninja',('-DCMAKE_MAKE_PROGRAM='+$make)) $f (Join-Path $f 'configure.log');Need ($r.exit -eq 0) ('Source projection failed: '+$r.text)
	$files=@(Get-Content -LiteralPath $selected);$bad=@($files|Where-Object{$_ -match 'RendererEfficiencyInstallIdentityTests\.cpp$'});$receipts.Add(@{contract='CMake-executed-source-AST-projection';count=$files.Count;windowsOnly=$bad;limit='NOT actual Linux compile or registry proof'})
	Need ($files.Count -gt 0 -and $bad.Count -eq 0) 'Non-Windows selection still includes Windows-only identity TESTS; portable support symbols remain linked.'
}
$evidence=@{schemaVersion=1;qualification='ISOLATED PRODUCER PROTOCOLS';workRoot=$work;producers=@((Pin $module),(Pin $rootFile),(Pin $editorFile));producerSpans=$spans;results=$results;receipts=$receipts;limits=@('No real SDK restore/platform build/shader compile or source cache mutation.','Host outputs use actual producer with only configure verifier substituted. Actual managed second-build stability still needs platform acceptance.','OFF generated build verifier invokes exact copied module; non-HTTPS tripwire prevents network if AUTO escapes.','Shader compiler outputs synthetic bytes; actual remove/copy/install producer executes.','CR053 source-AST execution is partial: actual Linux target compile/declarations/registry remain parent gates.')};WriteText (Join-Path $work 'evidence.json') ($evidence|ConvertTo-Json -Depth 50)
$failed=@($results|Where-Object{!$_.passed});Write-Host "$($results.Count) selected protocols, $($failed.Count) failures; evidence:$work";if($failed.Count -ne 0){exit 1}
