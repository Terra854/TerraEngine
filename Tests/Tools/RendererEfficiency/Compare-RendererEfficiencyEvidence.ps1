<#
.SYNOPSIS
Validates and compares completion-qualified renderer efficiency evidence.
.DESCRIPTION
Use the installed schema/manifest beside this script. Example:
pwsh -File Compare-RendererEfficiencyEvidence.ps1 -Reference reference.json -Candidate candidate.json -Stage P1 -Output summary.json
Input arrays contain one schema-v1 document per fixture/run. Missing observations
or unavailable metrics return 2. Invalid identities/schema return 3; failed gates
return 1. Raw artifacts resolve beside their evidence document, never a checkout.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string[]]$Reference,
	[Parameter(Mandatory=$true)][string[]]$Candidate,
	[ValidateSet('P1','P2','P3','P4','P5','All')][string]$Stage='All',
	[string]$Manifest='',
	[string]$Schema='',
	[Parameter(Mandatory=$true)][string]$Output,
	[switch]$PostP1Baseline
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
# Pin standard hashing/JSON helpers to this host's bundled module even when a
# parent process passes a module search path belonging to another edition.
try {
	Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force -ErrorAction Stop
} catch {
	Write-Output '{"schemaVersion":1,"status":"Invalid","reason":"Built-in PowerShell Utility module could not load","exitCode":3}'
	exit 3
}
$utf8=[Text.UTF8Encoding]::new($false)
$summary=[ordered]@{schemaVersion=1;stage=$Stage;status='Unverified';reason='';comparisons=@();exitCode=2}
function Has($Value,[string]$Name) { return $null -ne $Value -and $null -ne $Value.PSObject.Properties[$Name] }
function Canonical($Value) { return ConvertTo-Json -InputObject $Value -Depth 100 -Compress }
function JsonEqual($First,$Second) {
	# Windows PowerShell parses decimal notation as Decimal and exponent notation
	# as Double. JSON numbers have no such type distinction; compare their value.
	$numericFirst=$First -is [ValueType] -and $First -isnot [bool] -and $First -isnot [DateTime]
	$numericSecond=$Second -is [ValueType] -and $Second -isnot [bool] -and $Second -isnot [DateTime]
	if($numericFirst -and $numericSecond){return [double]$First -eq [double]$Second}
	return (Canonical $First) -ceq (Canonical $Second)
}
function Require([bool]$Condition,[string]$Message) { if(!$Condition) { throw $Message } }
function Unverified([string]$Message) { throw [InvalidOperationException]::new('UNVERIFIED: '+$Message) }

# This deliberately small schema interpreter covers every keyword in the frozen
# bundled schema. Unknown validation keywords fail closed instead of being ignored.
function Validate($Value,$Rule,[string]$At='$') {
	if(Has $Rule '$ref') {
		Require ($Rule.'$ref' -match '^#/\$defs/([^/]+)$') "$At unsupported schema reference"
		Validate $Value $script:definitionRoot.'$defs'.($Matches[1]) $At
	}
	if(Has $Rule 'allOf') { foreach($r in $Rule.allOf) { Validate $Value $r $At } }
	if(Has $Rule 'anyOf') {
		$matched=$false
		foreach($r in $Rule.anyOf) { try { Validate $Value $r $At; $matched=$true; break } catch {} }
		Require $matched "$At does not match any schema alternative"
	}
	if(Has $Rule 'if') {
		$matchesCondition=$true
		try { Validate $Value $Rule.if $At } catch { $matchesCondition=$false }
		if($matchesCondition -and (Has $Rule 'then')) { Validate $Value $Rule.then $At }
		if(!$matchesCondition -and (Has $Rule 'else')) { Validate $Value $Rule.else $At }
	}
	if(Has $Rule 'const') { Require (JsonEqual $Value $Rule.const) "$At wrong constant" }
	if(Has $Rule 'enum') { Require (@($Rule.enum | Where-Object { JsonEqual $_ $Value }).Count -gt 0) "$At invalid enum" }
	if(Has $Rule 'type') {
		$validType=$false
		foreach($type in @($Rule.type)) {
			$isNumber=$Value -is [ValueType] -and $Value -isnot [bool] -and $Value -isnot [DateTime]
			if(($type -eq 'null' -and $null -eq $Value) -or ($type -eq 'string' -and $Value -is [string]) -or
				($type -eq 'boolean' -and $Value -is [bool]) -or ($type -eq 'object' -and $Value -is [pscustomobject]) -or
				($type -eq 'array' -and $Value -is [array]) -or ($type -eq 'number' -and $isNumber) -or
				($type -eq 'integer' -and $isNumber -and [double]$Value -eq [Math]::Truncate([double]$Value))) { $validType=$true }
		}
		Require $validType "$At wrong JSON type"
	}
	if($Value -is [ValueType] -and $Value -isnot [bool]) {
		Require (![double]::IsNaN([double]$Value) -and ![double]::IsInfinity([double]$Value)) "$At nonfinite value"
		if(Has $Rule 'minimum') { Require ([double]$Value -ge [double]$Rule.minimum) "$At below minimum" }
		if(Has $Rule 'maximum') { Require ([double]$Value -le [double]$Rule.maximum) "$At above maximum" }
	}
	if($Value -is [string]) {
		if(Has $Rule 'minLength') { Require ($Value.Length -ge $Rule.minLength) "$At empty/short text" }
		if(Has $Rule 'pattern') { Require ($Value -cmatch $Rule.pattern) "$At invalid pattern" }
	}
	if($Value -is [array]) {
		if(Has $Rule 'minItems') { Require ($Value.Count -ge $Rule.minItems) "$At too few entries" }
		if(Has $Rule 'maxItems') { Require ($Value.Count -le $Rule.maxItems) "$At too many entries" }
		if(Has $Rule 'items') { for($i=0;$i -lt $Value.Count;$i++) { Validate $Value[$i] $Rule.items "$At[$i]" } }
	}
	if($Value -is [pscustomobject]) {
		if(Has $Rule 'required') { foreach($name in $Rule.required) { Require (Has $Value $name) "$At missing $name" } }
		foreach($property in $Value.PSObject.Properties) {
			if((Has $Rule 'properties') -and (Has $Rule.properties $property.Name)) { Validate $property.Value $Rule.properties.($property.Name) "$At.$($property.Name)" }
			elseif((Has $Rule 'additionalProperties') -and !$Rule.additionalProperties) { throw "$At unexpected $($property.Name)" }
		}
	}
	$known=@('$ref','$schema','$id','$defs','title','description','type','required','properties','additionalProperties','const','enum','allOf','anyOf','if','then','else','minimum','maximum','minLength','pattern','items','minItems','maxItems')
	foreach($name in $Rule.PSObject.Properties.Name) { Require ($name -in $known) "$At unsupported schema keyword $name" }
}
function Metric($Value,[string]$Name) {
	if($null -eq $Value -or $Value.status -ne 'Valid' -or $null -eq $Value.value) { Unverified "$Name unavailable" }
	return $Value.value
}
function ArtifactPath($Artifact,[string]$Directory) {
	if($Artifact.status -ne 'Valid') { Unverified "$($Artifact.kind) artifact unavailable" }
	Require (![IO.Path]::IsPathRooted($Artifact.path)) 'Artifact path must be relative to evidence directory'
	$path=[IO.Path]::GetFullPath((Join-Path $Directory $Artifact.path))
	$prefix=[IO.Path]::GetFullPath($Directory).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
	Require ($path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) 'Artifact escapes evidence directory'
	Require (Test-Path -LiteralPath $path -PathType Leaf) "Missing artifact: $path"
	$ancestor=$path
	# Every reference freshly checks every existing ancestor. Direct attributes
	# avoid provider overhead without caching proof or changing SHA check order.
	# All I/O failures (including a vanished ancestor) remain fail-closed: unlike
	# fresh-work-root checks, this artifact and its ancestors must already exist.
	while($ancestor) {
		$attributes=[IO.File]::GetAttributes($ancestor)
		Require (($attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Artifact cannot be redirected'
		$next=[IO.Path]::GetDirectoryName($ancestor)
		if($next -eq $ancestor){break};$ancestor=$next
	}
	Require ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ieq $Artifact.sha256) "Artifact hash mismatch: $path"
	return $path
}
function ObservationKey($Frame) {
	if((Has $Frame 'evidenceKind') -and $Frame.evidenceKind -ceq 'LightingDepth'){return @($Frame.fixtureStepId,$Frame.stepFrame,$Frame.viewType,$Frame.viewId,'LightingDepth') -join '|'}
	if((Has $Frame 'evidenceKind') -and $Frame.evidenceKind -ceq 'Lighting') {
		return @($Frame.fixtureStepId,$Frame.stepFrame,$Frame.viewType,$Frame.viewId,$Frame.payloadPurpose) -join '|'
	}
	return @($Frame.fixtureStepId,$Frame.stepFrame,$Frame.viewType,$Frame.viewId,(Metric $Frame.lightId 'lightId'),(Metric $Frame.faceIndex 'faceIndex')) -join '|'
}
function Percentile($Samples,[double]$Fraction) {
	if($Samples.Count -eq 0) { Unverified 'Empty timing samples' }
	$sorted=@($Samples|Sort-Object)
	return [double]$sorted[[Math]::Max(0,[Math]::Ceiling($sorted.Count*$Fraction)-1)]
}
function InitializeEvidenceIndex {
	if('TerraRendererEvidenceIndex' -as [type]){return}
	# Framework JSON readers stream token structure. Only bounded routing fields
	# are materialized here; frame objects never become XML/JSON trees in memory.
	$source=@'
using System;
using System.IO;
using System.Text;
using System.Xml;
using System.Collections.Generic;
using System.Runtime.Serialization.Json;
public static class TerraRendererEvidenceIndex {
 sealed class LimitedWriter : StringWriter {
  public LimitedWriter() : base(new StringBuilder()) {}
  void Bound(int n) {if(GetStringBuilder().Length+n>1048576)throw new InvalidDataException("Evidence index field exceeds bounded size");}
  public override void Write(char c){Bound(1);base.Write(c);}
  public override void Write(string s){Bound(s==null?0:s.Length);base.Write(s);}
  public override void Write(char[] b,int i,int n){Bound(n);base.Write(b,i,n);}
 }
 static string Name(XmlReader r){return r.GetAttribute("item")??r.LocalName;}
 static void CheckBoundary(string path) {
  // Framework readers can stop after the first JSON root. A fixed-size lexical
  // pass rejects trailing roots without retaining their text or any frame data.
  bool quoted=false,escaped=false,finished=false;int depth=0;var bytes=new byte[65536];
  using(var file=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read)){int n;
   while((n=file.Read(bytes,0,bytes.Length))!=0)for(int i=0;i<n;i++){byte b=bytes[i];
    if(finished){if(b!=32&&b!=9&&b!=10&&b!=13)throw new InvalidDataException("Trailing content after evidence JSON root");continue;}
    if(quoted){if(escaped)escaped=false;else if(b==92)escaped=true;else if(b==34)quoted=false;continue;}
    if(b==34)quoted=true;else if(b==123||b==91)depth++;else if(b==125||b==93){depth--;if(depth==0)finished=true;}
   }
  }
  if(!finished||quoted)throw new InvalidDataException("Incomplete evidence JSON root");
 }
 static void CheckTree(XmlElement e) {
  var names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
  foreach(XmlNode n in e.ChildNodes)if(n is XmlElement){var c=(XmlElement)n;
   if(e.GetAttribute("type")=="object"&&!names.Add(c.HasAttribute("item")?c.GetAttribute("item"):c.LocalName))throw new InvalidDataException("Duplicate JSON property in evidence index");
   CheckTree(c);
  }
 }
 static string Capture(XmlReader reader) {
  string xml;using(var text=new LimitedWriter())using(var writer=XmlWriter.Create(text,new XmlWriterSettings{OmitXmlDeclaration=true,ConformanceLevel=ConformanceLevel.Fragment})){
   writer.WriteNode(reader,true);writer.Flush();xml=text.ToString();
  }
  var document=new XmlDocument();document.XmlResolver=null;document.LoadXml(xml);CheckTree(document.DocumentElement);
  var root=document.CreateElement("root");foreach(XmlAttribute a in document.DocumentElement.Attributes)root.Attributes.Append((XmlAttribute)a.Clone());
  foreach(XmlNode child in document.DocumentElement.ChildNodes)root.AppendChild(child.Clone());
  using(var bytes=new MemoryStream())using(var writer=JsonReaderWriterFactory.CreateJsonWriter(bytes,Encoding.UTF8,false)){
   using(var source=new XmlNodeReader(root))writer.WriteNode(source,true);writer.Flush();return Encoding.UTF8.GetString(bytes.ToArray());
  }
 }
 public static string[] Read(string path) {
	 CheckBoundary(path);
  var quota=new XmlDictionaryReaderQuotas{MaxDepth=128,MaxStringContentLength=1048576,MaxArrayLength=int.MaxValue,MaxBytesPerRead=65536,MaxNameTableCharCount=1048576};
  var rootNames=new HashSet<string>(StringComparer.OrdinalIgnoreCase);var objects=new Dictionary<int,HashSet<string>>();
  string identity=null,options=null;bool rootSeen=false;
  using(var file=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read))using(var reader=JsonReaderWriterFactory.CreateJsonReader(file,quota)) {
   bool current=reader.Read();while(current){
    if(reader.NodeType==XmlNodeType.Element){int depth=reader.Depth;string name=Name(reader);HashSet<string> parent;
     if(objects.TryGetValue(depth-1,out parent)&&!parent.Add(name))throw new InvalidDataException("Duplicate JSON property in evidence");
     if(depth==0){if(rootSeen||reader.LocalName!="root"||reader.GetAttribute("type")!="object")throw new InvalidDataException("Evidence root must be one JSON object");rootSeen=true;}
     if(depth==1){if(!rootNames.Add(name)||Array.IndexOf(new[]{"schemaVersion","identity","requestedOptions","measurementPolicy","frames","successCriteria","artifacts"},name)<0)throw new InvalidDataException("Unexpected/duplicate evidence root property");}
     if(depth==1&&(name=="identity"||name=="requestedOptions")){string value=Capture(reader);if(name=="identity")identity=value;else options=value;current=!reader.EOF;continue;}
     if(reader.GetAttribute("type")=="object"&&!reader.IsEmptyElement)objects[depth]=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    } else if(reader.NodeType==XmlNodeType.EndElement)objects.Remove(reader.Depth);
    current=reader.Read();
   }
  }
  if(!rootSeen||rootNames.Count!=7||identity==null||options==null)throw new InvalidDataException("Evidence root fields missing");
  return new[]{identity,options};
 }
}
'@
	if($PSVersionTable.PSVersion.Major -le 5){Add-Type -TypeDefinition $source -ReferencedAssemblies @('System.Runtime.Serialization','System.Xml','System.Core')}
	else{Add-Type -TypeDefinition $source}
}
function LoadEvidence([string[]]$Paths,[string]$Mode) {
	InitializeEvidenceIndex
	$list=[Collections.Generic.List[object]]::new()
	$seen=@{}
	foreach($path in $Paths) {
		$full=[IO.Path]::GetFullPath($path);Require (!$seen.ContainsKey($full)) 'Duplicate evidence input path';$seen[$full]=$true
		$hash=(Get-FileHash -LiteralPath $full).Hash.ToLowerInvariant();$parts=[TerraRendererEvidenceIndex]::Read($full)
		Require ((Get-FileHash -LiteralPath $full).Hash.ToLowerInvariant() -ceq $hash) 'Evidence input changed during indexing'
		$value=[pscustomobject]@{identity=($parts[0]|ConvertFrom-Json);requestedOptions=($parts[1]|ConvertFrom-Json)}
		Validate $value.identity $script:definitionRoot.properties.identity;Validate $value.requestedOptions $script:definitionRoot.properties.requestedOptions
		Require ($value.identity.mode -eq $Mode) 'Evidence mode does not match input side'
		$list.Add(@{path=$full;directory=(Split-Path $full);hash=$hash;value=$value})
	}
	return ,$list.ToArray()
}
function LoadIndexedEvidence($Index) {
	Require ((Get-FileHash -LiteralPath $Index.path).Hash.ToLowerInvariant() -ceq $Index.hash) 'Evidence input changed after indexing'
	$value=Get-Content -LiteralPath $Index.path -Raw|ConvertFrom-Json
	Validate $value $script:definitionRoot
	Require ((Get-FileHash -LiteralPath $Index.path).Hash.ToLowerInvariant() -ceq $Index.hash) 'Evidence input changed during full validation'
	Require ((JsonEqual $value.identity $Index.value.identity) -and (JsonEqual $value.requestedOptions $Index.value.requestedOptions)) 'Full evidence identity/options differ from immutable index'
	return @{path=$Index.path;directory=$Index.directory;value=$value}
}
function ValidateEvidenceIndexSelection($Indexes,$Workloads,[string]$RequestedStage) {
	foreach($index in $Indexes) {
		Require (@($Workloads|Where-Object id -ceq $index.value.identity.fixtureId).Count -eq 1) 'Unexpected evidence fixture outside requested stage'
		Require ($RequestedStage -eq 'All' -or $index.value.identity.stage -ceq $RequestedStage) 'Evidence input stage differs from requested stage'
	}
}
# Bulk byte comparisons run in standard managed code rather than invoking a
# PowerShell command for each native pixel. No third-party package is required.
function InitializeNativeProof {
	if('TerraRendererNativeProof' -as [type]) { return }
	Add-Type -AssemblyName System.Drawing
	$managedReferences=@('System.Drawing','System.Core')
	if($PSVersionTable.PSEdition -ne 'Desktop') {
		# Modern PowerShell uses reference assemblies plus its bundled drawing
		# implementation; Desktop uses the standard .NET Framework assemblies.
		$managedReferences=@(Get-ChildItem (Join-Path $PSHOME 'ref') -Filter '*.dll'|ForEach-Object FullName)+@([Drawing.Bitmap].Assembly.Location)+@(Get-ChildItem $PSHOME -Filter 'System.Private.Windows*.dll'|ForEach-Object FullName)
	}
	Add-Type -ReferencedAssemblies $managedReferences -TypeDefinition @'
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class TerraRendererNativeProof {
 public static void Depth(byte[] depth,byte[] coverage,int width,int height) {
  long pixels=(long)width*height;
  if(width<=0||height<=0||pixels>134217728||depth.LongLength!=pixels*4||coverage.LongLength!=pixels)throw new InvalidDataException("Full native depth/coverage extent byte count mismatch");
  if(!BitConverter.IsLittleEndian)throw new InvalidDataException("float32-le requires a supported little-endian verifier");
  for(int p=0;p<coverage.Length;p++){float d=BitConverter.ToSingle(depth,p*4);if(float.IsNaN(d)||float.IsInfinity(d)||d<0||d>1)throw new InvalidDataException("Native D32 contains nonfinite/out-of-range depth");if(coverage[p]!=(d<1?1:0))throw new InvalidDataException("Native coverage disagrees with occupied depth");}
 }
 public static double DepthDelta(byte[] a,byte[] b) {
  if(a.Length!=b.Length)throw new InvalidDataException("Native depth dimensions mismatch");double maximum=0;
  for(int i=0;i<a.Length;i+=4)maximum=Math.Max(maximum,Math.Abs((double)BitConverter.ToSingle(a,i)-BitConverter.ToSingle(b,i)));return maximum;
 }
 static byte[] Pixels(byte[] png,int width,int height) {
  byte[] signature={137,80,78,71,13,10,26,10};
  if(png.Length<33)throw new InvalidDataException("PNG native header missing");for(int i=0;i<8;i++)if(png[i]!=signature[i])throw new InvalidDataException("Native image is not PNG");
  if(png[12]!=73||png[13]!=72||png[14]!=68||png[15]!=82)throw new InvalidDataException("PNG IHDR missing");
  uint w=((uint)png[16]<<24)|((uint)png[17]<<16)|((uint)png[18]<<8)|png[19],h=((uint)png[20]<<24)|((uint)png[21]<<16)|((uint)png[22]<<8)|png[23];
  if(width<=0||height<=0||(long)width*height>67108864||w!=width||h!=height)throw new InvalidDataException("PNG actual native camera extent mismatch");
  using(var stream=new MemoryStream(png))using(var bitmap=new Bitmap(stream)) {
   if(bitmap.Width!=width||bitmap.Height!=height)throw new InvalidDataException("Decoded PNG native extent mismatch");
   var data=bitmap.LockBits(new Rectangle(0,0,width,height),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb);
   try{byte[] result=new byte[checked(width*height*4)];for(int y=0;y<height;y++)Marshal.Copy(IntPtr.Add(data.Scan0,y*data.Stride),result,y*width*4,width*4);return result;}finally{bitmap.UnlockBits(data);}
  }
 }
 // Each observation owns newly decoded channels; no mutable pixels escape.
 public sealed class DecodedImage {
  readonly byte[] pixels;
  public readonly int Width;
  public readonly int Height;
  private DecodedImage(byte[] ownedPixels,int width,int height){pixels=ownedPixels;Width=width;Height=height;}
  internal static DecodedImage Create(byte[] png,int width,int height){return new DecodedImage(Pixels(png,width,height),width,height);}
  internal double Delta(DecodedImage other,int width,int height){
   if(other==null||Width!=width||Height!=height||other.Width!=width||other.Height!=height||width<=0||height<=0||(long)width*height>67108864||pixels.Length!=(long)width*height*4||other.pixels.Length!=pixels.Length)throw new InvalidDataException("Decoded PNG native extent mismatch");
   int maximum=0;for(int i=0;i<pixels.Length;i++)maximum=Math.Max(maximum,Math.Abs((int)pixels[i]-other.pixels[i]));return maximum/255.0;
  }
 }
 public static DecodedImage DecodeImage(byte[] png,int width,int height){return DecodedImage.Create(png,width,height);}
 public static double DecodedImageDelta(DecodedImage a,DecodedImage b,int width,int height){if(a==null)throw new InvalidDataException("Decoded PNG native proof missing");return a.Delta(b,width,height);}
 // Raw callers still validate both operands, even when their bytes are identical.
 public static double ImageDelta(byte[] a,byte[] b,int width,int height){return DecodedImageDelta(DecodeImage(a,width,height),DecodeImage(b,width,height),width,height);}
 public static void Image(byte[] a,int width,int height){DecodeImage(a,width,height);}
 public static int CoverageDelta(byte[] a,byte[] b){if(a.Length!=b.Length)throw new InvalidDataException("Native coverage dimensions mismatch");int changed=0;for(int i=0;i<a.Length;i++)if(a[i]!=b[i])changed++;return changed;}
 public static long Occupied(byte[] coverage){long count=0;foreach(byte pixel in coverage)if(pixel==1)count++;return count;}
}
'@
}
function NativeRequire([bool]$Condition,[string]$Reason) { if(!$Condition){throw ('NATIVE_FAILED: '+$Reason)} }
function NativeUnsigned($Value,[string]$Name,[bool]$Nonzero=$false) {
	$text=[string]$Value
	NativeRequire ($text -cmatch '^(0|[1-9][0-9]*)$') "$Name lacks exact unsigned integer text"
	$parsed=[uint64]0; NativeRequire ([uint64]::TryParse($text,[ref]$parsed)) "$Name exceeds uint64 range"
	if($Nonzero){NativeRequire ($parsed -gt 0) "$Name lacks nonzero actual receipt"}
	return $parsed
}
function NativeArtifact($Frame,[string]$Kind,[string]$Directory,[long]$Limit=536870912) {
	$entries=@($Frame.artifacts|Where-Object kind -eq $Kind)
	if($entries.Count -ne 1){Unverified "Missing unique native $Kind artifact"}
	$path=ArtifactPath $entries[0] $Directory
	NativeRequire ((Get-Item -LiteralPath $path).Length -le $Limit) "$Kind artifact exceeds bounded native proof input"
	return @{path=$path;bytes=[IO.File]::ReadAllBytes($path)}
}
function NativeFiniteArray($Value,[int]$Count,[string]$Name) {
	NativeRequire ($Value -is [array] -and $Value.Count -eq $Count) "$Name has missing native components"
	foreach($number in $Value){NativeRequire ($number -is [ValueType] -and $number -isnot [bool] -and ![double]::IsNaN([double]$number) -and ![double]::IsInfinity([double]$number)) "$Name has nonfinite/non-numeric native components"}
}
# Lighting bytes remain IEEE bit patterns: signed zero and NaN payloads are
# shader inputs, so floating-point conversion must not normalize their identity.
function InitializeNativeLightingProof {
	if('TerraRendererLightingProof' -as [type]){return}
	Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
public static class TerraRendererLightingProof {
	public const int RecordBytes=176, Capacity=1024, BlockBytes=180224;
	public static void Validate(byte[] bytes,int active) {
		if(bytes==null || bytes.Length!=BlockBytes || active<0 || active>Capacity) throw new ArgumentException("Lighting full native capacity/extent invalid");
		for(int i=active*RecordBytes;i<bytes.Length;i++) if(bytes[i]!=0) throw new ArgumentException("Lighting inactive tail is not all-zero bytes");
	}
	public static int[] Dirty(byte[] before,byte[] after) {
		if(before==null || after==null || before.Length!=BlockBytes || after.Length!=BlockBytes) throw new ArgumentException("Lighting full payload missing");
		var dirty=new List<int>();
		for(int r=0;r<Capacity;r++) for(int b=r*RecordBytes;b<(r+1)*RecordBytes;b++) if(before[b]!=after[b]) {dirty.Add(r);break;}
		return dirty.ToArray();
	}
}
'@
}
function VerifyNativeLightingRanges($Upload) {
	# Allocation-relative logical writes and physical flushes have distinct
	# accounting. A coherent memory type genuinely performs no cache flush.
	foreach($field in @('allocationId','allocationGeneration','allocationSizeBytes','regionOffsetBytes','regionSizeBytes','nonCoherentAtomSize','frameSlot','snapshotSlot','activeRecordCount','logicalWriteBytes','logicalWriteRecords','logicalWriteRanges','physicalFlushBytes','physicalFlushRanges','mirrorCpuDurationNanoseconds','coherent','hostWriteStatus')) {
		if(!(Has $Upload $field)){Unverified "Missing native lighting mapping field $field"}
	}
	NativeRequire ($Upload.purpose -in @('FinalLights','Analysis') -and $Upload.hostWriteStatus -ceq 'Succeeded' -and $Upload.coherent -is [bool]) 'Lighting host publication unavailable or failed'
	foreach($field in @('allocationId','allocationGeneration','allocationSizeBytes','nonCoherentAtomSize')) {NativeRequire ((NativeUnsigned (Metric $Upload.$field $field) $field $true) -gt 0) "Lighting $field missing"}
	$size=NativeUnsigned (Metric $Upload.allocationSizeBytes allocationSizeBytes) allocationSizeBytes
	$offset=NativeUnsigned (Metric $Upload.regionOffsetBytes regionOffsetBytes) regionOffsetBytes
	$atom=NativeUnsigned (Metric $Upload.nonCoherentAtomSize nonCoherentAtomSize) nonCoherentAtomSize
	NativeRequire (($atom -band ($atom-1)) -eq 0) 'Lighting noncoherent atom is not a native power of two'
	NativeRequire ((Metric $Upload.frameSlot frameSlot) -lt 3 -and (Metric $Upload.snapshotSlot snapshotSlot) -lt 8 -and (Metric $Upload.activeRecordCount activeRecordCount) -le 1024) 'Lighting native slots/count exceed capacity'
	NativeRequire ($size -ge 180224 -and $offset -le $size-180224 -and $offset -eq 180224*(Metric $Upload.snapshotSlot snapshotSlot) -and (Metric $Upload.regionSizeBytes regionSizeBytes) -eq 180224) 'Lighting physical slice is outside its actual allocation'
	$logical=@(Metric $Upload.logicalWriteRanges logicalWriteRanges);$flush=@(Metric $Upload.physicalFlushRanges physicalFlushRanges)
	$bytes=[uint64]0;$last=$offset;$records=[Collections.Generic.List[int]]::new()
	foreach($range in $logical) {
		$start=NativeUnsigned $range.offsetBytes writeOffset;$length=NativeUnsigned $range.lengthBytes writeLength $true
		NativeRequire ($start -ge $last -and $start -ge $offset -and $start -le $offset+180224 -and $length -le $offset+180224-$start -and ($start-$offset)%176 -eq 0 -and $length%176 -eq 0) 'Lighting logical range overlaps or leaves exact record slice'
		for($r=[int](($start-$offset)/176);$r -lt [int](($start-$offset+$length)/176);$r++){$records.Add($r)}
		$bytes+=$length;$last=$start+$length
	}
	NativeRequire ($bytes -eq (Metric $Upload.logicalWriteBytes logicalWriteBytes) -and $records.Count -eq (Metric $Upload.logicalWriteRecords logicalWriteRecords)) 'Lighting logical byte/record accounting mismatch'
	$expected=[Collections.Generic.List[object]]::new()
	if(!$Upload.coherent){foreach($range in $logical){
		$start=[uint64]$range.offsetBytes;$end=$start+[uint64]$range.lengthBytes;$start-=$start%$atom
		$extra=if($end%$atom -eq 0){[uint64]0}else{$atom-$end%$atom}
		$end=if($extra -gt $size-$end){$size}else{$end+$extra}
		if($expected.Count -and $start -le $expected[$expected.Count-1].offsetBytes+$expected[$expected.Count-1].lengthBytes){$prior=$expected[$expected.Count-1];$prior.lengthBytes=[Math]::Max($prior.offsetBytes+$prior.lengthBytes,$end)-$prior.offsetBytes}
		else{$expected.Add([pscustomobject]@{offsetBytes=$start;lengthBytes=$end-$start})}
	}}
	NativeRequire ($flush.Count -eq $expected.Count) 'Lighting physical flush range count mismatch'
	$physical=[uint64]0
	for($i=0;$i -lt $flush.Count;$i++) {
		NativeRequire ((NativeUnsigned $flush[$i].offsetBytes flushOffset) -eq $expected[$i].offsetBytes -and (NativeUnsigned $flush[$i].lengthBytes flushLength $true) -eq $expected[$i].lengthBytes) 'Lighting flush is not the bounded atom-aligned logical union'
		$physical+=$expected[$i].lengthBytes
	}
	NativeRequire ($physical -eq (Metric $Upload.physicalFlushBytes physicalFlushBytes)) 'Lighting physical flush byte accounting mismatch'
	$null=Metric $Upload.mirrorCpuDurationNanoseconds mirrorCpuDurationNanoseconds
	return ,$records.ToArray()
}
function CompareNativeLightingUpload($Previous,$Current,[bool]$ForceFull=$false) {
	InitializeNativeLightingProof
	$actual=VerifyNativeLightingRanges $Current.upload
	try{$dirty=[TerraRendererLightingProof]::Dirty($Previous.payload,$Current.payload)}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}
	$expected=if($ForceFull){@(0..1023)}else{@($dirty)}
	NativeRequire (($actual -join ',') -ceq ($expected -join ',')) 'Lighting logical writes do not equal actual canonical record changes'
	return $dirty.Count
}
function ReadNativeLightingProof($Frame,$Identity,[string]$Directory,$RootArtifactIndex=$null) {
	InitializeNativeProof;InitializeNativeLightingProof
	NativeRequire ($Frame.evidenceKind -ceq 'Lighting' -and $Frame.publicationState -ceq 'Complete' -and $Frame.payloadPurpose -in @('FinalLights','Analysis')) 'Native lighting completed kind/purpose missing'
	foreach($field in @('lightId','faceIndex')){NativeRequire ($Frame.$field.status -ceq 'Unavailable' -and $null -eq $Frame.$field.value) 'Lighting row manufactured a shadow region'}
	foreach($field in $Frame.shadow.PSObject.Properties.Name){NativeRequire ($Frame.shadow.$field.status -ceq 'Unavailable') 'Lighting row manufactured shadow metrics'}
	foreach($entry in $Frame.artifacts){if($null -ne $RootArtifactIndex){NativeRequire ($RootArtifactIndex.Contains((Canonical $entry))) 'Native lighting artifact is outside root descriptor closure'}}
	$artifact=NativeArtifact $Frame Counters $Directory 16777216
	$counters=[Text.Encoding]::UTF8.GetString($artifact.bytes)|ConvertFrom-Json
	NativeReceiptFields $counters @('schemaVersion','evidenceKind','analysisPurpose','fixtureId','fixtureHash','operationId','fixtureStepId','runId','runIndex','seed','mode','stage','measurementPhase','stepFrame','payloadEncoding','recordBytes','recordCapacity','payloadPurpose','fullSourceCandidateCount','lightingUploads','colorSourceFormat','colorNativeExtent','imageEncoding','effectiveOptions','readbackProvenance','nativePreparation') 'lighting counters'
	NativeRequire ($counters.schemaVersion -eq 1 -and $counters.evidenceKind -ceq 'Lighting' -and $counters.analysisPurpose -is [bool] -and $counters.analysisPurpose -eq ($Frame.payloadPurpose -ceq 'Analysis') -and $counters.payloadPurpose -ceq $Frame.payloadPurpose) 'Native lighting kind/purpose correlation mismatch'
	foreach($field in @('fixtureId','runId','runIndex','seed','mode','stage')){NativeRequire (JsonEqual $counters.$field $Identity.$field) "Native lighting $field correlation mismatch"}
	NativeRequire ($counters.fixtureHash -ceq (Metric $Identity.fixtureHash fixtureHash) -and $counters.operationId -cmatch '^[0-9a-f]{32}$' -and $counters.operationId -cne ('0'*32)) 'Native lighting fixture/operation identity mismatch'
	NativeRequire ($counters.fixtureStepId -ceq $Frame.fixtureStepId -and $counters.measurementPhase -ceq $Frame.measurementPhase -and (NativeUnsigned $counters.stepFrame stepFrame) -eq $Frame.stepFrame) 'Native lighting captured step correlation mismatch'
	$p=$counters.readbackProvenance
	NativeRequire ($p.kind -ceq 'FullNativeSameCommandCompletedReadback') 'Lighting readback lacks actual completed command provenance'
	foreach($field in @('frameSerial','commandSerial','submitSerial','completionSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration')){NativeRequire ((NativeUnsigned $p.$field $field $true) -eq (Metric $Frame.$field $field)) "Native lighting $field mismatch"}
	foreach($field in @('fixtureControlGeneration','optionsSessionGeneration')){$null=NativeUnsigned $p.$field $field $true}
	NativeRequire ((NativeUnsigned $p.completionSerial completionSerial) -ge (NativeUnsigned $p.submitSerial submitSerial)) 'Lighting completion precedes actual submit'
	NativeRequire (JsonEqual $counters.effectiveOptions $Frame.effectiveOptions) 'Native lighting effective settings mismatch'
	$sourceCount=NativeUnsigned $counters.fullSourceCandidateCount fullSourceCandidateCount
	NativeRequire ($counters.fullSourceCandidateCount -is [ValueType] -and $counters.fullSourceCandidateCount -isnot [bool] -and $sourceCount -le 4096 -and $sourceCount -eq (Metric $Frame.fullSourceCandidateCount fullSourceCandidateCount)) 'Native uncapped source candidate count differs from frame or bounds'
	NativeRequire ($counters.payloadEncoding -ceq 'float32-ieee-le' -and $counters.recordBytes -eq 176 -and $counters.recordCapacity -eq 1024) 'Lighting full payload ABI/encoding mismatch'
	NativeRequire ($Frame.lighting -is [array] -and $Frame.lighting.Count -eq 2 -and $counters.lightingUploads -is [array] -and $counters.lightingUploads.Count -eq 2) 'Both native lighting purposes are required'
	$selected=$null;$purposes=@{}
	foreach($upload in $Frame.lighting) {
		NativeRequire (!$purposes.ContainsKey($upload.purpose)) 'Duplicate lighting purpose';$purposes[$upload.purpose]=$true
		$null=VerifyNativeLightingRanges $upload
		NativeRequire ($sourceCount -ge (Metric $upload.activeRecordCount activeRecordCount)) 'Native source count is below selected payload count'
		$raw=@($counters.lightingUploads|Where-Object purpose -CEQ $upload.purpose)
		if($raw.Count -eq 1){NativeReceiptFields $raw[0] @('available','succeeded','analysisPurpose','coherent','allocationId','allocationGeneration','allocationSizeBytes','regionOffsetBytes','regionSizeBytes','nonCoherentAtomSize','frameSlot','snapshotSlot','activeRecordCount','logicalWriteBytes','logicalWriteRecords','logicalWriteRanges','physicalFlushBytes','physicalFlushRanges','mirrorCpuDurationNanoseconds','payloadLightIds') 'lighting raw upload'}
		NativeRequire ($raw.Count -eq 1 -and $raw[0].available -is [bool] -and $raw[0].available -and $raw[0].succeeded -is [bool] -and $raw[0].succeeded -and $raw[0].analysisPurpose -is [bool] -and $raw[0].analysisPurpose -eq ($upload.purpose -ceq 'Analysis') -and $raw[0].coherent -is [bool] -and $raw[0].coherent -eq $upload.coherent) 'Lighting raw successful purpose receipt mismatch'
		foreach($field in @('allocationId','allocationGeneration','allocationSizeBytes','regionOffsetBytes','regionSizeBytes','nonCoherentAtomSize','frameSlot','snapshotSlot','activeRecordCount','logicalWriteBytes','logicalWriteRecords','physicalFlushBytes','mirrorCpuDurationNanoseconds')){NativeRequire ((NativeUnsigned $raw[0].$field $field) -eq (Metric $upload.$field $field)) "Forged lighting $field counter"}
		NativeRequire ($raw[0].payloadLightIds -is [array] -and $raw[0].payloadLightIds.Count -eq (Metric $upload.activeRecordCount activeRecordCount) -and (JsonEqual @(Metric $upload.payloadLightIds payloadLightIds) $raw[0].payloadLightIds)) 'Lighting raw/frame actual selected IDs differ'
		$purposeIds=@{};foreach($id in $raw[0].payloadLightIds){NativeRequire ($id -is [string] -and $id -cmatch '^[0-9a-f]{32}$' -and $id -cne ('0'*32) -and !$purposeIds.ContainsKey($id)) 'Lighting actual purpose IDs invalid or duplicated';$purposeIds[$id]=$true}
		foreach($field in @('logicalWriteRanges','physicalFlushRanges')) {
			$ranges=@(Metric $upload.$field $field);NativeRequire ($raw[0].$field -is [array] -and $raw[0].$field.Count -eq $ranges.Count) 'Lighting raw range count mismatch'
			for($i=0;$i -lt $ranges.Count;$i++){NativeRequire ($raw[0].$field[$i].Count -eq 2 -and (NativeUnsigned $raw[0].$field[$i][0] rangeOffset) -eq $ranges[$i].offsetBytes -and (NativeUnsigned $raw[0].$field[$i][1] rangeBytes) -eq $ranges[$i].lengthBytes) 'Lighting raw/frame exact range mismatch'}
		}
		if($upload.purpose -ceq $Frame.payloadPurpose){$selected=$upload;$selectedRaw=$raw[0]}
	}
	NativeRequire ($purposes.ContainsKey('FinalLights') -and $purposes.ContainsKey('Analysis') -and $null -ne $selected) 'Lighting purpose set incomplete'
	NativeRequire ((Metric $Frame.lighting[0].allocationId allocationId) -ne (Metric $Frame.lighting[1].allocationId allocationId)) 'Lighting purposes alias the same native allocation'
	foreach($field in @('frameSlot','snapshotSlot','regionOffsetBytes','regionSizeBytes')){NativeRequire ((Metric $Frame.lighting[0].$field $field) -eq (Metric $Frame.lighting[1].$field $field)) 'Lighting purposes name different physical snapshot slices'}
	$payload=NativeArtifact $Frame LightingPayload $Directory 180224
	$descriptor=@($Frame.artifacts|Where-Object kind -CEQ LightingPayload)[0]
	NativeRequire ($descriptor.payloadPurpose -ceq $Frame.payloadPurpose) 'Native payload artifact purpose mismatch'
	try{[TerraRendererLightingProof]::Validate($payload.bytes,[int](Metric $selected.activeRecordCount activeRecordCount))}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}
	NativeRequire ($selectedRaw.payloadLightIds -is [array] -and $selectedRaw.payloadLightIds.Count -eq (Metric $selected.activeRecordCount activeRecordCount)) 'Lighting actual payload order identities missing'
	$ids=@{};foreach($id in $selectedRaw.payloadLightIds){NativeRequire ($id -is [string] -and $id -cmatch '^[0-9a-f]{32}$' -and $id -cne ('0'*32) -and !$ids.ContainsKey($id)) 'Lighting payload IDs invalid or duplicated';$ids[$id]=$true}
	$colorResolution=$Identity.resolution
	if($Identity.stage -ceq 'P3'){
		NativeRequire ($counters.colorNativeExtent.Count -eq 2 -and $counters.colorNativeExtent[0] -gt 0 -and $counters.colorNativeExtent[1] -gt 0 -and $counters.colorNativeExtent[0] -le 16384 -and $counters.colorNativeExtent[1] -le 16384) 'P3 current resized color extent invalid'
		$colorResolution=[pscustomobject]@{width=$counters.colorNativeExtent[0];height=$counters.colorNativeExtent[1]}
	}
	NativeRequire ($counters.colorNativeExtent.Count -eq 2 -and $counters.colorNativeExtent[0] -eq $colorResolution.width -and $counters.colorNativeExtent[1] -eq $colorResolution.height -and $counters.colorSourceFormat -in @(37,43,44,50,97) -and $counters.imageEncoding -ceq 'png-rgba8') 'Lighting same-rendered-color extent/format unavailable'
	$image=NativeArtifact $Frame Image $Directory
	try{$decoded=[TerraRendererNativeProof]::DecodeImage($image.bytes,[int]$colorResolution.width,[int]$colorResolution.height)}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}
	$owner=@($Frame.viewType,$Frame.viewId,$Frame.payloadPurpose,(Metric $Frame.rendererSessionGeneration rendererSessionGeneration),(Metric $selected.allocationId allocationId),(Metric $selected.allocationGeneration allocationGeneration),(Metric $selected.frameSlot frameSlot),(Metric $selected.snapshotSlot snapshotSlot)) -join '|'
	return @{counters=$counters;frame=$Frame;upload=$selected;payload=$payload.bytes;payloadLightIds=$selectedRaw.payloadLightIds;image=$decoded;imageHash=@($Frame.artifacts|Where-Object kind -CEQ Image)[0].sha256;operationId=$counters.operationId;owner=$owner;occupiedPixels=0;semanticRows=@()}
}
function VerifyNativeLightingSourceCount($Proof,$Step,[string]$FixtureId,[bool]$Baseline=$false) {
	# Installed manifests select declarative frozen actions. These counts are the
	# matching compiled fixture recipe contract, not inferred from capped GPU IDs.
	# If a supplied authoritative recipe includes its actual candidate arrays,
	# use that explicit frozen input rather than a payload-derived estimate.
	if((Has $Step 'nativePayload') -and (Has $Step.nativePayload $(if($Baseline){'baselineCandidateOrder'}else{'candidateOrder'}))){
		$expected=@($Step.nativePayload.$(if($Baseline){'baselineCandidateOrder'}else{'candidateOrder'})).Count
	}elseif($FixtureId -ceq 'lighting-dirty'){
		$expected=1000
		if(!$Baseline){switch($Step.action){'add-light'{$expected=1001};'remove-light'{$expected=999};'shrink-light-count'{$expected=16};'zero-lights'{$expected=0}}}
	}elseif($FixtureId -ceq 'lighting-thresholds'){
		$counts=@{'zero'=0;'fallback-16'=16;'clustered-17'=17;'capacity-1024'=1024;'over-cap-1025'=1025}
		NativeRequire ($counts.ContainsKey($Step.action)) 'Unknown frozen lighting threshold source action';$expected=$counts[$Step.action]
	}elseif($FixtureId -in @('depth-coverage','depth-heavy')){
		$expected=if($FixtureId -ceq 'depth-coverage'){17}else{1000}
	}elseif($FixtureId -in @('multiview-lights','recovery-capacity')){
		$hasCounters=if($Proof -is [Collections.IDictionary]){$Proof.Contains('counters')}else{Has $Proof 'counters'}
		$expected=if(!$Baseline -and $Step.action -ceq 'buffer-overflow'){1025}elseif($hasCounters -and (Has $Proof.counters 'stage') -and $Proof.counters.stage -ceq 'P3'){32}else{5}
	}else{Unverified 'Frozen actual lighting source recipe unavailable'}
	NativeRequire ((Metric $Proof.frame.fullSourceCandidateCount fullSourceCandidateCount) -eq $expected) 'Actual uncapped native candidate count differs from frozen source recipe'
	if($FixtureId -in @('multiview-lights','recovery-capacity')){
		# Shared controlled fixtures have upload/retention budgets of 1024. Their
		# one area light fits the special budget; screen influence only ranks the
		# remaining lights. Both actual payloads must therefore retain the whole
		# bounded input, rather than accepting matching omissions in both runs.
		$expected=[Math]::Min([uint64](Metric $Proof.frame.fullSourceCandidateCount fullSourceCandidateCount),[uint64]1024)
		NativeRequire ((Metric $Proof.upload.activeRecordCount activeRecordCount) -eq $expected) 'Shared native payload omits frozen eligible light candidates'
	}
}
function VerifyNativeLightingSelection($Proof,$Step,[string]$FixtureId) {
	# This frozen source recipe generates IDs directly from the authored ordinal.
	# The verifier joins actual copied backend record order, not declared counts.
	VerifyNativeLightingSourceCount $Proof $Step $FixtureId
	if($FixtureId -in @('multiview-lights','recovery-capacity')){return}

	if($FixtureId -notin @('lighting-dirty','lighting-thresholds')){return}
	$count=1000;$omit=0;$reverse=$false;$final=$Proof.frame.payloadPurpose -ceq 'FinalLights'
	switch($Step.action){
		'edit-outside-retained-subset' {if($final){$count=999}}
		'add-light' {$count=1001;if($final){$omit=1000}}
		'remove-light' {$omit=500}
		'reorder-lights' {$reverse=$true}
		'shrink-light-count' {$count=16}
		'zero-lights' {$count=0}
		'zero' {$count=0}
		'fallback-16' {$count=16}
		'clustered-17' {$count=17}
		'capacity-1024' {$count=1024}
		'over-cap-1025' {$count=1024}
	}
	$expected=[Collections.Generic.List[string]]::new()
	for($i=1;$i -le $count;$i++){if($i -ne $omit){$expected.Add('6520000000000000'+$i.ToString('x16'))}}
	if($reverse){$expected.Reverse()}
	NativeRequire (($Proof.payloadLightIds -join '|') -ceq ($expected.ToArray() -join '|')) 'Frozen lighting retained count/order/cap differs from actual payload identities'
}
function CompareNativeLightingProof($Candidate,$Reference,$Resolution) {
	if($Candidate.counters.stage -ceq 'P3'){
		NativeRequire (JsonEqual $Candidate.counters.colorNativeExtent $Reference.counters.colorNativeExtent) 'Paired P3 resized lighting extent differs'
		$Resolution=[pscustomobject]@{width=$Candidate.counters.colorNativeExtent[0];height=$Candidate.counters.colorNativeExtent[1]}
	}
	NativeRequire (JsonEqual $Candidate.payloadLightIds $Reference.payloadLightIds) 'Paired actual lighting record identity/order differs'
	NativeRequire ($Candidate.frame.viewType -ceq $Reference.frame.viewType -and $Candidate.frame.viewId -eq $Reference.frame.viewId -and $Candidate.frame.payloadPurpose -ceq $Reference.frame.payloadPurpose -and (Metric $Candidate.upload.activeRecordCount activeRecordCount) -eq (Metric $Reference.upload.activeRecordCount activeRecordCount)) 'Paired native lighting view/purpose/count differs'
	NativeRequire ([TerraRendererLightingProof]::Dirty($Candidate.payload,$Reference.payload).Length -eq 0) 'Full native canonical lighting payload differs from reference'
	try{$image=[TerraRendererNativeProof]::DecodedImageDelta($Candidate.image,$Reference.image,[int]$Resolution.width,[int]$Resolution.height)}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}
	NativeRequire ($image -le (2.0/255.0)) 'Native lighting image correctness tolerance exceeded'
	return @{maxDepthAbsoluteError=0.0;maxImageNormalizedChannelError=$image}
}
function VerifyNativeLightingCohorts($Proofs,$Cameras,[bool]$RequireAllSlots=$false) {
	# Each completed primary produces exactly two payload purposes per declared
	# view. Their metadata cannot infer missing views, commands, or physical slots.
	$cohorts=@{};$slots=@{}
	foreach($proof in $Proofs){
		$f=$proof.frame;$p=$proof.counters.readbackProvenance;$view="$($f.viewType)|$($f.viewId)"
		NativeRequire (@($Cameras|Where-Object {$_.viewType -ceq $f.viewType -and $_.viewId -eq $f.viewId}).Count -eq 1) 'Lighting cohort names undeclared view'
		$key="$($f.fixtureStepId)|$($f.stepFrame)|$($f.measurementPhase)"
		if(!$cohorts.ContainsKey($key)){$cohorts[$key]=@{provenance=$p;options=$f.effectiveOptions;rows=@{};commands=@{};uploads=@{};images=@{};physical=@{}}}
		$c=$cohorts[$key]
		foreach($field in @('frameSerial','submitSerial','completionSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration','fixtureControlGeneration','optionsSessionGeneration')){NativeRequire ($p.$field -ceq $c.provenance.$field) "Lighting cohort mixes $field"}
		NativeRequire (JsonEqual $f.effectiveOptions $c.options) 'Lighting cohort mixes effective settings'
		if(!$c.commands.ContainsKey($view)){$c.commands[$view]=$p.commandSerial};NativeRequire ($c.commands[$view] -ceq $p.commandSerial) 'Lighting cohort mixes commands within view'
		if(!$c.uploads.ContainsKey($view)){$c.uploads[$view]=Canonical $proof.counters.lightingUploads;$c.images[$view]=$proof.imageHash}
		NativeRequire ($c.uploads[$view] -ceq (Canonical $proof.counters.lightingUploads) -and $c.images[$view] -ceq $proof.imageHash) 'Lighting purposes mix native upload receipts or rendered colors'
		$u=$proof.upload;$physical="$(Metric $u.allocationId allocationId)|$(Metric $u.allocationGeneration allocationGeneration)"
		$offset=Metric $u.regionOffsetBytes regionOffsetBytes;$end=$offset+(Metric $u.regionSizeBytes regionSizeBytes)
		if(!$c.physical.ContainsKey($physical)){$c.physical[$physical]=[Collections.Generic.List[object]]::new()}
		foreach($prior in $c.physical[$physical]){NativeRequire ($view -ceq $prior.view -or $end -le $prior.offset -or $offset -ge $prior.end) 'Lighting qualified views overlap actual physical payload region'}
		$c.physical[$physical].Add(@{view=$view;offset=$offset;end=$end})
		$rowKey="$view|$($f.payloadPurpose)";NativeRequire (!$c.rows.ContainsKey($rowKey)) 'Duplicate lighting purpose/view in cohort';$c.rows[$rowKey]=$true
		if(!$slots.ContainsKey($rowKey)){$slots[$rowKey]=@{}}
		$slots[$rowKey]["$(Metric $proof.upload.frameSlot frameSlot)|$(Metric $proof.upload.snapshotSlot snapshotSlot)"]=$true
	}
	NativeRequire ($cohorts.Count -gt 0) 'Native lighting cohort is empty'
	foreach($c in $cohorts.Values){foreach($camera in $Cameras){foreach($purpose in @('FinalLights','Analysis')){NativeRequire ($c.rows.ContainsKey("$($camera.viewType)|$($camera.viewId)|$purpose")) 'Lighting cohort omitted purpose/view'}}}
	if($RequireAllSlots){foreach($camera in $Cameras){foreach($purpose in @('FinalLights','Analysis')){NativeRequire ($slots["$($camera.viewType)|$($camera.viewId)|$purpose"].Count -eq 24) 'Lighting native observation omitted reusable physical frame/snapshot slot'}}}
}
function ReadNativeLightingBaselines($Envelope,$Document,$Identity,$Directory,$Steps,$Cameras) {
	$index=NewNativeArtifactClosureIndex $Document.artifacts;$result=@{}
	foreach($step in $Steps){
		$matches=@($Envelope.receipts|Where-Object {$_.kind -ceq 'LightingBaseline' -and $_.fixtureStepId -ceq $step.id})
		if($matches.Count -ne 1){Unverified "Missing exact captured LightingBaseline for $($step.id)"}
		$deviceAuthority=$false;$deviceSession=0;$deviceOptionsEpoch=0;$deviceOldSession=0;$deviceOldEpoch=0
		if($step.action -ceq 'device-recovery'){
			# Only the frozen, independently verified native recreation receipt can
			# authorize a new device owner; a changed allocation alone cannot.
			foreach($receipt in (NativeReceiptStep $Envelope DeviceRecreation $step.id)){VerifyNativeDeviceRecreation $receipt $Envelope $step.id;$deviceSession=NativeUnsigned $receipt.newRendererSessionGeneration newSession $true;$deviceOptionsEpoch=NativeUnsigned $receipt.newOptionsEpoch newOptionsEpoch $true;$deviceOldSession=NativeUnsigned $receipt.oldRendererSessionGeneration oldSession $true;$deviceOldEpoch=NativeUnsigned $receipt.priorOptionsEpoch priorOptionsEpoch $true}
			$deviceAuthority=$true
		}
		$r=$matches[0];Validate $r $script:definitionRoot.'$defs'.lightingBaseline
		NativeRequire ($r.frames.Count -eq 120*$Cameras.Count) 'Lighting baseline lacks exact60 two-purpose view cohorts'
		$receiptIndex=NewNativeArtifactClosureIndex $r.artifacts;$proofs=[Collections.Generic.List[object]]::new();$owners=@{};$seen=@{}
		foreach($a in $r.artifacts){NativeRequire ($index.Contains((Canonical $a))) 'Lighting baseline receipt is outside root artifact closure';$null=ArtifactPath $a $Directory}
		foreach($f in $r.frames){
			NativeRequire ($f.fixtureStepId -ceq $step.id -and $f.evidenceKind -ceq 'Lighting' -and $f.measurementPhase -ceq 'LightingBaseline' -and $f.stepFrame -lt 60) 'Lighting baseline changed original tokens'
			NativeRequire (JsonEqual $f.effectiveOptions $Document.requestedOptions) 'Lighting baseline differs from requested effective options'
			$key=ObservationKey $f;NativeRequire (!$seen.ContainsKey($key)) 'Duplicate lighting baseline frame';$seen[$key]=$true
			foreach($a in $f.artifacts){NativeRequire ($receiptIndex.Contains((Canonical $a))) 'Lighting baseline artifact omitted from receipt closure'}
			$p=ReadNativeLightingProof $f $Identity $Directory $index
			NativeRequire ($p.operationId -ceq $Envelope.operationId) 'Lighting baseline changed operation owner'
			VerifyNativeLightingSourceCount $p $step $Identity.fixtureId $true
			if($deviceAuthority){NativeRequire ((Metric $f.rendererSessionGeneration rendererSessionGeneration) -eq $deviceOldSession -and (NativeUnsigned $p.counters.readbackProvenance.optionsSessionGeneration optionsEpoch $true) -eq $deviceOldEpoch) 'Lighting baseline does not belong to requested old device lifetime'}
			$proofs.Add(@{frame=$p.frame;counters=$p.counters;upload=$p.upload;imageHash=$p.imageHash})
			if(!$owners.ContainsKey($p.owner) -or (Metric $owners[$p.owner].frame.frameSerial frameSerial) -lt (Metric $f.frameSerial frameSerial)){$owners[$p.owner]=@{frame=$p.frame;upload=$p.upload;payload=$p.payload;owner=$p.owner}}
		}
		VerifyNativeLightingCohorts $proofs.ToArray() $Cameras $true
		$result[$step.id]=@{owners=$owners;slots=@{};firstChanges=@{};baseline=$true;deviceAuthority=$deviceAuthority;deviceSession=$deviceSession;deviceOptionsEpoch=$deviceOptionsEpoch;deviceAllocations=@{};baselineFrame=(@($proofs|ForEach-Object {Metric $_.frame.frameSerial frameSerial}|Measure-Object -Maximum).Maximum)}
	}
	return $result
}
function CompareNativeLightingBaselines($Candidate,$Reference,$CandidateIdentity,$ReferenceIdentity,$CandidateDirectory,$ReferenceDirectory) {
	# Process clocks and allocation IDs differ legitimately. Match actual physical
	# slot coordinates, then freshly hash/read one pair at a time so decoded
	# images are never retained for every baseline owner across all steps.
	$maximum=0.0
	foreach($step in $Candidate.Keys){
		NativeRequire ($Reference.ContainsKey($step)) 'Unpaired native lighting baseline step'
		$references=@{}
		foreach($p in $Reference[$step].owners.Values){$f=$p.frame;$u=$p.upload;$key="$($f.viewType)|$($f.viewId)|$($f.payloadPurpose)|$(Metric $u.frameSlot frameSlot)|$(Metric $u.snapshotSlot snapshotSlot)";NativeRequire (!$references.ContainsKey($key)) 'Ambiguous baseline physical owner';$references[$key]=$p}
		NativeRequire ($references.Count -eq $Candidate[$step].owners.Count) 'Unpaired baseline physical owner count'
		foreach($p in $Candidate[$step].owners.Values){
			$f=$p.frame;$u=$p.upload;$key="$($f.viewType)|$($f.viewId)|$($f.payloadPurpose)|$(Metric $u.frameSlot frameSlot)|$(Metric $u.snapshotSlot snapshotSlot)"
			NativeRequire ($references.ContainsKey($key)) 'Missing paired baseline physical owner'
			$cp=ReadNativeLightingProof $f $CandidateIdentity $CandidateDirectory
			$rp=ReadNativeLightingProof $references[$key].frame $ReferenceIdentity $ReferenceDirectory
			$delta=CompareNativeLightingProof $cp $rp $CandidateIdentity.resolution
			$maximum=[Math]::Max($maximum,$delta.maxImageNormalizedChannelError);$cp=$rp=$null
		}
	}
	return $maximum
}
function UpdateNativeLightingTimeline($State,$Proof,$Step,[bool]$ForceFull=$false) {
	$owner=$Proof.owner;$stepState=$State[$Step.id]
	if($null -eq $stepState){Unverified 'Native lighting baseline state unavailable'}
	if($Step.action -ceq 'device-recovery'){
		NativeRequire ($stepState.ContainsKey('deviceAuthority') -and $stepState.deviceAuthority -and (Metric $Proof.frame.rendererSessionGeneration rendererSessionGeneration) -eq $stepState.deviceSession -and (NativeUnsigned $Proof.counters.readbackProvenance.optionsSessionGeneration optionsEpoch $true) -eq $stepState.deviceOptionsEpoch) 'Post-recovery lighting proof belongs to old or unrelated device lifetime'
	}
	$newDeviceOwner=!$stepState.owners.ContainsKey($owner)
	if($newDeviceOwner){
		NativeRequire ($Step.action -ceq 'device-recovery' -and $stepState.ContainsKey('deviceAuthority') -and $stepState.deviceAuthority) 'Lighting current upload lacks same physical baseline owner or native device authority'
		NativeRequire ((Metric $Proof.frame.rendererSessionGeneration rendererSessionGeneration) -eq $stepState.deviceSession -and (NativeUnsigned $Proof.counters.readbackProvenance.optionsSessionGeneration optionsEpoch $true) -eq $stepState.deviceOptionsEpoch) 'New lighting owner does not belong to actual recreated session/options lifetime'
		NativeRequire ((Metric $Proof.frame.frameSerial frameSerial) -gt $stepState.baselineFrame) 'New device upload does not follow retained baseline'
		$slotKey="$($Proof.frame.viewType)|$($Proof.frame.viewId)|$($Proof.frame.payloadPurpose)|$(Metric $Proof.upload.frameSlot frameSlot)"
		$allocation="$(Metric $Proof.upload.allocationId allocationId)|$(Metric $Proof.upload.allocationGeneration allocationGeneration)"
		if($stepState.deviceAllocations.ContainsKey($slotKey)){NativeRequire ($stepState.deviceAllocations[$slotKey] -ceq $allocation) 'Recreated lighting frame slot changes allocation within current session'}else{$stepState.deviceAllocations[$slotKey]=$allocation}
		# A new device has no predecessor mirror. Its first captured upload must
		# initialize every record, including the inactive zero tail.
		$actual=VerifyNativeLightingRanges $Proof.upload
		if(($actual -join ',') -cne ((0..1023) -join ',')){Unverified 'New device lighting owner lacks observed full capacity initialization'}
		$previous=@{frame=$Proof.frame;payload=$Proof.payload}
	}else{$previous=$stepState.owners[$owner]}
	NativeRequire ($newDeviceOwner -or (Metric $Proof.frame.frameSerial frameSerial) -gt (Metric $previous.frame.frameSerial frameSerial)) 'Lighting current frame does not follow native baseline/predecessor'
	# Native rendering can upload between sampled frames. Only the retained first
	# transition and its same-owner baseline prove the exact edit write. Later
	# observations prove steady bytes and zero writes, never invented predecessor
	# continuity across an unsampled interval.
	$dirty=0
	if($newDeviceOwner){$dirty=1024}
	elseif($Proof.frame.measurementPhase -ceq 'Transition'){$dirty=CompareNativeLightingUpload $previous $Proof $ForceFull}
	elseif($ForceFull){NativeRequire ((Metric $Proof.upload.logicalWriteBytes logicalWriteBytes) -eq 180224 -and (Metric $Proof.upload.logicalWriteRecords logicalWriteRecords) -eq 1024) 'Full lighting reference did not write complete capacity'}
	$pair="$(Metric $Proof.upload.frameSlot frameSlot)|$(Metric $Proof.upload.snapshotSlot snapshotSlot)"
	$key="$($Proof.frame.viewType)|$($Proof.frame.viewId)|$($Proof.frame.payloadPurpose)|$pair"
	$stepState.slots[$key]=$true
	if($Proof.frame.measurementPhase -ceq 'Transition' -and !$stepState.firstChanges.ContainsKey($owner)){
		$stepState.firstChanges[$owner]=$dirty
		if($Step.action -eq 'edit-retained-light-500'){NativeRequire ($dirty -eq 1 -and [TerraRendererLightingProof]::Dirty($previous.payload,$Proof.payload)[0] -eq 499) 'Retained edit did not change exactly actual record500'}
		if($Step.action -eq 'edit-outside-retained-subset'){NativeRequire ($dirty -eq $(if($Proof.frame.payloadPurpose -ceq 'Analysis'){1}else{0})) 'Outside-retained edit changed wrong upload purpose'}
	}
	if($Proof.frame.measurementPhase -ceq 'Measured' -and !$ForceFull){NativeRequire ((Metric $Proof.upload.logicalWriteBytes logicalWriteBytes) -eq 0 -and (Metric $Proof.upload.physicalFlushBytes physicalFlushBytes) -eq 0) 'Stable measured lighting writes/flushes are nonzero'}
	$stepState.owners[$owner]=@{frame=$Proof.frame;upload=$Proof.upload;payload=$Proof.payload;owner=$Proof.owner}
}
function ReadNativeLightingTransitions($Envelope,$Document,$Identity,$Directory,$Steps,$Cameras,$State) {
	$result=@{};$index=NewNativeArtifactClosureIndex $Document.artifacts
	foreach($step in $Steps){
		$matches=@($Envelope.receipts|Where-Object {$_.kind -ceq 'ControlTransition' -and $_.fixtureStepId -ceq $step.id -and @($_.frames|Where-Object { (Has $_ 'evidenceKind') -and $_.evidenceKind -ceq 'Lighting'}).Count})
		if($matches.Count -ne 1){Unverified "Missing first actual lighting transition for $($step.id)"}
		$r=$matches[0];Validate $r $script:definitionRoot.'$defs'.controlTransitionReceipt
		NativeRequire ($r.action -ceq $step.action -and $r.originalCorrelation.operationId -ceq $Envelope.operationId -and $r.originalCorrelation.fixtureStepId -ceq $step.id -and $r.originalCorrelation.stepFrame -eq 0) 'Lighting transition changed original action/correlation'
		$warmup=@($Document.frames|Where-Object {$_.fixtureStepId -ceq $step.id -and $_.measurementPhase -ceq 'Warmup' -and (Has $_ 'evidenceKind') -and $_.evidenceKind -ceq 'Lighting'}|Sort-Object {Metric $_.frameSerial frameSerial}|Select-Object -First 1)
		NativeRequire ($warmup.Count -eq 1 -and (NativeUnsigned $r.frameSerial transitionFrame $true) -lt (Metric $warmup[0].frameSerial warmupFrame)) 'Lighting first transition does not precede admitted warmup'
		$proofs=[Collections.Generic.List[object]]::new();$map=@{};$receiptIndex=NewNativeArtifactClosureIndex $r.artifacts
		foreach($a in $r.artifacts){NativeRequire ($index.Contains((Canonical $a))) 'Lighting transition receipt is outside root artifact closure';$null=ArtifactPath $a $Directory}
		foreach($f in @($r.frames|Where-Object {(Has $_ 'evidenceKind') -and $_.evidenceKind -ceq 'Lighting'})){
			NativeRequire ($f.evidenceKind -ceq 'Lighting' -and $f.fixtureStepId -ceq $step.id -and $f.measurementPhase -ceq 'Transition' -and $f.stepFrame -eq 0) 'Lighting transition cohort has wrong original frame'
			NativeRequire (JsonEqual $f.effectiveOptions $Document.requestedOptions) 'Lighting transition differs from requested effective options'
			foreach($field in @('frameSerial','submitSerial','completionSerial')){NativeRequire ((Metric $f.$field $field) -eq (NativeUnsigned $r.$field $field $true)) 'Lighting transition serial receipt mismatch'}
			foreach($field in @('mutationGeneration','settingsGeneration','rendererSessionGeneration')){NativeRequire ((Metric $f.$field $field) -eq (NativeUnsigned $r.originalCorrelation.$field $field $true)) 'Lighting transition original generation mismatch'}
			foreach($a in $f.artifacts){NativeRequire ($receiptIndex.Contains((Canonical $a))) 'Lighting transition artifact omitted from receipt closure'}
			$p=ReadNativeLightingProof $f $Identity $Directory $index
			NativeRequire ($p.operationId -ceq $Envelope.operationId -and (NativeUnsigned $p.counters.readbackProvenance.fixtureControlGeneration controlGeneration $true) -eq (NativeUnsigned $r.controlGeneration controlGeneration $true)) 'Lighting transition actual control/operation mismatch'
			VerifyNativeLightingSelection $p $step $Identity.fixtureId
			UpdateNativeLightingTimeline $State $p $step ($Identity.mode -ceq 'Reference')
			$key="$($f.viewType)|$($f.viewId)|$($f.payloadPurpose)";NativeRequire (!$map.ContainsKey($key)) 'Duplicate lighting transition purpose/view';$map[$key]=$p;$proofs.Add($p)
		}
		NativeRequire ((Metric $r.frames[0].commandSerial commandSerial) -eq (NativeUnsigned $r.commandSerial commandSerial $true)) 'Lighting transition representative command mismatch'
		VerifyNativeLightingCohorts $proofs.ToArray() $Cameras
		$result[$step.id]=$map
	}
	return $result
}
function CompareNativeLightingTransitions($Candidate,$Reference,$Resolution) {
	NativeRequire ($Candidate.Count -eq $Reference.Count) 'Unpaired lighting transition steps'
	$maxImage=0.0
	foreach($step in $Candidate.Keys){NativeRequire ($Reference.ContainsKey($step) -and $Candidate[$step].Count -eq $Reference[$step].Count) 'Missing paired lighting transition';foreach($key in $Candidate[$step].Keys){NativeRequire ($Reference[$step].ContainsKey($key)) 'Unpaired lighting transition purpose/view';$delta=CompareNativeLightingProof $Candidate[$step][$key] $Reference[$step][$key] $Resolution;$maxImage=[Math]::Max($maxImage,$delta.maxImageNormalizedChannelError)}}
	return @{maxDepthAbsoluteError=0.0;maxImageNormalizedChannelError=$maxImage}
}
function NativeOrigin($Value,[string]$Name) {
	NativeRequire ($Value.cell -is [array] -and $Value.cell.Count -eq 3) "$Name canonical origin cells missing"
	foreach($cell in $Value.cell){$parsed=[long]0;NativeRequire ([string]$cell -cmatch '^(0|-?[1-9][0-9]*)$' -and [long]::TryParse([string]$cell,[ref]$parsed)) "$Name canonical cell invalid"}
	NativeFiniteArray $Value.offsetMetres 3 "$Name offsets"
}
# Canonical native semantic identities exclude publication epochs (which can
# differ with importer scheduling) while retaining complete 128-bit resource IDs,
# exact world inputs, selected geometry, and actual deformation shader inputs.
function NativeCasterRows($Rows,[bool]$CrossRun=$false) {
	$seen=@{};$result=[Collections.Generic.List[string]]::new()
	$fields=@('objectId','segmentId','meshId','assetId','materialId','shaderId','textureId','worldPosition','referenceOrigin','rotation','scale','shadowBehavior','materialShadowParameters','deformationPalette','morphWeights','geometryRoute','selectedLod','selectionFlags','firstMeshlet','meshletCount','firstCluster','clusterCount','pageId','fallbackPageId','payloadByteOffset','payloadByteSize','payloadSource','emittedSource','staticEligible','staticClassificationReason','evidenceMask')
	foreach($row in @($Rows)) {
		foreach($field in @('objectId','meshId','assetId','materialId','shaderId','textureId')) {NativeRequire ((Has $row $field) -and $row.$field -cmatch '^[0-9a-f]{32}$') "Caster $field lacks full 128-bit identity"}
		NativeRequire ($row.objectId -cne ('0'*32)) 'Caster object identity is empty'
		$lifetime=NativeUnsigned $row.lifetimeGeneration 'caster lifetime' $true
		$segment=NativeUnsigned $row.segmentId 'caster segment'
		foreach($field in $fields){NativeRequire (Has $row $field) "Caster semantic field $field missing"}
		if(!(Has $row 'texturePublications')){Unverified 'Caster exact texture publications unavailable'}
		NativeRequire ($row.texturePublications -is [array]) 'Caster texture publications are not a native array'
		$textureBindings=@{};$textureSemantic=[Collections.Generic.List[string]]::new()
		foreach($texture in $row.texturePublications){
			NativeReceiptFields $texture @('textureId','publicationGeneration','bindingIndex') 'caster texture'
			NativeRequire ($texture.textureId -cmatch '^[0-9a-f]{32}$' -and $texture.textureId -cne ('0'*32)) 'Caster texture publication lacks full identity'
			$null=NativeUnsigned $texture.publicationGeneration texturePublication $true;$binding=NativeUnsigned $texture.bindingIndex textureBinding
			NativeRequire ($binding -le [uint32]::MaxValue -and !$textureBindings.ContainsKey($binding)) 'Caster texture binding duplicated or out of range'
			$textureBindings[$binding]=$true;$textureSemantic.Add((Canonical ([ordered]@{textureId=$texture.textureId;bindingIndex=$binding})))
		}
		NativeOrigin $row.worldPosition 'caster world position';NativeOrigin $row.referenceOrigin 'caster reference origin'
		NativeFiniteArray $row.rotation 4 'caster rotation';NativeFiniteArray $row.scale 3 'caster scale';NativeFiniteArray $row.materialShadowParameters 4 'caster material shadow parameters'
		NativeRequire ($row.deformationPalette -is [array] -and $row.morphWeights -is [array]) 'Caster deformation inputs missing'
		foreach($matrix in $row.deformationPalette){NativeFiniteArray $matrix 16 'caster deformation matrix'}
		if($row.morphWeights.Count){NativeFiniteArray $row.morphWeights $row.morphWeights.Count 'caster morph weights'}
		foreach($field in @('firstMeshlet','meshletCount','firstCluster','clusterCount','pageId','fallbackPageId','payloadByteOffset','payloadByteSize','shadowBehavior','evidenceMask')){$null=NativeUnsigned $row.$field "caster $field"}
		foreach($field in @('geometryRoute','selectedLod','selectionFlags','payloadSource','emittedSource','staticClassificationReason')){$null=NativeUnsigned $row.$field "caster $field"}
		NativeRequire ($row.staticEligible -is [bool]) 'Caster static classification is not native boolean evidence'
		if((NativeUnsigned $row.evidenceMask 'caster evidenceMask') -ne 511){Unverified 'Caster semantic proof is incomplete'}
		$selection=@($row.geometryRoute,$row.selectedLod,$row.selectionFlags,$row.firstMeshlet,$row.meshletCount,$row.firstCluster,$row.clusterCount,$row.pageId,$row.fallbackPageId,$row.payloadByteOffset,$row.payloadByteSize,$row.payloadSource,$row.emittedSource) -join ':'
		$key=@($row.objectId,$lifetime,$segment,$selection) -join '|';NativeRequire (!$seen.ContainsKey($key)) 'Duplicate exact caster identity/selection row';$seen[$key]=$true
		if(!$CrossRun){$result.Add((Canonical $row));continue}
		$semantic=[ordered]@{};foreach($field in $fields){$semantic[$field]=$row.$field};$textures=$textureSemantic.ToArray();[Array]::Sort($textures,[StringComparer]::Ordinal);$semantic.texturePublications=$textures;$result.Add((Canonical $semantic))
	}
	$array=$result.ToArray();[Array]::Sort($array,[StringComparer]::Ordinal);return ,$array
}
function InitializeNativeDepthProof {
	if('TerraRendererDepthProof' -as [type]){return}
	# This offline oracle uses the shader's float32 divide-before-multiply tile
	# partition. It compares actual copied GPU bytes; it creates no GPU receipt.
	Add-Type -TypeDefinition @'
using System;
public static class TerraRendererDepthProof {
 public static bool Eligible(float v){return !float.IsNaN(v)&&!float.IsInfinity(v)&&v>=0&&v<1;}
 public static bool BytesEqual(byte[] a,byte[] b){if(a.Length!=b.Length)return false;for(int i=0;i<a.Length;i++)if(a[i]!=b[i])return false;return true;}
 public static int Edge(int i,int size,int divisor){return Math.Min(size,(int)((float)i*((float)size/(float)divisor)));}
 public static bool Nonempty(byte[] bounds){ValidateBounds(bounds);for(int i=0;i<144;i++)if(BitConverter.ToSingle(bounds,i*8)!=1||BitConverter.ToSingle(bounds,i*8+4)!=1)return true;return false;}
 public static void ValidateBounds(byte[] b){if(b.Length!=1152)throw new Exception("Depth bounds must contain all 144 float2 records");for(int i=0;i<144;i++){float lo=BitConverter.ToSingle(b,i*8),hi=BitConverter.ToSingle(b,i*8+4);if(float.IsNaN(lo)||float.IsInfinity(lo)||float.IsNaN(hi)||float.IsInfinity(hi)||lo<0||hi>1||lo>hi||(hi==1&&lo!=1))throw new Exception("Invalid native depth interval");}}
 public static double BoundsDelta(byte[] a,byte[] b){ValidateBounds(a);ValidateBounds(b);double delta=0;for(int i=0;i<144;i++){bool ea=BitConverter.ToSingle(a,i*8)==1&&BitConverter.ToSingle(a,i*8+4)==1,eb=BitConverter.ToSingle(b,i*8)==1&&BitConverter.ToSingle(b,i*8+4)==1;if(ea!=eb)throw new Exception("Empty native tile class differs");for(int j=0;j<2;j++)delta=Math.Max(delta,Math.Abs((double)BitConverter.ToSingle(a,i*8+j*4)-BitConverter.ToSingle(b,i*8+j*4)));}return delta;}
 public static byte[] Materialize(int w,int h,uint fill,int[] xs,int[] ys,uint[] bits){if(w<1||h<1||w>16384||h>16384||xs.Length!=ys.Length||xs.Length!=bits.Length)throw new Exception("Invalid oracle recipe extent");byte[] b=new byte[checked(w*h*4)];byte[] f=BitConverter.GetBytes(fill);for(int i=0;i<w*h;i++)Array.Copy(f,0,b,i*4,4);var seen=new System.Collections.Generic.HashSet<int>();for(int i=0;i<xs.Length;i++){if(xs[i]<0||xs[i]>=w||ys[i]<0||ys[i]>=h||!seen.Add(ys[i]*w+xs[i]))throw new Exception("Invalid duplicate oracle override");Array.Copy(BitConverter.GetBytes(bits[i]),0,b,(ys[i]*w+xs[i])*4,4);}return b;}
 public static void Oracle(int w,int h,byte[] input,byte[] bounds,byte[] coverage){if(input.Length!=checked(w*h*4)||coverage.Length!=1152)throw new Exception("Incomplete GPU oracle payload");ValidateBounds(bounds);for(int ty=0;ty<9;ty++)for(int tx=0;tx<16;tx++){int index=ty*16+tx;uint visited=0,eligible=0;float lo=1,hi=0;for(int y=Edge(ty,h,9);y<Edge(ty+1,h,9);y++)for(int x=Edge(tx,w,16);x<Edge(tx+1,w,16);x++){visited++;float v=BitConverter.ToSingle(input,(y*w+x)*4);if(Eligible(v)){eligible++;lo=Math.Min(lo,v);hi=Math.Max(hi,v);}}if(BitConverter.ToUInt32(coverage,index*8)!=visited||BitConverter.ToUInt32(coverage,index*8+4)!=eligible)throw new Exception("Actual GPU visited/eligible counts differ");float actualLo=BitConverter.ToSingle(bounds,index*8),actualHi=BitConverter.ToSingle(bounds,index*8+4);if(eligible==0){if(actualLo!=1||actualHi!=1)throw new Exception("Empty oracle tile is not exactly 1/1");}else{if(actualLo==1&&actualHi==1)throw new Exception("Eligible near-clear texel lost");if(Math.Abs((double)actualLo-lo)>1e-6||Math.Abs((double)actualHi-hi)>1e-6)throw new Exception("GPU oracle depth bounds differ");}}}
}
'@
}
function VerifyNativeDepthQuery($Query,$Frame,$Provenance,[string]$Purpose,[bool]$Nonempty) {
	# Raw executed-call receipts are mandatory. Aggregate timing declarations,
	# whole-view profiler samples and an oracle query cannot prove stage speed.
	NativeReceiptFields $Query @('owner','range','status','reason','beginTicks','endTicks','rawTicks','submitSerial','completionSerial','timestampValidBits','timestampPeriodNanoseconds','durationNanoseconds','beginAvailable','endAvailable','nonemptyWork','cooperative','computeStageMarkers','noWaitRead','queryReadFlags','beginStageMask','endStageMask') 'depth query'
	if($Query.status -in @('Pending','Unavailable')){Unverified 'Native isolated depth query is not available'}
	NativeRequire ($Query.status -ceq 'Valid' -and $Query.reason -ceq 'None') 'Unknown depth query status/reason'
	$o=$Query.owner
	NativeReceiptFields $o @('rendererSessionGeneration','backendResourceGeneration','queryPoolGeneration','frameSerial','commandSerial','frameSlot','snapshotSlot','outputId','purpose','view','scope') 'depth query owner'
	foreach($field in @('rendererSessionGeneration','frameSerial','commandSerial')){NativeRequire ((NativeUnsigned $o.$field $field $true) -eq (NativeUnsigned $Provenance.$field $field $true)) "Depth query $field owner mismatch"}
	foreach($field in @('backendResourceGeneration','queryPoolGeneration')){$null=NativeUnsigned $o.$field $field $true}
	foreach($field in @('submitSerial','completionSerial')){NativeRequire ((NativeUnsigned $Query.$field $field $true) -eq (NativeUnsigned $Provenance.$field $field $true)) "Depth query $field mismatch"}
	NativeRequire ($o.purpose -ceq $Purpose -and $o.scope -ceq 'IsolatedDepth' -and $o.view -ceq $Frame.viewType -and $o.outputId -eq $Frame.viewId -and $o.frameSlot -ge 0 -and $o.frameSlot -lt 3 -and $o.snapshotSlot -ge 0 -and $o.snapshotSlot -lt 8) 'Depth query purpose/view/physical slot mismatch'
	foreach($field in @('frameSlot','snapshotSlot','outputId')){$null=NativeUnsigned $o.$field $field}
	foreach($field in @('firstQuery','count')){$null=NativeUnsigned $Query.range.$field $field}
	$first=(($o.frameSlot*8+$o.snapshotSlot)*2+$(if($Purpose -ceq 'Oracle'){1}else{0}))*2
	NativeRequire ($Query.range.count -eq 2 -and $Query.range.firstQuery -eq $first) 'Depth query range aliases another physical owner'
	foreach($flag in @('beginAvailable','endAvailable','nonemptyWork','cooperative','computeStageMarkers','noWaitRead')){NativeRequire ($Query.$flag -is [bool]) "Depth $flag is not an executed boolean"}
	foreach($field in @('queryReadFlags','beginStageMask','endStageMask')){$null=NativeUnsigned $Query.$field $field}
	NativeRequire ($Query.beginAvailable -and $Query.endAvailable -and $Query.computeStageMarkers -and $Query.noWaitRead -and $Query.queryReadFlags -eq 5 -and $Query.beginStageMask -eq 2048 -and $Query.endStageMask -eq 2048 -and $Query.nonemptyWork -eq $Nonempty) 'Depth query availability/stage/read flags/work mismatch'
	$requestedCooperative=$Frame.effectiveOptions.lightingDepth -ceq 'Cooperative'
	$hasFallback=(Has $Frame 'depthFallbackReason') -and $Frame.depthFallbackReason.status -ceq 'Valid' -and $Frame.depthFallbackReason.value -is [string] -and ![string]::IsNullOrWhiteSpace($Frame.depthFallbackReason.value)
	NativeRequire ($Query.cooperative -eq $requestedCooperative -or ($requestedCooperative -and !$Query.cooperative -and $hasFallback)) 'Depth query executed mode differs without actual fallback reason'
	$bits=NativeUnsigned $Query.timestampValidBits timestampValidBits
	NativeRequire ($bits -ge 1 -and $bits -le 64) 'Invalid depth timestamp valid-bit width'
	# Decimal uses exact integer arithmetic for the complete uint64 domain and
	# 2^64 modulus on both Windows PowerShell and PowerShell 7.
	$modulus=[decimal]1;for($i=0;$i -lt $bits;$i++){$modulus*=2}
	$begin=[decimal](NativeUnsigned $Query.beginTicks beginTicks);$end=[decimal](NativeUnsigned $Query.endTicks endTicks)
	$ticks=(($end%$modulus)-($begin%$modulus)+$modulus)%$modulus
	NativeRequire ($ticks -eq [decimal](NativeUnsigned $Query.rawTicks rawTicks)) 'Depth timestamp rollover/raw tick mismatch'
	$period=[double]$Query.timestampPeriodNanoseconds;$duration=[double]$Query.durationNanoseconds
	foreach($field in @('timestampPeriodNanoseconds','durationNanoseconds')){NativeRequire ($Query.$field -is [ValueType] -and $Query.$field -isnot [bool]) 'Native query time is not a JSON number'}
	NativeRequire (![double]::IsNaN($period) -and ![double]::IsInfinity($period) -and $period -gt 0 -and ![double]::IsNaN($duration) -and ![double]::IsInfinity($duration) -and $duration -ge 0) 'Invalid native depth period/duration'
	$expected=[double]$ticks*$period
	NativeRequire ([Math]::Abs($duration-$expected) -le [Math]::Max(1e-9,[Math]::Abs($expected)*1e-12)) 'Native depth duration is not raw ticks times period'
	if($Nonempty -and $ticks -eq 0){Unverified 'Nonempty depth query has no positive native duration'}
	if($Purpose -ceq 'Production'){$metric=Metric $Frame.depthStageTimingNanoseconds depthStageTimingNanoseconds;NativeRequire ([Math]::Abs($metric-$duration) -le [Math]::Max(1e-9,[Math]::Abs($duration)*1e-12)) 'Frame stage timing differs from actual production query'}
	return $duration
}
function ReadNativeDepthProof($Frame,$Identity,[string]$Directory,$RootArtifactIndex=$null) {
	InitializeNativeProof;InitializeNativeDepthProof
	NativeRequire ($Identity.stage -ceq 'P3' -and $Frame.evidenceKind -ceq 'LightingDepth' -and $Frame.payloadPurpose -ceq 'Production' -and $Frame.publicationState -ceq 'Complete') 'Native depth row kind/stage/purpose missing'
	foreach($field in @('lightId','faceIndex')){NativeRequire ($Frame.$field.status -ceq 'Unavailable') 'Depth row manufactured shadow identity'}
	foreach($field in $Frame.shadow.PSObject.Properties.Name){NativeRequire ($Frame.shadow.$field.status -ceq 'Unavailable') 'Depth row manufactured shadow metrics'}
	foreach($a in $Frame.artifacts){if($null -ne $RootArtifactIndex){NativeRequire ($RootArtifactIndex.Contains((Canonical $a))) 'Depth artifact outside root closure'}}
	$artifact=NativeArtifact $Frame Counters $Directory 16777216;$c=[Text.Encoding]::UTF8.GetString($artifact.bytes)|ConvertFrom-Json
	NativeReceiptFields $c @('schemaVersion','evidenceKind','fixtureId','fixtureHash','operationId','fixtureStepId','runId','runIndex','seed','mode','stage','measurementPhase','stepFrame','viewType','viewId','readbackProvenance','effectiveOptions','nativePreparation','productionNativeExtent','tileCount','boundsEncoding','productionQuery','colorNativeExtent','colorSourceFormat','imageEncoding') 'depth counters'
	NativeRequire ($c.schemaVersion -eq 1 -and $c.evidenceKind -ceq 'LightingDepth') 'Depth counters kind/schema mismatch'
	foreach($field in @('fixtureId','runId','runIndex','seed','mode','stage')){NativeRequire (JsonEqual $c.$field $Identity.$field) "Depth $field correlation mismatch"}
	NativeRequire ($c.fixtureHash -ceq (Metric $Identity.fixtureHash fixtureHash) -and $c.operationId -cmatch '^[0-9a-f]{32}$' -and $c.operationId -cne ('0'*32) -and $c.fixtureStepId -ceq $Frame.fixtureStepId -and $c.measurementPhase -ceq $Frame.measurementPhase -and (NativeUnsigned $c.stepFrame stepFrame) -eq $Frame.stepFrame) 'Depth captured fixture/step correlation mismatch'
	NativeRequire ($c.viewType -ceq $Frame.viewType -and $c.viewId -eq $Frame.viewId) 'Depth raw qualified view mismatch'
	$p=$c.readbackProvenance;NativeRequire ($p.kind -ceq 'FullNativeSameCommandCompletedReadback') 'Depth lacks completed same-command provenance'
	foreach($field in @('frameSerial','commandSerial','submitSerial','completionSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration')){NativeRequire ((NativeUnsigned $p.$field $field $true) -eq (Metric $Frame.$field $field)) "Depth current $field mismatch"}
	foreach($field in @('fixtureControlGeneration','optionsSessionGeneration')){$null=NativeUnsigned $p.$field $field $true}
	NativeRequire ((NativeUnsigned $p.completionSerial completionSerial) -ge (NativeUnsigned $p.submitSerial submitSerial) -and (JsonEqual $c.effectiveOptions $Frame.effectiveOptions)) 'Depth completion/options mismatch'
	if((Has $c 'depthFallbackReason') -and ![string]::IsNullOrWhiteSpace([string]$c.depthFallbackReason)){NativeRequire ((Has $Frame 'depthFallbackReason') -and $Frame.depthFallbackReason.status -ceq 'Valid' -and $Frame.depthFallbackReason.value -ceq $c.depthFallbackReason) 'Raw/frame depth fallback reason differs'}
	if((Has $Frame 'depthFallbackReason') -and $Frame.depthFallbackReason.status -ceq 'Valid'){NativeRequire ((Has $c 'depthFallbackReason') -and $c.depthFallbackReason -is [string] -and $Frame.depthFallbackReason.value -ceq $c.depthFallbackReason) 'Frame fallback is not the actual native reason'}
	NativeRequire ($c.productionNativeExtent.Count -eq 2 -and $c.productionNativeExtent[0] -gt 0 -and $c.productionNativeExtent[1] -gt 0 -and $c.productionNativeExtent[0] -le 1920 -and $c.productionNativeExtent[1] -le 1080 -and $c.tileCount -eq 144 -and $c.boundsEncoding -ceq 'float32-ieee-le') 'Depth native extent/ABI mismatch'
	foreach($value in $c.productionNativeExtent){$null=NativeUnsigned $value nativeExtent $true}
	$null=NativeUnsigned $c.tileCount tileCount $true
	if($Identity.fixtureId -cne 'depth-coverage'){NativeRequire ($c.productionNativeExtent[0] -eq $Identity.resolution.width -and $c.productionNativeExtent[1] -eq $Identity.resolution.height) 'Production depth does not use frozen native camera extent'}
	$bounds=NativeArtifact $Frame ProductionDepthBounds $Directory 1152
	try{$nonempty=[TerraRendererDepthProof]::Nonempty($bounds.bytes)}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}
	$duration=VerifyNativeDepthQuery $c.productionQuery $Frame $p Production $nonempty
	# Query slots are the same physical lighting snapshot, not merely any valid
	# index in the query pool. Join both copied upload purposes to their metric
	# wrappers and the actual backend allocation generation before using bytes.
	NativeReceiptFields $c @('lightingUploads') 'depth physical upload owner'
	NativeRequire ($c.lightingUploads -is [array] -and $c.lightingUploads.Count -eq 2 -and $Frame.lighting -is [array] -and $Frame.lighting.Count -eq 2) 'Depth physical lighting purposes incomplete'
	foreach($purpose in @('FinalLights','Analysis')){
		$raw=@($c.lightingUploads|Where-Object purpose -CEQ $purpose);$wrapped=@($Frame.lighting|Where-Object purpose -CEQ $purpose)
		NativeRequire ($raw.Count -eq 1 -and $wrapped.Count -eq 1 -and $raw[0].available -is [bool] -and $raw[0].available -and $raw[0].succeeded -is [bool] -and $raw[0].succeeded -and $wrapped[0].hostWriteStatus -ceq 'Succeeded') 'Depth physical upload is not successful native owner'
		foreach($field in @('allocationGeneration','frameSlot','snapshotSlot')){NativeRequire ((NativeUnsigned $raw[0].$field $field) -eq (Metric $wrapped[0].$field $field)) 'Depth raw/upload physical owner differs'}
		NativeRequire ((NativeUnsigned $raw[0].allocationGeneration allocationGeneration $true) -eq (NativeUnsigned $c.productionQuery.owner.backendResourceGeneration backendResourceGeneration $true) -and $raw[0].frameSlot -eq $c.productionQuery.owner.frameSlot -and $raw[0].snapshotSlot -eq $c.productionQuery.owner.snapshotSlot) 'Depth query belongs to another physical upload slice'
	}
	$oracle=$null
	if($Identity.fixtureId -ceq 'depth-coverage'){
		NativeReceiptFields $c @('oracleQuery','oracleRecipe','coverageEncoding') 'oracle counters'
		$r=$c.oracleRecipe;NativeReceiptFields $r @('width','height','fillBits','overrides') 'oracle recipe'
		NativeRequire ($r.overrides -is [array] -and $r.overrides.Count -le 1024 -and (NativeUnsigned $r.fillBits fillBits) -le [uint32]::MaxValue) 'Oracle recipe bits/override count invalid'
		NativeRequire ($c.coverageEncoding -ceq 'uint32-pair-le' -and $r.width -eq $c.productionNativeExtent[0] -and $r.height -eq $c.productionNativeExtent[1]) 'Oracle extent/encoding mismatch'
		$input=NativeArtifact $Frame DepthInput $Directory ([long]$r.width*$r.height*4);$ob=NativeArtifact $Frame DepthBounds $Directory 1152;$oc=NativeArtifact $Frame DepthCoverage $Directory 1152
		try{$expected=[TerraRendererDepthProof]::Materialize($r.width,$r.height,[uint32]$r.fillBits,[int[]]@($r.overrides|ForEach-Object x),[int[]]@($r.overrides|ForEach-Object y),[uint32[]]@($r.overrides|ForEach-Object bits));NativeRequire ([TerraRendererDepthProof]::BytesEqual($expected,$input.bytes)) 'Actual oracle input bits differ from captured recipe';[TerraRendererDepthProof]::Oracle($r.width,$r.height,$input.bytes,$ob.bytes,$oc.bytes);$null=VerifyNativeDepthQuery $c.oracleQuery $Frame $p Oracle ([TerraRendererDepthProof]::Nonempty($ob.bytes))}catch{if($_.Exception.Message.StartsWith('UNVERIFIED: ')){throw};throw ('NATIVE_FAILED: '+$_.Exception.Message)}
		foreach($field in @('backendResourceGeneration','queryPoolGeneration','frameSlot','snapshotSlot')){NativeRequire (JsonEqual $c.productionQuery.owner.$field $c.oracleQuery.owner.$field) 'Oracle query belongs to different native resource slice'}
		NativeRequire ($c.productionQuery.cooperative -eq $c.oracleQuery.cooperative) 'Oracle and production used different actual depth modes'
		$oracle=@{recipe=$r;input=$input.bytes;bounds=$ob.bytes;coverage=$oc.bytes}
	}
	NativeRequire ($c.colorNativeExtent.Count -eq 2 -and $c.colorNativeExtent[0] -eq $c.productionNativeExtent[0] -and $c.colorNativeExtent[1] -eq $c.productionNativeExtent[1] -and $c.imageEncoding -ceq 'png-rgba8' -and $c.colorSourceFormat -in @(37,43,44,50,97)) 'Depth same-command color owner extent/format mismatch'
	$image=NativeArtifact $Frame Image $Directory
	try{$decoded=[TerraRendererNativeProof]::DecodeImage($image.bytes,[int]$c.colorNativeExtent[0],[int]$c.colorNativeExtent[1])}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}
	return @{frame=$Frame;counters=$c;bounds=$bounds.bytes;oracle=$oracle;duration=$duration;nonempty=$nonempty;image=$decoded;imageHash=@($Frame.artifacts|Where-Object kind -CEQ Image)[0].sha256;operationId=$c.operationId;occupiedPixels=0;semanticRows=@()}
}
function CompareNativeDepthProof($Candidate,$Reference) {
	NativeRequire (JsonEqual $Candidate.counters.productionNativeExtent $Reference.counters.productionNativeExtent) 'Paired native depth extent differs'
	try{$depth=[TerraRendererDepthProof]::BoundsDelta($Candidate.bounds,$Reference.bounds);$image=[TerraRendererNativeProof]::DecodedImageDelta($Candidate.image,$Reference.image,[int]$Candidate.counters.productionNativeExtent[0],[int]$Candidate.counters.productionNativeExtent[1])}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}
	if($null -ne $Candidate.oracle -or $null -ne $Reference.oracle){NativeRequire ($null -ne $Candidate.oracle -and $null -ne $Reference.oracle -and (JsonEqual $Candidate.oracle.recipe $Reference.oracle.recipe) -and [TerraRendererDepthProof]::BytesEqual($Candidate.oracle.input,$Reference.oracle.input) -and [TerraRendererDepthProof]::BytesEqual($Candidate.oracle.coverage,$Reference.oracle.coverage)) 'Paired oracle recipe/input/coverage differs'}
	NativeRequire ($depth -le 1e-6 -and $image -le (2.0/255.0)) 'Native production depth/color parity failed'
	return @{coverageMismatchCount=0;casterIdentityMismatchCount=0;maxDepthAbsoluteError=$depth;maxImageNormalizedChannelError=$image}
}
function VerifyNativeDepthCohorts($DepthRows,$LightingRows,$Cameras) {
	# Expected views come from the frozen workload, not an observed subset. Each
	# completed view needs one depth dispatch and both actual lighting payloads.
	if(!@($DepthRows).Count){Unverified 'Native LightingDepth observations missing'}
	$expected=@{};foreach($camera in $Cameras){$expected["$($camera.viewType)|$($camera.viewId)"]=$true}
	$groups=@($DepthRows+$LightingRows|Group-Object {"$($_.frame.fixtureStepId)|$($_.frame.measurementPhase)|$($_.frame.stepFrame)"})
	foreach($group in $groups){
		$members=@($group.Group);$first=$members[0];$keys=@{};$commands=@{};$images=@{}
		foreach($row in $members){
			$view="$($row.frame.viewType)|$($row.frame.viewId)";NativeRequire ($expected.ContainsKey($view)) 'Unexpected P3 qualified view'
			$kind=if($row.frame.evidenceKind -ceq 'LightingDepth'){'Production'}else{$row.frame.payloadPurpose}
			$key="$view|$kind";NativeRequire (!$keys.ContainsKey($key)) 'Duplicate P3 purpose/view';$keys[$key]=$true
			foreach($field in @('frameSerial','submitSerial','completionSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration','fixtureControlGeneration','optionsSessionGeneration')){NativeRequire (JsonEqual $row.counters.readbackProvenance.$field $first.counters.readbackProvenance.$field) "P3 cohort $field mismatch"}
			NativeRequire (JsonEqual $row.frame.effectiveOptions $first.frame.effectiveOptions) 'P3 cohort options mismatch'
			NativeRequire (JsonEqual $row.counters.colorNativeExtent $first.counters.colorNativeExtent) 'P3 depth/lighting color extent differs'
			if($commands.ContainsKey($view)){NativeRequire ($commands[$view] -ceq $row.counters.readbackProvenance.commandSerial) 'P3 same-view command mismatch'}else{$commands[$view]=$row.counters.readbackProvenance.commandSerial}
			if($images.ContainsKey($view)){NativeRequire ($images[$view] -ceq $row.imageHash) 'P3 same-command rendered color differs across depth/lighting purposes'}else{$images[$view]=$row.imageHash}
		}
		NativeRequire ($keys.Count -eq $expected.Count*3) 'P3 cohort lacks depth or lighting purpose'
	}
}
function VerifyNativeDepthSpeed($CandidateSamples,$ReferenceSamples) {
	# These samples are admitted only from actual nonempty isolated Production
	# queries. Call once per independent run; no oracle or warmup aggregation.
	if(@($CandidateSamples).Count -ne 300 -or @($ReferenceSamples).Count -ne 300){Unverified 'SC004 requires exactly 300 actual measured production samples per run'}
	$cm=Percentile $CandidateSamples 0.5;$rm=Percentile $ReferenceSamples 0.5
	NativeRequire ($rm -gt 0 -and $cm -gt 0 -and $cm -le $rm*0.9) 'Depth GPU stage median improvement below 10%'
}
function ReadNativeDepthRecipes($Envelope,$Document,$Identity,[string]$Directory,$Steps) {
	$recipes=@{}
	foreach($step in $Steps){
		$receipts=@($Envelope.receipts|Where-Object {$_.kind -ceq 'DepthInputRecipe' -and $_.fixtureStepId -ceq $step.id})
		if($receipts.Count -ne 1){Unverified 'Missing unique authored depth recipe authority'}
		$r=$receipts[0];NativeReceiptFields $r @('recipePath','recipeSha256','nativeExtent','recipe') 'depth recipe authority'
		$descriptors=@($Document.artifacts|Where-Object {$_.kind -ceq 'Counters' -and $_.path -ceq $r.recipePath -and $_.sha256 -ceq $r.recipeSha256})
		NativeRequire ($descriptors.Count -eq 1 -and $r.recipePath -ceq "renderer-efficiency-depth/$($step.id).json") 'Authored depth recipe is not in exact root closure'
		$path=ArtifactPath $descriptors[0] $Directory;NativeRequire ((Get-Item -LiteralPath $path).Length -le 1048576) 'Authored depth recipe exceeds bounded input'
		$file=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
		NativeReceiptFields $file @('schemaVersion','kind','fixtureId','fixtureHash','seed','fixtureStepId','viewType','viewId','grid','recipe') 'sealed depth recipe'
		NativeRequire ($file.schemaVersion -eq 1 -and $file.kind -ceq 'LightingDepthInputRecipe' -and $file.fixtureId -ceq $Identity.fixtureId -and $file.fixtureHash -ceq (Metric $Identity.fixtureHash fixtureHash) -and $file.seed -eq $Identity.seed -and $file.fixtureStepId -ceq $step.id -and (JsonEqual $file.grid @(16,9,24)) -and (JsonEqual $file.recipe $r.recipe) -and (JsonEqual $file.recipe.extent $r.nativeExtent)) 'Sealed authored depth recipe identity/content mismatch'
		$recipes[$step.id]=@{viewType=$file.viewType;viewId=$file.viewId;recipe=[pscustomobject]@{width=$file.recipe.extent.width;height=$file.recipe.extent.height;fillBits=$file.recipe.fillBits;overrides=$file.recipe.overrides}}
	}
	return $recipes
}
function VerifyNativeDepthRecipe($Proof,$Recipes) {
	NativeRequire ($Recipes.ContainsKey($Proof.frame.fixtureStepId)) 'Unknown depth recipe step'
	$r=$Recipes[$Proof.frame.fixtureStepId]
	NativeRequire ($Proof.frame.viewType -ceq $r.viewType -and $Proof.frame.viewId -eq $r.viewId -and $null -ne $Proof.oracle -and (JsonEqual $Proof.oracle.recipe $r.recipe)) 'GPU oracle input is not the sealed authored step recipe'
}
function CompareNativeDepthTransitions($CandidateEnvelope,$ReferenceEnvelope,$CandidateDocument,$ReferenceDocument,$CandidateDirectory,$ReferenceDirectory,$Steps,$Cameras,$CandidateRecipes,$ReferenceRecipes) {
	$maximumImage=0.0;$maximumDepth=0.0
	foreach($step in $Steps){
		$paired=@()
		foreach($side in @(@($CandidateEnvelope,$CandidateDocument,$CandidateDirectory,$CandidateRecipes),@($ReferenceEnvelope,$ReferenceDocument,$ReferenceDirectory,$ReferenceRecipes))){
			$envelope=$side[0];$document=$side[1];$directory=$side[2];$recipes=$side[3]
			$receipts=@($envelope.receipts|Where-Object {$_.kind -ceq 'ControlTransition' -and $_.fixtureStepId -ceq $step.id})
			if($receipts.Count -ne 1){Unverified 'Missing complete first native P3 control transition'}
			$r=$receipts[0];NativeReceiptFields $r @('frames','artifacts','originalCorrelation','controlGeneration','frameSerial','submitSerial','completionSerial') 'P3 transition'
			NativeRequire ($r.originalCorrelation.operationId -ceq $envelope.operationId -and $r.originalCorrelation.fixtureStepId -ceq $step.id -and (NativeUnsigned $r.originalCorrelation.stepFrame stepFrame) -eq 0) 'P3 transition original correlation mismatch'
			$rootIndex=NewNativeArtifactClosureIndex $document.artifacts;$receiptIndex=NewNativeArtifactClosureIndex $r.artifacts
			foreach($a in $r.artifacts){NativeRequire ($rootIndex.Contains((Canonical $a))) 'P3 transition artifact absent root closure';$null=ArtifactPath $a $directory}
			$depth=[Collections.Generic.List[object]]::new();$lighting=[Collections.Generic.List[object]]::new();$byView=@{}
			foreach($f in @($r.frames|Where-Object {(Has $_ 'evidenceKind') -and $_.evidenceKind -in @('Lighting','LightingDepth')})){
				NativeRequire ($f.measurementPhase -ceq 'Transition' -and $f.fixtureStepId -ceq $step.id -and $f.stepFrame -eq 0 -and (JsonEqual $f.effectiveOptions $document.requestedOptions)) 'P3 transition phase/options mismatch'
				foreach($field in @('frameSerial','submitSerial','completionSerial')){NativeRequire ((Metric $f.$field $field) -eq (NativeUnsigned $r.$field $field $true)) 'P3 transition actual completion cohort differs'}
				foreach($field in @('settingsGeneration','mutationGeneration','rendererSessionGeneration')){NativeRequire ((Metric $f.$field $field) -eq (NativeUnsigned $r.originalCorrelation.$field $field $true)) 'P3 transition original generation differs'}
				foreach($a in $f.artifacts){NativeRequire ($receiptIndex.Contains((Canonical $a))) 'P3 transition frame outside nested closure'}
				$proof=if($f.evidenceKind -ceq 'LightingDepth'){ReadNativeDepthProof $f $document.identity $directory $rootIndex}else{ReadNativeLightingProof $f $document.identity $directory $rootIndex}
				NativeRequire ($proof.operationId -ceq $envelope.operationId -and (NativeUnsigned $proof.counters.readbackProvenance.fixtureControlGeneration fixtureControlGeneration $true) -eq (NativeUnsigned $r.controlGeneration controlGeneration $true)) 'P3 transition actual control differs'
				if($f.evidenceKind -ceq 'LightingDepth'){if($document.identity.fixtureId -ceq 'depth-coverage'){VerifyNativeDepthRecipe $proof $recipes};$depth.Add(@{frame=$f;counters=$proof.counters;imageHash=$proof.imageHash});$byView["$($f.viewType)|$($f.viewId)|Production"]=$proof}else{VerifyNativeLightingSourceCount $proof $step $document.identity.fixtureId;NativeRequire ((Metric $proof.upload.activeRecordCount activeRecordCount) -eq [Math]::Min((Metric $proof.frame.fullSourceCandidateCount fullSourceCandidateCount),1024)) 'P3 transition lighting payload underfilled';$lighting.Add(@{frame=$f;counters=$proof.counters;upload=$proof.upload;imageHash=$proof.imageHash});$byView["$($f.viewType)|$($f.viewId)|$($f.payloadPurpose)"]=$proof}
			}
			VerifyNativeDepthCohorts $depth.ToArray() $lighting.ToArray() $Cameras;VerifyNativeLightingCohorts $lighting.ToArray() $Cameras $false
			$warmup=@($document.frames|Where-Object {$_.fixtureStepId -ceq $step.id -and $_.measurementPhase -ceq 'Warmup'}|Sort-Object {Metric $_.frameSerial frameSerial}|Select-Object -First 1)
			if($warmup.Count -ne 1){Unverified 'P3 transition lacks admitted warmup'}
			NativeRequire ((NativeUnsigned $r.frameSerial frameSerial $true) -lt (Metric $warmup[0].frameSerial frameSerial)) 'P3 transition must precede admitted warmup'
			$paired+=@($byView)
		}
		NativeRequire ($paired[0].Count -eq $paired[1].Count) 'P3 transition qualified bundle differs'
		foreach($key in $paired[0].Keys){NativeRequire ($paired[1].ContainsKey($key)) 'P3 transition paired view/purpose missing';$a=$paired[0][$key];$b=$paired[1][$key];$delta=if($a.frame.evidenceKind -ceq 'LightingDepth'){CompareNativeDepthProof $a $b}else{CompareNativeLightingProof $a $b $CandidateDocument.identity.resolution};$maximumDepth=[Math]::Max($maximumDepth,$delta.maxDepthAbsoluteError);$maximumImage=[Math]::Max($maximumImage,$delta.maxImageNormalizedChannelError)}
	}
	return @{maxDepthAbsoluteError=$maximumDepth;maxImageNormalizedChannelError=$maximumImage}
}
# P4 counts are accepted only after the actual executed groups/singletons cover
# the independently resolved light list. Planned groups never authorize draws.
function NativeP4SharedIdentity($Identity) { return $Identity.stage -ceq 'P4' -and $Identity.fixtureId -cin @('multiview-lights','recovery-capacity') }
function NativeP5SharedIdentity($Identity) { return $Identity.stage -ceq 'P5' -and $Identity.fixtureId -cin @('multiview-lights','recovery-capacity') }
function VerifyNativeLightingStage($Identity) {
	# P4/P5 shared fixtures retain actual lighting-pressure proof without claiming
	# P2 upload savings. Dedicated P4 remains a shadow-only recipe.
	NativeRequire ($Identity.stage -cin @('P2','P3') -or (NativeP4SharedIdentity $Identity) -or (NativeP5SharedIdentity $Identity)) 'Lighting proof is outside the native stage/fixture contract'
}
function VerifyNativeSharedShadowBatchRecipe($Frame,$Counters,$Step,[string]$FixtureId) {
	NativeRequire ($FixtureId -cin @('multiview-lights','recovery-capacity')) 'Unknown shared P4 fixture'
	$e=$Counters.shadowBatchExecution
	if($Step.action -ceq 'group-overflow'){
		$owners=@{};foreach($row in $Counters.eligibleCasterIdentityRows){$owners[$row.objectId]=$true}
		NativeRequire ($owners.Count -eq 1000) 'Shared group overflow lacks its real 1000-owner pool'
		# IDs follow the frozen producer's decimal-padded UUID convention. Dedupe
		# owners above permits real multisection/page geometry without accepting a
		# same-sized unrelated scene as the group-capacity fixture.
		for($id=20000;$id -le 20999;$id++){NativeRequire ($owners.ContainsKey('650'+('0'*24)+[string]$id)) 'Shared group overflow contains a missing or substituted pool owner'}
		if($Frame.effectiveOptions.shadowSubmission -ceq 'Grouped'){
			NativeRequire ($e.groups.Count -ge 2) 'Shared group overflow lacks actual split commands'
			foreach($group in $e.groups){NativeRequire ($group.instanceCount -le 64) 'Shared actual group exceeds the sealed64 control'}
		}
	}
	if($Step.action -ceq 'unsupported-grouped-route'){
		# This is a real CPU-deformed route, not a fabricated capacity failure.
		# NativeCasterRows already validates full deformation/resource evidence.
		$pose=@($Counters.eligibleCasterIdentityRows|Where-Object objectId -CEQ '65000000000000000000000000000021')
		$morph=@($Counters.eligibleCasterIdentityRows|Where-Object objectId -CEQ '65000000000000000000000000000031')
		NativeRequire ($pose.Count -gt 0 -and $morph.Count -gt 0 -and $pose.Count+$morph.Count -eq @($Counters.eligibleCasterIdentityRows).Count) 'Unsupported route does not select the real pose/morph owners'
		foreach($row in $pose){NativeRequire ((Has $row 'deformationPalette') -and $row.deformationPalette -is [array] -and $row.deformationPalette.Count -gt 0) 'Unsupported pose lacks actual palette input'}
		foreach($row in $morph){NativeRequire ((Has $row 'morphWeights') -and $row.morphWeights -is [array] -and @($row.morphWeights|Where-Object {$_ -ne 0}).Count -gt 0) 'Unsupported morph lacks actual active weight input'}
		NativeRequire ($e.groups.Count -eq 0 -and $e.singletons.Count -eq @($Counters.submittedCasterIdentityRows).Count) 'Unsupported route lost its complete actual singleton fallback'
	}
}
function NativeShadowBatchUnsigned64($Value,[string]$Name,[bool]$Positive=$false) {
	NativeRequire ($Value -is [string] -and $Value -cmatch '^(0|[1-9][0-9]*)$') "Shadow batch $Name is not a canonical uint64 string"
	return NativeUnsigned $Value $Name $Positive
}
function NativeShadowBatchUnsigned32($Value,[string]$Name,[bool]$Positive=$false) {
	NativeRequire ($Value -is [ValueType] -and $Value -isnot [bool]) "Native $Name must be a numeric uint32 lane"
	$n=NativeUnsigned $Value $Name $Positive;NativeRequire ($n -le [uint32]::MaxValue) "Native $Name exceeds uint32";return $n
}
function NativeShadowBatchMemberKey($Member) {
	NativeReceiptFields $Member @('objectId','lifetimeGeneration','segmentId') 'shadow batch member'
	NativeRequire ($Member.objectId -cmatch '^[0-9a-f]{32}$' -and $Member.objectId -cne ('0'*32)) 'Shadow batch member UUID is invalid'
	return @($Member.objectId,(NativeShadowBatchUnsigned64 $Member.lifetimeGeneration 'batch lifetime' $true),(NativeShadowBatchUnsigned64 $Member.segmentId 'batch segment')) -join '|'
}
function VerifyNativeShadowBatchExecution($Frame,$Counters) {
	NativeRequire ($Frame.effectiveOptions.shadowReuse -ceq 'AlwaysRefresh' -and $Frame.effectiveOptions.shadowSubmission -cin @('Grouped','Singleton')) 'P4 requires actual forced refresh and a supported submission mode'
	NativeReceiptFields $Counters @('shadowBatchExecution') 'shadow batch'
	$e=$Counters.shadowBatchExecution
	NativeReceiptFields $Counters.readbackProvenance @('fixtureControlGeneration','optionsSessionGeneration') 'P4 current provenance';foreach($field in @('fixtureControlGeneration','optionsSessionGeneration')){$null=NativeUnsigned $Counters.readbackProvenance.$field $field $true}
	NativeReceiptFields $e @('available','recordedComplete','frameSlotSafe','uploadAttempted','uploadSucceeded','allocationIdentity','allocationGeneration','backendGeneration','instanceBytes','frameSlot','snapshotSlot','sources','groups','singletons') 'shadow batch execution'
	foreach($field in @('available','recordedComplete','frameSlotSafe','uploadAttempted','uploadSucceeded')){NativeRequire ($e.$field -is [bool]) "Shadow batch $field is not an actual boolean"}
	if(!$e.available -or !$e.recordedComplete -or !$e.frameSlotSafe){Unverified 'Complete safe native shadow batch execution is unavailable'}
	foreach($field in @('frameSlot','snapshotSlot')){$null=NativeShadowBatchUnsigned32 $e.$field "batch $field"}
	NativeRequire ($e.frameSlot -lt 3 -and $e.snapshotSlot -lt 8) 'Shadow batch physical slot is invalid'
	$null=NativeShadowBatchUnsigned64 $e.backendGeneration 'batch backend generation' $true
	foreach($field in @('sources','groups','singletons')){NativeRequire ($e.$field -is [array]) "Shadow batch $field is not an array"}
	$eligible=@($Counters.eligibleCasterIdentityRows);$submitted=@($Counters.submittedCasterIdentityRows)
	$expected=@{};$ranges=@{};$actual=@{};$sourceRanges=@{};$ordinals=@{}
	foreach($row in $eligible){
		$key=NativeShadowBatchMemberKey $row;NativeRequire (!$expected.ContainsKey($key)) 'Duplicate eligible batch member';$expected[$key]=$true
		$owner=@($row.objectId,(NativeShadowBatchUnsigned64 $row.lifetimeGeneration 'eligible lifetime' $true)) -join '|'
		$first=NativeUnsigned $row.firstMeshlet 'eligible first meshlet';$count=NativeUnsigned $row.meshletCount 'eligible meshlet count' $true
		NativeRequire ($first -le [uint32]::MaxValue -and $count -le [uint32]::MaxValue -and [decimal]$first+$count -le [decimal][uint32]::MaxValue+1) 'Eligible owner range overflows'
		if(!$ranges.ContainsKey($owner)){$ranges[$owner]=[Collections.Generic.List[object]]::new()};$ranges[$owner].Add(@{first=$first;count=$count})
	}
	foreach($source in $e.sources){
		NativeReceiptFields $source @('objectId','lifetimeGeneration','sourceOrdinal','ownerFirstMeshlet','ownerMeshletCount','resolved','excludedByMaterial','cameraVisible') 'shadow batch source'
		NativeRequire ($source.objectId -cmatch '^[0-9a-f]{32}$' -and $source.objectId -cne ('0'*32) -and $source.resolved -is [bool] -and $source.excludedByMaterial -is [bool] -and $source.cameraVisible -is [bool]) 'Shadow source identity/resolution is invalid'
		$ordinal=NativeShadowBatchUnsigned32 $source.sourceOrdinal 'source ordinal';NativeRequire ($ordinal -le [uint32]::MaxValue -and !$ordinals.ContainsKey($ordinal)) 'Duplicate source ordinal';$ordinals[$ordinal]=$true
		$owner=@($source.objectId,(NativeShadowBatchUnsigned64 $source.lifetimeGeneration 'source lifetime' $true)) -join '|'
		if(!$source.resolved){Unverified 'Unresolved source caster cannot prove complete batch coverage'}
		$first=NativeShadowBatchUnsigned32 $source.ownerFirstMeshlet 'source first meshlet';$count=NativeShadowBatchUnsigned32 $source.ownerMeshletCount 'source meshlet count'
		NativeRequire ($first -le [uint32]::MaxValue -and $count -le [uint32]::MaxValue -and [decimal]$first+$count -le [decimal][uint32]::MaxValue+1) 'Source owner range overflows'
		if($source.excludedByMaterial){if($ranges.ContainsKey($owner)){foreach($span in $ranges[$owner]){NativeRequire ([decimal]$first+$count -le [decimal]$span.first -or [decimal]$span.first+$span.count -le [decimal]$first) 'Material-excluded source selected span was emitted'}};continue}
		if($count -eq 0){Unverified 'Source owner selected range is unavailable'}
		if(!$sourceRanges.ContainsKey($owner)){$sourceRanges[$owner]=[Collections.Generic.List[object]]::new()};$sourceRanges[$owner].Add(@{first=$first;count=$count})
	}
	# Compare exact interval partitions, permitting selected page segmentation
	# while rejecting gaps, overlaps and omitted off-camera source owners.
	NativeRequire ($sourceRanges.Count -eq $ranges.Count) 'Source/eligible owner set mismatch'
	foreach($owner in $sourceRanges.Keys){
		NativeRequire ($ranges.ContainsKey($owner)) 'Source owner has no eligible geometry'
		if($sourceRanges[$owner].Count -eq 1 -and $ranges[$owner].Count -eq 1){NativeRequire ($sourceRanges[$owner][0].first -eq $ranges[$owner][0].first -and $sourceRanges[$owner][0].count -eq $ranges[$owner][0].count) 'Source selected span differs from eligible span';continue}
		$normalized=@()
		foreach($side in @(0,1)){
			# Index the two lists explicitly: PS5 array construction can flatten generic
			# collections and accidentally combine the independent interval partitions.
			$list=if($side -eq 0){$sourceRanges[$owner]}else{$ranges[$owner]}
			$ordered=@($list|Sort-Object {[decimal]$_.first});$merged=[Collections.Generic.List[string]]::new();$start=$null;$end=[decimal]0
			foreach($range in $ordered){$next=[decimal]$range.first;NativeRequire ($null -eq $start -or $next -ge $end) 'Shadow selected ranges overlap';if($null -ne $start -and $next -ne $end){$merged.Add("$start|$end");$start=$null};if($null -eq $start){$start=$next};$end=$next+[decimal]$range.count}
			if($null -ne $start){$merged.Add("$start|$end")};$normalized+=,$merged.ToArray()
		}
		NativeRequire (($normalized[0] -join ';') -ceq ($normalized[1] -join ';')) 'Source selected span is not exactly partitioned by eligible segments'
	}
	$instanceBytes=NativeShadowBatchUnsigned64 $e.instanceBytes 'batch instance bytes';$streamRanges=[Collections.Generic.List[object]]::new()
	foreach($group in $e.groups){
		NativeReceiptFields $group @('firstInstance','instanceCount','taskGroupsX','pipelineIdentity','layoutIdentity','members') 'actual shadow group'
		$first=NativeShadowBatchUnsigned32 $group.firstInstance 'group first instance';$count=NativeShadowBatchUnsigned32 $group.instanceCount 'group instance count' $true
		$null=NativeShadowBatchUnsigned64 $group.pipelineIdentity 'group pipeline' $true;$null=NativeShadowBatchUnsigned64 $group.layoutIdentity 'group layout' $true;$dispatch=NativeShadowBatchUnsigned32 $group.taskGroupsX 'group dispatch' $true
		NativeRequire ($first -le [uint32]::MaxValue -and $count -ge 2 -and $count -le [uint32]::MaxValue -and $dispatch -le [uint32]::MaxValue -and $group.members -is [array] -and $group.members.Count -eq $count) 'Actual group member count/dispatch is invalid'
		NativeRequire ([decimal]($first+$count)*176 -le $instanceBytes -and $instanceBytes%176 -eq 0) 'Actual group escapes the uploaded 176-byte instance stream';$streamRanges.Add(@{first=$first;end=[decimal]$first+$count})
		foreach($member in $group.members){$key=NativeShadowBatchMemberKey $member;NativeRequire (!$actual.ContainsKey($key)) 'Caster executed more than once';$actual[$key]=$true}
	}
	$end=[decimal]0;foreach($range in @($streamRanges|Sort-Object first)){NativeRequire ([decimal]$range.first -ge $end) 'Executed group instance ranges overlap';$end=$range.end}
	foreach($member in $e.singletons){$key=NativeShadowBatchMemberKey $member;NativeRequire (!$actual.ContainsKey($key)) 'Caster executed in multiple groups/singletons';$actual[$key]=$true}
	NativeRequire ($actual.Count -eq $expected.Count) 'Executed caster count differs from complete eligible membership'
	foreach($key in $expected.Keys){NativeRequire ($actual.ContainsKey($key)) 'Eligible caster absent from actual execution'}
	NativeRequire ($submitted.Count -eq $actual.Count) 'Submitted/executed caster count mismatch';$submittedKeys=@{};foreach($row in $submitted){$key=NativeShadowBatchMemberKey $row;NativeRequire ($actual.ContainsKey($key) -and !$submittedKeys.ContainsKey($key)) 'Submitted caster has no unique actual execution';$submittedKeys[$key]=$true}
	foreach($field in @('groups','singletons','actualDrawSubmissions','eligibleCasterSegments','submittedCasterSegments')){NativeRequire ((Metric $Counters.nativeCounters.$field "raw batch $field") -eq (Metric $Frame.shadow.$field "batch $field")) 'Actual batch raw/frame counter mismatch'}
	NativeRequire ((Metric $Frame.shadow.groups groups) -eq $e.groups.Count -and (Metric $Frame.shadow.singletons singletons) -eq $e.singletons.Count -and (Metric $Frame.shadow.actualDrawSubmissions draws) -eq $e.groups.Count+$e.singletons.Count -and (Metric $Frame.shadow.eligibleCasterSegments eligible) -eq $expected.Count -and (Metric $Frame.shadow.submittedCasterSegments submitted) -eq $expected.Count -and (Metric $Frame.shadow.reuseRegionCount reuse) -eq 0) 'Actual execution counters do not describe the recorded command list'
	if($Frame.effectiveOptions.shadowSubmission -ceq 'Singleton'){NativeRequire ($e.groups.Count -eq 0 -and $e.singletons.Count -eq $expected.Count) 'Singleton reference contains grouped work'}
	if($e.groups.Count){NativeRequire ($e.uploadAttempted -and $e.uploadSucceeded) 'Grouped command lacks successful native stream upload';$null=NativeShadowBatchUnsigned64 $e.allocationIdentity 'batch allocation' $true;$null=NativeShadowBatchUnsigned64 $e.allocationGeneration 'batch allocation generation' $true}
}
function GetNativeShadowBatchCameras([string]$StepId,$Cameras) {
	$keys=if($StepId -ceq 'shadow-batching-04'){@('Scene|1','Game|2')}else{@('Scene|1')}
	$result=@();foreach($key in $keys){$matches=@($Cameras|Where-Object {"$($_.viewType)|$($_.viewId)" -ceq $key});if($matches.Count -ne 1){Unverified 'Frozen P4 required camera is unavailable'};$result+=,$matches[0]};return $result
}
function VerifyNativeShadowBatchRecipe($Frame,$Counters,$Step) {
	$actions=@('compatible-1000','mixed-route','deformed','cutout','off-camera','dispatch-limit-split','upload-failure-fallback')
	NativeRequire ($Step.id -cmatch '^shadow-batching-0[0-6]$' -and $Step.action -ceq $actions[[int]$Step.id.Substring($Step.id.Length-1)]) 'P4 step/action differs from frozen recipe'
	$owners=@{};foreach($row in $Counters.eligibleCasterIdentityRows){$owners[$row.objectId]=$true};NativeRequire ($owners.Count -eq 1000) 'P4 logical caster set is not the actual 1000-object fixture'
	# Camera membership is captured before main draw compression; shadow proof must
	# actually include a caster outside Scene visibility for the off-camera step.
	if($Step.action -ceq 'off-camera' -and $Frame.viewType -ceq 'Scene'){NativeRequire (@($Counters.shadowBatchExecution.sources|Where-Object {!$_.excludedByMaterial -and !$_.cameraVisible}).Count -gt 0) 'Off-camera Scene fixture lacks an actual nonvisible shadow source'}
	if($Step.action -ceq 'dispatch-limit-split' -and $Frame.effectiveOptions.shadowSubmission -ceq 'Grouped'){
		NativeRequire ($Counters.shadowBatchExecution.groups.Count -ge 2) 'Dispatch-limit fixture lacks actual split groups'
		foreach($group in $Counters.shadowBatchExecution.groups){NativeRequire ($group.instanceCount -le 64) 'Actual split group exceeds sealed 64-instance control'}
	}
	# Failure injection is one-shot. Its retained first transition must prove the
	# fallback; later normal samples may successfully group unchanged casters.
	if($Step.action -ceq 'upload-failure-fallback' -and $Frame.measurementPhase -ceq 'Transition' -and $Frame.effectiveOptions.shadowSubmission -ceq 'Grouped'){
		$e=$Counters.shadowBatchExecution
		NativeRequire ($e.uploadAttempted -and !$e.uploadSucceeded -and $e.groups.Count -eq 0 -and $e.singletons.Count -eq @($Counters.submittedCasterIdentityRows).Count) 'First upload failure did not retain the complete actual singleton fallback'
		NativeReceiptFields $Counters @('nativeShadowDiagnostics') 'batch failure';NativeReceiptFields $Counters.nativeShadowDiagnostics @('fallbackReason') 'batch failure reason'
		NativeRequire ($Counters.nativeShadowDiagnostics.fallbackReason -ceq 'ShadowInstanceUploadUnavailable' -and (Metric $Frame.shadow.fallbackReason 'batch fallback reason') -ceq $Counters.nativeShadowDiagnostics.fallbackReason) 'Upload failure lacks the actual current native fallback reason'
	}
}
function VerifyNativeShadowBatchRegions($Counters,[string]$StepId,[string]$FixtureId='shadow-batching',$Cameras=@()) {
	# This region set is the sealed P4 producer contract, never a set inferred
	# from the observed rows. In particular, a missing 2D light cannot disappear
	# from both runs and pass merely because the remaining cube is complete.
	$expected=@{};$shared=$FixtureId -cin @('multiview-lights','recovery-capacity');$views=@(if($shared){$Cameras|ForEach-Object {"$($_.viewType)|$($_.viewId)"}}elseif($StepId -ceq 'shadow-batching-04'){@('Scene|1','Game|2')}else{@('Scene|1')});if(!$views.Count){Unverified 'Shared P4 declared cameras are missing'}
	foreach($view in $views){
		if($shared -or $StepId -ceq 'shadow-batching-04'){foreach($light in @(@($(if($shared){'02'}else{'40'}),1,6),@('41',0,1),@('42',2,1),@('43',3,1))){for($face=0;$face -lt $light[2];$face++){$expected["$view|650000000000000000000000000000$($light[0])|$($light[1])|$face"]=$true}}}
		else{$expected["$view|65000000000000000000000000000041|0|0"]=$true}
	}
	$seen=@{};$common=$null;$commands=@{}
	foreach($counter in @($Counters)){
		$k=$counter.qualifiedShadowViewKey;$view="$($k.viewType)|$($k.viewId)";$region="$view|$($k.lightId)|$($k.resourceKind)|$($k.faceIndex)"
		NativeRequire ($expected.ContainsKey($region) -and !$seen.ContainsKey($region)) 'P4 omitted, duplicated or unexpected qualified region';$seen[$region]=$true
		$p=$counter.readbackProvenance;NativeReceiptFields $p @('frameSerial','commandSerial','submitSerial','completionSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration','fixtureControlGeneration','optionsSessionGeneration') 'P4 region provenance';$parts=@();foreach($field in @('frameSerial','submitSerial','completionSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration','fixtureControlGeneration','optionsSessionGeneration')){$parts+=,[string](NativeUnsigned $p.$field "P4 cohort $field" $true)}
		NativeRequire ($counter.fixtureStepId -ceq $StepId) 'P4 region belongs to another fixture step'
		$parts+=@($counter.operationId,$counter.fixtureStepId,$counter.measurementPhase,[string]$counter.stepFrame,(Canonical $counter.effectiveOptions));$signature=$parts -join '|';if($null -eq $common){$common=$signature};NativeRequire ($common -ceq $signature) 'P4 regions mix native completion/control/options cohorts'
		$command=NativeUnsigned $p.commandSerial 'P4 view command' $true;if(!$commands.ContainsKey($view)){$commands[$view]=$command};NativeRequire ($commands[$view] -eq $command) 'P4 one view mixes actual primary commands'
	}
	if($seen.Count -ne $expected.Count){Unverified 'Complete required P4 qualified region cohort is missing'}
	VerifyNativeBundle $Counters
}

function VerifyNativeP4SharedCohorts($Rows,$Cameras,[string]$FixtureId) {
	# The mixed bundle must be one real native cohort. Matching shadow and lighting
	# independently would permit a stale payload from another actual command.
	foreach($group in @($Rows|Group-Object {"$($_.frame.fixtureStepId)|$($_.frame.measurementPhase)|$($_.frame.stepFrame)"})){
		$common=$null;$commands=@{};$images=@{};$purposeKeys=@{};$shadow=[Collections.Generic.List[object]]::new()
		foreach($row in $group.Group){
			$f=$row.frame;$c=$row.counters;$p=$c.readbackProvenance;$view="$($f.viewType)|$($f.viewId)"
			NativeRequire (@($Cameras|Where-Object {$_.viewType -ceq $f.viewType -and $_.viewId -eq $f.viewId}).Count -eq 1) 'Shared P4 bundle names an undeclared view'
			$parts=@();foreach($field in @('frameSerial','submitSerial','completionSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration','fixtureControlGeneration','optionsSessionGeneration')){$parts+=,[string](NativeUnsigned $p.$field "shared P4 $field" $true)}
			$parts+=@($c.operationId,(Canonical $f.effectiveOptions));$signature=$parts -join '|';if($null -eq $common){$common=$signature};NativeRequire ($common -ceq $signature) 'Shared P4 shadow and lighting mix completion/control/options cohorts'
			if($commands.ContainsKey($view)){NativeRequire ($commands[$view] -ceq [string]$p.commandSerial) 'Shared P4 same-view primary differs'}else{$commands[$view]=[string]$p.commandSerial}
			if($images.ContainsKey($view)){NativeRequire ($images[$view] -ceq $row.imageHash) 'Shared P4 same-command color differs'}else{$images[$view]=$row.imageHash}
			if((Has $f 'evidenceKind') -and $f.evidenceKind -ceq 'Lighting'){$key="$view|$($f.payloadPurpose)";NativeRequire ($f.payloadPurpose -cin @('FinalLights','Analysis') -and !$purposeKeys.ContainsKey($key)) 'Shared P4 lighting purpose is missing/duplicated';$purposeKeys[$key]=$true}
			else{NativeRequire (!(Has $f 'evidenceKind') -or $f.evidenceKind -ceq 'Shadow') 'Unexpected shared P4 evidence kind';$shadow.Add($c)}
		}
		NativeRequire ($purposeKeys.Count -eq @($Cameras).Count*2) 'Shared P4 both actual lighting purposes are required'
		VerifyNativeShadowBatchRegions $shadow.ToArray() $group.Group[0].frame.fixtureStepId $FixtureId $Cameras
	}
}
function VerifyNativeShadowBatchReduction($Candidate,$Reference,[int]$MinimumCasters=1000) {
	# Grade each paired qualified region independently; aggregation must not hide
	# a bad light/face/view/frame behind a more efficient unrelated region.
	$c=Metric $Candidate.shadow.actualDrawSubmissions 'candidate actual submissions';$r=Metric $Reference.shadow.actualDrawSubmissions 'reference actual submissions'
	$n=Metric $Candidate.shadow.eligibleCasterSegments 'candidate eligible segments'
	NativeRequire ($n -ge $MinimumCasters -and $n -eq (Metric $Reference.shadow.eligibleCasterSegments 'reference eligible segments') -and $r -eq $n -and $c -gt 0 -and $c -le $r*0.1) 'Per-region actual shadow submissions reduction is below 90% or lacks the compatible caster reference'
}

# P5 consumes exact completed native copy receipts. These offline checks do
# not infer static work from a zero metric or manufacture GPU completion.
function NativeStaticSemantic($Row) {
	# Raw pose/morph observation counters are diagnostics, not drawn inputs.
	$value=[ordered]@{};foreach($property in $Row.PSObject.Properties){if($property.Name -cnotin @('poseGeneration','morphGeneration')){$value[$property.Name]=$property.Value}}
	return Canonical $value
}
function NativeStaticRows($Rows) {
	$null=NativeCasterRows $Rows
	$seen=@{};foreach($row in $Rows){$key=@($row.objectId,$row.lifetimeGeneration,$row.segmentId) -join '|';NativeRequire (!$seen.ContainsKey($key)) 'Static partition duplicates actual owner/segment identity';$seen[$key]=$true}
	$result=@($Rows|ForEach-Object {NativeStaticSemantic $_});[Array]::Sort($result,[StringComparer]::Ordinal);return ,$result
}
function NativeStaticAllocationKey($Owner) {
	return @($Owner.sessionGeneration,$Owner.backendGeneration,$Owner.allocationIdentity,$Owner.allocationGeneration) -join '|'
}
function VerifyNativeStaticInventory($Frame,$Execution) {
	NativeReceiptFields $Execution @('allocations','completedSubmissions','discardedCommands','acceptedSubmissions') 'static physical inventory'
	foreach($field in @('allocations','completedSubmissions','discardedCommands','acceptedSubmissions')){NativeRequire ($Execution.$field -is [array]) 'Static physical inventory is not actual array'}
	foreach($field in @('completedSubmissions','discardedCommands')){$seen=@{};foreach($value in $Execution.$field){$id=NativeShadowBatchUnsigned64 $value $field;NativeRequire ($id -gt 0 -and !$seen.ContainsKey([string]$id)) 'Static terminal receipt duplicate or zero';$seen[[string]$id]=$true}}
	# Accepted command mappings are native queue receipts, not inferred serial order.
	$accepted=@{};foreach($use in $Execution.acceptedSubmissions){NativeReceiptFields $use @('commandSerial','submitSerial','frameSlot') 'static accepted use';$command=NativeShadowBatchUnsigned64 $use.commandSerial commandSerial;$submit=NativeShadowBatchUnsigned64 $use.submitSerial submitSerial;$slot=NativeShadowBatchUnsigned32 $use.frameSlot frameSlot;NativeRequire ($command -gt 0 -and $submit -gt 0 -and $slot -lt 3 -and !$accepted.ContainsKey([string]$command)) 'Static accepted command mapping invalid or duplicate';$accepted[[string]$command]=$use}
	$currentCommand=[string](Metric $Frame.commandSerial commandSerial);if($accepted.ContainsKey($currentCommand)){NativeRequire ((NativeShadowBatchUnsigned64 $accepted[$currentCommand].submitSerial submitSerial) -eq (Metric $Frame.submitSerial submitSerial)) 'Static acceptance disagrees with actual completed row submit'}
	if($Execution.copyOverlay -and $accepted.ContainsKey($Execution.sourceCommandSerial)){NativeRequire ($accepted[$Execution.sourceCommandSerial].submitSerial -ceq $Execution.sourceSubmitSerial) 'Static acceptance disagrees with retained copied source submit'}
	$owners=@{};$physical=@{};$active=[decimal]0;$retiring=[decimal]0;$backend=$null
	foreach($owner in $Execution.allocations){
		NativeReceiptFields $owner @('sessionGeneration','backendGeneration','allocationIdentity','allocationGeneration','actualBytes','depthFormat','layers','extent','retiring','uses') 'static allocation'
		foreach($field in @('sessionGeneration','backendGeneration','allocationIdentity','allocationGeneration','actualBytes')){NativeRequire ((NativeShadowBatchUnsigned64 $owner.$field $field) -gt 0) 'Static physical owner identity/charge missing'}
		NativeRequire ((NativeUnsigned $owner.sessionGeneration allocationSession $true) -eq (Metric $Frame.rendererSessionGeneration rendererSessionGeneration)) 'Static inventory uses another renderer session'
		NativeRequire ($owner.backendGeneration -ceq $Execution.backendGeneration) 'Static owner belongs to another actual backend'
		if($null -ne $backend){NativeRequire ($owner.backendGeneration -ceq $backend) 'Static inventory mixes backend lifetimes'}else{$backend=$owner.backendGeneration}
		$null=NativeShadowBatchUnsigned32 $owner.depthFormat depthFormat;$null=NativeShadowBatchUnsigned32 $owner.layers layers
		NativeRequire ($owner.depthFormat -in @(124,125,126) -and $owner.layers -in @(1,6) -and $owner.extent -is [array] -and $owner.extent.Count -eq 2 -and $owner.retiring -is [bool] -and $owner.uses -is [array]) 'Static owner format/layers/extent/retirement malformed'
		foreach($dimension in $owner.extent){NativeRequire ((NativeShadowBatchUnsigned32 $dimension extent) -gt 0 -and $dimension -le 32768) 'Static owner extent invalid'}
		$physicalKey=@($owner.sessionGeneration,$owner.backendGeneration,$owner.allocationIdentity) -join '|';NativeRequire (!$physical.ContainsKey($physicalKey)) 'Static allocation handle overlaps another live generation';$physical[$physicalKey]=$true
		$key=NativeStaticAllocationKey $owner;NativeRequire (!$owners.ContainsKey($key)) 'Static owner charged more than once';$owners[$key]=$owner
		if($owner.retiring){$retiring+=NativeShadowBatchUnsigned64 $owner.actualBytes actualBytes}else{$active+=NativeShadowBatchUnsigned64 $owner.actualBytes actualBytes}
		$uses=@{};foreach($use in $owner.uses){NativeReceiptFields $use @('commandSerial','submitSerial','frameSlot') 'static pending use';$command=NativeShadowBatchUnsigned64 $use.commandSerial commandSerial;$null=NativeShadowBatchUnsigned64 $use.submitSerial submitSerial;$slot=NativeShadowBatchUnsigned32 $use.frameSlot frameSlot;NativeRequire ($command -gt 0 -and $slot -lt 3 -and !$uses.ContainsKey([string]$command)) 'Static pending command identity/slot duplicated or invalid';$uses[[string]$command]=$true;if($accepted.ContainsKey([string]$command)){$map=$accepted[[string]$command];NativeRequire ($map.frameSlot -eq $slot -and ($use.submitSerial -ceq '0' -or $use.submitSerial -ceq $map.submitSerial)) 'Static pending use contradicts actual accepted queue mapping'}}
	}
	NativeRequire ($active -eq (Metric $Frame.memory.activeBytes activeBytes) -and $retiring -eq (Metric $Frame.memory.retiringBytes retiringBytes) -and (Metric $Frame.memory.reservedBytes reservedBytes) -eq 0) 'Static physical inventory does not equal measured active/retiring/reserved charges'
	if($Execution.copyOverlay){
		$matching=@($Execution.allocations|Where-Object {$_.allocationIdentity -ceq $Execution.allocationIdentity -and $_.allocationGeneration -ceq $Execution.allocationGeneration -and $_.backendGeneration -ceq $Execution.backendGeneration})
		NativeRequire ($matching.Count -eq 1 -and !$matching[0].retiring -and $matching[0].actualBytes -ceq $Execution.actualBytes -and $matching[0].depthFormat -eq $Execution.staticView.depthFormat -and (JsonEqual $matching[0].extent $Execution.staticView.extent) -and $matching[0].layers -eq $(if($Execution.staticView.resourceKind -eq 1){6}else{1})) 'Static copied source is absent/stale/retiring in measured physical inventory'
	}
}
function VerifyNativeStaticRetirementTimeline($Rows) {
	# Keep observed terminal receipts across bounded native history-ring expiry.
	# Clocks and mappings belong only to their actual renderer/backend lifetime.
	$prior=@{};$completed=@{};$discarded=@{};$accepted=@{}
	foreach($row in @($Rows|Sort-Object {Metric $_.frame.frameSerial frameSerial},{Metric $_.frame.commandSerial commandSerial})){
		$e=$row.counters.staticShadowExecution;$scope=@((Metric $row.frame.rendererSessionGeneration rendererSessionGeneration),$e.backendGeneration) -join '|'
		foreach($submit in $e.completedSubmissions){$completed["$scope|$submit"]=$true}
		foreach($command in $e.discardedCommands){$discarded["$scope|$command"]=$true}
		foreach($map in $e.acceptedSubmissions){$key="$scope|$($map.commandSerial)";if($accepted.ContainsKey($key)){NativeRequire (JsonEqual $accepted[$key] $map) 'Static accepted mapping changed within one native lifetime'}else{$accepted[$key]=$map}}
		$current=@{};foreach($owner in $e.allocations){$current[(NativeStaticAllocationKey $owner)]=$owner}
		foreach($key in $prior.Keys){$old=$prior[$key];$oldScope=@($old.sessionGeneration,$old.backendGeneration) -join '|';foreach($use in $old.uses){
			$found=$false;if($current.ContainsKey($key)){$matches=@($current[$key].uses|Where-Object {$_.commandSerial -ceq $use.commandSerial});if($matches.Count -eq 1){$now=$matches[0];NativeRequire ($now.frameSlot -eq $use.frameSlot -and ($now.submitSerial -ceq $use.submitSerial -or ($use.submitSerial -ceq '0' -and $now.submitSerial -cne '0'))) 'Static retained pending use changed identity';$found=$true}}
			if(!$found){$submit=$use.submitSerial;$mapKey="$oldScope|$($use.commandSerial)";if($accepted.ContainsKey($mapKey)){$map=$accepted[$mapKey];NativeRequire ($map.frameSlot -eq $use.frameSlot -and ($submit -ceq '0' -or $submit -ceq $map.submitSerial)) 'Static released use contradicts actual queue acceptance';$submit=$map.submitSerial}
				$terminal=if($submit -ceq '0'){$discarded.ContainsKey("$oldScope|$($use.commandSerial)")}else{$completed.ContainsKey("$oldScope|$submit")}
				NativeRequire $terminal 'Pending static physical use vanished or uncharged without actual completion/discard'}
		}}
		$prior=$current
	}
}
function VerifyNativeStaticShadowExecution($Frame,$Counters) {
	if(!(Has $Counters 'staticShadowExecution')){Unverified 'Missing actual native static execution receipt'}
	$e=$Counters.staticShadowExecution
	NativeReceiptFields $e @('available','partitionComplete','copyOverlay','staticReused','recordedComplete','requiredFaceMask','staticRecordedFaceMask','copiedFaceMask','staticView','staticContentToken','sourceCommandSerial','sourceSubmitSerial','allocationIdentity','allocationGeneration','backendGeneration','actualBytes','staticDraws','dynamicDraws','staticMembership','dynamicMembership','fallbackReason') 'static execution'
	foreach($field in @('available','partitionComplete','copyOverlay','staticReused','recordedComplete')){NativeRequire ($e.$field -is [bool]) 'Static execution flag is not actual native boolean'}
	if(!$e.available){Unverified 'Native static execution unavailable'}
	NativeRequire ((NativeShadowBatchUnsigned64 $e.backendGeneration backendGeneration) -gt 0) 'Static available execution lacks actual backend lifetime'
	NativeRequire ((Metric $Frame.shadow.staticReuseRegionCount staticReuseRegionCount) -eq $(if($e.copyOverlay -and $e.staticReused){1}else{0})) 'Static reuse metric is not backed by actual copy receipt'
	NativeRequire $e.recordedComplete 'Static final recording is incomplete'
	foreach($field in @('requiredFaceMask','staticRecordedFaceMask','copiedFaceMask')){$null=NativeShadowBatchUnsigned32 $e.$field $field}
	foreach($field in @('staticContentToken','sourceCommandSerial','sourceSubmitSerial','allocationIdentity','allocationGeneration','backendGeneration','actualBytes','staticDraws','dynamicDraws')){$null=NativeShadowBatchUnsigned64 $e.$field $field}
	NativeRequire ($e.staticMembership -is [array] -and $e.dynamicMembership -is [array] -and $e.fallbackReason -is [string]) 'Static execution arrays/reason malformed'
	$m=$Frame.memory;$n=$Counters.nativeCounters
	foreach($pair in @(@('staticCacheBudgetRequestedBytes','staticCacheBudgetRequestedBytes'),@('staticCacheBudgetEffectiveBytes','staticCacheBudgetEffectiveBytes'),@('reservedBytes','staticReservedBytes'),@('activeBytes','staticActiveBytes'),@('retiringBytes','staticRetiringBytes'))){foreach($value in @((Metric $m.($pair[0]) $pair[0]),(Metric $n.($pair[1]) $pair[1]))){NativeRequire ($value -is [ValueType] -and $value -isnot [bool]) 'Static memory metric is not native numeric';$null=NativeUnsigned $value memoryBytes};NativeRequire ((Metric $m.($pair[0]) $pair[0]) -eq (Metric $n.($pair[1]) $pair[1])) 'Raw/static memory charge differs from frame'}
	$requested=Metric $m.staticCacheBudgetRequestedBytes requestedBudget;$effective=Metric $m.staticCacheBudgetEffectiveBytes effectiveBudget
	$active=Metric $m.activeBytes activeBytes;$charged=[decimal]$active+(Metric $m.reservedBytes reservedBytes)+(Metric $m.retiringBytes retiringBytes)
	NativeRequire ($requested -le 1073741824 -and $effective -le 1073741824 -and (Metric $m.auxiliaryBytes auxiliaryBytes) -eq 0 -and $charged -le $effective) 'Static charged bytes exceed effective bound or auxiliary contract'
	NativeRequire ($m.draining -is [bool] -and $n.staticCacheDraining -is [bool] -and $m.draining -eq $n.staticCacheDraining) 'Static draining state is not actual consistent boolean'
	NativeRequire (($m.draining -and $requested -lt $charged) -or (!$m.draining -and $requested -eq $effective -and $charged -le $requested)) 'Static budget lowering lacks truthful draining bound'
	VerifyNativeStaticInventory $Frame $e
	$eligible=@($Counters.eligibleCasterIdentityRows);$submitted=@($Counters.submittedCasterIdentityRows)
	$eligibleSemantic=NativeStaticRows $eligible;$submittedSemantic=NativeStaticRows $submitted
	if(!$e.copyOverlay){
		# Ordinary combined fallback still covers all eligible geometry. Its
		# grouped draws may cross categories, so zero category call counts are honest.
		NativeRequire (!$e.staticReused -and (NativeShadowBatchUnsigned64 $e.staticDraws staticDraws) -eq 0 -and (NativeShadowBatchUnsigned64 $e.dynamicDraws dynamicDraws) -eq 0) 'Combined fallback claims static copy work'
		NativeRequire (($eligibleSemantic -join "`n") -ceq ($submittedSemantic -join "`n")) 'Combined fallback omits exact eligible contribution'
		if(!$e.partitionComplete){return}
	}
	NativeRequire $e.partitionComplete 'Static copy lacks complete classification partition'
	$partition=@($e.staticMembership)+@($e.dynamicMembership)
	$partitionSemantic=NativeStaticRows $partition
	NativeRequire (($partitionSemantic -join "`n") -ceq ($eligibleSemantic -join "`n")) 'Static/dynamic semantic partition differs from eligible set'
	foreach($row in $e.staticMembership){
		NativeRequire ($row.staticEligible -and (NativeUnsigned $row.staticClassificationReason staticReason) -eq 0 -and (Has $row 'staticSupport')) 'Static member lacks actual classification'
		foreach($field in @('authoredStatic','knownDefaultShadowAbi','ownerPublicationsComplete','materialShadowSupported','geometrySupported','deformationAbsent')){NativeRequire ($row.staticSupport.$field -is [bool] -and $row.staticSupport.$field) 'Static member lacks complete captured support'}
		NativeRequire ($row.deformationPalette.Count -eq 0 -and $row.morphWeights.Count -eq 0) 'Static source claims actual deformation absent while inputs deform'
	}
	foreach($row in $e.dynamicMembership){NativeRequire (!$row.staticEligible) 'Eligible static member incorrectly routed dynamic'}
	if(!$e.copyOverlay){return}
	NativeRequire ($e.staticMembership.Count -gt 0 -and $requested -gt 0 -and !$m.draining) 'Static copy attempted without admitted nonempty static owner'
	foreach($field in @('staticContentToken','sourceCommandSerial','sourceSubmitSerial','allocationIdentity','allocationGeneration','backendGeneration','actualBytes')){NativeRequire ((NativeShadowBatchUnsigned64 $e.$field $field) -gt 0) 'Static copy lacks positive native source receipt'}
	NativeRequire ((NativeShadowBatchUnsigned64 $e.actualBytes actualBytes) -le $active) 'Static copy owner is not physically charged'
	$source=$e.staticView;$final=$Counters.qualifiedShadowViewKey
	NativeReceiptFields $source @('rendererSessionGeneration','viewType','viewId','referenceOrigin','lightId','resourceKind','faceIndex','allocationGeneration','regionId','extent','depthFormat','lightProjectionStamp') 'static source key'
	NativeRequire ((NativeShadowBatchUnsigned64 $e.allocationGeneration allocationGeneration) -eq (NativeUnsigned $source.allocationGeneration sourceGeneration $true) -and (NativeShadowBatchUnsigned64 $e.allocationIdentity allocationIdentity) -eq (NativeUnsigned $source.regionId sourceRegion $true)) 'Static physical source key differs from actual allocation'
	$qualified=ConvertTo-Json -InputObject $source -Compress -Depth 100|ConvertFrom-Json
	$qualified.allocationGeneration=$final.allocationGeneration;$qualified.regionId=$final.regionId
	$cube=$final.resourceKind -eq 1;$mask=if($cube){63}else{1}
	if($cube){
		NativeRequire ($source.faceIndex -eq 0 -and $source.lightProjectionStamp.cubeProjectionEvidenceComplete -and $final.lightProjectionStamp.cubeProjectionEvidenceComplete -and $source.lightProjectionStamp.cubeViewProjections.Count -eq 6 -and $final.faceIndex -ge 0 -and $final.faceIndex -lt 6) 'Static cube lacks complete projection cohort'
		NativeRequire ((JsonEqual $source.lightProjectionStamp.viewProjection $source.lightProjectionStamp.cubeViewProjections[0]) -and (JsonEqual $final.lightProjectionStamp.viewProjection $final.lightProjectionStamp.cubeViewProjections[$final.faceIndex])) 'Static cube selected face projection differs'
		$qualified.faceIndex=$final.faceIndex;$qualified.lightProjectionStamp.viewProjection=$final.lightProjectionStamp.viewProjection
	}
	NativeRequire (JsonEqual $qualified $final) 'Static source does not match exact final logical light/view/projection'
	NativeRequire ($e.requiredFaceMask -eq $mask -and $e.copiedFaceMask -eq $mask -and $e.staticRecordedFaceMask -eq $(if($e.staticReused){0}else{$mask})) 'Static copy/recorded masks are incomplete'
	$command=Metric $Frame.commandSerial commandSerial;$submit=Metric $Frame.submitSerial submitSerial
	NativeRequire ((NativeShadowBatchUnsigned64 $e.sourceCommandSerial sourceCommand) -le $command -and (NativeShadowBatchUnsigned64 $e.sourceSubmitSerial sourceSubmit) -le $submit) 'Static source receipt comes from future native work'
	if(!$e.staticReused){NativeRequire ((NativeShadowBatchUnsigned64 $e.sourceCommandSerial sourceCommand) -eq $command -and (NativeShadowBatchUnsigned64 $e.sourceSubmitSerial sourceSubmit) -eq $submit) 'Fresh static source does not join current recorded command/submit'}
	$staticDraws=if($e.staticReused){0}else{$e.staticMembership.Count};$dynamicDraws=$e.dynamicMembership.Count
	NativeRequire ((NativeShadowBatchUnsigned64 $e.staticDraws staticDraws) -eq $staticDraws -and (NativeShadowBatchUnsigned64 $e.dynamicDraws dynamicDraws) -eq $dynamicDraws -and (Metric $Frame.shadow.actualDrawSubmissions actualDraws) -eq $staticDraws+$dynamicDraws) 'Static/dynamic actual singleton draw counts differ'
	foreach($pair in @(@('staticRefreshCasterSegments','staticRefreshes',$staticDraws),@('dynamicRefreshCasterSegments','dynamicRefreshes',$dynamicDraws))){NativeRequire ((Metric $Frame.shadow.($pair[0]) $pair[0]) -eq $pair[2] -and (Metric $n.($pair[1]) $pair[1]) -eq $pair[2]) 'Actual static/dynamic refresh count differs from copy commands'}
	$expected=@($e.dynamicMembership);if(!$e.staticReused){$expected=@($e.staticMembership)+$expected}
	NativeRequire (((NativeStaticRows $expected) -join "`n") -ceq ($submittedSemantic -join "`n")) 'Static copy submitted cached, missing or forged contribution'
	NativeRequire ((Metric $Frame.shadow.submittedCasterSegments submittedCount) -eq $expected.Count -and (Metric $Frame.shadow.eligibleCasterSegments eligibleCount) -eq $eligible.Count -and (Metric $Frame.shadow.reuseRegionCount reuseCount) -eq 0) 'Static copy counters disagree with complete current final rendering'
}
function VerifyNativeStaticOptions($Frame,$Document,$Envelope,$Step,$Provenance) {
	# Only the authored budget lane may differ from the operation's start options.
	# A queued request alone is insufficient: the exact protected publication
	# generation/session/epoch and effective value must join the native frame.
	$actual=$Frame.effectiveOptions;$start=$Document.requestedOptions
	foreach($field in $start.PSObject.Properties.Name){if($field -cne 'staticShadowCacheBudgetBytes'){NativeRequire (JsonEqual $actual.$field $start.$field) 'P5 changed an unrelated renderer option'}}
	$budget=NativeUnsigned $actual.staticShadowCacheBudgetBytes effectiveStaticBudget
	if(!(Has $Frame 'evidenceKind') -or $Frame.evidenceKind -ceq 'Shadow'){NativeRequire ((Metric $Frame.memory.staticCacheBudgetRequestedBytes requestedBudget) -eq $budget) 'P5 physical requested budget contradicts actual effective option'}
	if($Document.identity.mode -ceq 'Reference'){NativeRequire ($budget -eq 0) 'P5 reference budget must remain zero'}
	else{
		$expected=if($Step.action -ceq 'zero-budget'){0}elseif($Step.action -ceq 'too-small-budget'){1}else{67108864}
		NativeRequire ($budget -eq $expected -or ($Step.action -ceq 'lower-budget-in-flight' -and $budget -eq 0)) 'P5 effective budget is not an authored control'
	}
	$matches=@($Envelope.receipts|Where-Object {$_.kind -ceq 'StaticBudgetPublication' -and $_.fixtureStepId -ceq $Frame.fixtureStepId -and (Has $_ 'settingsGeneration') -and $_.settingsGeneration -eq (Metric $Frame.settingsGeneration settingsGeneration) -and (Has $_ 'effectiveOptions') -and (JsonEqual $_.effectiveOptions $actual)})
	if($matches.Count -ne 1){Unverified 'P5 lacks one actual protected budget publication for native frame'}
	$r=$matches[0];NativeReceiptFields $r @('operationId','requestedGeneration','priorOptions','requestedOptions','effectiveOptions','settingsGeneration','rendererSessionGeneration','optionsSessionGeneration') 'P5 budget publication'
	foreach($field in @('requestedGeneration','settingsGeneration','rendererSessionGeneration','optionsSessionGeneration')){NativeRequire ($r.$field -is [ValueType] -and $r.$field -isnot [bool] -and $r.$field -isnot [DateTime]) 'P5 protected budget clock is not an actual numeric receipt'}
	NativeRequire ($r.operationId -ceq $Envelope.operationId -and (NativeUnsigned $r.requestedGeneration requestedGeneration $true) -le (NativeUnsigned $r.settingsGeneration settingsGeneration $true) -and (NativeUnsigned $r.rendererSessionGeneration rendererSessionGeneration $true) -eq (Metric $Frame.rendererSessionGeneration rendererSessionGeneration) -and (NativeUnsigned $r.optionsSessionGeneration optionsSessionGeneration $true) -eq (NativeUnsigned $Provenance.optionsSessionGeneration optionsSessionGeneration $true)) 'P5 budget publication belongs to another native generation/owner'
	NativeRequire (JsonEqual $r.requestedOptions $r.effectiveOptions) 'P5 requested budget did not become exact effective options'
	foreach($field in $start.PSObject.Properties.Name){if($field -cne 'staticShadowCacheBudgetBytes'){NativeRequire ((JsonEqual $r.priorOptions.$field $start.$field) -and (JsonEqual $r.requestedOptions.$field $start.$field)) 'P5 budget request changed another option'}}
}
function VerifyNativeStaticBudgetLowering($Envelope,$Steps,$Rows) {
	$steps=@($Steps|Where-Object action -ceq 'lower-budget-in-flight');foreach($step in $steps){
		$loweringReceipts=@($Envelope.receipts|Where-Object {$_.kind -ceq 'StaticBudgetLowering' -and $_.fixtureStepId -ceq $step.id})
		if($loweringReceipts.Count -ne 1){Unverified 'P5 requires actual in-flight budget lowering receipt'}
		$r=$loweringReceipts[0];NativeReceiptFields $r @('stepFrame','requestedGeneration','priorOptions','requestedOptions','rendererSessionGeneration','optionsSessionGeneration','allocationIdentity','allocationGeneration','backendGeneration','allocations','acceptedSubmissions','completedSubmissions','discardedCommands') 'P5 in-flight lowering'
		NativeRequire ((NativeUnsigned $r.priorOptions.staticShadowCacheBudgetBytes priorBudget) -eq 67108864 -and (NativeUnsigned $r.requestedOptions.staticShadowCacheBudgetBytes requestedBudget) -eq 0) 'P5 in-flight control is not actual 64MiB to zero'
		foreach($field in $r.priorOptions.PSObject.Properties.Name){if($field -cne 'staticShadowCacheBudgetBytes'){NativeRequire (JsonEqual $r.priorOptions.$field $r.requestedOptions.$field) 'P5 lowering changed another option'}}
		$session=NativeUnsigned $r.rendererSessionGeneration rendererSessionGeneration $true;$epoch=NativeUnsigned $r.optionsSessionGeneration optionsSessionGeneration $true;$generation=NativeUnsigned $r.requestedGeneration requestedGeneration $true
		foreach($field in @('allocationIdentity','allocationGeneration','backendGeneration')){NativeRequire ((NativeShadowBatchUnsigned64 $r.$field $field) -gt 0) 'P5 lowering selected owner lacks positive physical identity'}
		$priorFrame=NativeUnsigned $r.stepFrame stepFrame $true
		$observed=@($Rows|Where-Object {$_.frame.fixtureStepId -ceq $step.id -and $_.frame.stepFrame -eq ($priorFrame - 1) -and (Metric $_.frame.rendererSessionGeneration rendererSessionGeneration) -eq $session -and $_.counters.staticShadowExecution.copyOverlay -and $_.counters.staticShadowExecution.recordedComplete -and $_.counters.staticShadowExecution.allocationIdentity -ceq $r.allocationIdentity -and $_.counters.staticShadowExecution.allocationGeneration -ceq $r.allocationGeneration -and $_.counters.staticShadowExecution.backendGeneration -ceq $r.backendGeneration})
		if(!$observed.Count){Unverified 'P5 lowering owner is not the actual last accepted native copy source'}
		$owner=@($r.allocations|Where-Object {$_.allocationIdentity -ceq $r.allocationIdentity -and $_.allocationGeneration -ceq $r.allocationGeneration -and $_.backendGeneration -ceq $r.backendGeneration -and (NativeUnsigned $_.sessionGeneration sessionGeneration $true) -eq $session})
		NativeRequire ($owner.Count -eq 1 -and $owner[0].retiring -is [bool] -and !$owner[0].retiring -and (NativeShadowBatchUnsigned64 $owner[0].actualBytes actualBytes) -gt 0) 'P5 lowering lacks exact current live charged allocation'
		$pending=$false;foreach($use in $owner[0].uses){$command=NativeShadowBatchUnsigned64 $use.commandSerial commandSerial;$submit=NativeShadowBatchUnsigned64 $use.submitSerial submitSerial;NativeRequire ($command -gt 0 -and (NativeShadowBatchUnsigned32 $use.frameSlot frameSlot) -lt 3) 'P5 lowering pending use malformed'
			$maps=@($r.acceptedSubmissions|Where-Object commandSerial -CEQ $use.commandSerial);NativeRequire ($maps.Count -le 1) 'P5 lowering duplicates acceptance';if($maps.Count){NativeRequire ($maps[0].frameSlot -eq $use.frameSlot -and ($submit -eq 0 -or $use.submitSerial -ceq $maps[0].submitSerial)) 'P5 lowering acceptance owner mismatch';$submit=NativeShadowBatchUnsigned64 $maps[0].submitSerial submitSerial;NativeRequire ($submit -gt 0) 'P5 lowering acceptance lacks real submission'}
			if($submit -gt 0){if($r.completedSubmissions -cnotcontains [string]$submit){$pending=$true}}elseif($r.discardedCommands -cnotcontains [string]$command){$pending=$true}
		}
		NativeRequire $pending 'P5 budget was not lowered while actual native use remained outstanding'
		$published=@($Envelope.receipts|Where-Object {$_.kind -ceq 'StaticBudgetPublication' -and $_.fixtureStepId -ceq $step.id -and $_.requestedGeneration -eq $generation -and $_.rendererSessionGeneration -eq $session -and $_.optionsSessionGeneration -eq $epoch -and (JsonEqual $_.requestedOptions $r.requestedOptions) -and (JsonEqual $_.effectiveOptions $r.requestedOptions)})
		if($published.Count -ne 1){Unverified 'P5 pending lowering lacks actual protected zero-budget publication'}
	}
}
function VerifyNativeSharedStaticRecipe($Frame,$Counters,$Step) {
	# Shared actions preserve their original selection. Only the declared
	# overflow action substitutes the real thousand-object capacity pool.
	$owners=@{};foreach($row in $Counters.eligibleCasterIdentityRows){$owners[$row.objectId]=$true}
	if($Step.action -ceq 'group-overflow'){
		NativeRequire ($owners.Count -eq 1000) 'Shared P5 overflow lacks its authored thousand-owner pool'
		for($i=20000;$i -le 20999;$i++){NativeRequire ($owners.ContainsKey('650'+('0'*24)+[string]$i)) 'Shared P5 overflow substitutes or omits pool owner'}
	}
	if($Step.action -ceq 'unsupported-grouped-route'){
		NativeRequire ($owners.Count -eq 2 -and $owners.ContainsKey('65000000000000000000000000000021') -and $owners.ContainsKey('65000000000000000000000000000031')) 'Shared P5 unsupported route lacks actual pose/morph owners'
		foreach($row in $Counters.eligibleCasterIdentityRows){NativeRequire (!$row.staticEligible -and ($row.deformationPalette.Count -gt 0 -or $row.morphWeights.Count -gt 0)) 'Shared P5 unsupported route lacks captured CPU deformation'}
		NativeRequire (!$Counters.staticShadowExecution.copyOverlay -and (Metric $Frame.shadow.actualDrawSubmissions actualDrawSubmissions) -eq $Counters.submittedCasterIdentityRows.Count) 'Shared P5 unsupported route is not complete actual singleton fallback'
	}
}
function VerifyNativeStaticShadowRecipe($Frame,$Counters,$Step) {
	# These are actual authored selection IDs; native multi-section rows are
	# deduplicated only by object owner, never replaced with a planned draw count.
	$expected=@{};foreach($i in 1..1000){$expected['6560000000000000'+$i.ToString('x16')]=$true}
	if($Step.action -ceq 'remove-static'){$expected.Remove('65600000000000000000000000000001')}
	if($Step.action -cne 'remove-dynamic'){$expected[$(if($Step.action -ceq 'morph-animation'){'65000000000000000000000000000031'}else{'65000000000000000000000000000021'})]=$true}
	$actual=@{};foreach($row in $Counters.eligibleCasterIdentityRows){$actual[$row.objectId]=$true}
	NativeRequire ($actual.Count -eq $expected.Count) 'P5 actual eligible owner count differs from authored selected pool'
	foreach($id in $expected.Keys){NativeRequire ($actual.ContainsKey($id)) 'P5 actual eligible owner set omits/substitutes authored pool'}
	$budget=Metric $Frame.memory.staticCacheBudgetRequestedBytes requestedBudget
	if((Has $Counters 'mode') -and $Counters.mode -ceq 'Reference'){NativeRequire ($budget -eq 0) 'P5 reference must preserve actual zero-cache control'}
	elseif($Step.action -ceq 'zero-budget'){NativeRequire ($budget -eq 0) 'P5 zero-budget control was not actually applied'}
	elseif($Step.action -ceq 'too-small-budget'){NativeRequire ($budget -eq 1) 'P5 too-small actual budget differs from frozen byte control'}
	elseif($Step.action -ceq 'lower-budget-in-flight'){NativeRequire ($budget -in @(0,67108864)) 'P5 pending budget lowering uses an unrelated request'}
	else{NativeRequire ($budget -eq 67108864) 'P5 actual static budget differs from frozen 64MiB control'}
	if($Step.action -in @('zero-budget','too-small-budget','unsupported-depth-transfer','allocation-failure') -and $Frame.measurementPhase -ceq 'Transition'){
		NativeRequire (!$Counters.staticShadowExecution.copyOverlay) 'P5 requested unsupported/admission fault did not take complete combined fallback'
	}
}
function GetNativeStaticShadowCameras([string]$StepId,$Cameras) {
	# Dedicated P5 camera coverage is authored per step; never infer it from rows.
	$expected=if($StepId -ceq 'static-dynamic-13'){@('Scene|1','Game|2')}else{@('Scene|1')}
	$result=@();foreach($key in $expected){$match=@($Cameras|Where-Object {"$($_.viewType)|$($_.viewId)" -ceq $key});NativeRequire ($match.Count -eq 1) 'Frozen P5 camera authority missing or duplicate';$result+=,$match[0]};return ,$result
}
function VerifyNativeStaticShadowRegions($Counters,[string]$StepId,$Cameras) {
	$views=GetNativeStaticShadowCameras $StepId $Cameras;$cube=$StepId -ceq 'static-dynamic-09';$faces=if($cube){6}else{1};$light=if($cube){'65000000000000000000000000000040'}else{'65000000000000000000000000000041'};$seen=@{}
	foreach($counter in $Counters){$key=$counter.qualifiedShadowViewKey;$view="$($key.viewType)|$($key.viewId)";NativeRequire (@($views|Where-Object {"$($_.viewType)|$($_.viewId)" -ceq $view}).Count -eq 1) 'P5 dedicated region uses an unexpected view'
		NativeRequire ($key.lightId -ceq $light -and $key.resourceKind -eq $(if($cube){1}else{0}) -and $key.faceIndex -ge 0 -and $key.faceIndex -lt $faces) 'P5 dedicated region differs from frozen actual light/face'
		$id="$view|$($key.faceIndex)";NativeRequire (!$seen.ContainsKey($id)) 'P5 dedicated region duplicated';$seen[$id]=$true
	}
	if($seen.Count -ne $views.Count*$faces){Unverified 'P5 dedicated cohort omits required light/view/face'}
	VerifyNativeBundle $Counters
}
function VerifyNativeStaticAnimationWindow($Rows,$Envelope) {
	# Per qualified region, a changing drawn palette/weight input is necessary;
	# pose publication counters alone cannot prove animation or savings.
	if($null -eq $Envelope){Unverified 'SC006 lacks native CPU animation publication receipts'}
	$groups=@($Rows|Group-Object {"$($_.counters.qualifiedShadowViewKey.viewType)|$($_.counters.qualifiedShadowViewKey.viewId)|$($_.counters.qualifiedShadowViewKey.lightId)|$($_.counters.qualifiedShadowViewKey.faceIndex)"})
	if(!$groups.Count){Unverified 'SC006 lacks actual changing animation observations'}
	foreach($group in $groups){
		$samples=@($group.Group|Sort-Object {$_.frame.stepFrame});if($samples.Count -ne 300){Unverified 'SC006 requires all 300 actual measured samples per region'}
		$prior=$null;$staticSemantic=$null;$sourceOwner=$null;$nativeFrame=0
		for($i=0;$i -lt 300;$i++){
			$f=$samples[$i].frame;$e=$samples[$i].counters.staticShadowExecution
			$receipts=@($Envelope.receipts|Where-Object {$_.kind -ceq 'AnimationPublication' -and $_.fixtureStepId -ceq $f.fixtureStepId -and (Has $_ 'perSample') -and $_.perSample -and (Has $_ 'stepFrame') -and $_.stepFrame -eq $f.stepFrame -and $_.objectId -ceq '65000000000000000000000000000020'})
			if($receipts.Count -ne 1){Unverified 'SC006 lacks one native CPU seek publication for actual measured sample'}
			$r=$receipts[0];NativeReceiptFields $r @('measurementPhase','requestedPlaybackTick','sampleTick','observableTick','status','cpuPosePresent','cpuContentChanged','commandPublicationGeneration','authorityPublicationGeneration','authoritySessionGeneration','snapshotPublicationGeneration','previousSnapshotPublicationGeneration') 'SC006 CPU animation'
			NativeRequire ($r.perSample -is [bool] -and $r.measurementPhase -ceq 'Measured' -and $r.status -ceq 'Ok' -and (NativeUnsigned $r.requestedPlaybackTick requestedTick) -eq 31+$f.stepFrame -and (NativeUnsigned $r.sampleTick sampleTick) -eq 31+$f.stepFrame -and (NativeUnsigned $r.observableTick observableTick) -eq 31+$f.stepFrame) 'SC006 seek receipt does not match actual measured tick/phase'
			NativeReceiptBool $r.cpuPosePresent cpuPosePresent $true;NativeReceiptBool $r.cpuContentChanged cpuContentChanged $true
			foreach($field in @('commandPublicationGeneration','authorityPublicationGeneration','authoritySessionGeneration','snapshotPublicationGeneration')){$null=NativeUnsigned $r.$field $field $true}
			NativeRequire ((NativeUnsigned $r.snapshotPublicationGeneration snapshotPublication) -gt (NativeUnsigned $r.previousSnapshotPublicationGeneration previousSnapshot)) 'SC006 CPU snapshot did not advance'

			NativeRequire ($f.measurementPhase -ceq 'Measured' -and $f.stepFrame -eq 60+$i) 'SC006 measured window duplicates or skips frame index'
			if(!$e.available -or !$e.copyOverlay -or !$e.staticReused -or $e.staticDraws -cne '0'){Unverified 'SC006 lacks actual static copy reuse under changing animation'}
			$actualFrame=Metric $f.frameSerial frameSerial;NativeRequire ($actualFrame -gt $nativeFrame) 'SC006 reuses the same native completion frame';$nativeFrame=$actualFrame
			$owner=Canonical ([ordered]@{allocationIdentity=$e.allocationIdentity;allocationGeneration=$e.allocationGeneration;backendGeneration=$e.backendGeneration;staticContentToken=$e.staticContentToken;sourceCommandSerial=$e.sourceCommandSerial;sourceSubmitSerial=$e.sourceSubmitSerial;staticView=$e.staticView})
			if($null -ne $sourceOwner){NativeRequire ($owner -ceq $sourceOwner) 'SC006 static source owner/token changed while claiming no static refresh'}else{$sourceOwner=$owner}
			$current=NativeStaticRows $e.staticMembership
			if($null -ne $staticSemantic){NativeRequire (($current -join "`n") -ceq $staticSemantic) 'SC006 static drawn inputs changed'}else{$staticSemantic=$current -join "`n"}
			$animated=@($e.dynamicMembership|Where-Object {$_.deformationPalette.Count -gt 0 -or $_.morphWeights.Count -gt 0})
			if(!$animated.Count -or !@($animated|Where-Object objectId -CEQ '65000000000000000000000000000021').Count){Unverified 'SC006 actual CPU actor palette inputs unavailable'}
			$signature=Canonical @($animated|Sort-Object objectId,segmentId|ForEach-Object {[ordered]@{objectId=$_.objectId;deformationPalette=$_.deformationPalette;morphWeights=$_.morphWeights}})
			if($null -ne $prior -and $signature -ceq $prior){Unverified 'SC006 animation inputs did not progress'}
			$prior=$signature
		}
	}
}
function ReadNativeProof($Frame,$Identity,[string]$Directory) {
	if((Has $Frame 'evidenceKind') -and $Frame.evidenceKind -ceq 'LightingDepth'){return ReadNativeDepthProof $Frame $Identity $Directory}
	if((Has $Frame 'evidenceKind') -and $Frame.evidenceKind -ceq 'Lighting'){VerifyNativeLightingStage $Identity;return ReadNativeLightingProof $Frame $Identity $Directory}
	InitializeNativeProof
	if($Frame.publicationState -ne 'Complete'){Unverified 'Native frame is not completion-qualified'}
	$artifact=NativeArtifact $Frame Counters $Directory 16777216
	$counters=[Text.Encoding]::UTF8.GetString($artifact.bytes)|ConvertFrom-Json
	foreach($field in @('schemaVersion','fixtureId','fixtureHash','operationId','fixtureStepId','runId','runIndex','seed','mode','stage','measurementPhase','stepFrame','qualifiedShadowViewKey','sourceContentOwner','contentToken','depthSourceFormat','depthNativeExtent','depthEncoding','coverageEncoding','colorSourceFormat','colorNativeExtent','imageEncoding','readbackProvenance','eligibleCasterIdentityRows','submittedCasterIdentityRows','nativeCounters','effectiveOptions')){if(!(Has $counters $field)){Unverified "Missing native counter proof field $field"}}
	if(!(Has $counters 'depthEncoding') -or $counters.depthEncoding -cne 'float32-le' -or $counters.coverageEncoding -cne 'occupied-u8' -or $counters.depthSourceFormat -ne 126){Unverified 'Unknown native depth/coverage encoding or format'}
	NativeRequire ($counters.schemaVersion -eq 1) 'Native counters schema mismatch'
	foreach($field in @('fixtureId','runId','runIndex','seed','mode','stage')){NativeRequire (JsonEqual $counters.$field $Identity.$field) "Native counters $field correlation mismatch"}
	NativeRequire ($counters.fixtureHash -ceq (Metric $Identity.fixtureHash fixtureHash)) 'Native fixture hash mismatch'
	NativeRequire ($counters.operationId -cmatch '^[0-9a-f]{32}$' -and $counters.operationId -cne ('0'*32)) 'Native operation identity missing'
	NativeRequire ($counters.fixtureStepId -ceq $Frame.fixtureStepId -and $counters.measurementPhase -ceq $Frame.measurementPhase -and (NativeUnsigned $counters.stepFrame stepFrame) -eq $Frame.stepFrame) 'Native captured fixture step correlation mismatch'
	$provenance=$counters.readbackProvenance;$key=$counters.qualifiedShadowViewKey
	NativeRequire ($provenance.kind -ceq 'FullNativeSameCommandCompletedReadback') 'Native complete readback provenance missing'
	foreach($field in @('frameSerial','commandSerial','submitSerial','completionSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration')) {
		$actual=NativeUnsigned $provenance.$field $field $true
		NativeRequire ($actual -eq (Metric $Frame.$field $field)) "Native provenance $field mismatch"
	}
	NativeRequire ((NativeUnsigned $provenance.completionSerial completionSerial) -ge (NativeUnsigned $provenance.submitSerial submitSerial)) 'Native completion precedes submission'
	NativeRequire ($key.viewType -ceq $Frame.viewType -and $key.viewId -eq $Frame.viewId -and $key.lightId -ceq (Metric $Frame.lightId lightId) -and $key.faceIndex -eq (Metric $Frame.faceIndex faceIndex) -and
		(NativeUnsigned $key.rendererSessionGeneration rendererSessionGeneration $true) -eq (Metric $Frame.rendererSessionGeneration rendererSessionGeneration)) 'Qualified native shadow view/light/face/session mismatch'
	$null=NativeUnsigned $key.allocationGeneration allocationGeneration $true
	NativeOrigin $key.referenceOrigin 'qualified view origin'
	NativeFiniteArray $key.lightProjectionStamp.viewProjection 16 'qualified light projection'
	NativeFiniteArray $key.lightProjectionStamp.lightParameters 4 'qualified light parameters'
	NativeFiniteArray $key.lightProjectionStamp.shadowFilterParameters 4 'qualified shadow filter'
	NativeRequire ($key.resourceKind -in @(0,1,2,3) -and $key.depthFormat -eq 126 -and $key.faceIndex -ge 0 -and $key.faceIndex -lt 6) 'Unsupported qualified native shadow resource'
	$null=NativeUnsigned $key.regionId regionId
	NativeRequire ((NativeUnsigned $key.extent[0] nativeWidth $true) -le 32768 -and (NativeUnsigned $key.extent[1] nativeHeight $true) -le 32768) 'Native shadow extent exceeds bounded integer dimensions'
	NativeRequire ($counters.sourceContentOwner.nativeOwner -cmatch '^[1-9][0-9]*$') 'Native source owner missing'
	$token=NativeUnsigned $counters.contentToken contentToken $true
	NativeRequire ((NativeUnsigned $counters.sourceContentOwner.nativeOwner nativeOwner $true) -eq $token -and (NativeUnsigned $counters.sourceContentOwner.contentToken sourceToken $true) -eq $token -and (Canonical $key) -ceq (Canonical $counters.sourceContentOwner.qualifiedShadowViewKey)) 'Source content owner/key/token mismatch'
	NativeRequire ($counters.depthNativeExtent.Count -eq 2 -and $key.extent.Count -eq 2 -and (Canonical $key.extent) -ceq (Canonical $counters.depthNativeExtent)) 'Native shadow extent provenance mismatch'
	$width=[int]$key.extent[0];$height=[int]$key.extent[1]
	NativeRequire ($counters.colorNativeExtent.Count -eq 2 -and $counters.colorNativeExtent[0] -eq $Identity.resolution.width -and $counters.colorNativeExtent[1] -eq $Identity.resolution.height) 'Frozen native camera resolution mismatch'
	if($counters.imageEncoding -cne 'png-rgba8' -or $counters.colorSourceFormat -notin @(37,43,44,50,97)){Unverified 'Unknown native color encoding or format'}
	NativeRequire ((Canonical $counters.effectiveOptions) -ceq (Canonical $Frame.effectiveOptions)) 'Native effective settings mismatch'
	$depth=NativeArtifact $Frame Depth $Directory;$coverage=NativeArtifact $Frame Coverage $Directory;$image=NativeArtifact $Frame Image $Directory
	try{[TerraRendererNativeProof]::Depth($depth.bytes,$coverage.bytes,$width,$height);$decodedImage=[TerraRendererNativeProof]::DecodeImage($image.bytes,[int]$Identity.resolution.width,[int]$Identity.resolution.height)}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}
	$eligible=NativeCasterRows $counters.eligibleCasterIdentityRows;$submitted=NativeCasterRows $counters.submittedCasterIdentityRows
	foreach($field in @('eligibleCasterSegments','submittedCasterSegments','actualDrawSubmissions','shadowReuses')) {$frameField=if($field -eq 'shadowReuses'){'reuseRegionCount'}else{$field};NativeRequire ((Metric $counters.nativeCounters.$field $field) -eq (Metric $Frame.shadow.$frameField $field)) "Forged native $field counter mismatch"}
	NativeRequire ((Metric $Frame.shadow.eligibleCasterSegments eligibleCasterSegments) -eq $eligible.Count -and (Metric $Frame.shadow.submittedCasterSegments submittedCasterSegments) -eq $submitted.Count) 'Native caster counter/actual identity row count mismatch'
	$reuse=Metric $Frame.shadow.reuseRegionCount shadowReuses;NativeRequire ($reuse -in @(0,1)) 'Native reuse counter must name one actual shadow observation'
	if($reuse -eq 1){NativeRequire ($submitted.Count -eq 0 -and (Metric $Frame.shadow.actualDrawSubmissions actualDrawSubmissions) -eq 0) 'Reused native shadow issued submitted caster rows/draws'}
	elseif($Identity.stage -ceq 'P5'){VerifyNativeStaticShadowExecution $Frame $counters}
	else{
		NativeRequire (($eligible -join "`n") -ceq ($submitted -join "`n")) 'Native refresh does not submit whole exact eligible caster set'
		if($submitted.Count){NativeRequire ((Metric $Frame.shadow.actualDrawSubmissions actualDrawSubmissions) -gt 0) 'Nonempty native refresh lacks actual draw submissions'}
	}
	if($Frame.effectiveOptions.shadowReuse -eq 'AlwaysRefresh'){NativeRequire ($reuse -eq 0) 'Forced reference refreshed shadow claims reuse'}
	if($Identity.stage -ceq 'P4'){VerifyNativeShadowBatchExecution $Frame $counters}
	if($Identity.stage -ceq 'P5' -and $reuse -eq 1){VerifyNativeStaticShadowExecution $Frame $counters}
	return @{counters=$counters;depth=$depth.bytes;coverage=$coverage.bytes;image=$decodedImage;semanticRows=(NativeCasterRows $counters.eligibleCasterIdentityRows $true);width=$width;height=$height;operationId=$counters.operationId;occupiedPixels=[TerraRendererNativeProof]::Occupied($coverage.bytes)}
}
# Lifecycle proof is a separately hashed native receipt artifact. Producer
# successCriteria labels never authorize a gate; every receipt is correlated to
# the already verified operation and frozen fixture/run identity.
function NativeReceiptFields($Value,[string[]]$Fields,[string]$Name) {
	foreach($field in $Fields){if(!(Has $Value $field)){Unverified "Missing $Name receipt field $field"}}
}
function NativeReceiptBool($Value,[string]$Name,[bool]$Expected) {
	NativeRequire ($Value -is [bool] -and $Value -eq $Expected) "$Name contradicts actual native receipt"
}
function ReadNativeValidationReceipts($Document,$Identity,[string]$Directory,[string]$Operation) {
	$artifacts=@($Document.artifacts|Where-Object { $_.kind -eq 'Counters' -and $_.path -cmatch '\.receipts\.json$' })
	if($artifacts.Count -ne 1){Unverified 'Missing unique native validation receipts artifact'}
	$path=ArtifactPath $artifacts[0] $Directory
	NativeRequire ((Get-Item -LiteralPath $path).Length -le 16777216) 'Native receipt artifact exceeds bounded input'
	$proof=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
	NativeReceiptFields $proof @('schemaVersion','operationId','fixtureId','fixtureHash','stage','runId','runIndex','mode','seed','receipts') 'envelope'
	NativeRequire ($proof.schemaVersion -eq 1 -and $proof.receipts -is [array] -and $proof.receipts.Count -le 128) 'Native receipt envelope schema/capacity invalid'
	NativeRequire ($proof.operationId -ceq $Operation -and $Operation -cmatch '^[0-9a-f]{32}$' -and $Operation -cne ('0'*32)) 'Native receipt operation differs from actual completed frames'
	foreach($field in @('fixtureId','stage','runId','runIndex','mode','seed')){NativeRequire (JsonEqual $proof.$field $Identity.$field) "Native receipt $field mismatch"}
	NativeRequire ($proof.fixtureHash -ceq (Metric $Identity.fixtureHash fixtureHash)) 'Native receipt fixture hash mismatch'
	foreach($receipt in $proof.receipts){NativeReceiptFields $receipt @('kind') 'native';NativeRequire ($receipt.kind -is [string] -and $receipt.kind.Length -gt 0) 'Native receipt kind invalid'}
	return $proof
}
# Verify native reload decisions against subsequently captured caster owners.
# Producer booleans alone cannot prove that the GPU-facing owner changed.
function VerifyNativeResourcePublications($Envelope,$Steps,$Counters) {
	$actions=@{'mesh-revision'=@('Mesh','DeformableMesh');'page-revision'=@('Mesh','DeformableMesh');'material-revision'=@('Material');'texture-revision'=@('Texture');'cutout'=@('Material')}
	$semantic=[Collections.Generic.List[string]]::new();$seen=@{}
	foreach($step in @($Steps|Where-Object {$actions.ContainsKey($_.action)})) {
		$receipts=@($Envelope.receipts|Where-Object {$_.kind -ceq 'ResourcePublication' -and (Has $_ 'fixtureStepId') -and $_.fixtureStepId -ceq $step.id -and (Has $_ 'action') -and $_.action -ceq $step.action})
		if(!$receipts.Count){Unverified "Missing actual native resource publication for $($step.id)"}
		$requiredTypeFound=$false
		foreach($receipt in $receipts) {
			NativeReceiptFields $receipt @('kind','fixtureStepId','action','assetId','assetType','sourcePath','sourceSha256','baselineSourceSha256','publicationGeneration','previousPublicationGeneration','actualNativeOwnerAccepted') 'resource publication'
			NativeRequire ($receipt.assetId -cmatch '^[0-9a-f]{32}$' -and $receipt.assetId -cne ('0'*32)) 'Resource publication lacks full asset UUID'
			if($receipt.assetType -notin @('Mesh','DeformableMesh','Material','Texture')){Unverified 'Resource publication type lacks a supported native caster owner'}
			NativeRequire ($receipt.sourcePath -is [string] -and $receipt.sourcePath -cmatch '^Assets/[^:]+$' -and $receipt.sourcePath -notmatch '[\\\x00-\x1f]' -and @($receipt.sourcePath.Split('/')|Where-Object {$_ -in @('','.','..')}).Count -eq 0) 'Resource source path is not a contained portable Assets path'
			foreach($field in @('sourceSha256','baselineSourceSha256')){NativeRequire ($receipt.$field -cmatch '^[0-9a-f]{64}$') "Resource $field is not an actual SHA256"}
			NativeRequire ($receipt.sourceSha256 -cne $receipt.baselineSourceSha256) 'Changed resource source equals retained baseline bytes'
			$generation=NativeUnsigned $receipt.publicationGeneration resourcePublication $true;$prior=NativeUnsigned $receipt.previousPublicationGeneration previousResourcePublication $true
			NativeRequire ($generation -gt $prior) 'Native resource owner publication did not advance'
			NativeReceiptBool $receipt.actualNativeOwnerAccepted actualNativeOwnerAccepted $true
			$key=@($step.id,$receipt.assetType,$receipt.assetId) -join '|';NativeRequire (!$seen.ContainsKey($key)) 'Duplicate native resource owner receipt';$seen[$key]=$true
			$observed=$false
			foreach($counter in @($Counters|Where-Object {$_.fixtureStepId -ceq $step.id})) {foreach($row in @($counter.eligibleCasterIdentityRows)) {
				if($receipt.assetType -eq 'Texture') {
					foreach($texture in @($row.texturePublications|Where-Object {$_.textureId -ceq $receipt.assetId})) {NativeRequire ((NativeUnsigned $texture.publicationGeneration texturePublication $true) -eq $generation) 'Captured texture owner differs from native reload publication';$observed=$true}
				} else {
					$idField=if($receipt.assetType -eq 'Material'){'materialId'}else{'meshId'};$revisionField=if($receipt.assetType -eq 'Material'){'materialRevision'}else{'meshRevision'}
					if($row.$idField -ceq $receipt.assetId){NativeReceiptFields $row @($revisionField) 'captured resource owner';NativeRequire ((NativeUnsigned $row.$revisionField capturedResourcePublication $true) -eq $generation) 'Captured caster owner differs from native reload publication';$observed=$true
						if($step.action -eq 'page-revision' -and $receipt.assetType -in @('Mesh','DeformableMesh')) {
							NativeReceiptFields $row @('pageId','pageContentRevision','payloadContentRevision','residencyGeneration','payloadByteSize','meshletCount','payloadSource') 'captured native page'
							$null=NativeUnsigned $row.pageId nativePageId
							NativeRequire ((NativeUnsigned $row.pageContentRevision pagePublication $true) -eq $generation -and (NativeUnsigned $row.payloadContentRevision payloadPublication $true) -eq $generation) 'Captured page bytes differ from actual mesh source publication'
							$null=NativeUnsigned $row.residencyGeneration actualPageAllocation $true;$null=NativeUnsigned $row.payloadByteSize actualPageBytes $true;$null=NativeUnsigned $row.meshletCount actualPageMeshlets $true
							NativeRequire ((NativeUnsigned $row.payloadSource actualPageRoute) -eq 256) 'Page revision lacks actual shadow-local native page payload route'
						}
					}
				}
			}}
			if(!$observed){Unverified 'Native resource publication has no matching actual captured caster asset owner'}
			if($receipt.assetType -in $actions[$step.action]){$requiredTypeFound=$true}
			$semantic.Add((Canonical ([ordered]@{fixtureStepId=$step.id;action=$step.action;assetId=$receipt.assetId;assetType=$receipt.assetType;sourcePath=$receipt.sourcePath;sourceSha256=$receipt.sourceSha256;baselineSourceSha256=$receipt.baselineSourceSha256})))
		}
		if(!$requiredTypeFound){Unverified "Required native resource type missing for $($step.action)"}
	}
	$result=$semantic.ToArray();[Array]::Sort($result,[StringComparer]::Ordinal);return ,$result
}
function CompareNativeResourcePublications($CandidateEnvelope,$ReferenceEnvelope,$Steps,$CandidateCounters,$ReferenceCounters) {
	$c=VerifyNativeResourcePublications $CandidateEnvelope $Steps $CandidateCounters
	$r=VerifyNativeResourcePublications $ReferenceEnvelope $Steps $ReferenceCounters
	NativeRequire (($c -join "`n") -ceq ($r -join "`n")) 'Reference/candidate native resource source/asset publication mismatch'
}
# Native shadow stamps name the real shader collection, not the private mesh
# stage lease. Join both owners through the collection's copied stage bindings.
function VerifyNativeShaderPublications($Envelope,$Steps,$Counters) {
	$result=[Collections.Generic.List[string]]::new()
	foreach($step in @($Steps|Where-Object action -ceq shader-revision)) {
		$rows=@($Counters|Where-Object fixtureStepId -ceq $step.id|ForEach-Object {$_.eligibleCasterIdentityRows})
		$ids=@($rows|ForEach-Object shaderId|Sort-Object -Unique)
		if(!$ids.Count){Unverified 'Shader publication has no actual captured caster collection'}
		foreach($id in $ids) {
			$receipts=@($Envelope.receipts|Where-Object {$_.kind -ceq 'ShaderPublication' -and (Has $_ 'fixtureStepId') -and $_.fixtureStepId -ceq $step.id -and (Has $_ 'shaderId') -and $_.shaderId -ceq $id})
			if(!$receipts.Count){Unverified 'Actual captured shader collection lacks a native publication receipt'}
			NativeRequire ($receipts.Count -eq 1) 'Duplicate captured shader collection receipt';$receipt=$receipts[0]
			NativeReceiptFields $receipt @('action','shaderId','shaderStageId','shaderCollectionGeneration','previousShaderCollectionGeneration','shaderStagePublicationGeneration','previousObservedShaderStagePublicationGeneration','sourcePath','sourceSha256','actualNativeOwnerAccepted','stages') 'shader publication'
			NativeRequire ($receipt.action -ceq $step.action) 'Shader publication action differs from frozen step'
			foreach($field in @('shaderId','shaderStageId')){NativeRequire ($receipt.$field -cmatch '^[0-9a-f]{32}$' -and $receipt.$field -cne ('0'*32)) "Shader $field lacks full native UUID"}
			$generation=NativeUnsigned $receipt.shaderCollectionGeneration shaderCollection $true;$prior=NativeUnsigned $receipt.previousShaderCollectionGeneration previousShaderCollection $true
			$stageGeneration=NativeUnsigned $receipt.shaderStagePublicationGeneration shaderStagePublication $true;$stagePrior=NativeUnsigned $receipt.previousObservedShaderStagePublicationGeneration previousShaderStagePublication $true
			NativeRequire ($generation -gt $prior -and $stageGeneration -gt $stagePrior) 'Actual shader collection/stage owner did not advance'
			NativeReceiptBool $receipt.actualNativeOwnerAccepted shaderNativeOwnerAccepted $true
			NativeRequire ($receipt.sourcePath -ceq ('renderer-efficiency-shaders/'+$Envelope.operationId+'/Default.mesh.tspf') -and $receipt.sourceSha256 -cmatch '^[0-9a-f]{64}$') 'Shader source path/hash differs from owned copied TSPF'
			NativeRequire ($receipt.stages -is [array] -and $receipt.stages.Count -gt 0 -and $receipt.stages.Count -le 16) 'Shader collection actual stage bindings missing or oversized'
			$meshAccepted=$false;$stageIds=@{};$stageSemantic=[Collections.Generic.List[string]]::new()
			foreach($binding in $receipt.stages) {
				NativeReceiptFields $binding @('stage','shaderId','publicationGeneration','acceptedOverrideOwner') 'shader stage binding'
				$stage=NativeUnsigned $binding.stage actualShaderStage;NativeRequire ($stage -le 13 -and !$stageIds.ContainsKey($stage)) 'Duplicate/unknown native shader stage';$stageIds[$stage]=$true
				NativeRequire ($binding.shaderId -cmatch '^[0-9a-f]{32}$' -and $binding.shaderId -cne ('0'*32) -and $binding.acceptedOverrideOwner -is [bool]) 'Shader stage binding identity/acceptance invalid'
				$pub=NativeUnsigned $binding.publicationGeneration actualShaderStagePublication $true
				# ShaderStage::Mesh is 6 in Engine/File Formats/TerraEngineShaderFile.h.
				if($stage -eq 6){NativeRequire ($binding.shaderId -ceq $receipt.shaderStageId -and $pub -eq $stageGeneration) 'Accepted mesh stage does not match exact native lease';NativeReceiptBool $binding.acceptedOverrideOwner acceptedMeshShaderOwner $true;$meshAccepted=$true}
				$stageSemantic.Add((Canonical ([ordered]@{stage=$stage;shaderId=$binding.shaderId;acceptedOverrideOwner=$binding.acceptedOverrideOwner})))
			}
			NativeRequire $meshAccepted 'Shader collection lacks accepted actual mesh stage'
			foreach($row in @($rows|Where-Object shaderId -ceq $id)){NativeReceiptFields $row @('shaderRevision') 'captured shader owner';NativeRequire ((NativeUnsigned $row.shaderRevision actualCapturedShaderCollection $true) -eq $generation) 'Captured shader collection differs from native accepted publication'}
			$bindings=$stageSemantic.ToArray();[Array]::Sort($bindings,[StringComparer]::Ordinal)
			$result.Add((Canonical ([ordered]@{fixtureStepId=$step.id;action=$step.action;shaderId=$id;shaderStageId=$receipt.shaderStageId;sourceName='Default.mesh.tspf';sourceSha256=$receipt.sourceSha256;stages=$bindings})))
		}
	}
	$output=$result.ToArray();[Array]::Sort($output,[StringComparer]::Ordinal);return ,$output
}
function CompareNativeShaderPublications($CandidateEnvelope,$ReferenceEnvelope,$Steps,$CandidateCounters,$ReferenceCounters) {
	$c=VerifyNativeShaderPublications $CandidateEnvelope $Steps $CandidateCounters;$r=VerifyNativeShaderPublications $ReferenceEnvelope $Steps $ReferenceCounters
	NativeRequire (($c -join "`n") -ceq ($r -join "`n")) 'Reference/candidate native shader source/collection publication mismatch'
}
function NativeReceiptStep($Receipts,[string]$Kind,[string]$Step) {
	$rows=@($Receipts.receipts|Where-Object { $_.kind -ceq $Kind -and (Has $_ 'fixtureStepId') -and $_.fixtureStepId -ceq $Step })
	if(!$rows.Count){Unverified "Missing native $Kind receipt for $Step"};return ,$rows
}
function VerifyNativeFixtureAction($Row,[string]$Operation,[string]$Step,[string]$Injection) {
	NativeReceiptFields $Row @('injection','correlation','fixtureStepId','frameSerial','commandSerial','submitSerial','completionSerial','oldAllocationGeneration','newAllocationGeneration','requiredFaceMask','recordedFaceMask','recorded','submitted','completed','priorOwnerRejected','reason') 'fixture action'
	NativeReceiptFields $Row.correlation @('operationId','fixtureStepId','stepFrame','mutationGeneration','settingsGeneration','rendererSessionGeneration') 'action correlation'
	NativeRequire ($Row.injection -ceq $Injection -and $Row.fixtureStepId -ceq $Step -and $Row.correlation.fixtureStepId -ceq $Step -and $Row.correlation.operationId -ceq $Operation) 'Native action owner/step/injection mismatch'
	$null=NativeUnsigned $Row.correlation.stepFrame actionStepFrame
	foreach($field in @('mutationGeneration','settingsGeneration','rendererSessionGeneration')){$null=NativeUnsigned $Row.correlation.$field "action $field" $true}
	$null=NativeUnsigned $Row.frameSerial actionFrame $true;$null=NativeUnsigned $Row.commandSerial actionCommand $true
	NativeReceiptBool $Row.recorded actionRecorded $true
	$submit=NativeUnsigned $Row.submitSerial actionSubmit;$complete=NativeUnsigned $Row.completionSerial actionCompletion
	if($Injection -eq 'RecordedSubmitAbort') {
		NativeReceiptBool $Row.submitted actionSubmitted $false;NativeReceiptBool $Row.completed actionCompleted $false
		NativeRequire ($submit -eq 0 -and $complete -eq 0) 'Recorded abort claims an actual submit/completion'
	} else {
		NativeReceiptBool $Row.submitted actionSubmitted $true;NativeReceiptBool $Row.completed actionCompleted $true
		NativeRequire ($submit -gt 0 -and $complete -ge $submit) 'Native action lacks actual submission/completion'
		NativeReceiptBool $Row.priorOwnerRejected priorOwnerRejected $true
		$old=NativeUnsigned $Row.oldAllocationGeneration oldAllocation $true;$new=NativeUnsigned $Row.newAllocationGeneration newAllocation $true
		if($Injection -eq 'IncompletePointFaces') {
			NativeRequire ((NativeUnsigned $Row.requiredFaceMask requiredFaces) -eq 63 -and (NativeUnsigned $Row.recordedFaceMask recordedFaces) -eq 31 -and $old -eq $new) 'Incomplete point action lacks exact five-of-six native face proof'
		} elseif($Injection -eq 'ShadowResourceRecreation') {NativeRequire ($new -gt $old) 'Native shadow allocation did not advance'}
	}
}
function VerifyNativeTransactionRollback($Row,[string]$Step) {
	NativeReceiptFields $Row @('fixtureStepId','transactionId','status','appliedWriteCount','rejectedWriteCount','spatialPublicationGeneration','before','after','message') 'transaction rollback'
	# This frozen recipe is DeleteObject followed by SetWorldTransform. Require
	# its real Scene::ToString(Conflicted) result and both rejected operations;
	# a generic rejection or a partial count cannot prove this specific conflict.
	NativeRequire ($Row.fixtureStepId -ceq $Step -and $Row.transactionId -is [string] -and $Row.transactionId.Length -gt 0 -and $Row.status -ceq 'conflicted') 'Native frozen conflict receipt lacks actual conflicted transaction'
	NativeRequire ((NativeUnsigned $Row.appliedWriteCount appliedWrites) -eq 0 -and (NativeUnsigned $Row.rejectedWriteCount rejectedWrites $true) -eq 2) 'Conflicted two-write transaction lacks whole-batch rollback'
	# Rejection performs no spatial publication. Preserve the queue's actual zero
	# receipt; unchanged nonzero target revisions below prove atomic rollback.
	$null=NativeUnsigned $Row.spatialPublicationGeneration rollbackPublication
	foreach($state in @($Row.before,$Row.after)) {
		NativeReceiptFields $state @('objectId','lifetimeGeneration','metadataGeneration','componentGeneration','hierarchyRevision','transformRevision','coordinateFrameRevision','spatialRevision','parentObjectId','active','canonicalWorldPose') 'rollback state'
		NativeRequire ($state.objectId -cmatch '^[0-9a-f]{32}$' -and $state.objectId -cne ('0'*32)) 'Rollback target identity invalid'
		foreach($field in @('lifetimeGeneration','metadataGeneration','componentGeneration','hierarchyRevision','transformRevision','coordinateFrameRevision','spatialRevision')){$null=NativeUnsigned $state.$field "rollback $field" $true}
		NativeRequire ($state.parentObjectId -cmatch '^[0-9a-f]{32}$' -and $state.active -is [bool]) 'Rollback hierarchy/active proof invalid'
		NativeReceiptFields $state.canonicalWorldPose @('position','rotation','scale') 'rollback pose'
		NativeOrigin $state.canonicalWorldPose.position 'rollback position';NativeFiniteArray $state.canonicalWorldPose.rotation 4 'rollback rotation';NativeFiniteArray $state.canonicalWorldPose.scale 3 'rollback scale'
	}
	NativeRequire ((Canonical $Row.before) -ceq (Canonical $Row.after)) 'Rejected conflict changed exact protected state'
}
function VerifyNativeDeviceRecreation($Row,$Envelope,[string]$Step) {
	NativeReceiptFields $Row @('fixtureStepId','status','idleProven','rebound','oldRendererSessionGeneration','newRendererSessionGeneration','oldBackendResourceGeneration','newBackendResourceGeneration','priorOptionsEpoch','newOptionsEpoch','visibleSettingsGeneration','request','reason') 'device recreation'
	NativeReceiptFields $Row.request @('operationId','fixtureStepId','fixtureId','fixtureHash','projectGeneration','expectedRendererSessionGeneration','expectedBackendResourceGeneration') 'device request'
	NativeRequire ($Row.fixtureStepId -ceq $Step -and $Row.status -ceq 'Completed' -and $Row.request.operationId -ceq $Envelope.operationId -and $Row.request.fixtureStepId -ceq $Step -and $Row.request.fixtureId -ceq $Envelope.fixtureId -and $Row.request.fixtureHash -ceq $Envelope.fixtureHash) 'Native device receipt owner/status mismatch'
	NativeReceiptBool $Row.idleProven deviceIdle $true;NativeReceiptBool $Row.rebound deviceUiRebound $true
	$old=NativeUnsigned $Row.oldRendererSessionGeneration oldSession $true;$new=NativeUnsigned $Row.newRendererSessionGeneration newSession $true
	$oldBackend=NativeUnsigned $Row.oldBackendResourceGeneration oldBackend $true;$newBackend=NativeUnsigned $Row.newBackendResourceGeneration newBackend $true
	NativeRequire ($new -gt $old -and $newBackend -gt $oldBackend -and (NativeUnsigned $Row.request.expectedRendererSessionGeneration expectedSession $true) -eq $old -and (NativeUnsigned $Row.request.expectedBackendResourceGeneration expectedBackend $true) -eq $oldBackend) 'Native device generations did not advance from requested actual lifetime'
	NativeRequire ((NativeUnsigned $Row.newOptionsEpoch newOptionsEpoch $true) -gt (NativeUnsigned $Row.priorOptionsEpoch priorOptionsEpoch $true)) 'Native recovery did not revoke old options lifetime'
	$null=NativeUnsigned $Row.request.projectGeneration deviceProjectGeneration $true;$null=NativeUnsigned $Row.visibleSettingsGeneration visibleSettings $true
}
function VerifyNativeRecoveryReceipts($Envelope,$Steps) {
	$actions=@{'recorded-submit-abort'='RecordedSubmitAbort';'incomplete-point-faces'='IncompletePointFaces';'resource-recreation'='ShadowResourceRecreation'}
	foreach($action in $actions.Keys){$step=@($Steps|Where-Object action -ceq $action);if($step.Count -ne 1){Unverified "Frozen recovery step missing $action"};foreach($row in (NativeReceiptStep $Envelope FixtureAction $step[0].id)){VerifyNativeFixtureAction $row $Envelope.operationId $step[0].id $actions[$action]}}
	$device=@($Steps|Where-Object action -ceq 'device-recovery');if($device.Count -ne 1){Unverified 'Frozen device recovery step missing'};foreach($row in (NativeReceiptStep $Envelope DeviceRecreation $device[0].id)){VerifyNativeDeviceRecreation $row $Envelope $device[0].id}
	$conflict=@($Steps|Where-Object action -ceq 'transaction-conflict-rollback');if($conflict.Count -ne 1){Unverified 'Frozen transaction conflict step missing'};foreach($row in (NativeReceiptStep $Envelope TransactionRollback $conflict[0].id)){VerifyNativeTransactionRollback $row $conflict[0].id}
}
function VerifyNativePreparation($Counters,[string]$Preparation) {
	NativeReceiptFields $Counters @('nativePreparation') 'preparation';$proof=$Counters.nativePreparation
	NativeReceiptFields $proof @('workerTasks','inlineTasks','settled') 'preparation'
	NativeReceiptBool $proof.settled preparationSettled $true
	$workers=NativeUnsigned $proof.workerTasks workerTasks;$inline=NativeUnsigned $proof.inlineTasks inlineTasks
	if($Preparation -eq 'Serial'){NativeRequire ($workers -eq 0 -and $inline -gt 0) 'Serial preparation lacks actual inline task execution'}
	elseif($Preparation -eq 'Concurrent'){NativeRequire ($workers -gt 0 -and $inline -eq 0) 'Concurrent preparation lacks actual worker task execution'}
	else{Unverified 'Requested native preparation mode unavailable'}
}
function VerifyNativeSchedulerReceipt($Envelope,[string]$Preparation) {
	$rows=@($Envelope.receipts|Where-Object kind -ceq Scheduler);if(!$rows.Count){Unverified 'Actual published scheduler snapshot missing'}
	foreach($row in $rows){
		NativeReceiptFields $row @('frameId','valid','completed','executionMode','parallelExecutionUsed','preparationOption','systems','phases','flushPoints','mainThreadSystems','workerCategories','protectedCategories','workloads') 'scheduler'
		$null=NativeUnsigned $row.frameId schedulerFrame $true;NativeReceiptBool $row.valid schedulerValid $true;NativeReceiptBool $row.completed schedulerCompleted $true
		NativeRequire ($row.preparationOption -ceq $Preparation -and $row.executionMode -in @('parallel','compatibility') -and $row.parallelExecutionUsed -is [bool]) 'Scheduler mode differs from requested native preparation'
		foreach($field in @('systems','phases','flushPoints','mainThreadSystems','workerCategories','protectedCategories','workloads')){NativeRequire ($row.$field -is [array] -and $row.$field.Count -gt 0) "Scheduler $field declaration missing"}
		NativeRequire ($row.systems -contains 'RenderSystem' -and $row.systems -contains 'EditorSystem' -and $row.phases -contains 'Render' -and $row.phases -contains 'PostFrame' -and @($row.flushPoints|Where-Object {$_ -match 'Frame End'}).Count -gt 0) 'Scheduler protected lifecycle/flush declarations incomplete'
		$rendererReads=@{};$rendererWrites=@{};$rendererWorker=$false;$rendererProtected=$false;$editorProtected=$false
		foreach($work in $row.workloads){
			NativeReceiptFields $work @('name','ownerSystem','phase','reads','writes','dependencies','workerEligible','mainThreadRequired','phaseKind','boundaryClassification','mutationPolicy','protectedCommit','protectedReason','safeSubwork') 'workload'
			foreach($field in @('reads','writes','dependencies')){NativeRequire ($work.$field -is [array]) "Workload $field is not a copied declaration"}
			NativeRequire ($work.workerEligible -is [bool] -and $work.mainThreadRequired -is [bool]) 'Workload worker/protected flags malformed'
			if($work.ownerSystem -eq 'RenderSystem'){
				foreach($resource in $work.reads){$rendererReads[$resource]=$true};foreach($resource in $work.writes){$rendererWrites[$resource]=$true}
				if($work.phaseKind -eq 'worker-prepare' -and $work.workerEligible -and !$work.mainThreadRequired -and $work.safeSubwork -is [string] -and $work.safeSubwork.Length){
					NativeRequire (@($work.writes|Where-Object {$_ -in @('ObjectManager','SceneGraph','RendererEfficiencyOptions','RendererEfficiencyEvidence','RendererOutput','PresentationState')}).Count -eq 0 -and $work.reads -notcontains 'RendererEfficiencyOptionsQueue') 'Worker workload crosses actual protected mutation/capture boundary'
					$rendererWorker=$true
				}
				if($work.phaseKind -eq 'protected-commit' -and $work.mainThreadRequired -and $work.boundaryClassification -eq 'protected' -and $work.protectedCommit -is [string] -and $work.protectedCommit.Length -and $work.protectedReason -is [string] -and $work.protectedReason.Length){$rendererProtected=$true}
			}
			if($work.ownerSystem -eq 'EditorSystem' -and $work.phase -eq 'PostFrame' -and $work.phaseKind -eq 'protected-commit' -and $work.mainThreadRequired -and $work.boundaryClassification -eq 'protected' -and $work.protectedReason -is [string] -and $work.protectedReason.Length){$editorProtected=$true}
		}
		NativeRequire ($rendererWorker -and $rendererProtected -and $editorProtected -and $rendererReads.ContainsKey('RendererEfficiencyOptionsQueue') -and $rendererWrites.ContainsKey('RendererEfficiencyOptions') -and $rendererWrites.ContainsKey('RendererEfficiencyEvidence')) 'Actual renderer worker/read/write/protected boundary contract incomplete'
	}
}
function VerifyNativeShadowDiagnostics($Frame,$Counters) {
	NativeReceiptFields $Counters @('nativeShadowDiagnostics') 'shadow diagnostics';$native=$Counters.nativeShadowDiagnostics
	NativeReceiptFields $native @('refreshReason','fallbackReason','contentOwner') 'shadow diagnostics'
	NativeRequire ($native.refreshReason -is [string] -and $native.refreshReason.Length -gt 0 -and $native.fallbackReason -is [string] -and $native.contentOwner -is [string]) 'Native shadow diagnostic text unavailable'
	NativeRequire ((NativeUnsigned $native.contentOwner diagnosticOwner $true) -eq (NativeUnsigned $Counters.contentToken contentToken $true)) 'Native diagnostic owner differs from submitted content proof'
	$reuse=Metric $Frame.shadow.reuseRegionCount shadowReuses
	if($reuse -eq 1){NativeRequire ($native.refreshReason -ceq 'ExactSubmittedContent') 'Reused native shadow lacks exact-owner diagnostic'}
	elseif($Frame.effectiveOptions.shadowReuse -eq 'AlwaysRefresh'){NativeRequire ($native.refreshReason -ceq 'AlwaysRefresh') 'Forced refresh diagnostic contradicts actual settings'}
	else{NativeRequire ($native.refreshReason -in @('ContentOrOwnerChanged','MissingSemanticProof')) 'Native refreshed shadow reason invalid'}
	foreach($field in @('refreshReason','contentOwner')){NativeRequire ((Metric $Frame.shadow.$field $field) -ceq $native.$field) "Native shadow $field differs from frame diagnostics"}
	if($native.fallbackReason.Length){NativeRequire ((Metric $Frame.shadow.fallbackReason fallbackReason) -ceq $native.fallbackReason) 'Native fallback diagnostic differs from frame'}
	else{NativeRequire (($Frame.shadow.fallbackReason.status -eq 'Unavailable' -and $null -eq $Frame.shadow.fallbackReason.value) -or ($Frame.shadow.fallbackReason.status -eq 'Valid' -and $Frame.shadow.fallbackReason.value -ceq '')) 'Frame reports fallback absent from native diagnostics'}
}
function ReadNativeRunnerLifecycle($Document,$Identity,[string]$Directory,[string]$Operation,[string]$Preparation) {
	$artifacts=@($Document.artifacts|Where-Object { $_.kind -eq 'Counters' -and $_.path -cmatch '\.lifecycle\.json$' })
	if($artifacts.Count -ne 1){Unverified 'Missing unique owned editor lifecycle artifact'}
	$path=ArtifactPath $artifacts[0] $Directory;NativeRequire ((Get-Item -LiteralPath $path).Length -le 1048576) 'Runner lifecycle proof exceeds bounded input'
	$proof=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
	NativeReceiptFields $proof @('kind','schemaVersion','fixtureId','fixtureHash','runId','runIndex','preparation','operationId','optionsRestored','ownedEditorProcessId','normalClose','processExitCode','watchdogFailed','originalOptions','restoredOptions','restoredSettingsGeneration','restoredRendererSessionGeneration') 'runner lifecycle'
	NativeRequire ($proof.kind -ceq 'RendererEfficiencyRunnerLifecycle' -and $proof.schemaVersion -eq 1 -and $proof.operationId -ceq $Operation -and $proof.preparation -ceq $Preparation) 'Runner lifecycle owner/schema/preparation mismatch'
	foreach($field in @('fixtureId','runId','runIndex')){NativeRequire (JsonEqual $proof.$field $Identity.$field) "Runner lifecycle $field mismatch"}
	NativeRequire ($proof.fixtureHash -ceq (Metric $Identity.fixtureHash fixtureHash)) 'Runner lifecycle fixture hash mismatch'
	NativeReceiptBool $proof.optionsRestored optionsRestored $true;NativeReceiptBool $proof.normalClose editorNormalClose $true;NativeReceiptBool $proof.watchdogFailed editorWatchdogFailed $false
	$null=NativeUnsigned $proof.ownedEditorProcessId ownedEditorProcess $true;NativeRequire ($proof.processExitCode -is [ValueType] -and $proof.processExitCode -isnot [bool] -and $proof.processExitCode -eq 0) 'Owned editor did not exit normally'
	$null=NativeUnsigned $proof.restoredSettingsGeneration restoredSettings $true;$null=NativeUnsigned $proof.restoredRendererSessionGeneration restoredSession $true
	foreach($options in @($proof.originalOptions,$proof.restoredOptions)){
		NativeReceiptFields $options @('shadowReuse','lightingUploads','lightingDepth','shadowSubmission','staticShadowCacheBudgetBytes','validationPreparation') 'restored options'
		NativeRequire ($options.shadowReuse -in @('Enabled','AlwaysRefresh') -and $options.lightingUploads -in @('Dirty','Full') -and $options.lightingDepth -in @('Cooperative','CompleteScan') -and $options.shadowSubmission -in @('Grouped','Singleton') -and $options.validationPreparation -in @('Serial','Concurrent') -and (NativeUnsigned $options.staticShadowCacheBudgetBytes restoredBudget) -le 1073741824) 'Restored renderer options malformed'
	}
	NativeRequire ((Canonical $proof.originalOptions) -ceq (Canonical $proof.restoredOptions)) 'Protected renderer options were not restored exactly'
	return $proof
}
function VerifyNativeBundle($Counters) {
	$faces=@{}
	foreach($counter in $Counters){$key=$counter.qualifiedShadowViewKey
		if($key.resourceKind -ne 1){NativeRequire ($key.faceIndex -eq 0) 'Noncube shadow observation names a cube face';continue}
		NativeRequire ($key.lightProjectionStamp.cubeProjectionEvidenceComplete -eq $true -and $key.lightProjectionStamp.cubeViewProjections.Count -eq 6) 'Point cube lacks all native projection stamps'
		foreach($matrix in $key.lightProjectionStamp.cubeViewProjections){NativeFiniteArray $matrix 16 'native cube projection'}
		$id=@($counter.operationId,$counter.measurementPhase,$counter.fixtureStepId,$counter.stepFrame,$counter.readbackProvenance.frameSerial,$counter.readbackProvenance.submitSerial,$key.viewType,$key.viewId,$key.lightId) -join '|'
		if(!$faces.ContainsKey($id)){$faces[$id]=0};$bit=1 -shl [int]$key.faceIndex
		NativeRequire (($faces[$id] -band $bit) -eq 0) 'Duplicate point cube face in completed bundle';$faces[$id]=$faces[$id] -bor $bit
	}
	foreach($mask in $faces.Values){NativeRequire ($mask -eq 63) 'Incomplete point cube face bundle cannot prove native correctness'}
}
function VerifyNativeLifetimeReplacement($Counters,$Steps,[string]$FixtureId,$Cameras) {
	# This is a within-run invariant for the frozen P1 recipe, not a comparison
	# of independently allocated lifetime numbers across Reference/Candidate.
	if($FixtureId -cne 'shadow-invalidation' -or !@($Steps|Where-Object action -ceq 'lifetime-replacement').Count) { return }
	$timeline=@(@('shadow-invalidation-14','light-assignment'),@('shadow-invalidation-15','membership-removal'),@('shadow-invalidation-16','lifetime-replacement'))
	# Expected views come from the supplied frozen manifest, never the observed subset.
	$expectedViews=@{}
	foreach($camera in @($Cameras)) {
		NativeRequire ($camera.viewType -cin @('Scene','Game') -and (NativeUnsigned $camera.viewId 'lifetime expected view' $true) -le [uint32]::MaxValue) 'Invalid expected lifetime view'
		$view=@($camera.viewType,$camera.viewId) -join '|'
		NativeRequire (!$expectedViews.ContainsKey($view)) 'Duplicate expected lifetime view';$expectedViews[$view]=$true
	}
	NativeRequire ($expectedViews.Count -gt 0) 'Expected lifetime views are missing'
	$target='65000000000000000000000000000010';$oldLifetime=$null;$newLifetime=$null;$regions=@{};$lastFrame=[uint64]0
	foreach($entry in $timeline) {
		$step=@($Steps|Where-Object {$_.id -ceq $entry[0] -and $_.action -ceq $entry[1]})
		if($step.Count -ne 1) { Unverified 'Missing frozen lifetime replacement action timeline' }
		$first=@($Counters|Where-Object {$_.fixtureStepId -ceq $entry[0] -and $_.measurementPhase -ceq 'Warmup' -and [string]$_.stepFrame -ceq '0'})
		if(!$first.Count) { Unverified ('Missing first native lifetime cohort '+$entry[0]) }
		VerifyNativeBundle $first
		$currentRegions=@{};$viewCommands=@{};$cohort=$first[0].readbackProvenance
		$frame=NativeUnsigned $cohort.frameSerial 'lifetime cohort frame' $true
		NativeRequire ($frame -gt $lastFrame) 'Lifetime action cohorts are not chronological';$lastFrame=$frame
		foreach($counter in $first) {
			$key=$counter.qualifiedShadowViewKey;$p=$counter.readbackProvenance
			# The native generation token is common only within this completed cohort;
			# mutations/settings may legitimately change between separate action steps.
			foreach($field in @('frameSerial','submitSerial','completionSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration','fixtureControlGeneration','optionsSessionGeneration')) {
				$actual=NativeUnsigned $p.$field ('lifetime '+$field) $true
				NativeRequire ($actual -eq (NativeUnsigned $cohort.$field ('lifetime cohort '+$field) $true)) 'Lifetime first cohort mixes native completions or generations'
			}
			$null=NativeUnsigned $p.commandSerial 'lifetime qualified command' $true
			NativeRequire ((Canonical $counter.effectiveOptions) -ceq (Canonical $first[0].effectiveOptions)) 'Lifetime first cohort mixes effective options'
			$view=@($key.viewType,$key.viewId) -join '|'
			NativeRequire ($expectedViews.ContainsKey($view)) 'Lifetime cohort names an unexpected manifest view'
			if($viewCommands.ContainsKey($view)) { NativeRequire ($viewCommands[$view] -ceq $p.commandSerial) 'Lifetime first cohort mixes qualified-view commands' } else { $viewCommands[$view]=$p.commandSerial }
			# Step14 really reassigns point02 to point40. Normalize only that region
			# join label; retain and validate the original native light identity.
			$light=$key.lightId
			$point=if($entry[1] -ceq 'light-assignment'){'40'}else{'02'}
			NativeRequire ($light -cin @(('650000000000000000000000000000'+$point),'65000000000000000000000000000041','65000000000000000000000000000042','65000000000000000000000000000043')) 'Lifetime action light assignment differs from frozen recipe'
			if($entry[1] -ceq 'light-assignment' -and $light -ceq '65000000000000000000000000000040') { $light='65000000000000000000000000000002' }
			$region=@($view,$light,$key.faceIndex) -join '|'
			NativeRequire (!$currentRegions.ContainsKey($region)) 'Duplicate lifetime action region';$currentRegions[$region]=$true
			$eligible=@($counter.eligibleCasterIdentityRows|Where-Object objectId -ceq $target)
			$submitted=@($counter.submittedCasterIdentityRows|Where-Object objectId -ceq $target)
			if($entry[1] -ceq 'membership-removal') {
				NativeRequire ($eligible.Count -eq 0 -and $submitted.Count -eq 0) 'Removed caster remains in native membership';continue
			}
			NativeRequire ($eligible.Count -eq 1) 'Lifetime target must occur exactly once in every eligible region'
			$lifetime=NativeUnsigned $eligible[0].lifetimeGeneration 'lifetime target generation' $true
			if($entry[1] -ceq 'light-assignment') {
				if($null -eq $oldLifetime) { $oldLifetime=$lifetime }
				NativeRequire ($lifetime -eq $oldLifetime) 'Prior caster lifetime differs between regions';continue
			}
			if($null -eq $newLifetime) { $newLifetime=$lifetime }
			NativeRequire ($lifetime -eq $newLifetime -and $lifetime -ne $oldLifetime) 'Replacement reused or mixed the prior object lifetime'
			NativeRequire ($submitted.Count -eq 1 -and (NativeUnsigned $submitted[0].lifetimeGeneration 'submitted replacement lifetime' $true) -eq $lifetime) 'First replacement did not submit its exact new lifetime'
			# A genuine new caster invalidates every applicable region. Existing raw
			# proof still checks complete rows/hashes/pixels; no producer claim suffices.
			NativeRequire ((Metric $counter.nativeCounters.shadowReuses 'replacement reuse') -eq 0 -and (Metric $counter.nativeCounters.actualDrawSubmissions 'replacement draws') -gt 0) 'First replacement reused old shadow content'
			NativeRequire ((Metric $counter.nativeCounters.submittedCasterSegments 'replacement submitted') -eq (Metric $counter.nativeCounters.eligibleCasterSegments 'replacement eligible')) 'First replacement omitted eligible casters'
			NativeRequire (((NativeCasterRows $counter.eligibleCasterIdentityRows) -join "`n") -ceq ((NativeCasterRows $counter.submittedCasterIdentityRows) -join "`n")) 'Replacement submitted membership differs from eligible input'
		}
		# Each declared view has point cube6 and the three ordinary regions. Do not
		# let a missing region conceal the target's old or new lifetime.
		foreach($view in $expectedViews.Keys) {
			foreach($face in 0..5) { NativeRequire ($currentRegions.ContainsKey($view+'|65000000000000000000000000000002|'+$face)) 'Lifetime point cube region missing' }
			foreach($light in @('41','42','43')) { NativeRequire ($currentRegions.ContainsKey($view+'|650000000000000000000000000000'+$light+'|0')) 'Lifetime ordinary shadow region missing' }
		}
		NativeRequire ($viewCommands.Count -eq $expectedViews.Count -and $currentRegions.Count -eq $expectedViews.Count*9) 'Lifetime cohort has unexpected native regions'
		if($regions.Count) { NativeRequire (($regions.Count -eq $currentRegions.Count) -and @($regions.Keys|Where-Object {!$currentRegions.ContainsKey($_)}).Count -eq 0) 'Lifetime action qualified regions differ' }
		$regions=$currentRegions
	}
}
function CompareNativeProof($Candidate,$Reference,$Resolution) {
	NativeRequire ($Candidate.width -eq $Reference.width -and $Candidate.height -eq $Reference.height) 'Native depth extent differs across matched observation'
	NativeRequire (($Candidate.semanticRows -join "`n") -ceq ($Reference.semanticRows -join "`n")) 'Exact semantic eligible caster identity mismatch'
	try {
		$coverage=[TerraRendererNativeProof]::CoverageDelta($Candidate.coverage,$Reference.coverage)
		$depth=[TerraRendererNativeProof]::DepthDelta($Candidate.depth,$Reference.depth)
		NativeRequire ($Candidate.image -is [TerraRendererNativeProof+DecodedImage] -and $Reference.image -is [TerraRendererNativeProof+DecodedImage]) 'Validated decoded native image proof missing'
		$image=[TerraRendererNativeProof]::DecodedImageDelta($Candidate.image,$Reference.image,[int]$Resolution.width,[int]$Resolution.height)
	}catch{throw ('NATIVE_FAILED: '+$_.Exception.Message)}
	NativeRequire ($coverage -eq 0 -and $depth -le 0.000001 -and $image -le (2.0/255.0)) 'Independent native coverage/depth/image correctness tolerance exceeded'
	return @{coverageMismatchCount=$coverage;casterIdentityMismatchCount=0;maxDepthAbsoluteError=$depth;maxImageNormalizedChannelError=$image}
}
function VerifyNativeStepCoverage([long]$OccupiedPixels,[long]$EligibleRows) {
	NativeRequire ($OccupiedPixels -gt 0 -and $EligibleRows -gt 0) 'Empty native fixture output cannot prove correctness'
}
function CompareNativeObservationProof($Candidate,$Reference,$Resolution,$Phase) {
	if((Has $Candidate.counters 'evidenceKind') -and $Candidate.counters.evidenceKind -ceq 'LightingDepth'){NativeRequire ($Reference.counters.evidenceKind -ceq 'LightingDepth') 'Paired depth kind differs';return CompareNativeDepthProof $Candidate $Reference}
	if((Has $Candidate.counters 'evidenceKind') -and $Candidate.counters.evidenceKind -ceq 'Lighting'){NativeRequire ((Has $Reference.counters 'evidenceKind') -and $Reference.counters.evidenceKind -ceq 'Lighting') 'Paired evidence kinds differ';return CompareNativeLightingProof $Candidate $Reference $Resolution}
	# Warmup is completed native output too. Only timing aggregation excludes it;
	# pixel and semantic correctness use exactly the measured-frame guard.
	NativeRequire ($Phase -in @('Warmup','Measured','Transition')) 'Unknown native observation phase'
	$a=$Candidate.counters.qualifiedShadowViewKey;$b=$Reference.counters.qualifiedShadowViewKey
	foreach($field in @('viewType','viewId','lightId','faceIndex','resourceKind','regionId','extent','depthFormat','referenceOrigin','lightProjectionStamp')){NativeRequire (JsonEqual $a.$field $b.$field) "Paired native qualified $field mismatch"}
	return CompareNativeProof $Candidate $Reference $Resolution
}
function VerifyNativeObservationPhase($Frame) {
	# Frozen indices are absolute: warmup precedes the 300 measured samples.
	# A row cannot move between phases to conceal a mismatched early frame.
	$index=NativeUnsigned $Frame.stepFrame stepFrame
	NativeRequire (($Frame.measurementPhase -ceq 'Warmup' -and $index -lt 60) -or ($Frame.measurementPhase -ceq 'Measured' -and $index -ge 60 -and $index -lt 360)) 'Native observation lies outside frozen phase window'
}
function VerifyNativeSelectionTransition($Proof,$Frame,$Document,$Identity,$Directory,$Steps,$Action) {
	# The frozen controls target the authored grid object. Compare its actual
	# native range with the first stable cohort, rather than inferring a change
	# from a receipt label or from import-generation bookkeeping.
	$stable=@($Steps|Where-Object action -eq stable|Select-Object -First 1)
	if($stable.Count -ne 1){Unverified 'Selection transition lacks sealed stable baseline'}
	$baseline=@($Document.frames|Where-Object {$_.fixtureStepId -ceq $stable[0].id -and $_.measurementPhase -ceq 'Warmup' -and $_.stepFrame -eq 0 -and $_.viewType -ceq $Frame.viewType -and $_.viewId -eq $Frame.viewId -and (Metric $_.lightId lightId) -ceq (Metric $Frame.lightId lightId) -and (Metric $_.faceIndex faceIndex) -eq (Metric $Frame.faceIndex faceIndex)})
	if($baseline.Count -ne 1){Unverified 'Selection transition lacks exact qualified baseline region'}
	$prior=ReadNativeProof $baseline[0] $Identity $Directory
	NativeRequire ($prior.operationId -ceq $Proof.operationId) 'Selection baseline belongs to another operation'
	$target='650'+('0'*27)+'11'
	$before=@($prior.counters.eligibleCasterIdentityRows|Where-Object objectId -CEQ $target)
	$after=@($Proof.counters.eligibleCasterIdentityRows|Where-Object objectId -CEQ $target)
	NativeRequire ($before.Count -gt 0 -and $before.Count -eq $after.Count) 'Controlled target missing from native selection proof'
	foreach($row in $after) {
		# Segment IDs encode the selected range and legitimately change with it.
		# Match the actual object/resource owner, retaining range identity in each
		# native eligible/submitted proof rather than equating two different cuts.
		$old=@($before|Where-Object {$_.meshId -ceq $row.meshId -and $_.assetId -ceq $row.assetId -and $_.lifetimeGeneration -ceq $row.lifetimeGeneration});NativeRequire ($old.Count -eq 1) 'Controlled selection owner identity changed'
		foreach($field in @('meshId','assetId','lifetimeGeneration')){NativeRequire (JsonEqual $old[0].$field $row.$field) "Controlled selection $field changed"}
		$null=NativeUnsigned $row.lifetimeGeneration lifetimeGeneration $true
		if($Action -eq 'selected-lod'){NativeRequire ($row.selectedLod -eq 1 -and $row.selectedLod -ne $old[0].selectedLod) 'Declared LOD1 did not change actual target selection'}
		else {
			NativeRequire ($row.selectedLod -eq $old[0].selectedLod) 'Meshlet control changed unrelated LOD'
			$begin=NativeUnsigned $row.firstMeshlet firstMeshlet;$count=NativeUnsigned $row.meshletCount meshletCount $true
			$oldBegin=NativeUnsigned $old[0].firstMeshlet baselineFirstMeshlet;$oldCount=NativeUnsigned $old[0].meshletCount baselineMeshletCount $true
			# Decimal arithmetic retains every uint64 bit and cannot wrap an end
			# offset while checking containment of malformed evidence ranges.
			$oldEnd=[decimal]$oldBegin+[decimal]$oldCount
			NativeRequire ($oldEnd -le [decimal][uint64]::MaxValue -and [decimal]$begin -ge [decimal]$oldBegin -and [decimal]$begin -le $oldEnd -and [decimal]$count -le $oldEnd-[decimal]$begin -and ($begin -ne $oldBegin -or $count -lt $oldCount)) 'Meshlet control did not select a genuinely changed contained range'
			# Variant 1 in the sealed P1 controller excludes exactly the first
			# meshlet of LOD0. A different smaller span is not that requested action.
			NativeRequire ($oldCount -gt 1 -and [decimal]$begin -eq [decimal]$oldBegin+1 -and $count -eq $oldCount-1) 'Meshlet control differs from frozen exclude-first selection'
		}
	}
}
function NewNativeArtifactClosureIndex($Artifacts) {
	# Index the complete existing descriptor, not just its path or content hash.
	# Ordinal comparison preserves the old case-sensitive Canonical equality.
	# This metadata index belongs only to the current immutable verification;
	# ArtifactPath still checks every referenced file and its actual bytes.
	$index=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach($artifact in $Artifacts){$null=$index.Add((Canonical $artifact))}
	# Prevent PowerShell from enumerating the set into unrelated string results.
	return ,$index
}
function VerifyNativeControlTransitions($Envelope,$Document,$Identity,$Directory,$Steps,$Cameras,$RootArtifactIndex=$null) {
	# These receipts retain the first completed cohort before control admission.
	# Later measured hits cannot replace evidence that the first write was valid.
	$actions=@('origin','selected-lod','selected-meshlet','distinct-origins','alternate-view-order','different-caster-selections','alternate-snapshot-slot','B-record-overwrite-before-A-old-owner-hit')
	$p5=$Identity.stage -ceq 'P5' -and $Identity.fixtureId -ceq 'static-dynamic';if($p5){$actions=@($Steps|ForEach-Object action)}
	$p5shared=NativeP5SharedIdentity $Identity;$shared=(NativeP4SharedIdentity $Identity) -or $p5shared
	$p4shared=NativeP4SharedIdentity $Identity;if($shared){$actions=@($Steps|ForEach-Object action)};$p4=$Identity.stage -ceq 'P4' -and $Identity.fixtureId -ceq 'shadow-batching';if($p4){$actions=@('compatible-1000','mixed-route','deformed','cutout','off-camera','dispatch-limit-split','upload-failure-fallback')}
	if($null -eq $RootArtifactIndex){$RootArtifactIndex=NewNativeArtifactClosureIndex $Document.artifacts}
	$result=@{}
	foreach($step in @($Steps|Where-Object {$_.action -in $actions})) {
		$stepCameras=if($p4){@(GetNativeShadowBatchCameras $step.id $Cameras)}elseif($p5){@(GetNativeStaticShadowCameras $step.id $Cameras)}else{@($Cameras)}
		$matches=@($Envelope.receipts|Where-Object {$_.kind -ceq 'ControlTransition' -and $_.fixtureStepId -ceq $step.id})
		if($matches.Count -ne 1){Unverified "Missing/duplicate first control transition for $($step.id)"}
		$receipt=$matches[0]
		Validate $receipt $script:definitionRoot.'$defs'.controlTransitionReceipt
		NativeRequire ($receipt.action -ceq $step.action) 'Transition action differs from sealed step'
		$correlation=$receipt.originalCorrelation
		NativeRequire ($correlation.operationId -ceq $Envelope.operationId -and $correlation.fixtureStepId -ceq $step.id) 'Transition original operation/step mismatch'
		$generation=NativeUnsigned $receipt.controlGeneration controlGeneration $true
		foreach($field in @('mutationGeneration','settingsGeneration','rendererSessionGeneration')){$null=NativeUnsigned $correlation.$field $field $true}
		$null=NativeUnsigned $correlation.stepFrame stepFrame
		foreach($field in @('frameSerial','commandSerial','submitSerial','completionSerial')){$null=NativeUnsigned $receipt.$field $field $true}
		NativeRequire ((NativeUnsigned $receipt.completionSerial completionSerial) -ge (NativeUnsigned $receipt.submitSerial submitSerial)) 'Transition completion precedes submit'
		$firstWarmup=@($Document.frames|Where-Object {$_.fixtureStepId -ceq $step.id -and $_.measurementPhase -ceq 'Warmup'}|Sort-Object {Metric $_.frameSerial frameSerial}|Select-Object -First 1)
		if($firstWarmup.Count -ne 1){Unverified 'Transition lacks same-step admitted warmup observation'}
		NativeRequire ((NativeUnsigned $receipt.frameSerial frameSerial $true) -lt (Metric $firstWarmup[0].frameSerial frameSerial)) 'Transition is not earlier than first admitted warmup'
		foreach($field in @('settingsGeneration','rendererSessionGeneration')){NativeRequire ((Metric $firstWarmup[0].$field $field) -eq (NativeUnsigned $correlation.$field $field $true)) "Transition/admitted warmup $field mismatch"}
		# Original mutation and admitted control generations can be different
		# publication domains. Their exact identities matter; numeric ordering
		# between those clocks would invent causality and reject valid evidence.
		$proofs=@{};$counters=[Collections.Generic.List[object]]::new();$sharedRows=[Collections.Generic.List[object]]::new();$sharedLighting=[Collections.Generic.List[object]]::new();$epoch=$null;$regions=@{};$viewCommands=@{};$first=$true
		$receiptArtifactIndex=NewNativeArtifactClosureIndex $receipt.artifacts
		# Every nested reference must also occur in the root artifact closure. The
		# ordinary artifact resolver proves containment and the actual byte hash.
		foreach($artifact in $receipt.artifacts) {
			NativeRequire ($RootArtifactIndex.Contains((Canonical $artifact))) 'Transition artifact missing from root closure'
			$null=ArtifactPath $artifact $Directory
		}
		foreach($frame in $receipt.frames) {
			if(($Identity.stage -in @('P2','P3')) -and (Has $frame 'evidenceKind') -and $frame.evidenceKind -in @('Lighting','LightingDepth')){continue} # These native bytes have separate purpose guards.
			NativeRequire ($frame.publicationState -ceq 'Complete' -and $frame.measurementPhase -ceq 'Transition' -and $frame.fixtureStepId -ceq $step.id -and $frame.stepFrame -eq $correlation.stepFrame) 'Transition frame is not the original completed observation'
			foreach($field in @('frameSerial','submitSerial','completionSerial')){NativeRequire ((Metric $frame.$field $field) -eq (NativeUnsigned $receipt.$field $field $true)) "Transition cohort $field mismatch"}
			if($first){NativeRequire ((Metric $frame.commandSerial commandSerial) -eq (NativeUnsigned $receipt.commandSerial commandSerial $true)) 'Transition first command receipt mismatch';$first=$false}
			foreach($field in @('mutationGeneration','settingsGeneration','rendererSessionGeneration')){NativeRequire ((Metric $frame.$field $field) -eq (NativeUnsigned $correlation.$field $field $true)) "Transition original $field mismatch"}
			if($Identity.stage -cne 'P5'){NativeRequire ((Canonical $frame.effectiveOptions) -ceq (Canonical $Document.requestedOptions)) 'Transition settings not actually observed'}
			foreach($artifact in $frame.artifacts){NativeRequire ($receiptArtifactIndex.Contains((Canonical $artifact))) 'Transition frame artifact absent from receipt closure'}
			$proof=ReadNativeProof $frame $Identity $Directory;$provenance=$proof.counters.readbackProvenance;if($Identity.stage -ceq 'P5'){VerifyNativeStaticOptions $frame $Document $Envelope $step $provenance}
			if($p5){VerifyNativeStaticShadowRecipe $frame $proof.counters $step};if($p5shared -and (!(Has $frame 'evidenceKind') -or $frame.evidenceKind -ceq 'Shadow')){VerifyNativeSharedStaticRecipe $frame $proof.counters $step};if($p4){VerifyNativeShadowBatchRecipe $frame $proof.counters $step};if($p4shared -and (!(Has $frame 'evidenceKind') -or $frame.evidenceKind -ceq 'Shadow')){VerifyNativeSharedShadowBatchRecipe $frame $proof.counters $step $Identity.fixtureId}
			NativeReceiptFields $provenance @('fixtureControlGeneration','optionsSessionGeneration') 'transition provenance'
			NativeRequire ($proof.operationId -ceq $Envelope.operationId -and (NativeUnsigned $provenance.fixtureControlGeneration fixtureControlGeneration $true) -eq $generation) 'Transition native control generation/operation mismatch'
			$currentEpoch=NativeUnsigned $provenance.optionsSessionGeneration optionsSessionGeneration $true
			if($null -eq $epoch){$epoch=$currentEpoch};NativeRequire ($epoch -eq $currentEpoch) 'Transition mixes options lifetimes'
			$key=ObservationKey $frame;NativeRequire (!$proofs.ContainsKey($key)) 'Duplicate transition view/light/face';$proofs[$key]=$proof;if($shared -and (Has $frame 'evidenceKind') -and $frame.evidenceKind -ceq 'Lighting'){VerifyNativeLightingSourceCount $proof $step $Identity.fixtureId;$sharedLighting.Add(@{frame=$frame;counters=$proof.counters;upload=$proof.upload;imageHash=$proof.imageHash})}else{$counters.Add($proof.counters)};if($shared){$sharedRows.Add(@{frame=$frame;counters=$proof.counters;imageHash=@($frame.artifacts|Where-Object kind -CEQ Image)[0].sha256})}
			$view="$($frame.viewType)|$($frame.viewId)";NativeRequire (@($stepCameras|Where-Object {$_.viewType -ceq $frame.viewType -and $_.viewId -eq $frame.viewId}).Count -eq 1) 'Transition names an unexpected view'
			# Scene and Game own separate primary commands. Coherence is per view;
			# the receipt's top command is only its first row's representative.
			$command=NativeUnsigned $provenance.commandSerial commandSerial $true
			if(!$viewCommands.ContainsKey($view)){$viewCommands[$view]=$command};NativeRequire ($viewCommands[$view] -eq $command) 'Transition mixes commands within one view'
			if($shared -and (Has $frame 'evidenceKind') -and $frame.evidenceKind -ceq 'Lighting'){continue}
			if(!$regions.ContainsKey($view)){$regions[$view]=@{}}
			$kind=[int]$proof.counters.qualifiedShadowViewKey.resourceKind
			if(!$regions[$view].ContainsKey($kind)){$regions[$view][$kind]=[Collections.Generic.List[object]]::new()};$regions[$view][$kind].Add($proof.counters.qualifiedShadowViewKey)
			# These frozen steps select different real LOD/ranges. Their retained
			# first observation must be the refresh, never the later cache hit.
			if($step.action -in @('selected-lod','selected-meshlet')){
				NativeRequire ((Metric $frame.shadow.reuseRegionCount reuseRegionCount) -eq 0) 'Changed selection first transition reused old content'
				VerifyNativeSelectionTransition $proof $frame $Document $Identity $Directory $Steps $step.action
			}
		}
		if($p5){VerifyNativeStaticShadowRegions $counters.ToArray() $step.id $Cameras};if($p4){VerifyNativeShadowBatchRegions $counters.ToArray() $step.id};if($shared){VerifyNativeP4SharedCohorts $sharedRows.ToArray() $stepCameras $Identity.fixtureId;VerifyNativeLightingCohorts $sharedLighting.ToArray() $stepCameras $false}
		foreach($camera in $(if($p4 -or $p5){@()}else{$stepCameras})) {
			$view="$($camera.viewType)|$($camera.viewId)";NativeRequire ($regions.ContainsKey($view)) 'Transition omitted a required view'
			foreach($kind in 0..3){$expected=if($kind -eq 1){6}else{1};NativeRequire ($regions[$view].ContainsKey($kind) -and $regions[$view][$kind].Count -eq $expected) 'Transition omitted a required shadow region'}
			NativeRequire (@($regions[$view][1]|ForEach-Object lightId|Sort-Object -Unique).Count -eq 1) 'Transition cube faces name different lights'
		}
		VerifyNativeBundle $counters.ToArray()
		$occupied=[long]0;$eligible=[long]0;foreach($proof in $proofs.Values){$occupied+=$proof.occupiedPixels;$eligible+=$proof.semanticRows.Count};VerifyNativeStepCoverage $occupied $eligible
		$result[$step.id]=$proofs
	}
	return $result
}
function VerifyNativeResidencyPublications($Envelope,$Document,$Identity,$Directory,$Steps,$Cameras,$RootArtifactIndex=$null) {
	# Residency is a native reupload, separate from preparatory source restoration.
	# Retain both actual cohorts; a later warmup hit cannot prove the first refresh.
	$result=@{}
	if($null -eq $RootArtifactIndex){$RootArtifactIndex=NewNativeArtifactClosureIndex $Document.artifacts}
	foreach($step in @($Steps|Where-Object action -eq residency)) {
		if($Identity.fixtureId -cne 'shadow-invalidation'){Unverified 'Residency target contract unavailable for this fixture'}
		$matches=@($Envelope.receipts|Where-Object {$_.kind -ceq 'ResidencyPublication' -and $_.fixtureStepId -ceq $step.id})
		if($matches.Count -ne 1){Unverified "Missing/duplicate residency observation for $($step.id)"}
		$r=$matches[0];Validate $r $script:definitionRoot.'$defs'.residencyPublicationReceipt
		# The manifest seals the action but omits a target field; the installed
		# native P1 recipe binds residency to the selection controls' grid UUID.
		NativeRequire ($r.targetObjectId -ceq ('650'+('0'*27)+'11') -and $r.meshId -cne ('0'*32)) 'Residency target differs from sealed native fixture'
		$baseline=$r.baselineObservation;$reupload=$r.reuploadObservation
		NativeRequire ($baseline.originalCorrelation.stepFrame -eq ($step.warmupFrames+$step.measuredFrames) -and $reupload.originalCorrelation.stepFrame -gt $baseline.originalCorrelation.stepFrame) 'Residency original sample tokens are not baseline then reupload'
		NativeRequire ((NativeUnsigned $baseline.frameSerial baselineFrame $true) -lt (NativeUnsigned $reupload.frameSerial reuploadFrame $true)) 'Residency reupload does not follow baseline'
		foreach($field in @('settingsGeneration','rendererSessionGeneration','mutationGeneration')){NativeRequire ($baseline.originalCorrelation.$field -eq $reupload.originalCorrelation.$field) "Residency changed original $field"}
		NativeRequire ($baseline.controlGeneration -ceq $reupload.controlGeneration) 'Residency changed control generation'
		foreach($pair in @(@('previousActualFrameSerial','frameSerial',$baseline),@('newActualFrameSerial','frameSerial',$reupload),@('submitSerial','submitSerial',$reupload),@('completionSerial','completionSerial',$reupload))){NativeRequire ((NativeUnsigned $r.($pair[0]) $pair[0] $true) -eq (NativeUnsigned $pair[2].($pair[1]) $pair[1] $true)) 'Residency scalar receipt differs from native observation'}
		# The legacy scalar selected a real target-bearing view command; Scene
		# and Game may differ. The nested representative remains frames[0].
		NativeRequire (@($reupload.frames|Where-Object {(Metric $_.commandSerial commandSerial) -eq (NativeUnsigned $r.commandSerial commandSerial $true)}).Count -gt 0) 'Residency scalar command is absent from actual cohort'
		NativeRequire ($r.previousMeshPublicationGeneration -eq $r.newMeshPublicationGeneration -and $r.previousAllocationGeneration -ne $r.newAllocationGeneration) 'Residency must retain CPU publication and change allocation'
		$cohorts=@{}
		foreach($phase in @('baseline','reupload')) {
			$o=if($phase -eq 'baseline'){$baseline}else{$reupload}
			# Reuse the exact completed-cohort verifier. This internal structural
			# adapter adds no evidence: original native fields and files stay intact.
			$control=[pscustomobject]@{kind='ControlTransition';fixtureStepId=$step.id;action='origin';controlGeneration=$o.controlGeneration;originalCorrelation=$o.originalCorrelation;frameSerial=$o.frameSerial;commandSerial=$o.commandSerial;submitSerial=$o.submitSerial;completionSerial=$o.completionSerial;frames=$o.frames;artifacts=$o.artifacts}
			$envelope=[pscustomobject]@{operationId=$Envelope.operationId;receipts=@($control)}
			$map=VerifyNativeControlTransitions $envelope $Document $Identity $Directory @([pscustomobject]@{id=$step.id;action='origin'}) $Cameras $RootArtifactIndex
			# Retry sample tokens are process-local. After verifying their exact
			# native correlation, join baseline/reupload and independent runs by
			# qualified region, never by an unrelated numeric sample clock.
			$regions=@{};foreach($proof in $map[$step.id].Values){$key=$proof.counters.qualifiedShadowViewKey;$region=@($key.viewType,$key.viewId,$key.lightId,$key.faceIndex)-join '|';NativeRequire (!$regions.ContainsKey($region)) 'Duplicate residency qualified region';$regions[$region]=$proof}
			$cohorts[$phase]=$regions;$result["$($step.id)|$phase"]=$regions
		}
		NativeRequire ($cohorts.baseline.Count -eq $cohorts.reupload.Count) 'Partial residency cohort'
		foreach($key in $cohorts.baseline.Keys) {
			NativeRequire ($cohorts.reupload.ContainsKey($key)) 'Residency qualified region changed'
			$b=$cohorts.baseline[$key].counters;$n=$cohorts.reupload[$key].counters
			NativeRequire ($b.readbackProvenance.optionsSessionGeneration -ceq $n.readbackProvenance.optionsSessionGeneration) 'Residency options lifetime changed'
			NativeRequire ((Metric $n.nativeCounters.shadowReuses shadowReuses) -eq 0) 'First residency reupload reused old content'
			$old=@($b.eligibleCasterIdentityRows|Where-Object objectId -CEQ $r.targetObjectId);$new=@($n.eligibleCasterIdentityRows|Where-Object objectId -CEQ $r.targetObjectId)
			NativeRequire ($old.Count -gt 0 -and $old.Count -eq $new.Count) 'Residency target membership missing'
			foreach($row in $new) {
				$prior=@($old|Where-Object {$_.segmentId -ceq $row.segmentId -and $_.meshId -ceq $row.meshId -and $_.assetId -ceq $row.assetId -and $_.lifetimeGeneration -ceq $row.lifetimeGeneration})
				NativeRequire ($prior.Count -eq 1 -and $row.meshId -ceq $r.meshId -and $row.assetId -cne ('0'*32)) 'Residency owner identity changed'
				foreach($pair in @(@($prior[0],$r.previousMeshPublicationGeneration,$r.previousAllocationGeneration),@($row,$r.newMeshPublicationGeneration,$r.newAllocationGeneration))){NativeRequire ((NativeUnsigned $pair[0].meshRevision meshRevision $true) -eq $pair[1] -and (NativeUnsigned $pair[0].residencyGeneration residencyGeneration $true) -eq $pair[2]) 'Residency native allocation/publication receipt mismatch'}
			}
			NativeRequire (((NativeCasterRows $b.eligibleCasterIdentityRows $true) -join "`n") -ceq ((NativeCasterRows $n.eligibleCasterIdentityRows $true) -join "`n")) 'Residency altered immutable caster content'
			# Within this process, only GPU residency and raw animation observation
			# counters may change. Preserve every resource/lifetime proof locally;
			# cross-process comparison still uses immutable semantic identities.
			$local=@()
			foreach($rows in @($b.eligibleCasterIdentityRows,$n.eligibleCasterIdentityRows)) {
				$normalized=@();foreach($row in $rows){$copy=Canonical $row|ConvertFrom-Json;foreach($field in @('residencyGeneration','poseGeneration','morphGeneration')){$copy.PSObject.Properties.Remove($field)};$normalized+=Canonical $copy}
				$local+=,(@($normalized|Sort-Object -CaseSensitive)-join "`n")
			}
			NativeRequire ($local[0] -ceq $local[1]) 'Residency changed local caster proof beyond allocation'
		}
	}
	return $result
}
function CompareNativeControlTransitions($Candidate,$Reference,$Resolution) {
	$maximumDepth=0.0;$maximumImage=0.0
	NativeRequire ($Candidate.Count -eq $Reference.Count) 'Unpaired control transitions'
	foreach($step in $Candidate.Keys){NativeRequire ($Reference.ContainsKey($step)) 'Missing reference transition';$left=$Candidate[$step];$right=$Reference[$step]
		NativeRequire ($left.Count -eq $right.Count) 'Partial paired transition'
		foreach($key in $left.Keys){NativeRequire ($right.ContainsKey($key)) 'Transition region mismatch'
			# Backend allocation/session epochs can differ between independent runs.
			# All other physical view, projection, origin and format inputs must match.
			if(!(Has $left[$key].counters 'evidenceKind') -or $left[$key].counters.evidenceKind -ceq 'Shadow'){$a=$left[$key].counters.qualifiedShadowViewKey;$b=$right[$key].counters.qualifiedShadowViewKey
			foreach($field in @('viewType','viewId','lightId','faceIndex','resourceKind','regionId','extent','depthFormat','referenceOrigin','lightProjectionStamp')){NativeRequire (JsonEqual $a.$field $b.$field) "Transition qualified $field mismatch"}}
			$delta=CompareNativeObservationProof $left[$key] $right[$key] $Resolution Transition
			$maximumDepth=[Math]::Max($maximumDepth,$delta.maxDepthAbsoluteError);$maximumImage=[Math]::Max($maximumImage,$delta.maxImageNormalizedChannelError)
		}
	}
	return @{maxDepthAbsoluteError=$maximumDepth;maxImageNormalizedChannelError=$maximumImage}
}
try {
	# Windows PowerShell 5.1 may bind parameter defaults before PSScriptRoot is
	# populated. Resolve bundled defaults here, inside the ordinary error receipt.
	if(!$Manifest){$Manifest=Join-Path $PSScriptRoot '../../Fixtures/RendererEfficiency/workloads.json'}
	if(!$Schema){$Schema=Join-Path $PSScriptRoot 'Schemas/evidence.schema.json'}
	. (Join-Path $PSScriptRoot 'RendererEfficiencyStageSteps.ps1')
	$script:definitionRoot=Get-Content -LiteralPath $Schema -Raw|ConvertFrom-Json
	$manifestValue=Get-Content -LiteralPath $Manifest -Raw|ConvertFrom-Json
	Require ($manifestValue.schemaVersion -eq 1 -and @($manifestValue.workloads).Count -eq 9) 'Invalid frozen workload manifest'
	$refs=LoadEvidence $Reference 'Reference'; $candidates=LoadEvidence $Candidate 'Candidate'
	$requested=@($manifestValue.workloads|Where-Object { $Stage -eq 'All' -or $_.stage -eq $Stage -or $_.stage -eq 'All' })
	ValidateEvidenceIndexSelection (@($refs)+@($candidates)) $requested $Stage
	$validatedInputs=@{}
	$anyUnverified=$false; $anyFailed=$false
	foreach($workload in $requested) {
		$workRefs=@($refs|Where-Object {$_.value.identity.fixtureId -eq $workload.id})
		$workCandidates=@($candidates|Where-Object {$_.value.identity.fixtureId -eq $workload.id})
		if($workCandidates.Count -lt 3 -or $workRefs.Count -ne $workCandidates.Count) { Unverified "Missing three matched runs for $($workload.id)" }
		$seenRuns=@{};$preparationCounts=@{Serial=0;Concurrent=0}
		foreach($document in $workCandidates){if(Has $document.value.requestedOptions 'validationPreparation'){$mode=$document.value.requestedOptions.validationPreparation;if($preparationCounts.ContainsKey($mode)){$preparationCounts[$mode]++}}}
		foreach($candidateIndex in $workCandidates) {
			$cv=$candidateIndex.value; $run=$cv.identity.runIndex
			if(!(Has $cv.requestedOptions 'validationPreparation')){Unverified 'Native serial/concurrent preparation intent unavailable'}
			$preparation=$cv.requestedOptions.validationPreparation;$runKey="$run|$preparation"
			Require (!$seenRuns.ContainsKey($runKey)) 'Duplicate fixture/run/preparation identity'; $seenRuns[$runKey]=$true
			$matched=@($workRefs|Where-Object {$_.value.identity.runIndex -eq $run -and (Has $_.value.requestedOptions 'validationPreparation') -and $_.value.requestedOptions.validationPreparation -ceq $preparation}); Require ($matched.Count -eq 1) 'Missing/duplicate matched reference preparation run'
			# Sequential pair ownership bounds PowerShell's large nested object graph.
			# Every selected pair still receives the complete frozen schema validation.
			$c=LoadIndexedEvidence $candidateIndex;$r=LoadIndexedEvidence $matched[0];$cv=$c.value;$rv=$r.value
			$validatedInputs[$candidateIndex.path]=$true;$validatedInputs[$matched[0].path]=$true
			foreach($field in @('fixtureHash','seed','cameraHash','geometryMaterialRevisionHash','adapter','driver','resolution','referenceRevision','candidateRevision')) {
				Require ((Canonical $rv.identity.$field) -ceq (Canonical $cv.identity.$field)) "Unmatched identity: $field"
			}
			foreach($field in @('fixtureHash','cameraHash','geometryMaterialRevisionHash','installedBinaryHash','installedShaderClosureHash','installedAssetClosureHash','adapter','driver')) { $null=Metric $cv.identity.$field $field; $null=Metric $rv.identity.$field $field }
			Require ([uint64]$cv.identity.seed -eq [uint64]$workload.seed) 'Seed differs from frozen workload'
			Require ((Metric $cv.identity.fixtureHash 'fixtureHash') -ieq (Get-FileHash -LiteralPath $Manifest -Algorithm SHA256).Hash) 'Fixture manifest hash mismatch'
			if(!$PostP1Baseline) { foreach($field in @('installedBinaryHash','installedShaderClosureHash','installedAssetClosureHash')) { Require ((Metric $rv.identity.$field $field) -ceq (Metric $cv.identity.$field $field)) "Different installed $field" } }
			else { Require ((Metric $cv.identity.postP1BaselineClosureHash 'postP1BaselineClosureHash') -ceq (Metric $rv.identity.postP1BaselineClosureHash 'postP1BaselineClosureHash')) 'Archived post-P1 baseline identity mismatch' }
			$stageName=if($Stage -eq 'All') {$cv.identity.stage} else {$Stage}
			Require ($stageName -in @('P1','P2','P3','P4','P5')) 'Evidence must name a concrete comparison stage'
			$allowedResolutions=@($manifestValue.measurementPolicy.primaryResolution)+@($manifestValue.measurementPolicy.edgeResolutions)
			NativeRequire (@($allowedResolutions|Where-Object {$_.width -eq $cv.identity.resolution.width -and $_.height -eq $cv.identity.resolution.height}).Count -gt 0) 'Evidence camera resolution differs from frozen workload resolutions'
			$timedDepth=$stageName -eq 'P3' -and $workload.id -eq 'depth-heavy'
			$timedBaseline=$PostP1Baseline -and $workload.representativeForSC008
			$allowedDifference=@{P1='shadowReuse';P2='lightingUploads';P3='lightingDepth';P4='shadowSubmission';P5='staticShadowCacheBudgetBytes'}
			foreach($field in $cv.requestedOptions.PSObject.Properties.Name) { if($field -ne $allowedDifference[$stageName]) { Require ((Canonical $rv.requestedOptions.$field) -ceq (Canonical $cv.requestedOptions.$field)) "Unmatched option: $field" } }
			$expectedReference=@{P1='AlwaysRefresh';P2='Full';P3='CompleteScan';P4='Singleton';P5=0}
			Require ($rv.requestedOptions.($allowedDifference[$stageName]) -ceq $expectedReference[$stageName]) 'Wrong stage reference mode'
			if($stageName -in @('P4','P5')) { Require ($cv.requestedOptions.shadowReuse -eq 'AlwaysRefresh' -and $rv.requestedOptions.shadowReuse -eq 'AlwaysRefresh') 'Equivalent forced refreshes are required' }
			if($stageName -ceq 'P4'){NativeRequire ($cv.requestedOptions.shadowSubmission -ceq 'Grouped') 'P4 Candidate must request actual Grouped submissions'}
			# Each document owns its directory. Searching the Candidate array for
			# every artifact would make this provenance traversal quadratic.
			foreach($entry in $cv.artifacts){if($entry.status -eq 'Valid'){$null=ArtifactPath $entry $c.directory}}
			foreach($entry in $rv.artifacts){if($entry.status -eq 'Valid'){$null=ArtifactPath $entry $r.directory}}
			$selectedSteps=@(GetRendererEfficiencyStageSteps $workload $stageName)
			$lightingCameras=@($manifestValue.cameras|Where-Object {$_.id -in $workload.cameraIds})
			$cLightingState=$rLightingState=$null;$lightingCandidateCohorts=[Collections.Generic.List[object]]::new();$lightingReferenceCohorts=[Collections.Generic.List[object]]::new()
			$lightingCandidateIndex=$lightingReferenceIndex=$null
			if($stageName -in @('P2','P3') -or (NativeP4SharedIdentity $cv.identity) -or (NativeP5SharedIdentity $cv.identity)){$lightingCandidateIndex=NewNativeArtifactClosureIndex $cv.artifacts;$lightingReferenceIndex=NewNativeArtifactClosureIndex $rv.artifacts}
			$depthCandidateCohorts=[Collections.Generic.List[object]]::new();$depthReferenceCohorts=[Collections.Generic.List[object]]::new()
			$cDepthRecipes=$rDepthRecipes=$null
			$cframes=@($cv.frames|Where-Object {$_.measurementPhase -eq 'Measured'}); $rframes=@($rv.frames|Where-Object {$_.measurementPhase -eq 'Measured'})
			if($cframes.Count -lt 300 -or $rframes.Count -lt 300) { Unverified 'Fewer than 300 measured observations' }
			$referenceByKey=@{}
			foreach($frame in @($rv.frames|Where-Object {$_.measurementPhase -in @('Warmup','Measured')})) { $key=ObservationKey $frame; Require (!$referenceByKey.ContainsKey($key)) 'Duplicate reference observation'; $referenceByKey[$key]=$frame }
			$p5CandidateEnvelope=$null;$p5ReferenceEnvelope=$null
			$cseen=@{}; $referenceTiming=@(); $candidateTiming=@(); $candidateOperation=$null; $referenceOperation=$null; $rawMaximumDepth=0.0; $rawMaximumImage=0.0; $nativeBundles=[Collections.Generic.List[object]]::new();$nativeDiagnostics=[Collections.Generic.List[object]]::new();$occupiedByStep=@{};$eligibleByStep=@{}
			foreach($frame in @($cv.frames|Where-Object {$_.measurementPhase -in @('Warmup','Measured')})) {
				$key=ObservationKey $frame; Require (!$cseen.ContainsKey($key)) 'Duplicate candidate observation'; $cseen[$key]=$true
				Require ($referenceByKey.ContainsKey($key)) 'Mismatched fixture step/view/light/face'; $other=$referenceByKey[$key]
				NativeRequire ($frame.measurementPhase -ceq $other.measurementPhase) 'Paired observation phase mismatch'
				foreach($observed in @($frame,$other)) {
					VerifyNativeObservationPhase $observed
					if($observed.publicationState -ne 'Complete') { Unverified 'Observation lacks completed GPU publication' }
					foreach($field in @('frameSerial','settingsGeneration','mutationGeneration','rendererSessionGeneration','commandSerial','submitSerial','completionSerial')) { $null=Metric $observed.$field $field }
					Require ((Metric $observed.completionSerial 'completionSerial') -ge (Metric $observed.submitSerial 'submitSerial')) 'Completion precedes submission'
					# Static allocations are introduced by P5. Earlier gates keep those
					# counters explicitly unavailable rather than invent measured zeros.
					if($stageName -eq 'P5') {
						$budget=Metric $observed.memory.staticCacheBudgetEffectiveBytes 'effective static budget'
						$charged=(Metric $observed.memory.activeBytes 'active static bytes')+(Metric $observed.memory.reservedBytes 'reserved static bytes')+(Metric $observed.memory.retiringBytes 'retiring static bytes')
						Require ($charged -le $budget) 'Static cache admission exceeds effective budget'
					}
				}
				if($stageName -cne 'P5'){Require ((Canonical $frame.effectiveOptions) -ceq (Canonical $cv.requestedOptions)) 'Candidate settings not observed'
					Require ((Canonical $other.effectiveOptions) -ceq (Canonical $rv.requestedOptions)) 'Reference settings not observed'}
				if((Has $frame 'evidenceKind') -and $frame.evidenceKind -ceq 'Lighting'){
					VerifyNativeLightingStage $cv.identity;NativeRequire ((Has $other 'evidenceKind') -and $other.evidenceKind -ceq 'Lighting') 'Wrong paired lighting kind'
					$cproof=ReadNativeLightingProof $frame $cv.identity $c.directory $lightingCandidateIndex;$rproof=ReadNativeLightingProof $other $rv.identity $r.directory $lightingReferenceIndex
				}elseif((Has $frame 'evidenceKind') -and $frame.evidenceKind -ceq 'LightingDepth'){
					NativeRequire ($stageName -ceq 'P3' -and $other.evidenceKind -ceq 'LightingDepth') 'Wrong stage/paired depth kind'
					$cproof=ReadNativeDepthProof $frame $cv.identity $c.directory $lightingCandidateIndex;$rproof=ReadNativeDepthProof $other $rv.identity $r.directory $lightingReferenceIndex
					if($workload.id -ceq 'depth-coverage'){
						if($null -eq $cDepthRecipes){$ce=ReadNativeValidationReceipts $cv $cv.identity $c.directory $cproof.operationId;$re=ReadNativeValidationReceipts $rv $rv.identity $r.directory $rproof.operationId;$cDepthRecipes=ReadNativeDepthRecipes $ce $cv $cv.identity $c.directory $selectedSteps;$rDepthRecipes=ReadNativeDepthRecipes $re $rv $rv.identity $r.directory $selectedSteps}
						VerifyNativeDepthRecipe $cproof $cDepthRecipes;VerifyNativeDepthRecipe $rproof $rDepthRecipes
					}
					# Keep only small cohort metadata. Full input/bounds/color bytes are
					# compared immediately and released; retaining 360 HD input images
					# here would defeat the bounded-memory offline comparer.
					$depthCandidateCohorts.Add(@{frame=$frame;counters=$cproof.counters;imageHash=$cproof.imageHash});$depthReferenceCohorts.Add(@{frame=$other;counters=$rproof.counters;imageHash=$rproof.imageHash})
				}else{$cproof=ReadNativeProof $frame $cv.identity $c.directory;$rproof=ReadNativeProof $other $rv.identity $r.directory}
				$nativeBundles.Add($cproof.counters);$nativeBundles.Add($rproof.counters)
				$nativeDiagnostics.Add(@{frame=$frame;counters=$cproof.counters;imageHash=@($frame.artifacts|Where-Object kind -CEQ Image)[0].sha256});$nativeDiagnostics.Add(@{frame=$other;counters=$rproof.counters;imageHash=@($other.artifacts|Where-Object kind -CEQ Image)[0].sha256})
				if(!$candidateOperation){$candidateOperation=$cproof.operationId};if(!$referenceOperation){$referenceOperation=$rproof.operationId}
				NativeRequire ($candidateOperation -ceq $cproof.operationId -and $referenceOperation -ceq $rproof.operationId) 'One run mixes captured operation identities'
				if($stageName -ceq 'P5'){
					if($null -eq $p5CandidateEnvelope){$p5CandidateEnvelope=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$p5ReferenceEnvelope=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation}
					$step=@($selectedSteps|Where-Object id -ceq $frame.fixtureStepId);NativeRequire ($step.Count -eq 1) 'P5 budget frame names an unknown authored step'
					VerifyNativeStaticOptions $frame $cv $p5CandidateEnvelope $step[0] $cproof.counters.readbackProvenance;VerifyNativeStaticOptions $other $rv $p5ReferenceEnvelope $step[0] $rproof.counters.readbackProvenance
				}
				if((Has $frame 'evidenceKind') -and $frame.evidenceKind -ceq 'Lighting'){
					if($stageName -ceq 'P2' -and $null -eq $cLightingState){
						$ce=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$re=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation
						$cLightingState=ReadNativeLightingBaselines $ce $cv $cv.identity $c.directory $selectedSteps $lightingCameras;$rLightingState=ReadNativeLightingBaselines $re $rv $rv.identity $r.directory $selectedSteps $lightingCameras
						$rawMaximumImage=[Math]::Max($rawMaximumImage,(CompareNativeLightingBaselines $cLightingState $rLightingState $cv.identity $rv.identity $c.directory $r.directory))
						$ctLighting=ReadNativeLightingTransitions $ce $cv $cv.identity $c.directory $selectedSteps $lightingCameras $cLightingState;$rtLighting=ReadNativeLightingTransitions $re $rv $rv.identity $r.directory $selectedSteps $lightingCameras $rLightingState
						$ltDelta=CompareNativeLightingTransitions $ctLighting $rtLighting $cv.identity.resolution;$rawMaximumImage=[Math]::Max($rawMaximumImage,$ltDelta.maxImageNormalizedChannelError)
					}
					$lightingStep=@($selectedSteps|Where-Object id -CEQ $frame.fixtureStepId);NativeRequire ($lightingStep.Count -eq 1) 'Unexpected native lighting step'
					if($stageName -ceq 'P2'){
						VerifyNativeLightingSelection $cproof $lightingStep[0] $workload.id;VerifyNativeLightingSelection $rproof $lightingStep[0] $workload.id
						UpdateNativeLightingTimeline $cLightingState $cproof $lightingStep[0];UpdateNativeLightingTimeline $rLightingState $rproof $lightingStep[0] $true
					}else{VerifyNativeLightingSourceCount $cproof $lightingStep[0] $workload.id;VerifyNativeLightingSourceCount $rproof $lightingStep[0] $workload.id;NativeRequire ((Metric $cproof.upload.activeRecordCount activeRecordCount) -eq [Math]::Min((Metric $cproof.frame.fullSourceCandidateCount fullSourceCandidateCount),1024)) 'P3 native lighting payload is underfilled'}
					$lightingCandidateCohorts.Add(@{frame=$frame;counters=$cproof.counters;upload=$cproof.upload;imageHash=$cproof.imageHash});$lightingReferenceCohorts.Add(@{frame=$other;counters=$rproof.counters;upload=$rproof.upload;imageHash=$rproof.imageHash})
				}
				if((NativeP5SharedIdentity $cv.identity) -and (!(Has $frame 'evidenceKind') -or $frame.evidenceKind -ceq 'Shadow')){$sharedStep=@($selectedSteps|Where-Object id -CEQ $frame.fixtureStepId);NativeRequire ($sharedStep.Count -eq 1) 'Unexpected shared P5 action';VerifyNativeSharedStaticRecipe $frame $cproof.counters $sharedStep[0];VerifyNativeSharedStaticRecipe $other $rproof.counters $sharedStep[0]}
				if($stageName -ceq 'P5' -and $workload.id -ceq 'static-dynamic'){$step=@($selectedSteps|Where-Object id -ceq $frame.fixtureStepId);NativeRequire ($step.Count -eq 1) 'Unexpected P5 fixture step';VerifyNativeStaticShadowRecipe $frame $cproof.counters $step[0];VerifyNativeStaticShadowRecipe $other $rproof.counters $step[0]}
				if($stageName -ceq 'P4' -and $workload.id -ceq 'shadow-batching'){$step=@($selectedSteps|Where-Object id -ceq $frame.fixtureStepId);NativeRequire ($step.Count -eq 1) 'Unexpected P4 fixture step';VerifyNativeShadowBatchRecipe $frame $cproof.counters $step[0];VerifyNativeShadowBatchRecipe $other $rproof.counters $step[0];if($frame.fixtureStepId -ceq 'shadow-batching-00'){VerifyNativeShadowBatchReduction $frame $other}}
				if((NativeP4SharedIdentity $cv.identity) -and (!(Has $frame 'evidenceKind') -or $frame.evidenceKind -ceq 'Shadow')){$sharedStep=@($selectedSteps|Where-Object id -CEQ $frame.fixtureStepId);NativeRequire ($sharedStep.Count -eq 1) 'Unexpected shared P4 step';VerifyNativeSharedShadowBatchRecipe $frame $cproof.counters $sharedStep[0] $workload.id;VerifyNativeSharedShadowBatchRecipe $other $rproof.counters $sharedStep[0] $workload.id}
				$delta=CompareNativeObservationProof $cproof $rproof $cv.identity.resolution $frame.measurementPhase
				$rawMaximumDepth=[Math]::Max($rawMaximumDepth,$delta.maxDepthAbsoluteError)
				$rawMaximumImage=[Math]::Max($rawMaximumImage,$delta.maxImageNormalizedChannelError)
				if(!$occupiedByStep.ContainsKey($frame.fixtureStepId)){$occupiedByStep[$frame.fixtureStepId]=[long]0;$eligibleByStep[$frame.fixtureStepId]=[long]0}
				$occupiedByStep[$frame.fixtureStepId]+=$cproof.occupiedPixels;$eligibleByStep[$frame.fixtureStepId]+=$cproof.semanticRows.Count
				if($frame.measurementPhase -eq 'Measured' -and $timedBaseline){$referenceTiming+=Metric $other.totalFrameTimingNanoseconds totalFrameTimingNanoseconds;$candidateTiming+=Metric $frame.totalFrameTimingNanoseconds totalFrameTimingNanoseconds}
				if($frame.measurementPhase -eq 'Measured' -and $timedDepth -and (Has $frame 'evidenceKind') -and $frame.evidenceKind -ceq 'LightingDepth'){
					if(!$cproof.nonempty -or !$rproof.nonempty){Unverified 'Empty production depth cannot qualify a speed sample'}
					if(!$cproof.counters.productionQuery.cooperative -or $rproof.counters.productionQuery.cooperative){Unverified 'SC004 needs actual cooperative Candidate and complete-scan Reference dispatches'}
					$referenceTiming+=$rproof.duration;$candidateTiming+=$cproof.duration
				}
			}
			Require ($cseen.Count -eq $referenceByKey.Count) 'Unmatched reference observations'
			VerifyNativeBundle @($nativeBundles.ToArray()|Where-Object {!(Has $_ 'evidenceKind') -or $_.evidenceKind -eq 'Shadow'})
			if($stageName -ceq 'P4' -and $workload.id -ceq 'shadow-batching'){foreach($cohort in @($nativeBundles.ToArray()|Group-Object {"$($_.mode)|$($_.fixtureStepId)|$($_.measurementPhase)|$($_.stepFrame)"})){VerifyNativeShadowBatchRegions @($cohort.Group) $cohort.Group[0].fixtureStepId};$cp=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$rp=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation;$ct=VerifyNativeControlTransitions $cp $cv $cv.identity $c.directory $selectedSteps $manifestValue.cameras;$rt=VerifyNativeControlTransitions $rp $rv $rv.identity $r.directory $selectedSteps $manifestValue.cameras;$delta=CompareNativeControlTransitions $ct $rt $cv.identity.resolution;$rawMaximumDepth=[Math]::Max($rawMaximumDepth,$delta.maxDepthAbsoluteError);$rawMaximumImage=[Math]::Max($rawMaximumImage,$delta.maxImageNormalizedChannelError)}
			if(NativeP5SharedIdentity $cv.identity){
				VerifyNativeP4SharedCohorts @($nativeDiagnostics.ToArray()|Where-Object {$_.counters.mode -ceq 'Candidate'}) $lightingCameras $workload.id;VerifyNativeP4SharedCohorts @($nativeDiagnostics.ToArray()|Where-Object {$_.counters.mode -ceq 'Reference'}) $lightingCameras $workload.id
				VerifyNativeLightingCohorts $lightingCandidateCohorts.ToArray() $lightingCameras $false;VerifyNativeLightingCohorts $lightingReferenceCohorts.ToArray() $lightingCameras $false
				$cp=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$rp=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation
				$ct=VerifyNativeControlTransitions $cp $cv $cv.identity $c.directory $selectedSteps $lightingCameras;$rt=VerifyNativeControlTransitions $rp $rv $rv.identity $r.directory $selectedSteps $lightingCameras
				$delta=CompareNativeControlTransitions $ct $rt $cv.identity.resolution;$rawMaximumDepth=[Math]::Max($rawMaximumDepth,$delta.maxDepthAbsoluteError);$rawMaximumImage=[Math]::Max($rawMaximumImage,$delta.maxImageNormalizedChannelError)
			}
			if(NativeP4SharedIdentity $cv.identity){
				VerifyNativeP4SharedCohorts @($nativeDiagnostics.ToArray()|Where-Object {$_.counters.mode -ceq 'Candidate'}) $lightingCameras $workload.id;VerifyNativeP4SharedCohorts @($nativeDiagnostics.ToArray()|Where-Object {$_.counters.mode -ceq 'Reference'}) $lightingCameras $workload.id
				VerifyNativeLightingCohorts $lightingCandidateCohorts.ToArray() $lightingCameras $false;VerifyNativeLightingCohorts $lightingReferenceCohorts.ToArray() $lightingCameras $false
				$cp=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$rp=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation;$ct=VerifyNativeControlTransitions $cp $cv $cv.identity $c.directory $selectedSteps $lightingCameras;$rt=VerifyNativeControlTransitions $rp $rv $rv.identity $r.directory $selectedSteps $lightingCameras;$delta=CompareNativeControlTransitions $ct $rt $cv.identity.resolution;$rawMaximumDepth=[Math]::Max($rawMaximumDepth,$delta.maxDepthAbsoluteError);$rawMaximumImage=[Math]::Max($rawMaximumImage,$delta.maxImageNormalizedChannelError)
			}
			$selectedSteps=@(GetRendererEfficiencyStageSteps $workload $stageName)
			$cArtifactIndex=$null;$rArtifactIndex=$null
			if(@($selectedSteps|Where-Object {$_.action -in @('origin','selected-lod','selected-meshlet','distinct-origins','alternate-view-order','different-caster-selections','alternate-snapshot-slot','B-record-overwrite-before-A-old-owner-hit','residency')}).Count){
				# Share these invocation-local metadata indexes across controls and
				# both residency observations. No file or readback result is cached.
				$cArtifactIndex=NewNativeArtifactClosureIndex $cv.artifacts;$rArtifactIndex=NewNativeArtifactClosureIndex $rv.artifacts
			}
			if(!(NativeP4SharedIdentity $cv.identity) -and !(NativeP5SharedIdentity $cv.identity) -and @($selectedSteps|Where-Object {$_.action -in @('origin','selected-lod','selected-meshlet','distinct-origins','alternate-view-order','different-caster-selections','alternate-snapshot-slot','B-record-overwrite-before-A-old-owner-hit')}).Count) {
				$cp=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$rp=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation
				$cameras=@($manifestValue.cameras|Where-Object {$_.id -in $workload.cameraIds})
				$ct=VerifyNativeControlTransitions $cp $cv $cv.identity $c.directory $selectedSteps $cameras $cArtifactIndex
				$rt=VerifyNativeControlTransitions $rp $rv $rv.identity $r.directory $selectedSteps $cameras $rArtifactIndex
				$transitionDelta=CompareNativeControlTransitions $ct $rt $cv.identity.resolution
				$rawMaximumDepth=[Math]::Max($rawMaximumDepth,$transitionDelta.maxDepthAbsoluteError);$rawMaximumImage=[Math]::Max($rawMaximumImage,$transitionDelta.maxImageNormalizedChannelError)
			}
			if(@($selectedSteps|Where-Object action -eq residency).Count) {
				$cp=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$rp=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation
				$cameras=@($manifestValue.cameras|Where-Object {$_.id -in $workload.cameraIds})
				$ct=VerifyNativeResidencyPublications $cp $cv $cv.identity $c.directory $selectedSteps $cameras $cArtifactIndex
				$rt=VerifyNativeResidencyPublications $rp $rv $rv.identity $r.directory $selectedSteps $cameras $rArtifactIndex
				$delta=CompareNativeControlTransitions $ct $rt $cv.identity.resolution
				$rawMaximumDepth=[Math]::Max($rawMaximumDepth,$delta.maxDepthAbsoluteError);$rawMaximumImage=[Math]::Max($rawMaximumImage,$delta.maxImageNormalizedChannelError)
			}
			# Every changed source must reach the real renderer owner in both runs.
			# Independent import order can change epochs, but never source bytes/IDs.
			if(@($selectedSteps|Where-Object {$_.action -in @('mesh-revision','page-revision','material-revision','texture-revision','cutout','shader-revision')}).Count) {
				$cp=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$rp=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation
				CompareNativeResourcePublications $cp $rp $selectedSteps @($nativeBundles.ToArray()|Where-Object mode -ceq Candidate) @($nativeBundles.ToArray()|Where-Object mode -ceq Reference)
				CompareNativeShaderPublications $cp $rp $selectedSteps @($nativeBundles.ToArray()|Where-Object mode -ceq Candidate) @($nativeBundles.ToArray()|Where-Object mode -ceq Reference)
			}
			foreach($step in $selectedSteps) {
				if(@($cframes|Where-Object {$_.fixtureStepId -eq $step.id}).Count -lt 300) { Unverified "Missing 300 frames for step $($step.id)" }
				if(@($cv.frames|Where-Object {$_.fixtureStepId -ceq $step.id -and (!(Has $_ 'evidenceKind') -or $_.evidenceKind -eq 'Shadow')}).Count){VerifyNativeStepCoverage $occupiedByStep[$step.id] $eligibleByStep[$step.id]}
				foreach($document in @($cv,$rv)) {
					$stepCameras=if($stageName -ceq 'P4' -and $workload.id -ceq 'shadow-batching'){@(GetNativeShadowBatchCameras $step.id $manifestValue.cameras)}elseif($stageName -ceq 'P5' -and $workload.id -ceq 'static-dynamic'){@(GetNativeStaticShadowCameras $step.id $manifestValue.cameras)}else{@($manifestValue.cameras|Where-Object {$_.id -in $workload.cameraIds})}
					foreach($camera in $stepCameras) {
						$cameraId=$camera.id
						foreach($phase in @('Warmup','Measured')) {
							$observations=@($document.frames|Where-Object {$_.fixtureStepId -eq $step.id -and $_.measurementPhase -eq $phase -and $_.viewType -eq $camera.viewType -and $_.viewId -eq $camera.viewId})
							$minimum=if($phase -eq 'Warmup'){60}else{300}
							$serials=@($observations|ForEach-Object {Metric $_.frameSerial 'frameSerial'}|Sort-Object -Unique)
							if($serials.Count -lt $minimum){Unverified "Missing $minimum distinct $phase frames for $($step.id)/$cameraId"}
							if($workload.id -eq 'multiview-lights') { foreach($face in 0..5) { if(@($observations|Where-Object {$_.faceIndex.status -eq 'Valid' -and $_.faceIndex.value -eq $face}|ForEach-Object {Metric $_.frameSerial 'frameSerial'}|Sort-Object -Unique).Count -lt $minimum){Unverified "Missing point-light face $face observations"} } }
						}
					}
				}
			}
			# Grade object recreation independently inside each run; import/lifetime clocks
			# remain deliberately independent across processes.
			if($stageName -eq 'P1' -and $workload.id -ceq 'shadow-invalidation') {
				$lifetimeCameras=@($manifestValue.cameras|Where-Object {$_.id -in $workload.cameraIds})
				VerifyNativeLifetimeReplacement @($nativeBundles.ToArray()|Where-Object mode -ceq Candidate) $selectedSteps $workload.id $lifetimeCameras
				VerifyNativeLifetimeReplacement @($nativeBundles.ToArray()|Where-Object mode -ceq Reference) $selectedSteps $workload.id $lifetimeCameras
			}
			# SC002 zero-draw grading belongs only to actual frozen stable actions.
			# Multiview origins and recovery faults can legitimately refresh resources.
			$stableStepIds=@($selectedSteps|Where-Object action -eq stable|ForEach-Object id)
			if($stageName -eq 'P1' -or $stageName -eq 'P5') {foreach($f in @($cframes|Where-Object {$_.fixtureStepId -in $stableStepIds})){
				$field=if($stageName -eq 'P5'){'staticRefreshCasterSegments'}else{'submittedCasterSegments'}
				NativeRequire ((Metric $f.shadow.$field $field) -eq 0) 'Stable shadow cache unexpectedly refreshed'
				if($stageName -eq 'P1'){NativeRequire ((Metric $f.shadow.actualDrawSubmissions actualDrawSubmissions) -eq 0 -and (Metric $f.shadow.reuseRegionCount shadowReuses) -eq 1) 'Stable native shadow lacks observed zero-draw reuse'}
			}}
			if($stageName -ceq 'P3'){
				$ce=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$re=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation
				$transitionDelta=CompareNativeDepthTransitions $ce $re $cv $rv $c.directory $r.directory $selectedSteps $lightingCameras $cDepthRecipes $rDepthRecipes
				$rawMaximumDepth=[Math]::Max($rawMaximumDepth,$transitionDelta.maxDepthAbsoluteError);$rawMaximumImage=[Math]::Max($rawMaximumImage,$transitionDelta.maxImageNormalizedChannelError)
				VerifyNativeLightingCohorts $lightingCandidateCohorts.ToArray() $lightingCameras $false;VerifyNativeLightingCohorts $lightingReferenceCohorts.ToArray() $lightingCameras $false
				VerifyNativeDepthCohorts $depthCandidateCohorts.ToArray() $lightingCandidateCohorts.ToArray() $lightingCameras
				VerifyNativeDepthCohorts $depthReferenceCohorts.ToArray() $lightingReferenceCohorts.ToArray() $lightingCameras
				if($timedDepth){VerifyNativeDepthSpeed $candidateTiming $referenceTiming}
			}
			if($stageName -ceq 'P5'){foreach($mode in @('Candidate','Reference')){VerifyNativeStaticRetirementTimeline @($nativeDiagnostics.ToArray()|Where-Object {$_.counters.mode -ceq $mode -and (!(Has $_.counters 'evidenceKind') -or $_.counters.evidenceKind -ceq 'Shadow')})}}
			if($stageName -ceq 'P5' -and $workload.id -ceq 'static-dynamic'){
				VerifyNativeStaticBudgetLowering $p5CandidateEnvelope $selectedSteps @($nativeDiagnostics.ToArray()|Where-Object {$_.counters.mode -ceq 'Candidate' -and (!(Has $_.counters 'evidenceKind') -or $_.counters.evidenceKind -ceq 'Shadow')})
				foreach($cohort in @($nativeBundles.ToArray()|Group-Object {"$($_.mode)|$($_.fixtureStepId)|$($_.measurementPhase)|$($_.stepFrame)"})){VerifyNativeStaticShadowRegions @($cohort.Group) $cohort.Group[0].fixtureStepId $manifestValue.cameras}
				$cp=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$rp=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation
				$ct=VerifyNativeControlTransitions $cp $cv $cv.identity $c.directory $selectedSteps $manifestValue.cameras;$rt=VerifyNativeControlTransitions $rp $rv $rv.identity $r.directory $selectedSteps $manifestValue.cameras
				$delta=CompareNativeControlTransitions $ct $rt $cv.identity.resolution;$rawMaximumDepth=[Math]::Max($rawMaximumDepth,$delta.maxDepthAbsoluteError);$rawMaximumImage=[Math]::Max($rawMaximumImage,$delta.maxImageNormalizedChannelError)
				$animatedSteps=@($selectedSteps|Where-Object action -CEQ 'animate-dynamic-stable-static')
				$staticEnvelope=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation
				foreach($step in $animatedSteps){VerifyNativeStaticAnimationWindow @($nativeDiagnostics.ToArray()|Where-Object {$_.counters.mode -ceq 'Candidate' -and $_.frame.fixtureStepId -ceq $step.id -and $_.frame.measurementPhase -ceq 'Measured'}) $staticEnvelope}
			}
			$stageCriteria=@{P1=@('SC-001','SC-002','SC-007','SC-009');P2=@('SC-003','SC-007','SC-009');P3=@('SC-004','SC-007','SC-009');P4=@('SC-005','SC-007','SC-009');P5=@('SC-001','SC-006','SC-007','SC-009')}
			$scs=@($workload.successCriteria|Where-Object { if($PostP1Baseline){$_ -eq 'SC-008'}else{$_ -in $stageCriteria[$stageName]} })
			foreach($sc in $scs) {
				$result=$cv.successCriteria.$sc
				# Root producer claims cannot replace raw proof. These two criteria
				# are established by complete matched native observations above.
				if($sc -in @('SC-001','SC-002')) {
					if($workload.id -eq 'recovery-capacity') {
						try {
							$cp=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$rp=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation
							VerifyNativeRecoveryReceipts $cp $selectedSteps;VerifyNativeRecoveryReceipts $rp $selectedSteps
						} catch {if(!$_.Exception.Message.StartsWith('UNVERIFIED: ')){throw};$anyUnverified=$true;$summary.comparisons+=@{fixtureId=$workload.id;runIndex=$run;preparation=$preparation;criterion=$sc;status='Unverified';reason=$_.Exception.Message};continue}
					}
					$summary.comparisons+=@{fixtureId=$workload.id;runIndex=$run;preparation=$preparation;criterion=$sc;status='Passed';reason='Independently verified native bytes, exact caster rows and required recovery receipts';coverageMismatchCount=0;casterIdentityMismatchCount=0;maxDepthAbsoluteError=$rawMaximumDepth;maxImageNormalizedChannelError=$rawMaximumImage}
					continue
				}
				if($sc -eq 'SC-006'){
					NativeRequire ($stageName -ceq 'P5') 'Static cache criterion outside P5'
					if($workload.id -ceq 'static-dynamic' -and $animatedSteps.Count -ne 1){Unverified 'SC006 lacks the frozen changing-animation action'}
					$summary.comparisons+=@{fixtureId=$workload.id;runIndex=$run;preparation=$preparation;criterion=$sc;status='Passed';reason='Exact native static/dynamic partitions, copy owners, physical budget/use retirement, complete final depth/color parity and required changing CPU animation independently verified';maxDepthAbsoluteError=$rawMaximumDepth;maxImageNormalizedChannelError=$rawMaximumImage};continue
				}
				if($sc -eq 'SC-003'){NativeRequire ($stageName -ceq 'P2' -and $null -ne $cLightingState) 'Native lighting baseline/transition proof unavailable';$summary.comparisons+=@{fixtureId=$workload.id;runIndex=$run;preparation=$preparation;criterion=$sc;status='Passed';reason='Full native completed payloads, retained first edits, actual all-slot coverage, zero stable writes and bounded atom flush accounting independently verified'};continue}
				if($sc -eq 'SC-004'){$summary.comparisons+=@{fixtureId=$workload.id;runIndex=$run;preparation=$preparation;criterion=$sc;status='Passed';reason=$(if($timedDepth){'Full native production/oracle/color proof and 300 nonempty isolated production queries meet the per-run 10% median gate'}else{'Full native depth/color parity and exact oracle input/visited/eligible bytes verified; this fixture makes no speed claim'})};continue}
				if($sc -eq 'SC-007') {
					try {
						if($preparationCounts.Serial -lt 3 -or $preparationCounts.Concurrent -lt 3){Unverified 'Three matched native Serial and Concurrent runs are required for this fixture'}
						$cp=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$rp=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation
						VerifyNativeSchedulerReceipt $cp $preparation;VerifyNativeSchedulerReceipt $rp $preparation
						foreach($native in $nativeBundles){VerifyNativePreparation $native $preparation}
						if($workload.id -eq 'recovery-capacity'){VerifyNativeRecoveryReceipts $cp $selectedSteps;VerifyNativeRecoveryReceipts $rp $selectedSteps}
						$summary.comparisons+=@{fixtureId=$workload.id;runIndex=$run;preparation=$preparation;criterion=$sc;status='Passed';reason='Actual settled task receipts, scheduler worker/protected/read/write/flush declarations and matched serial/concurrent matrix verified'}
					} catch {if(!$_.Exception.Message.StartsWith('UNVERIFIED: ')){throw};$anyUnverified=$true;$summary.comparisons+=@{fixtureId=$workload.id;runIndex=$run;preparation=$preparation;criterion=$sc;status='Unverified';reason=$_.Exception.Message}}
					continue
				}
				if($sc -eq 'SC-009') {
					try {
						$null=ReadNativeRunnerLifecycle $cv $cv.identity $c.directory $candidateOperation $preparation;$null=ReadNativeRunnerLifecycle $rv $rv.identity $r.directory $referenceOperation $preparation
						foreach($pair in $nativeDiagnostics){if(!(Has $pair.counters 'evidenceKind') -or $pair.counters.evidenceKind -ceq 'Shadow'){VerifyNativeShadowDiagnostics $pair.frame $pair.counters}elseif($pair.counters.evidenceKind -ceq 'Lighting'){NativeRequire (@($pair.frame.lighting|Where-Object hostWriteStatus -CNE Succeeded).Count -eq 0) 'Lighting diagnostics lack successful upload status'}}
						if($workload.id -eq 'recovery-capacity'){$cp=ReadNativeValidationReceipts $cv $cv.identity $c.directory $candidateOperation;$rp=ReadNativeValidationReceipts $rv $rv.identity $r.directory $referenceOperation;VerifyNativeRecoveryReceipts $cp $selectedSteps;VerifyNativeRecoveryReceipts $rp $selectedSteps}
						$summary.comparisons+=@{fixtureId=$workload.id;runIndex=$run;preparation=$preparation;criterion=$sc;status='Passed';reason='Hashed native refresh/fallback diagnostics, exact protected option restoration and owned normal editor close verified'}
					} catch {if(!$_.Exception.Message.StartsWith('UNVERIFIED: ')){throw};$anyUnverified=$true;$summary.comparisons+=@{fixtureId=$workload.id;runIndex=$run;preparation=$preparation;criterion=$sc;status='Unverified';reason=$_.Exception.Message}}
					continue
				}
				if($result.status -eq 'Unverified') { $anyUnverified=$true }
				if($result.status -eq 'Failed') { $anyFailed=$true }
				if($result.status -eq 'Passed') {
					Require ((Metric $result.coverageMismatchCount 'coverageMismatch') -eq 0 -and (Metric $result.casterIdentityMismatchCount 'casterMismatch') -eq 0) 'Reported coverage/identity mismatch'
					Require ((Metric $result.maxDepthAbsoluteError 'depthError') -le 0.000001 -and (Metric $result.maxImageNormalizedChannelError 'imageError') -le (2.0/255.0)) 'Reported correctness tolerance exceeded'
				}
				$summary.comparisons+=@{fixtureId=$workload.id;runIndex=$run;criterion=$sc;status=$result.status;reason=$result.reason}
			}
			if($timedDepth -or $timedBaseline) {
				$rm=Percentile $referenceTiming 0.5; $cm=Percentile $candidateTiming 0.5; Require ($rm -gt 0) 'Reference median must be positive'
				if($timedBaseline) { Require ($cm -le $rm*1.05 -and (Percentile $candidateTiming 0.95) -le (Percentile $referenceTiming 0.95)*1.05) 'Total frame median/p95 regression exceeds 5%' }
				else { Require ($cm -le $rm*0.9) 'Depth GPU stage median improvement below 10%' }
			}
			# P4 reduction was checked on every complete paired native region above;
			# no aggregate metric sum can hide a failed face, view, or frame.
			if($stageName -eq 'P2'){foreach($step in $selectedSteps){VerifyNativeLightingCohorts @($lightingCandidateCohorts.ToArray()|Where-Object {$_.frame.fixtureStepId -ceq $step.id}) $lightingCameras $true;VerifyNativeLightingCohorts @($lightingReferenceCohorts.ToArray()|Where-Object {$_.frame.fixtureStepId -ceq $step.id}) $lightingCameras $true}}
			# Release pair graphs and last-loop aliases before parsing the next pair.
			# This offline comparison boundary has no renderer/GPU waiting work.
			$c=$r=$cv=$rv=$cframes=$rframes=$referenceByKey=$nativeBundles=$nativeDiagnostics=$cLightingState=$rLightingState=$lightingCandidateCohorts=$lightingReferenceCohorts=$ctLighting=$rtLighting=$null
			$p5CandidateEnvelope=$p5ReferenceEnvelope=$null
			$document=$observed=$observations=$frame=$other=$warmup=$f=$cproof=$rproof=$proof=$entry=$cp=$rp=$native=$pair=$null
			[GC]::Collect();[GC]::WaitForPendingFinalizers()
		}
	}
	Require ($validatedInputs.Count -eq $refs.Count+$candidates.Count) 'Provided evidence input was not fully validated in a matched pair'
	$summary.exitCode=if($anyFailed){1}elseif($anyUnverified){2}else{0};$summary.status=if($summary.exitCode -eq 0){'Passed'}elseif($summary.exitCode -eq 1){'Failed'}else{'Unverified'}
} catch {
	$summary.reason=$_.Exception.Message
	if($summary.reason.StartsWith('UNVERIFIED: ')) {$summary.exitCode=2;$summary.status='Unverified'} else {$summary.exitCode=3;$summary.status='Invalid'}
	if($summary.reason.StartsWith('NATIVE_FAILED: ')) {$summary.exitCode=1;$summary.status='Failed'}
	if($summary.reason -match 'exceeds|below 10%|below 90%|unexpectedly refreshed|writes are nonzero|Coverage bytes differ|mismatch$|Completion precedes') {$summary.exitCode=1;$summary.status='Failed'}
}
$parent=Split-Path ([IO.Path]::GetFullPath($Output)); if(!(Test-Path -LiteralPath $parent)) { [IO.Directory]::CreateDirectory($parent)|Out-Null }
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Output),($summary|ConvertTo-Json -Depth 100),$utf8)
Write-Output ($summary|ConvertTo-Json -Depth 100 -Compress)
exit $summary.exitCode
