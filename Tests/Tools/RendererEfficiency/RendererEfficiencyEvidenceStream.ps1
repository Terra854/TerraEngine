<#
Shared validation-only bounded Framework JSON stream utility; no native handles.
Internal callbacks belong to trusted validation orchestration, never an MCP endpoint.
Callers own containment/hashes/identity/atomic publication. Write preserves source
bytes exactly and inserts only an actual lifecycle artifact into root closure.
#>
function InitializeRendererEfficiencyEvidenceStream {
	if('RendererEfficiencyOfflineRecoveryStream' -as [type]){return}
	$source=@'
// Validation-only offline copier. Framework JSON readers retain one bounded
// metadata field, frame or artifact at a time; no operation-wide DOM is built.
using System;
using System.IO;
using System.Text;
using System.Xml;
using System.Collections.Generic;
using System.Runtime.Serialization.Json;
public static class RendererEfficiencyOfflineRecoveryStream {
	sealed class BoundedText : StringWriter {
		public BoundedText() : base(new StringBuilder()) {}
		void Limit(int count) { if(GetStringBuilder().Length + count > 4194304) throw new InvalidDataException("Recovery item exceeds 4 MiB bound"); }
		public override void Write(char c) { Limit(1); base.Write(c); }
		public override void Write(string s) { Limit(s == null ? 0 : s.Length); base.Write(s); }
		public override void Write(char[] b, int i, int n) { Limit(n); base.Write(b,i,n); }
	}
	static XmlDictionaryReader Reader(Stream stream) {
		return JsonReaderWriterFactory.CreateJsonReader(stream, new XmlDictionaryReaderQuotas {
			MaxDepth=128, MaxStringContentLength=1048576, MaxArrayLength=int.MaxValue,
			MaxBytesPerRead=65536, MaxNameTableCharCount=1048576 });
	}
	static string Name(XmlReader reader) { return reader.GetAttribute("item") ?? reader.LocalName; }
	static void CheckTree(XmlElement element) {
		var names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
		foreach(XmlNode child in element.ChildNodes) if(child is XmlElement) {
			var node=(XmlElement)child;
			if(element.GetAttribute("type")=="object" && !names.Add(node.HasAttribute("item") ? node.GetAttribute("item") : node.LocalName))
				throw new InvalidDataException("Duplicate recovery JSON property");
			CheckTree(node);
		}
	}
	static string Capture(XmlReader reader) {
		string xml;
		using(var text=new BoundedText()) using(var writer=XmlWriter.Create(text,new XmlWriterSettings { OmitXmlDeclaration=true, ConformanceLevel=ConformanceLevel.Fragment })) {
			writer.WriteNode(reader,true); writer.Flush(); xml=text.ToString();
		}
		var document=new XmlDocument(); document.XmlResolver=null; document.LoadXml(xml); CheckTree(document.DocumentElement);
		var root=document.CreateElement("root");
		foreach(XmlAttribute a in document.DocumentElement.Attributes) root.Attributes.Append((XmlAttribute)a.Clone());
		foreach(XmlNode child in document.DocumentElement.ChildNodes) root.AppendChild(child.Clone());
		using(var bytes=new MemoryStream()) using(var writer=JsonReaderWriterFactory.CreateJsonWriter(bytes,Encoding.UTF8,false)) {
			using(var source=new XmlNodeReader(root)) writer.WriteNode(source,true);
			writer.Flush(); return Encoding.UTF8.GetString(bytes.ToArray());
		}
	}
	static void Boundary(string path) {
		bool quoted=false, escaped=false, finished=false; int depth=0; var bytes=new byte[65536];
		using(var file=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read)) { int n;
			while((n=file.Read(bytes,0,bytes.Length))!=0) for(int i=0;i<n;i++) {
				byte b=bytes[i];
				if(finished) { if(b!=32 && b!=9 && b!=10 && b!=13) throw new InvalidDataException("Trailing recovery JSON content"); continue; }
				if(quoted) { if(escaped) escaped=false; else if(b==92) escaped=true; else if(b==34) quoted=false; continue; }
				if(b==34) quoted=true; else if(b==123 || b==91) depth++; else if(b==125 || b==93) { if(--depth==0) finished=true; }
			}
		}
		if(!finished || quoted || depth!=0) throw new InvalidDataException("Incomplete recovery JSON root");
	}
	// The callback verifies each item before any output can be published. Small
	// root metadata may arrive after arrays, so the caller validates it afterward.
	public static void Inspect(string path, Action<string,string> inspect) {
		Boundary(path); var names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
		using(var file=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read)) using(var reader=Reader(file)) {
			reader.MoveToContent(); if(reader.LocalName!="root" || reader.GetAttribute("type")!="object") throw new InvalidDataException("Recovery root is not an object");
			reader.Read();
			while(reader.NodeType!=XmlNodeType.EndElement || reader.Depth!=0) {
				if(reader.EOF) throw new InvalidDataException("Recovery root ended early");
				if(reader.NodeType!=XmlNodeType.Element || reader.Depth!=1) { reader.Read(); continue; }
				string name=Name(reader);
				if(!names.Add(name) || Array.IndexOf(new[]{"schemaVersion","identity","requestedOptions","measurementPolicy","frames","successCriteria","artifacts"},name)<0)
					throw new InvalidDataException("Unexpected or duplicate recovery root field");
				if(name=="frames" || name=="artifacts") {
					if(reader.GetAttribute("type")!="array") throw new InvalidDataException("Recovery frame/artifact field is not array");
					bool empty=reader.IsEmptyElement; reader.Read(); if(empty) continue;
					while(!(reader.NodeType==XmlNodeType.EndElement && reader.Depth==1)) {
						if(reader.EOF || reader.NodeType!=XmlNodeType.Element || reader.Depth!=2) throw new InvalidDataException("Malformed recovery array");
						inspect(name,Capture(reader));
					}
					reader.Read();
				} else inspect(name,Capture(reader));
			}
		}
		if(names.Count!=7) throw new InvalidDataException("Recovery root fields missing");
	}
	static void Start(XmlWriter writer, XmlReader reader) {
		writer.WriteStartElement(reader.Prefix,reader.LocalName,reader.NamespaceURI);
		if(reader.MoveToFirstAttribute()) { do { writer.WriteAttributeString(reader.Prefix,reader.LocalName,reader.NamespaceURI,reader.Value); } while(reader.MoveToNextAttribute()); reader.MoveToElement(); }
	}
	// Inspection has already validated structure. Locate the root artifact-array
	// closing byte while ignoring all quoted/escaped brackets. Root property names
	// are decoded by the same Framework JSON reader, including Unicode escapes.
	public static long LifecycleInsertionOffset(string sourcePath) {
		int depth=0; bool quoted=false, escaped=false, capture=false, artifacts=false;
		string property=null; var key=new MemoryStream(); long position=0;
		using(var file=new FileStream(sourcePath,FileMode.Open,FileAccess.Read,FileShare.Read)) { int value;
			while((value=file.ReadByte())!=-1) { byte b=(byte)value;
				if(quoted) {
					if(capture) { if(key.Length>=1048576) throw new InvalidDataException("Recovery root name exceeds bound"); key.WriteByte(b); }
					if(escaped) escaped=false; else if(b==92) escaped=true; else if(b==34) {
						quoted=false;
						if(capture) { key.Position=0; using(var r=Reader(key)) { r.MoveToContent(); property=r.ReadElementContentAsString(); } key=new MemoryStream(); capture=false; }
					}
				} else if(b==34) { quoted=true; capture=depth==1; if(capture) key.WriteByte(b); }
				else if(b==123 || b==91) { if(b==91 && depth==1 && property=="artifacts") artifacts=true; depth++; }
				else if(b==125 || b==93) { if(b==93 && depth==2 && artifacts) return position; depth--; }
				position++;
			}
		}
		throw new InvalidDataException("Root artifact insertion point missing");
	}
	// Independently prove a single insertion using bounded buffers. This checks
	// original bytes, including whitespace and numeric spelling, not a DOM copy.
	public static void VerifyUnchangedInsertion(string sourcePath,string recoveredPath,string lifecycleJson) {
		long offset=LifecycleInsertionOffset(sourcePath);
		using(var source=new FileStream(sourcePath,FileMode.Open,FileAccess.Read,FileShare.Read)) using(var recovered=new FileStream(recoveredPath,FileMode.Open,FileAccess.Read,FileShare.Read)) {
			long scan=offset-1; int previous;
			do { source.Position=scan--; previous=source.ReadByte(); } while(previous==32 || previous==9 || previous==10 || previous==13);
			byte[] insertion=Encoding.UTF8.GetBytes((previous==91 ? "" : ",")+lifecycleJson);
			if(recovered.Length!=source.Length+insertion.Length) throw new InvalidDataException("Recovery insertion length differs");
			source.Position=0; CompareBytes(source,recovered,offset);
			for(int i=0;i<insertion.Length;i++) if(recovered.ReadByte()!=insertion[i]) throw new InvalidDataException("Recovery lifecycle insertion differs");
			CompareBytes(source,recovered,source.Length-offset);
		}
	}
	static void CompareBytes(Stream source,Stream recovered,long count) {
		var a=new byte[65536]; var b=new byte[65536];
		while(count>0) {
			int size=(int)Math.Min(count,a.Length); int n=source.Read(a,0,size);
			if(n==0) throw new EndOfStreamException(); int filled=0;
			while(filled<n) { int got=recovered.Read(b,filled,n-filled); if(got==0) throw new EndOfStreamException(); filled+=got; }
			for(int i=0;i<n;i++) if(a[i]!=b[i]) throw new InvalidDataException("Original recovery bytes changed");
			count-=n;
		}
	}
	static void CopyBytes(Stream input, Stream output, long count) {
		var buffer=new byte[65536]; while(count>0) { int n=input.Read(buffer,0,(int)Math.Min(count,buffer.Length)); if(n==0) throw new EndOfStreamException(); output.Write(buffer,0,n); count-=n; }
	}
	// Byte-preserving insertion makes the original-to-recovered transformation
	// independently verifiable: every source byte remains identical, not merely
	// semantically equivalent. Native numbers, paths, fields and order never change.
	public static void Write(string sourcePath,string freshTemp,string lifecycleJson) {
		long offset=LifecycleInsertionOffset(sourcePath);
		using(var input=new FileStream(sourcePath,FileMode.Open,FileAccess.Read,FileShare.Read)) using(var output=new FileStream(freshTemp,FileMode.CreateNew,FileAccess.Write,FileShare.None)) {
			CopyBytes(input,output,offset); long scan=offset-1; int previous;
			do { input.Position=scan--; previous=input.ReadByte(); } while(previous==32 || previous==9 || previous==10 || previous==13);
			var insertion=Encoding.UTF8.GetBytes((previous==91 ? "" : ",")+lifecycleJson); output.Write(insertion,0,insertion.Length);
			input.Position=offset; CopyBytes(input,output,input.Length-offset); output.Flush(true);
		}
	}
}

'@
	if($PSVersionTable.PSVersion.Major -le 5){Add-Type -TypeDefinition $source -ReferencedAssemblies @('System.Runtime.Serialization','System.Xml','System.Core')}
	else{Add-Type -TypeDefinition $source}
}
function InspectRendererEfficiencyEvidenceStream([string]$Path,[Action[string,string]]$InternalInspect){
	InitializeRendererEfficiencyEvidenceStream
	[RendererEfficiencyOfflineRecoveryStream]::Inspect($Path,$InternalInspect)
}