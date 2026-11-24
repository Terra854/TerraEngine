#requires -Version 7.4
<# Offline native-capture image-quality scorer. Requires Windows and PowerShell
7.4+ (its bundled .NET runtime); uses the built-in Windows PNG decoder. It reads
phase-tagged captures and never launches or mutates an engine. See the committed
Tests/Fixtures/PostProcessing/MovingEdgesFixture.md for the complete input schema.

pwsh -NoProfile -File Tests/Tools/PostProcessing/TestPostProcessImageQuality.ps1 `
  -SelfTest -SelfTestOutput out/validation/post-processing/quality-selftest.json
pwsh -NoProfile -File Tests/Tools/PostProcessing/TestPostProcessImageQuality.ps1 `
  -Manifest <owned-captures>/manifest.json -Output <fresh-score.json> -Modes FXAA

Use a fresh PowerShell process. Self-tests exercise numerical fixtures only;
neither numerical success nor image scores replace the native acceptance gates.
#>
[CmdletBinding(DefaultParameterSetName='Score')]
param(
	[Parameter(Mandatory,ParameterSetName='Score')][string]$Manifest,
	[Parameter(Mandatory,ParameterSetName='Score')][string]$Output,
	[Parameter(Mandatory,ParameterSetName='SelfTest')][switch]$SelfTest,
	[Parameter(ParameterSetName='SelfTest')][string]$SelfTestOutput,
	[Parameter(ParameterSetName='Score')][ValidateSet('FXAA','SMAA','CMAA2','MSAA','TAA','TemporalUpscale')]
	[string[]]$Modes=@('FXAA','SMAA','CMAA2','MSAA','TAA','TemporalUpscale')
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if(!$IsWindows){throw 'This scorer uses the built-in Windows PNG decoder.'}
$frozenFixtureSha256='2ab4d4557f4f9a38069d21dfc314388248c153580bbd7d5662e32190caf60787'
$frozenFixturePath=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../Fixtures/PostProcessing/MovingEdgesPoses.json'))

# Pixel loops are typed .NET kernels. Native captures and reference arrays are
# immutable inputs. Only tiny result/provenance objects cross PowerShell.
$kernelSource=@'
using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Security.Cryptography;
using System.Runtime.InteropServices;
using System.Collections.Generic;
namespace TerraEngine.Tests.PostProcessImageQuality
{
	public sealed class RgbImage
	{
		public int Width,Height;public double[] Rgb;
		public RgbImage(int width,int height,double[] rgb){Kernels.Need(width>0&&height>0&&rgb.Length==checked(width*height*3),"RGB extent mismatch");Kernels.Need(rgb.All(Kernels.Normalized),"Invalid normalized display-linear RGB");Width=width;Height=height;Rgb=rgb;}
	}
	public sealed class Frame
	{
		public string Sequence;public long Phase;public ulong Generation;public double[] Reference,Candidate;public byte[] Mask;
		public Frame(string sequence,long phase,ulong generation,double[] reference,byte[] mask,double[] candidate){Sequence=sequence;Phase=phase;Generation=generation;Reference=reference;Mask=mask;Candidate=candidate;}
	}
	public sealed class Score
	{
		public double EdgeAbsoluteErrorSum,EdgeMean,ShimmerAbsoluteChangeSum,ShimmerMean;public long EdgeSamples,ShimmerSamples;public string[] MaskSha256,ReferenceSha256;
	}
	public sealed class Comparison { public double EdgeRatioToOff;public double? ShimmerRatioToOff;public bool Passed; }
	public sealed class ResetScore { public long Samples;public double MaximumLuminanceDifference;public double Limit=1.0/255;public bool Passed; }
	public static class Kernels
	{
		public static void Need(bool condition,string message){if(!condition)throw new InvalidDataException(message);}
		public static bool Normalized(double v){return !double.IsNaN(v)&&!double.IsInfinity(v)&&v>=0&&v<=1;}
		public static double Decode(double v){return v<=.04045?v/12.92:Math.Pow((v+.055)/1.055,2.4);}
		public static double Encode(double v){return v<=.0031308?v*12.92:1.055*Math.Pow(v,1.0/2.4)-.055;}
		public static string Hash(byte[] bytes){return Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();}
		static string HashDoubles(double[] values){var bytes=new byte[checked(values.Length*8)];Buffer.BlockCopy(values,0,bytes,0,bytes.Length);if(!BitConverter.IsLittleEndian)for(int i=0;i<bytes.Length;i+=8)Array.Reverse(bytes,i,8);return Hash(bytes);}
		// Neumaier compensation keeps global sample-weighted reductions stable.
		struct Sum { double total,correction;public void Add(double v){double n=total+v;correction+=Math.Abs(total)>=Math.Abs(v)?(total-n)+v:(v-n)+total;total=n;}public double Value{get{return total+correction;}} }
		public static RgbImage Gray(int width,int height,double[] values){Need(values.Length==width*height,"Gray extent mismatch");var rgb=new double[values.Length*3];for(int i=0;i<values.Length;i++)for(int c=0;c<3;c++)rgb[i*3+c]=values[i];return new RgbImage(width,height,rgb);}
		public static double[] Luminance(RgbImage image){var a=new double[image.Width*image.Height];for(int i=0;i<a.Length;i++)a[i]=image.Rgb[i*3]*.2126+image.Rgb[i*3+1]*.7152+image.Rgb[i*3+2]*.0722;return a;}
		public static RgbImage Box4(RgbImage source)
		{
			// Decode first, then average each exact 4x4 RGB footprint. Luminance
			// conversion happens afterward; encoded-space filtering is never used.
			Need(source.Width%4==0&&source.Height%4==0,"Reference must be exactly 4x on each axis");int w=source.Width/4,h=source.Height/4;var rgb=new double[w*h*3];
			for(int y=0;y<h;y++)for(int x=0;x<w;x++)for(int c=0;c<3;c++){var sum=new Sum();for(int dy=0;dy<4;dy++)for(int dx=0;dx<4;dx++)sum.Add(source.Rgb[((y*4+dy)*source.Width+x*4+dx)*3+c]);rgb[(y*w+x)*3+c]=sum.Value/16;}return new RgbImage(w,h,rgb);
		}
		public static RgbImage Enlarge2(RgbImage source,int width,int height)
		{
			// Output pixel centers map to (x+.5)/2-.5 on the source grid.
			// Clamp the two source neighbors, without shifting either output image.
			Need(width==source.Width*2&&height==source.Height*2,"Temporal Off comparator must enlarge exactly 2x");var rgb=new double[width*height*3];
			for(int y=0;y<height;y++){double fy=(y+.5)*source.Height/height-.5;int y0=(int)Math.Floor(fy);double ty=fy-y0;int ya=Math.Clamp(y0,0,source.Height-1),yb=Math.Clamp(y0+1,0,source.Height-1);
				for(int x=0;x<width;x++){double fx=(x+.5)*source.Width/width-.5;int x0=(int)Math.Floor(fx);double tx=fx-x0;int xa=Math.Clamp(x0,0,source.Width-1),xb=Math.Clamp(x0+1,0,source.Width-1);
					for(int c=0;c<3;c++){double a=source.Rgb[(ya*source.Width+xa)*3+c]*(1-tx)+source.Rgb[(ya*source.Width+xb)*3+c]*tx,b=source.Rgb[(yb*source.Width+xa)*3+c]*(1-tx)+source.Rgb[(yb*source.Width+xb)*3+c]*tx;rgb[(y*width+x)*3+c]=a*(1-ty)+b*ty;}}}return new RgbImage(width,height,rgb);
		}
		public static byte[] EdgeMask(double[] reference,int width,int height,int[] crop)
		{
			// The seed uses four axial neighbors. Its dilation is a full 3x3
			// square clipped to the frozen crop, not a second axial-neighbor pass.
			Need(crop.Length==4&&reference.Length==width*height&&0<crop[0]&&crop[0]<crop[2]&&crop[2]<width&&0<crop[1]&&crop[1]<crop[3]&&crop[3]<height,"Invalid fixed interior crop");var seed=new byte[reference.Length];var mask=new byte[reference.Length];
			for(int y=crop[1];y<crop[3];y++)for(int x=crop[0];x<crop[2];x++){int i=y*width+x;if(Math.Abs(reference[i]-reference[i-1])>=.125||Math.Abs(reference[i]-reference[i+1])>=.125||Math.Abs(reference[i]-reference[i-width])>=.125||Math.Abs(reference[i]-reference[i+width])>=.125)seed[i]=1;}
			for(int y=crop[1];y<crop[3];y++)for(int x=crop[0];x<crop[2];x++)if(seed[y*width+x]!=0)for(int yy=Math.Max(crop[1],y-1);yy<Math.Min(crop[3],y+2);yy++)for(int xx=Math.Max(crop[0],x-1);xx<Math.Min(crop[2],x+2);xx++)mask[yy*width+xx]=1;
			Need(mask.Any(v=>v!=0),"Empty reference mask invalidates the frame");return mask;
		}
		public static Score Measure(Frame[] frames)
		{
			// Accumulate pixel samples globally rather than averaging frame means.
			// Shimmer subtracts each reference first and uses both masks' union.
			Need(frames.Length>=2,"Missing continuous frames");var keys=new HashSet<string>();int count=frames[0].Reference.Length;var edge=new Sum();var shimmer=new Sum();long ec=0,sc=0;var masks=new List<string>();var references=new List<string>();double[] previousError=null;Frame previous=null;
			foreach(var f in frames){Need(keys.Add(f.Sequence+"\0"+f.Phase+"\0"+f.Generation),"Duplicate phase/generation key");Need(f.Reference.Length==count&&f.Candidate.Length==count&&f.Mask.Length==count&&f.Mask.Any(v=>v!=0),"Incompatible common grid or empty mask");Need(f.Reference.All(Normalized)&&f.Candidate.All(Normalized)&&f.Mask.All(v=>v==0||v==1),"Invalid luminance or nonbinary mask");var error=new double[count];for(int i=0;i<count;i++){error[i]=f.Candidate[i]-f.Reference[i];if(f.Mask[i]!=0){edge.Add(Math.Abs(error[i]));ec++;}}masks.Add(Hash(f.Mask));references.Add(HashDoubles(f.Reference));
				if(previous!=null){Need(f.Sequence==previous.Sequence&&f.Generation==previous.Generation&&f.Phase==previous.Phase+1,"Nonadjacent phase or changed logical generation");for(int i=0;i<count;i++)if(f.Mask[i]!=0||previous.Mask[i]!=0){shimmer.Add(Math.Abs(error[i]-previousError[i]));sc++;}}previous=f;previousError=error;}
			return new Score{EdgeAbsoluteErrorSum=edge.Value,EdgeSamples=ec,EdgeMean=edge.Value/ec,ShimmerAbsoluteChangeSum=shimmer.Value,ShimmerSamples=sc,ShimmerMean=shimmer.Value/sc,MaskSha256=masks.ToArray(),ReferenceSha256=references.ToArray()};
		}
		public static Comparison Compare(Score candidate,Score off,bool temporal)
		{
			Need(candidate.MaskSha256.SequenceEqual(off.MaskSha256)&&candidate.ReferenceSha256.SequenceEqual(off.ReferenceSha256)&&candidate.EdgeSamples==off.EdgeSamples,"Candidate changed frozen reference/masks");Need(off.EdgeMean>0&&(!temporal||off.ShimmerMean>0),"Zero Off denominator invalidates fixture");double e=candidate.EdgeMean/off.EdgeMean;double? s=temporal?candidate.ShimmerMean/off.ShimmerMean:(double?)null;return new Comparison{EdgeRatioToOff=e,ShimmerRatioToOff=s,Passed=e<=.99&&(!temporal||s.Value<=.99)};
		}
		public static ResetScore FreshReset(RgbImage first,RgbImage fresh,int[] crop)
		{
			// Resets compare the maximum difference over every crop pixel, including
			// non-edge interiors. No registration, warp, or history approximation.
			Need(first.Width==fresh.Width&&first.Height==fresh.Height&&crop.Length==4&&0<=crop[0]&&crop[0]<crop[2]&&crop[2]<=first.Width&&0<=crop[1]&&crop[1]<crop[3]&&crop[3]<=first.Height,"Invalid reset/fresh extent/crop");var a=Luminance(first);var b=Luminance(fresh);double maximum=0;long samples=0;for(int y=crop[1];y<crop[3];y++)for(int x=crop[0];x<crop[2];x++){maximum=Math.Max(maximum,Math.Abs(a[y*first.Width+x]-b[y*first.Width+x]));samples++;}return new ResetScore{Samples=samples,MaximumLuminanceDifference=maximum,Passed=maximum<=1.0/255};
		}

		// GDI+ is Windows' built-in decoder. Use the non-ICM entry after strict
		// PNG metadata/CRC preflight; no implicit profile/gamma conversion occurs.
		[StructLayout(LayoutKind.Sequential)]struct Startup{public uint Version;public IntPtr Callback;public int SuppressThread,SuppressCodecs;}
		[StructLayout(LayoutKind.Sequential)]struct Rect{public int X,Y,Width,Height;}
		[StructLayout(LayoutKind.Sequential)]struct BitmapData{public uint Width,Height;public int Stride,Format;public IntPtr Scan0;public UIntPtr Reserved;}
		[DllImport("gdiplus.dll",ExactSpelling=true)]static extern int GdiplusStartup(out UIntPtr token,ref Startup input,IntPtr output);
		[DllImport("gdiplus.dll",ExactSpelling=true)]static extern void GdiplusShutdown(UIntPtr token);
		[DllImport("gdiplus.dll",ExactSpelling=true,CharSet=CharSet.Unicode)]static extern int GdipCreateBitmapFromFile(string path,out IntPtr bitmap);
		[DllImport("gdiplus.dll",ExactSpelling=true)]static extern int GdipDisposeImage(IntPtr image);
		[DllImport("gdiplus.dll",ExactSpelling=true)]static extern int GdipBitmapLockBits(IntPtr bitmap,ref Rect rect,uint flags,int format,out BitmapData data);
		[DllImport("gdiplus.dll",ExactSpelling=true)]static extern int GdipBitmapUnlockBits(IntPtr bitmap,ref BitmapData data);
		static uint Be32(byte[] b,int i){return((uint)b[i]<<24)|((uint)b[i+1]<<16)|((uint)b[i+2]<<8)|b[i+3];}
		static uint Crc(byte[] b,int start,int length){uint c=0xffffffff;for(int i=start;i<start+length;i++){c^=b[i];for(int n=0;n<8;n++)c=(c&1)!=0?(c>>1)^0xedb88320:c>>1;}return~c;}
		static void Preflight(byte[] b,int width,int height,string encoding)
		{
			Need(b.Length>=8&&b.Take(8).SequenceEqual(new byte[]{137,80,78,71,13,10,26,10})&&(encoding=="srgb"||encoding=="linear"),"Unsupported PNG/transfer");bool header=false,ended=false;int offset=8;
			while(offset<b.Length){Need(offset<=b.Length-12,"Truncated PNG chunk");uint length=Be32(b,offset);Need(length<=int.MaxValue&&(long)offset+12+length<=b.Length,"Truncated PNG payload");int n=(int)length,end=offset+8+n;string kind=Encoding.ASCII.GetString(b,offset+4,4);Need(Crc(b,offset+4,n+4)==Be32(b,end),"PNG CRC mismatch");Need(header||kind=="IHDR","First PNG chunk must be IHDR");
				if(kind=="IHDR"){Need(!header&&n==13,"Duplicate/malformed PNG header");header=true;Need(Be32(b,offset+8)==width&&Be32(b,offset+12)==height,"PNG native extent mismatch");Need(b[offset+16]==8&&(b[offset+17]==2||b[offset+17]==6)&&b[offset+18]==0&&b[offset+19]==0&&b[offset+20]==0,"Unsupported PNG color/depth/interlace");}
				else if(kind=="iCCP")throw new InvalidDataException("Embedded ICC requires explicit common-space conversion");else if(kind=="sRGB")Need(encoding=="srgb","PNG transfer conflicts with native metadata");else if(kind=="gAMA")Need(n==4&&Be32(b,offset+8)==(encoding=="srgb"?45455u:100000u),"PNG gamma conflicts with native metadata");else if(kind=="IEND"){Need(end+4==b.Length,"Trailing PNG bytes");ended=true;break;}else if(kind!="IDAT"&&char.IsUpper(kind[0]))throw new InvalidDataException("Unsupported critical PNG chunk");offset=end+4;}
			Need(header&&ended,"Incomplete PNG");
		}
		public static RgbImage Load(string path,string format,string encoding,int width,int height,string expectedHash)
		{
			Need(width>0&&width<=2560&&height>0&&height<=1440,"Extent outside fixed protocol");
			// Hold a read-only share throughout hashing and native decode, so neither
			// replacement nor writing can change the file between those operations.
			using(var file=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read))
			{
				Need(file.Length<=128*1024*1024,"Capture exceeds fixed byte bound");var bytes=new byte[checked((int)file.Length)];file.ReadExactly(bytes);Need(Hash(bytes)==expectedHash,"Capture SHA mismatch");var rgb=new double[checked(width*height*3)];
				if(format=="RGBA32F"){Need(encoding=="linear"&&bytes.Length==checked(width*height*16),"RGBA32F extent/transfer mismatch");for(int i=0;i<width*height;i++){float alpha=BitConverter.ToSingle(bytes,i*16+12);Need(!float.IsNaN(alpha)&&Math.Abs(alpha-1)<=1e-6,"Opaque fixture needs alpha one");for(int c=0;c<3;c++)rgb[i*3+c]=BitConverter.ToSingle(bytes,i*16+c*4);}return new RgbImage(width,height,rgb);}
				Need(format=="PNG8","Unsupported lossless format");Preflight(bytes,width,height,encoding);var startup=new Startup{Version=1};UIntPtr token;Need(GdiplusStartup(out token,ref startup,IntPtr.Zero)==0,"Windows decoder startup failed");IntPtr bitmap=IntPtr.Zero;
				try{Need(GdipCreateBitmapFromFile(path,out bitmap)==0,"Windows PNG decoding failed");var rect=new Rect{Width=width,Height=height};BitmapData data;Need(GdipBitmapLockBits(bitmap,ref rect,1,2498570,out data)==0,"Windows PNG pixel access failed");
					try{var row=new byte[checked(width*4)];for(int y=0;y<height;y++){Marshal.Copy(IntPtr.Add(data.Scan0,checked(y*data.Stride)),row,0,row.Length);for(int x=0;x<width;x++){Need(row[x*4+3]==255,"Opaque fixture rejects translucent/premultiplied pixels");for(int c=0;c<3;c++){double v=row[x*4+2-c]/255.0;rgb[(y*width+x)*3+c]=encoding=="srgb"?Decode(v):v;}}}}
					finally{Need(GdipBitmapUnlockBits(bitmap,ref data)==0,"Windows PNG pixel release failed");}}
				finally{if(bitmap!=IntPtr.Zero)GdipDisposeImage(bitmap);GdiplusShutdown(token);}return new RgbImage(width,height,rgb);
			}
		}

		// Synthetic images below exercise numeric/decoder invariants only. They
		// cannot enter the native manifest gate or claim real AA quality.
		static void BeWrite(Stream s,uint v){s.WriteByte((byte)(v>>24));s.WriteByte((byte)(v>>16));s.WriteByte((byte)(v>>8));s.WriteByte((byte)v);}
		static void Chunk(Stream s,string kind,byte[] payload){BeWrite(s,(uint)payload.Length);var data=Encoding.ASCII.GetBytes(kind).Concat(payload).ToArray();s.Write(data);BeWrite(s,Crc(data,0,data.Length));}
		public static byte[] SyntheticPng(int width,int height,byte[] pixels,int filter)
		{
			Need(filter>=0&&filter<=4&&pixels.Length==width*height*4,"Synthetic PNG input mismatch");int stride=width*4;
			using(var raw=new MemoryStream()){for(int y=0;y<height;y++){raw.WriteByte((byte)filter);for(int i=0;i<stride;i++){int left=i>=4?pixels[y*stride+i-4]:0,up=y>0?pixels[(y-1)*stride+i]:0,ul=y>0&&i>=4?pixels[(y-1)*stride+i-4]:0;int p=left+up-ul,pa=Math.Abs(p-left),pb=Math.Abs(p-up),pc=Math.Abs(p-ul);int predictor=filter==1?left:filter==2?up:filter==3?(left+up)/2:filter==4?(pa<=pb&&pa<=pc?left:pb<=pc?up:ul):0;raw.WriteByte((byte)((pixels[y*stride+i]-predictor)&255));}}
				using(var compressed=new MemoryStream()){using(var z=new ZLibStream(compressed,CompressionLevel.Optimal,true))z.Write(raw.ToArray());using(var output=new MemoryStream()){output.Write(new byte[]{137,80,78,71,13,10,26,10});using(var header=new MemoryStream()){BeWrite(header,(uint)width);BeWrite(header,(uint)height);header.Write(new byte[]{8,6,0,0,0});Chunk(output,"IHDR",header.ToArray());}Chunk(output,"IDAT",compressed.ToArray());Chunk(output,"IEND",Array.Empty<byte>());return output.ToArray();}}}
		}
		public static byte[] SyntheticGamma(byte[] png,uint gamma)
		{
			// Numerical test helper: insert a correctly CRC-tagged gamma declaration
			// immediately after IHDR, so conflicting metadata is rejected explicitly.
			using(var output=new MemoryStream())
			{ output.Write(png,0,33);using(var payload=new MemoryStream()){BeWrite(payload,gamma);Chunk(output,"gAMA",payload.ToArray());}output.Write(png,33,png.Length-33);return output.ToArray(); }
		}
		public static void CheckKnownCrc(){var bytes=Encoding.ASCII.GetBytes("123456789");Need(Crc(bytes,0,bytes.Length)==0xcbf43926,"CRC polynomial does not match its independent known vector");}
		public static RgbImage SyntheticRaster(int width,int height,int phase)
		{
			var v=new double[width*height];for(int y=0;y<height;y++)for(int x=0;x<width;x++){double xx=(x+.5)*64/width,yy=(y+.5)*36/height;double color=xx>19.375+.4375*yy+phase*.3125?.85:.04;double rail=39.125+phase*.375+yy*.125;if(rail<=xx&&xx<rail+.75)color=.01;v[y*width+x]=color;}return Gray(width,height,v);
		}
	}
}
'@
Add-Type -TypeDefinition $kernelSource
$K=[TerraEngine.Tests.PostProcessImageQuality.Kernels]

function Require-Aa([bool]$Condition,[string]$Message){if(!$Condition){throw [IO.InvalidDataException]::new($Message)}}
function Test-AaInteger($Value){return $Value -is [int] -or $Value -is [long] -or $Value -is [uint32] -or $Value -is [uint64]}
function Test-AaFlag($Value,[bool]$Expected){return $Value -is [bool] -and $Value -eq $Expected}
function Read-AaNativeIdentity($Record,$Expected,[hashtable]$NativeMap,[bool]$Capture=$true){
	Require-Aa ((Test-AaInteger $Record.phase) -and $Record.phase -ge 0 -and (Test-AaInteger $Record.viewGeneration) -and $Record.viewGeneration -gt 0) 'Missing typed phase/logical generation'
	Require-Aa ($Record.sequence -is [string] -and $Record.poseSha256 -is [string] -and $Record.sequence -ceq $Expected.sequence -and $Record.phase -eq $Expected.phase -and $Record.viewGeneration -eq $Expected.viewGeneration -and $Record.poseSha256 -ceq $Expected.poseSha256) 'Missing/mismatched frozen phase/generation/pose'
	if($Capture){Require-Aa ((Test-AaFlag $Record.nativeBackendReadback $true) -and (Test-AaFlag $Record.deterministicFallback $false)) 'Synthetic/fallback capture cannot qualify AA'}else{Require-Aa (Test-AaFlag $Record.nativeRenderAccepted $true) 'Missing accepted warmup receipt'}
	$identity=@($Record.nativeRendererSession,$Record.nativeViewId,$Record.nativeViewGeneration)
	foreach($v in $identity){$n=[uint64]0;Require-Aa ($v -is [string] -and $v -cmatch '^(0|[1-9][0-9]*)$' -and [uint64]::TryParse($v,[Globalization.NumberStyles]::None,[Globalization.CultureInfo]::InvariantCulture,[ref]$n)) 'Native identity is not canonical unsigned decimal'}
	Require-Aa ([uint64]$identity[0] -gt 0 -and [uint64]$identity[2] -gt 0) 'Native session/generation must be nonzero; output zero remains valid'
	$key=[string]$Record.viewGeneration
	if($NativeMap.ContainsKey($key)){Require-Aa (($NativeMap[$key]-join '|') -ceq ($identity-join '|')) 'Logical generation changed native identity'}else{foreach($prior in $NativeMap.Values){Require-Aa (($prior-join '|') -cne ($identity-join '|')) 'New logical view reused native identity'};$NativeMap[$key]=$identity}
	return ,$identity
}
function Resolve-AaInput([string]$Root,[string]$Relative){$path=[IO.Path]::GetFullPath((Join-Path $Root $Relative));Require-Aa ($path.StartsWith($Root.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) 'Input escapes owned evidence';return $path}
function Test-AaExtent($Value,[int[]]$Expected){return $Value -is [Array] -and $Value.Count -eq 2 -and (Test-AaInteger $Value[0]) -and (Test-AaInteger $Value[1]) -and $Value[0] -eq $Expected[0] -and $Value[1] -eq $Expected[1]}
function Read-AaImage([string]$Root,$Record,[int[]]$Extent){Require-Aa ((Test-AaExtent $Record.extent $Extent) -and $Record.colorSpace -is [string] -and $Record.signalDomain -is [string] -and $Record.colorSpace -ceq 'BT709' -and $Record.signalDomain -ceq 'display-referred') 'Incompatible native extent/domain/space';Require-Aa ($Record.file -is [string] -and $Record.format -is [string] -and $Record.encoding -is [string] -and $Record.sha256 -is [string] -and $Record.sha256 -cmatch '^[0-9a-f]{64}$') 'Missing typed immutable capture fields';return $K::Load((Resolve-AaInput $Root $Record.file),$Record.format,$Record.encoding,$Extent[0],$Extent[1],$Record.sha256)}
function Get-AaConfiguration([string]$Name){
	if($Name -ceq 'Reference4x'){return @{mode='Off';sampleCount=1;internalExtent=@(2560,1440);outputExtent=@(2560,1440)}}
	if($Name -ceq 'OffHalf'){return @{mode='Off';sampleCount=1;internalExtent=@(320,180);outputExtent=@(320,180)}}
	if($Name -ceq 'OffNative'){return @{mode='Off';sampleCount=1;internalExtent=@(640,360);outputExtent=@(640,360)}}
	return @{mode=$Name;sampleCount=$(if($Name -ceq 'MSAA'){4}else{1});internalExtent=$(if($Name -ceq 'TemporalUpscale'){@(320,180)}else{@(640,360)});outputExtent=@(640,360)}
}
function Test-AaConfiguration($Actual,$Expected){return $Actual -is [Collections.IDictionary] -and $Actual.Count -eq 4 -and $Actual.mode -is [string] -and $Actual.mode -ceq $Expected.mode -and (Test-AaInteger $Actual.sampleCount) -and $Actual.sampleCount -eq $Expected.sampleCount -and (Test-AaExtent $Actual.internalExtent $Expected.internalExtent) -and (Test-AaExtent $Actual.outputExtent $Expected.outputExtent)}
function Read-AaRun([string]$Name,$Run,$Fixture){
	$config=Get-AaConfiguration $Name;Require-Aa ((Test-AaConfiguration $Run.effective $config) -and $Run.controlsSha256 -is [string] -and $Run.controlsSha256 -ceq $Fixture.controlsSha256) 'Mode/sample/extents/unrelated controls changed';$map=@{}
	Require-Aa ($Run.warmupRenderReceipts.Count -eq 64) 'Missing fixed warmup receipts'
	for($i=0;$i -lt 64;$i++){$null=Read-AaNativeIdentity $Run.warmupRenderReceipts[$i] $Fixture.poses[$i] $map $false;Require-Aa (Test-AaConfiguration $Run.warmupRenderReceipts[$i].effective $config) 'Mode changed during warmup'}
	Require-Aa ($Run.frames.Count -eq 120) 'Missing/extra continuous captures';$records=@{}
	foreach($f in $Run.frames){Require-Aa ((Test-AaInteger $f.phase) -and $f.phase -ge 64 -and $f.phase -lt 184 -and !$records.ContainsKey([string]$f.phase)) 'Unexpected/duplicate phase';$null=Read-AaNativeIdentity $f $Fixture.poses[[int]$f.phase] $map;Require-Aa (Test-AaConfiguration $f.effective $config) 'Measured effective mode changed';$nativeFrame=[uint64]0;Require-Aa ($f.backendFrameId -is [string] -and $f.backendFrameId -cmatch '^[1-9][0-9]*$' -and [uint64]::TryParse($f.backendFrameId,[Globalization.NumberStyles]::None,[Globalization.CultureInfo]::InvariantCulture,[ref]$nativeFrame)) 'Missing full-width native frame ID';$records[[string]$f.phase]=$f}
	return @{configuration=$config;records=$records;actualNativeIdentities=$map}
}
function Read-AaResetPairs([string]$Root,$Pairs,$Fixture,[string[]]$Selected){
	$results=[Collections.Generic.List[object]]::new();$seen=@{}
	foreach($p in $Pairs){Require-Aa ($p.mode -cin @('TAA','TemporalUpscale') -and $p.mode -cin $Selected -and $p.reason -cin @('cut','resize')) 'Unexpected reset pair';$key=$p.mode+'|'+$p.reason;Require-Aa (!$seen.ContainsKey($key)) 'Duplicate reset pair';$seen[$key]=$true
		$spec=$Fixture.separateSequences[$p.reason];$expected=$spec.frames[[int]$spec.resetPhase];$map=@{};$firstId=Read-AaNativeIdentity $p.first $expected $map
		$freshExpected=@{sequence=$p.reason+'-fresh';phase=$expected.phase;viewGeneration=$spec.freshLogicalViewGeneration;poseSha256=$expected.poseSha256};$freshId=Read-AaNativeIdentity $p.fresh $freshExpected $map;Require-Aa (($firstId-join '|') -cne ($freshId-join '|')) 'Fresh replay reused actual native view'
		$resetConfig=Get-AaConfiguration $p.mode;$resetConfig.outputExtent=[int[]]$expected.pose.outputExtent
		if($p.mode -ceq 'TemporalUpscale'){
			Require-Aa ($resetConfig.outputExtent[0]%2 -eq 0 -and $resetConfig.outputExtent[1]%2 -eq 0) 'Reset extent cannot use the required half-resolution temporal input'
			$resetConfig.internalExtent=[int[]]@(($resetConfig.outputExtent[0]/2),($resetConfig.outputExtent[1]/2))
		}else{$resetConfig.internalExtent=[int[]]$resetConfig.outputExtent}
		Require-Aa ((Test-AaConfiguration $p.first.effective $resetConfig) -and (Test-AaConfiguration $p.fresh.effective $resetConfig)) 'Reset/fresh actual mode, sample count or extents changed'
		foreach($field in @('poseSha256','simulationStateSha256','effectiveSettingsSha256')){Require-Aa ($p.first[$field] -is [string] -and $p.fresh[$field] -is [string] -and $p.first[$field] -cmatch '^[0-9a-f]{64}$' -and $p.first[$field] -ceq $p.fresh[$field]) "Reset/fresh changed $field"}
		Require-Aa ((Test-AaExtent $p.first.extent $expected.pose.outputExtent) -and (Test-AaExtent $p.fresh.extent $expected.pose.outputExtent) -and (Test-AaFlag $p.first.firstEffectiveFrameAfterReset $true) -and (Test-AaInteger $p.fresh.acceptedViewFrameIndex) -and $p.fresh.acceptedViewFrameIndex -eq 0 -and (Test-AaInteger $p.first.jitterPhase) -and (Test-AaInteger $p.fresh.jitterPhase) -and $p.first.jitterPhase -eq 0 -and $p.fresh.jitterPhase -eq 0) 'Not matching first effective/jitter frames'
		$score=$K::FreshReset((Read-AaImage $Root $p.first $expected.pose.outputExtent),(Read-AaImage $Root $p.fresh $expected.pose.outputExtent),[int[]]$spec.crop);$results.Add(@{mode=$p.mode;reason=$p.reason;firstNativeIdentity=$firstId;freshNativeIdentity=$freshId;score=$score})}
	foreach($mode in $Selected){if($mode -cin @('TAA','TemporalUpscale')){foreach($reason in @('cut','resize')){Require-Aa ($seen.ContainsKey($mode+'|'+$reason)) 'Missing first/fresh cut or resize pair'}}};return ,$results.ToArray()
}
function Invoke-AaScore([string]$Path,[string[]]$Selected){
	Require-Aa ($Selected.Count -gt 0 -and @($Selected|Select-Object -Unique).Count -eq $Selected.Count) 'Missing/duplicate selected mode';$root=[IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Path));$m=Get-Content -LiteralPath $Path -Raw|ConvertFrom-Json -AsHashtable
	Require-Aa ((Test-AaInteger $m.schemaVersion) -and $m.schemaVersion -eq 1 -and $m.evidenceKind -is [string] -and $m.evidenceKind -ceq 'NativeBackendReadback') 'Synthetic input cannot qualify native AA'
	Require-Aa ($m.fixtureFile -is [string] -and $m.fixtureSha256 -is [string]) 'Missing frozen fixture identity'
	$fixturePath=Resolve-AaInput $root $m.fixtureFile;Require-Aa ($m.fixtureSha256 -ceq $frozenFixtureSha256 -and (Get-FileHash -LiteralPath $fixturePath -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $frozenFixtureSha256) 'Frozen fixture hash changed';$f=Get-Content -LiteralPath $fixturePath -Raw|ConvertFrom-Json -AsHashtable
	Require-Aa ($f.schemaVersion -eq 1 -and $f.fixtureId -ceq 'moving-edges-v1' -and $f.status -ceq 'FrozenFixture' -and ($f.outputExtent-join ',') -ceq '640,360' -and $f.referenceScale -eq 4 -and $f.simulationHz -eq 60 -and $f.warmupCount -eq 64 -and ($f.continuousPhases-join ',') -ceq ((64..183)-join ',') -and ($f.crop-join ',') -ceq '48,32,592,328') 'Fixed quality protocol changed'
	$comparators=@('OffNative');if($Selected -ccontains 'TemporalUpscale'){$comparators+='OffHalf'};$runs=@{};$native=@{}
	foreach($name in @('Reference4x')+$comparators+$Selected){$run=Read-AaRun $name $m.runs[$name] $f;$runs[$name]=$run;$native[$name]=$run.actualNativeIdentities}
	# Prepare reference-only masks once, before loading candidate pixels.
	$reference=[Collections.Generic.List[object]]::new()
	foreach($phase in 64..183){$image=$K::Box4((Read-AaImage $root $runs.Reference4x.records[[string]$phase] @(2560,1440)));$luma=$K::Luminance($image);$mask=$K::EdgeMask($luma,640,360,[int[]]$f.crop);$reference.Add(@{phase=$phase;luma=$luma;mask=$mask})}
	$scores=@{}
	foreach($name in $comparators+$Selected){$run=$runs[$name];$frames=[Collections.Generic.List[TerraEngine.Tests.PostProcessImageQuality.Frame]]::new();foreach($ref in $reference){$image=Read-AaImage $root $run.records[[string]$ref.phase] $run.configuration.outputExtent;if($name -ceq 'OffHalf'){$image=$K::Enlarge2($image,640,360)};$frames.Add([TerraEngine.Tests.PostProcessImageQuality.Frame]::new('continuous',$ref.phase,1,$ref.luma,$ref.mask,$K::Luminance($image)))};$scores[$name]=$K::Measure($frames.ToArray())}
	$comparisons=@{};$passed=$true;foreach($name in $Selected){$off=if($name -ceq 'TemporalUpscale'){'OffHalf'}else{'OffNative'};$comparison=$K::Compare($scores[$name],$scores[$off],($name -cin @('TAA','TemporalUpscale')));$comparisons[$name]=@{comparator=$off;score=$comparison};$passed=$passed -and $comparison.Passed}
	$pairs=if($m.ContainsKey('resetPairs')){$m.resetPairs}else{@()};$resets=Read-AaResetPairs $root $pairs $f $Selected;foreach($r in $resets){$passed=$passed -and $r.score.Passed}
	return @{scope='PostProcessImageQualityScoring';host='PowerShell7-.NET-WindowsGDIPlus';rendererLaunchedByScorer=$false;status=$(if($passed){'PassedMeasuredSubset'}else{'FailedMeasuredSubset'});qualificationComplete=$false;qualificationPending=@('Native capture and effective-configuration validation','Separate native disocclusion and invalid-motion checks','Integrated engine/editor/export acceptance');fixtureSha256=$frozenFixtureSha256;actualNativeIdentities=$native;selectedModes=$Selected;rawScores=$scores;comparisons=$comparisons;resetPairs=$resets}
}
function Write-AaFresh([string]$Path,$Value){$full=[IO.Path]::GetFullPath($Path);$null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($full));$bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Value|ConvertTo-Json -Depth 80)+"`n");$file=[IO.File]::Open($full,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read);try{$file.Write($bytes);$file.Flush($true)}finally{$file.Dispose()}}

function Assert-AaNear([double]$A,[double]$B,[double]$Epsilon=1e-12){Require-Aa ([Math]::Abs($A-$B) -le $Epsilon) "Numerical mismatch: $A vs $B"}
function Reject-Aa([scriptblock]$Body){$rejected=$false;try{$null=& $Body}catch{$root=$_.Exception;while($root.InnerException){$root=$root.InnerException};if($root -is [IO.InvalidDataException]){$rejected=$true}else{throw}};Require-Aa $rejected 'Invalid fixture was accepted'}
function New-AaFrame([long]$Phase,[double[]]$Reference,[double[]]$Candidate,[byte[]]$Mask,[ulong]$Generation=1){return [TerraEngine.Tests.PostProcessImageQuality.Frame]::new('continuous',$Phase,$Generation,$Reference,$Mask,$Candidate)}

if($SelfTest){
	# Keep generated decoder images outside source/install trees by default.
	$workingParent=if($SelfTestOutput){[IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($SelfTestOutput))}else{[IO.Path]::GetTempPath()}
	$directory=Join-Path $workingParent ('quality-selftest-'+[Guid]::NewGuid().ToString('N'));$null=[IO.Directory]::CreateDirectory($directory)
	$checks=[Collections.Generic.List[object]]::new()
	function Check-Aa([string]$Name,[scriptblock]$Body){try{$null=& $Body;$checks.Add(@{name=$Name;passed=$true;error=$null})}catch{$checks.Add(@{name=$Name;passed=$false;error=$_.Exception.ToString()})}}
	Check-Aa 'srgb_exact_piecewise_conversion' {
		Assert-AaNear ($K::Decode(0)) 0;Assert-AaNear ($K::Decode(1)) 1;Assert-AaNear ($K::Decode(.5)) .21404114048223255
		foreach($v in @(0,.001,.0031308,.18,.5,1)){Assert-AaNear ($K::Decode($K::Encode($v))) $v 1e-7}
	}
	Check-Aa 'png_all_row_filters_and_no_premultiply' {
		$K::CheckKnownCrc()
		# Asymmetric rows catch bottom-up stride mistakes as well as BGR swaps.
		[byte[]]$pixels=@(20,80,150,255,200,35,65,255,100,90,200,255,210,10,40,255,15,140,99,255,61,72,83,255,9,250,31,255,33,66,199,255,240,120,3,255);$path=Join-Path $directory 'filters.png'
		foreach($filter in 0..4){$data=$K::SyntheticPng(3,3,$pixels,$filter);[IO.File]::WriteAllBytes($path,$data);$image=$K::Load($path,'PNG8','srgb',3,3,$K::Hash($data));$i=0;for($p=0;$p -lt $pixels.Length;$p++){if($p%4 -ne 3){Assert-AaNear $image.Rgb[$i] ($K::Decode($pixels[$p]/255.0));$i++}}}
		$data=[IO.File]::ReadAllBytes($path);$data[$data.Length-5]=$data[$data.Length-5] -bxor 1;[IO.File]::WriteAllBytes($path,$data);Reject-Aa {$K::Load($path,'PNG8','srgb',3,3,$K::Hash($data))}
	}
	Check-Aa 'box_average_is_linear_not_encoded' {
		[double[]]$values=for($y=0;$y -lt 8;$y++){for($x=0;$x -lt 8;$x++){($x+$y)%2}}
		$box=$K::Box4($K::Gray(8,8,$values));foreach($v in $box.Rgb){Assert-AaNear $v .5};Require-Aa ([Math]::Abs($box.Rgb[0]-$K::Decode(.5)) -gt .1) 'Encoded-space box averaging passed'
	}
	Check-Aa 'bilinear_uses_pixel_centers_and_clamped_edges' {
		$result=$K::Enlarge2($K::Gray(2,1,[double[]]@(0,1)),4,2);$expected=@(0,.25,.75,1,0,.25,.75,1);for($i=0;$i -lt 8;$i++){Assert-AaNear $result.Rgb[$i*3] $expected[$i]}
	}
	Check-Aa 'fixed_crop_axial_threshold_and_square_dilation' {
		[double[]]$values=for($y=0;$y -lt 8;$y++){for($x=0;$x -lt 8;$x++){if($x -ge 4){1}else{0}}};$mask=$K::EdgeMask($values,8,8,[int[]]@(1,1,7,7));Require-Aa (($mask|Measure-Object -Sum).Sum -eq 24) 'Crop/full-square dilation changed'
		[double[]]$point=[double[]]::new(81);$point[40]=1;$dilated=$K::EdgeMask($point,9,9,[int[]]@(1,1,8,8));Require-Aa (($dilated|Measure-Object -Sum).Sum -eq 21) 'Square dilation became an axial cross'
		Reject-Aa {$K::EdgeMask([double[]]::new(64),8,8,[int[]]@(1,1,7,7))}
	}
	Check-Aa 'global_sample_weighting_is_not_frame_mean_weighting' {
		$score=$K::Measure([TerraEngine.Tests.PostProcessImageQuality.Frame[]]@((New-AaFrame 0 @(0,0,0,0) @(.1,0,0,0) @(1,0,0,0)),(New-AaFrame 1 @(0,0,0,0) @(.3,.3,.3,0) @(1,1,1,0))))
		Require-Aa ($score.EdgeSamples -eq 4) 'Wrong sample count';Assert-AaNear $score.EdgeMean .25
	}
	Check-Aa 'shimmer_uses_signed_error_and_union_masks' {
		$a=$K::Measure([TerraEngine.Tests.PostProcessImageQuality.Frame[]]@((New-AaFrame 0 @(.5) @(.6) @(1)),(New-AaFrame 1 @(.5) @(.4) @(1))));Assert-AaNear $a.ShimmerMean .2
		$b=$K::Measure([TerraEngine.Tests.PostProcessImageQuality.Frame[]]@((New-AaFrame 0 @(.5,.5) @(.6,.7) @(1,0)),(New-AaFrame 1 @(.5,.5) @(.2,.9) @(0,1))));Require-Aa ($b.ShimmerSamples -eq 2) 'Wrong union count';Assert-AaNear $b.ShimmerMean .3
	}
	Check-Aa 'reference_subtraction_cancels_real_motion' {
		$a=$K::Measure([TerraEngine.Tests.PostProcessImageQuality.Frame[]]@((New-AaFrame 0 @(.2) @(.22) @(1)),(New-AaFrame 1 @(.8) @(.82) @(1))));Require-Aa ($a.ShimmerMean -lt 1e-15) 'Desired motion was counted as shimmer'
	}
	Check-Aa 'duplicate_missing_and_wrong_generation_pairs_reject' {
		$first=New-AaFrame 0 @(0) @(.1) @(1);foreach($next in @($first,(New-AaFrame 2 @(0) @(.1) @(1)),(New-AaFrame 1 @(0) @(.1) @(1) 2))){Reject-Aa {$K::Measure([TerraEngine.Tests.PostProcessImageQuality.Frame[]]@($first,$next))}}
	}
	Check-Aa 'zero_off_denominator_and_changed_masks_are_invalid' {
		$a=$K::Measure([TerraEngine.Tests.PostProcessImageQuality.Frame[]]@((New-AaFrame 0 @(0) @(0) @(1)),(New-AaFrame 1 @(0) @(0) @(1))));Reject-Aa {$K::Compare($a,$a,$true)}
		$a.EdgeMean=1;$a.ShimmerMean=1;$b=[TerraEngine.Tests.PostProcessImageQuality.Score]::new();$b.MaskSha256=@('different');$b.ReferenceSha256=$a.ReferenceSha256;$b.EdgeSamples=$a.EdgeSamples;Reject-Aa {$K::Compare($b,$a,$false)};$b.MaskSha256=$a.MaskSha256;$b.ReferenceSha256=@('different');Reject-Aa {$K::Compare($b,$a,$false)}
	}
	Check-Aa 'exact_one_percent_threshold' {
		$a=[TerraEngine.Tests.PostProcessImageQuality.Score]::new();$a.MaskSha256=@('mask');$a.ReferenceSha256=@('reference');$a.EdgeSamples=1;$a.EdgeMean=1;$a.ShimmerMean=1
		$b=[TerraEngine.Tests.PostProcessImageQuality.Score]::new();$b.MaskSha256=$a.MaskSha256;$b.ReferenceSha256=$a.ReferenceSha256;$b.EdgeSamples=1;$b.EdgeMean=.99;$b.ShimmerMean=.99
		Require-Aa ($K::Compare($b,$a,$true).Passed) 'Exact1percent rejected';$b.EdgeMean=.99000001;Require-Aa (!$K::Compare($b,$a,$true).Passed) 'Weakened1percent threshold'
	}
	Check-Aa 'reset_checks_worst_pixel_over_entire_crop' {
		$a=$K::Gray(8,8,[double[]]::new(64));$values=[double[]]::new(64);$values[27]=.99/255;Require-Aa ($K::FreshReset($a,$K::Gray(8,8,$values),[int[]]@(1,1,7,7)).Passed) 'Valid reset rejected'
		$values[27]=1.01/255;$result=$K::FreshReset($a,$K::Gray(8,8,$values),[int[]]@(1,1,7,7));Require-Aa ($result.Samples -eq 36 -and !$result.Passed) 'Reset used mean error or an edge-only crop'
	}
	Check-Aa 'native_ids_preserved_separate_from_logical_pairing' {
		$expected=@{sequence='continuous';phase=64;viewGeneration=1;poseSha256='pose'};$native=@{sequence='continuous';phase=64;viewGeneration=1;poseSha256='pose';nativeBackendReadback=$true;deterministicFallback=$false;nativeRendererSession='19';nativeViewId='0';nativeViewGeneration='18446744073709551599'};$map=@{};$identity=Read-AaNativeIdentity $native $expected $map
		Require-Aa ($identity[2] -ceq '18446744073709551599' -and $native.nativeViewGeneration -ceq $identity[2]) 'Actual native ID was rewritten'
		foreach($mutation in @(@{nativeViewGeneration='22'},@{viewGeneration=$true},@{viewGeneration=2},@{nativeViewGeneration='18446744073709551616'},@{deterministicFallback=$true},@{poseSha256=$true},@{nativeBackendReadback='true'})){$bad=$native.Clone();foreach($key in $mutation.Keys){$bad[$key]=$mutation[$key]};Reject-Aa {Read-AaNativeIdentity $bad $expected $map}}
		Require-Aa (!(Test-AaConfiguration @{mode='FXAA';sampleCount=1;internalExtent=@('640','360');outputExtent=@(640,360)} (Get-AaConfiguration 'FXAA'))) 'String extents bypassed actual native configuration'
		$badManifest=Join-Path $directory 'boolean-evidence-kind.json';[IO.File]::WriteAllText($badManifest,'{"schemaVersion":1,"evidenceKind":true}')
		Reject-Aa {Invoke-AaScore $badManifest @('FXAA')}
		$partialRun=@{effective=(Get-AaConfiguration 'FXAA');controlsSha256='control';warmupRenderReceipts=@();frames=@()}
		Reject-Aa {Read-AaRun 'FXAA' $partialRun @{controlsSha256='control';poses=@()}}
		$none=@();Reject-Aa {Read-AaResetPairs $directory $none @{} @('TAA')}
	}
	Check-Aa 'synthetic_reference_candidate_proves_metric_only' {
		$off=[Collections.Generic.List[TerraEngine.Tests.PostProcessImageQuality.Frame]]::new();$ideal=[Collections.Generic.List[TerraEngine.Tests.PostProcessImageQuality.Frame]]::new();$half=[Collections.Generic.List[TerraEngine.Tests.PostProcessImageQuality.Frame]]::new()
		foreach($phase in 0..7){$luma=$K::Luminance($K::Box4($K::SyntheticRaster(256,144,$phase)));$mask=$K::EdgeMask($luma,64,36,[int[]]@(4,3,60,33));$off.Add((New-AaFrame $phase $luma ($K::Luminance($K::SyntheticRaster(64,36,$phase))) $mask));$ideal.Add((New-AaFrame $phase $luma $luma $mask));$half.Add((New-AaFrame $phase $luma ($K::Luminance($K::Enlarge2($K::SyntheticRaster(32,18,$phase),64,36))) $mask))}
		$a=$K::Measure($ideal.ToArray());$b=$K::Measure($off.ToArray());$c=$K::Measure($half.ToArray());Require-Aa ($b.EdgeMean -gt 0 -and $b.ShimmerMean -gt 0 -and $K::Compare($a,$b,$true).Passed -and $K::Compare($a,$c,$true).Passed) 'Synthetic metric arithmetic failed'
	}
	Check-Aa 'invalid_luminance_grid_or_mask_cannot_score' {
		$first=New-AaFrame 0 @(.5,.5) @(.6,.6) @(1,0)
		foreach($bad in @((New-AaFrame 1 @(.5) @(.6) @(1)),(New-AaFrame 1 @(.5,.5) @([double]::NaN,.6) @(1,0)),(New-AaFrame 1 @(.5,.5) @(1.1,.6) @(1,0)),(New-AaFrame 1 @(.5,.5) @(.6,.6) @(255,0)),(New-AaFrame 1 @(.5,.5) @(.6,.6) @(0,0)))){Reject-Aa {$K::Measure([TerraEngine.Tests.PostProcessImageQuality.Frame[]]@($first,$bad))}}
	}
	Check-Aa 'frozen_pose_revision_and_motion_are_deterministic' {
		Require-Aa ((Get-FileHash -LiteralPath $frozenFixturePath -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $frozenFixtureSha256) 'Frozen pose revision changed';$fixture=Get-Content -LiteralPath $frozenFixturePath -Raw|ConvertFrom-Json -AsHashtable
		Require-Aa (($fixture.continuousPhases-join ',') -ceq ((64..183)-join ',') -and $fixture.poses[64].pose.cameraPositionTicks[0]-$fixture.poses[63].pose.cameraPositionTicks[0] -eq 512) 'Frozen motion changed'
		Require-Aa (($fixture.poses[63].pose.geometry[2].verticesTicks|ConvertTo-Json -Compress) -cne ($fixture.poses[64].pose.geometry[2].verticesTicks|ConvertTo-Json -Compress)) 'Animated ribbon was static'
		Require-Aa (Test-AaFlag $fixture.separateSequences.'invalid-motion'.frames[32].pose.geometry[2].previousOwnerUnavailable $true) 'Replacement frame must have no previous native owner'
		Require-Aa (!$fixture.separateSequences.'invalid-motion'.frames[33].pose.geometry[2].ContainsKey('previousOwnerUnavailable')) 'Replacement must acquire normal previous-owner history on its next frame'
	}
	Check-Aa 'png_conflicting_transfer_or_alpha_is_invalid' {
		$path=Join-Path $directory 'invalid.png';$data=$K::SyntheticPng(1,1,[byte[]]@(20,30,40,128),0);[IO.File]::WriteAllBytes($path,$data);Reject-Aa {$K::Load($path,'PNG8','srgb',1,1,$K::Hash($data))}
		$data=$K::SyntheticPng(1,1,[byte[]]@(20,30,40,255),0);[IO.File]::WriteAllBytes($path,$data);Reject-Aa {$K::Load($path,'PNG8','unknown',1,1,$K::Hash($data))}
		$wrong=$K::SyntheticGamma($data,100000);[IO.File]::WriteAllBytes($path,$wrong);Reject-Aa {$K::Load($path,'PNG8','srgb',1,1,$K::Hash($wrong))}
		$matching=$K::SyntheticGamma($data,45455);[IO.File]::WriteAllBytes($path,$matching);$image=$K::Load($path,'PNG8','srgb',1,1,$K::Hash($matching));Assert-AaNear $image.Rgb[0] ($K::Decode(20/255.0))
	}
	$failures=@($checks|Where-Object {!$_.passed});$result=@{scope='NumericalSyntheticFixtureOnly';host='PowerShell7-.NET-WindowsGDIPlus';powerShellVersion=$PSVersionTable.PSVersion.ToString();dotNetVersion=[Environment]::Version.ToString();workingDirectory=$directory;nativeRendererExecuted=$false;testsRun=$checks.Count;failures=$failures.Count;passed=($failures.Count -eq 0);checks=$checks.ToArray()}
	$path=if($SelfTestOutput){$SelfTestOutput}else{Join-Path $directory 'numerical-selftest.json'};Write-AaFresh $path $result
	$result|ConvertTo-Json -Depth 8
	if($failures.Count){throw 'Image-quality numerical tests failed.'}
}else{
	Require-Aa (!(Test-Path -LiteralPath $Output)) 'Score output must be fresh'
	try{$result=Invoke-AaScore $Manifest $Modes}catch{$result=@{scope='PostProcessImageQualityScoring';status='Invalid';qualificationComplete=$false;reason=$_.Exception.Message}}
	Write-AaFresh $Output $result
	if($result.status -cne 'PassedMeasuredSubset'){throw "Image scoring failed/invalid: $($result|ConvertTo-Json -Depth 4 -Compress)"}
	$result|ConvertTo-Json -Depth 80
}
