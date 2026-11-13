"""Join immutable historical source and reviewed current deltas; no runtime proof.

Only the native lexer defines current native selection. Git operates in a fresh
private repository/index; main source/index/objects are never written or fetched.
An explicit blob input can replace unavailable historical Git objects. The fixed
manifest and both SHA256 and Git object identity guard every supplied byte.
"""
from __future__ import annotations
import argparse,difflib,gzip,hashlib,io,json,os,re,subprocess,sys,zlib
from pathlib import Path
if sys.version_info < (3,9):
	raise SystemExit('Source review continuation requires Python3.9 or newer; no source was changed.')
REVISION='87d3fd037c1e881ff6630a4918c91be707673c1b'
TREE='47cf2ac37d0bee74c9da1b21e6516c3f9074a2f4'
HISTORICAL_MANIFEST_SHA='7a132db1f930d84f920796b0814b8a1439c44aceee4d8fa1a7a0b0add7472169'
IGNORED={'bin','obj','out','build','intermediate','.git','.vs','.cache','debug','release','relwithdebinfo'}
TEXT_SOURCE={'.h','.hpp','.hxx','.cpp','.cc','.cxx','.c','.cs','.inl','.hlsl','.hlsli','.vert','.frag','.comp','.ps1','.py','.cmake','.json','.csproj','.props','.targets','.md','.txt','.sh','.bat','.cmd'}
class Failure(ValueError):pass
def require(ok,message):
	if not ok:raise Failure(message)
def digest(data):return hashlib.sha256(data).hexdigest()
def unique(pairs):
	result={}
	for k,v in pairs:
		require(k not in result,'Duplicate JSON property: '+k);result[k]=v
	return result
def load(path):
	require(path.stat().st_size<=16*1024*1024,'JSON input exceeds16MiB')
	return json.loads(path.read_text(encoding='utf-8-sig'),object_pairs_hook=unique)
def safe(root,relative,exists=True):
	require(isinstance(relative,str)and relative and '\\'not in relative and ':'not in relative and not relative.startswith('/')and all(p not in('','.','..')for p in relative.split('/')),'Noncanonical relative path')
	# Resolve the trusted checkout root once: a user-selected symlink to the
	# checkout is valid, but redirects below that root must still be rejected.
	base=root.resolve()
	p=base/relative
	require(p.resolve()==p and p.is_relative_to(base),'Redirected/escaping path: '+relative)
	if exists:require(p.is_file(),'Missing file: '+relative)
	return p
def stable(path):
	before=path.stat();data=path.read_bytes();after=path.stat()
	require((before.st_size,before.st_mtime_ns,before.st_ino)==(after.st_size,after.st_mtime_ns,after.st_ino),'Source changed while reading: '+str(path));return data
def git_environment():
	# Git routing/config-injection variables must not redirect private writes.
	env={k:v for k,v in os.environ.items()if not k.upper().startswith('GIT_')}
	env.update({'GIT_CONFIG_NOSYSTEM':'1','GIT_CONFIG_GLOBAL':os.devnull,'GIT_ATTR_NOSYSTEM':'1'});return env
def run(command,env=None,input=None):
	p=subprocess.run(command,input=input,stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env,timeout=45)
	require(p.returncode==0,'Command failed without retry/network: '+str(command[0])+' '+p.stderr.decode(errors='replace')[:1024]);return p.stdout
def native(audit,root,strict=False):
	data=json.loads(run([str(audit),'--strict-final'if strict else'--inventory',str(root)]))
	require(data.get('fileCount',0)>0 and data['errorCount']==0 and data['fileCount']==len(data['files']),'Empty/invalid actual native inventory')
	if strict:require(data.get('occurrenceCount')==0,'Current native strict inventory retains filler')
	return data
def diff_bytes(path,before,after):
	# Preserve the authoritative review-index representation, including raw input
	# hashes. Readable hunks do not claim compiler or Unicode semantic coverage.
	return ''.join(difflib.unified_diff((before or b'').decode('utf-8-sig',errors='replace').splitlines(keepends=True),(after or b'').decode('utf-8-sig',errors='replace').splitlines(keepends=True),fromfile='historical/'+path,tofile='current/'+path,n=3)).encode('utf-8')
def policy_inputs(root):
	paths=[]
	if(root/'.gitattributes').is_file():paths.append('.gitattributes')
	for domain in('Engine','Editor','Tools'):
		for p in(root/domain).rglob('.gitattributes'):
			if not any(x.lower()in IGNORED for x in p.relative_to(root).parts[:-1]):paths.append(p.relative_to(root).as_posix())
	# Attribute metadata permits declared checkout newline representation; code
	# sources/diffs retain exact raw-byte hashes and are never normalized here.
	return [{'path':p,'textSha256':digest(stable(safe(root,p)).replace(b'\r\n',b'\n'))}for p in sorted(set(paths))]
def current_paths(root,historical,native_paths):
	paths={f['path']for f in historical['files']if f['role']=='production'and(root/f['path']).is_file()}|set(native_paths)
	for domain in('Engine','Editor','Tools'):
		for directory,dirs,files in os.walk(root/domain):
			dirs[:]=[x for x in dirs if x.lower()not in IGNORED]
			for name in files:
				p=Path(directory)/name
				if p.suffix in TEXT_SOURCE or name=='CMakeLists.txt':paths.add(p.relative_to(root).as_posix())
	return paths

def private_git(git,work,rows,blobs):
	store=work/'git';run([git,'-c','init.templateDir=','init','--bare','--object-format=sha1',str(store)],git_environment())
	for oid,data in blobs.items():
		body=b'blob '+str(len(data)).encode()+b'\0'+data;require(hashlib.sha1(body).hexdigest()==oid,'Historical Git object identity mismatch')
		p=store/'objects'/oid[:2]/oid[2:];p.parent.mkdir(exist_ok=True);p.write_bytes(zlib.compress(body))
	env=git_environment();env.update({'GIT_DIR':str(store),'GIT_INDEX_FILE':str(work/'index')})
	for key in('GIT_OBJECT_DIRECTORY','GIT_ALTERNATE_OBJECT_DIRECTORIES'):env.pop(key,None)
	run([git,'-c','core.splitIndex=false','update-index','-z','--index-info'],env,b''.join((r['mode']+' '+r['blobId']+'\t'+r['path']).encode()+b'\0'for r in rows));return env

def checkout(git,env,target,policy,paths,crlf):
	target.mkdir();env=env.copy();env['GIT_WORK_TREE']=str(policy)
	# No user/system filter drivers are admitted into source verification.
	attrs=run([git,'-C',str(policy),'-c','core.attributesFile='+os.devnull,'check-attr','-z','--stdin','filter','working-tree-encoding'],env,('\0'.join(paths)+'\0').encode()).split(b'\0')
	for i in range(0,len(attrs)-2,3):require(attrs[i+2]in(b'unspecified',b'unset'),'Unsupported checkout filter/encoding')
	run([git,'-C',str(policy),'-c','core.attributesFile='+os.devnull,'-c','core.autocrlf='+('true'if crlf else'false'),'-c','core.eol='+('crlf'if crlf else'lf'),'checkout-index','--force','-z','--stdin','--prefix='+str(target)+os.sep],env,('\0'.join(paths)+'\0').encode())

def materialize(repo,audit,git,work,historical,input_root=None):
	rows=historical['files'];seen=set()
	for row in rows:
		safe(work,row['path'],False);require(row['path']not in seen and row['mode']in('100644','100755')and re.fullmatch('[0-9a-f]{40}',row['blobId'])and row['role']in('production','attributes','stage-manifest'),'Invalid historical manifest row');seen.add(row['path'])
	oids=list(dict.fromkeys(r['blobId']for r in rows));blobs={}
	if input_root:
		for oid in oids:blobs[oid]=stable(safe(input_root,'blobs/'+oid))
	else:
		require(run([git,'-C',str(repo),'rev-parse',REVISION+'^{tree}'],git_environment()).decode().strip()==TREE,'Missing/wrong exact historical commit/tree; no HEAD fallback or fetch')
		data=run([git,'-C',str(repo),'cat-file','--batch'],env=git_environment(),input=('\n'.join(oids)+'\n').encode());position=0
		for oid in oids:
			end=data.index(b'\n',position);header=data[position:end].decode().split();position=end+1
			require(len(header)==3 and header[0]==oid and header[1]=='blob','Missing historical blob')
			size=int(header[2]);blobs[oid]=data[position:position+size];position+=size+1
	for row in rows:require(len(blobs[row['blobId']])==row['bytes']and digest(blobs[row['blobId']])==row['blobSha256'],'Historical blob byte pin mismatch: '+row['path'])
	env=private_git(git,work,rows,blobs);historical_policy=work/'historical-policy';historical_policy.mkdir()
	for row in rows:
		if Path(row['path']).name=='.gitattributes':
			p=safe(historical_policy,row['path'],False);p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(blobs[row['blobId']])
	history=work/'historical';checkout(git,env,history,historical_policy,[r['path']for r in rows],True)
	current_policy=work/'current-policy';current_policy.mkdir()
	for row in policy_inputs(repo):
		p=safe(current_policy,row['path'],False);p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(stable(safe(repo,row['path'])))
	production=[r['path']for r in rows if r['role']=='production'];lf=work/'baseline-lf';crlf=work/'baseline-crlf';checkout(git,env,lf,current_policy,production,False);checkout(git,env,crlf,current_policy,production,True)
	# These inventories are actual lexer selections, not a substitute root with
	# only the181pinned files. Historical and current roots remain distinct.
	hist_inventory=native(audit,history)
	# A clean working tree can contain mixed LF/CRLF after ordinary editing. Use
	# Git's real declared conversion, never a handwritten source normalizer.
	clean_env=env.copy();clean_env['GIT_WORK_TREE']=str(current_policy);by_path={r['path']:r for r in rows};representations=[]
	def clean_identity(path,actual):
		oid=run([git,'-C',str(current_policy),'-c','core.attributesFile='+os.devnull,'-c','core.autocrlf=true','-c','core.safecrlf=false','hash-object','-w','--stdin','--path='+path],clean_env,actual).decode().strip()
		cleaned=run([git,'cat-file','blob',oid],clean_env);expected=blobs[by_path[path]['blobId']]
		if cleaned!=expected:return False
		representations.append({'path':path,'actualSha256':digest(actual),'actualBytes':len(actual),'historicalBlobSha256':digest(expected),'historicalBlobBytes':len(expected),'crlfCount':actual.count(b'\r\n'),'lfCount':actual.count(b'\n'),'qualification':'Declared Git-clean content identity only; raw bytes, token locations, raw-string semantics and runtime equivalence unproven.'});return True
	return history,lf,crlf,hist_inventory,clean_identity,representations

def verify_reviews(repo,history,lf,crlf,historical,manifest,audit,base,clean_identity=None):
	require(manifest['schemaVersion']=='source-review-continuation-v1'and manifest['historicalRevision']==REVISION and manifest['historicalTree']==TREE,'Wrong continuation schema/history')
	require(manifest['structuralOnly']is True and manifest['semanticCompletenessProven']is False and manifest['lifetimeAcceptance']is False,'Incorrect acceptance scope')
	require(manifest['currentPolicies']==policy_inputs(repo),'Current declared checkout policy changed')
	require(isinstance(manifest['reviewArtifacts'],list)and manifest['reviewArtifacts'],'Missing review provenance artifacts')
	for ref in manifest['reviewArtifacts']:
		data=stable(safe(base,ref['path']));require(digest(data)==ref['sha256']and len(data)==ref['bytes'],'Review report provenance mismatch')
	inventory=native(audit,repo);native_paths={Path(f['path']).relative_to(repo).as_posix()for f in inventory['files']};paths=current_paths(repo,historical,native_paths);old={r['path']for r in historical['files']if r['role']=='production'};seen={};pins={};deltas={}
	for path in sorted(paths|old):
		before=stable(safe(history,path))if path in old else None;after=stable(safe(repo,path))if path in paths else None
		if after is not None:pins[path]=digest(after)
		# Non-delta content may use either legitimate declared Git checkout
		# representation. This pins stable blobs, not1210durable raw hashes.
		unchanged=before is not None and after is not None and (after==stable(safe(lf,path))or after==stable(safe(crlf,path)))
		if not unchanged and before is not None and after is not None and clean_identity is not None:unchanged=clean_identity(path,after)
		if not unchanged:
			diff=diff_bytes(path,before,after);deltas[path]={'beforeSha256':digest(before)if before is not None else None,'afterSha256':digest(after)if after is not None else None,'changeType':'added'if before is None else'deleted'if after is None else'modified','diff':diff}
	for row in manifest['files']:
		path=row['path'];require(path in deltas and path not in seen,'Extra/duplicate/non-delta review: '+path);seen[path]=True;d=deltas[path]
		require(isinstance(row['nativeScannerEligible'],bool)and row['nativeScannerEligible']==(Path(path).suffix in {'.h','.hpp','.hxx','.cpp','.cc','.cxx','.c','.cs','.inl'}),'Stale native syntax eligibility qualification: '+path)
		for key in('beforeSha256','afterSha256','changeType'):require(row[key]==d[key],'Stale delta '+key+': '+path)
		require(row['diffSha256']==digest(d['diff']),'Stale review diff: '+path)
		ref=row['diffArtifact'];raw=stable(safe(base,ref['path']))
		require(digest(raw)==ref['sha256']and len(raw)==ref['bytes'],'Diff artifact mismatch')
		if 'compression'in ref:
			# Hash the stored bytes first, then bound expansion by the independently
			# computed exact diff. Compression changes storage only, never coverage.
			require(ref['compression']=='gzip','Unsupported diff artifact compression')
			try:
				with gzip.GzipFile(fileobj=io.BytesIO(raw))as stream:raw=stream.read(len(d['diff'])+1)
			except(OSError,EOFError,zlib.error)as error:raise Failure('Invalid compressed diff artifact')from error
		require(digest(raw)==row['diffSha256']and raw==d['diff'],'Diff artifact mismatch')
		require(row['beforeBytes']==(len(stable(safe(history,path)))if path in old else None)and row['afterBytes']==(len(stable(safe(repo,path)))if path in paths else None),'Stale source byte counts')
		review=row['review'];require(review['status']=='reviewed'and isinstance(review['reviewer'],str)and review['reviewer'].strip()and review['reviewedDiffSha256']==row['diffSha256'],'Unreviewed/stale reviewer decision')
		for field in('commentIntent','ownershipDecision'):require(isinstance(review[field],str)and review[field].strip()and review[field]not in('TODO','TBD','Unknown'),'Missing bounded review '+field)
		require(review['findings']and review['artifactReferences'],'Missing concrete review notes/artifacts')
		for a in review['artifactReferences']:
			data=stable(safe(base,a['path']));require(digest(data)==a['sha256']and len(data)==a['bytes'],'Review artifact mismatch')
		hunks=re.findall(r'^@@.*@@.*$',d['diff'].decode(errors='replace'),re.M);covered=[]
		for note in review['findings']:
			require(note.get('introducedIssue')is not True,'Unresolved introduced issue in source review: '+path)
			covered+=note.get('reviewedHunkHeaders',note.get('sourceHunks',[]));require(note.get('message')or note.get('evidence'),'Empty review finding')
		covered +=[x['header']for x in review.get('hunkReviews',[])if x.get('status')=='reviewed']
		require(set(covered)==set(hunks),'Missing/extra hunk review coverage: '+path)
		kind=row['classification'];require(kind in('functional','mixed','comment-only'),'Unclassified source delta')
		if kind=='comment-only':
			require(d['changeType']=='modified','Comment-only cannot add/delete executable source')
			result=subprocess.run([str(audit),'--compare',str(safe(history,path)),str(safe(repo,path))],stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=45)
			require(result.returncode==0 and result.stdout.decode().strip()=='equivalent executable tokens','False comment-only token/physical-location equivalence: '+path)
	require(set(seen)==set(deltas),'Omitted source delta reviews: '+', '.join(sorted(set(deltas)-set(seen))))
	return pins,inventory,len(deltas)

def verify_source_pins(repo,pins,audit,historical):
	# Re-scan closure as well as known bytes: a newly added source cannot hide
	# between inventory and the final hash check.
	current=native(audit,repo);paths={Path(x['path']).relative_to(repo).as_posix()for x in current['files']}
	require(current_paths(repo,historical,paths)==set(pins),'Current source selection changed during verification')
	for path,h in pins.items():require(digest(stable(safe(repo,path)))==h,'Current source changed during verification: '+path)
def verify_historical_accounting(receipt):
	# Historical accounting alone never represents joined current acceptance.
	require(receipt.get('schemaVersion')=='historical-source-comment-accounting-v1'and receipt.get('mode')=='HistoricalAccounting'and receipt.get('status')=='Passed','Not a historical accounting receipt')
	for name in('stagePerformed','currentSourceAcceptance','tokenProofExecuted','semanticCompletenessProven','lifetimeAcceptance'):require(receipt.get(name)is False,'False historical acceptance scope: '+name)
	require(receipt.get('structuralOnly')is True and receipt.get('commentOnlyFiles')==181 and receipt.get('originalOccurrences')==2345 and receipt.get('batchCount')==6 and receipt.get('historicalStageMismatch'),'Incomplete historical accounting')

def write_exclusive(path,value):
	# Atomic creation rejects a competing writer instead of replacing its data.
	with path.open('x',encoding='utf-8')as stream:stream.write(json.dumps(value,indent=2)+'\n')

def main(argv=None):
	p=argparse.ArgumentParser(description=__doc__);p.add_argument('--repo-root',type=Path,required=True);p.add_argument('--audit-executable',type=Path,required=True);p.add_argument('--manifest',type=Path,required=True);p.add_argument('--historical-manifest',type=Path,required=True);p.add_argument('--work-root',type=Path);p.add_argument('--report',type=Path,required=True);p.add_argument('--git-executable',default='git');p.add_argument('--historical-input-root',type=Path);p.add_argument('--recheck-receipt',type=Path);p.add_argument('--historical-accounting-receipt',type=Path);args=p.parse_args(argv);result={'schemaVersion':'source-review-continuation-verification-v1','status':'Failed','structuralOnly':True,'semanticCompletenessProven':False,'lifetimeAcceptance':False,'tokenProofExecuted':False};code=1
	try:
		require(not args.report.exists(),'Report must be fresh');repo=args.repo_root.resolve();audit=args.audit_executable.resolve();manifest_data=stable(args.manifest);historical_data=stable(args.historical_manifest)
		require(digest(historical_data)==HISTORICAL_MANIFEST_SHA,'Untrusted historical source manifest');historical=load(args.historical_manifest);require(historical['revision']==REVISION and historical['tree']==TREE,'Wrong fixed historical source identity')
		manifest=load(args.manifest)
		if args.recheck_receipt:
			if args.historical_accounting_receipt:verify_historical_accounting(load(args.historical_accounting_receipt))
			prior=load(args.recheck_receipt);require(prior['status']=='Passed'and prior['inputPins']=={'manifest':digest(manifest_data),'historicalManifest':digest(historical_data)},'Stale/unverified recheck receipt')
			require(prior['currentPolicies']==policy_inputs(repo),'Checkout policy changed during verification')
			verify_source_pins(repo,{x['path']:x['sha256']for x in prior['currentSourcePins']},audit,historical)
			require(stable(args.manifest)==manifest_data and stable(args.historical_manifest)==historical_data,'Manifest changed during recheck')
			result.update({'status':'Passed','recheckedSourceCount':len(prior['currentSourcePins']),'inputPins':prior['inputPins']});write_exclusive(args.report,result);print(json.dumps(result));return 0
		require(args.work_root is not None and not args.work_root.exists(),'Work root must be fresh');args.work_root.mkdir(parents=True);work=args.work_root.resolve()
		history,lf,crlf,history_inventory,clean_identity,representations=materialize(repo,audit,args.git_executable,work,historical,args.historical_input_root);pins,inventory,count=verify_reviews(repo,history,lf,crlf,historical,manifest,audit,args.manifest.parent.resolve(),clean_identity)
		verify_source_pins(repo,pins,audit,historical)
		require(stable(args.manifest)==manifest_data and stable(args.historical_manifest)==historical_data,'Manifest changed during verification')
		result.update({'status':'Passed','historicalRoot':str(history),'historicalStageManifest':str(history/'Tests/Tools/CodeQuality/Baseline/stage-ownership.json'),'historicalRevision':REVISION,'historicalTree':TREE,'historicalNativeSourceCount':history_inventory['fileCount'],'currentNativeSourceCount':inventory['fileCount'],'reviewedDeltaFiles':count,'nonDeltaGitCleanRepresentations':representations,'currentNativePaths':sorted(Path(x['path']).relative_to(repo).as_posix()for x in inventory['files']),'currentSourcePins':[{'path':path,'sha256':h}for path,h in sorted(pins.items())],'inputPins':{'manifest':digest(manifest_data),'historicalManifest':digest(historical_data)},'currentPolicies':policy_inputs(repo)});code=0
	except(Exception)as e:result['error']=str(e)
	try:write_exclusive(args.report,result)
	except OSError as e:result['status']='Failed';result['error']='Report creation rejected: '+str(e);code=1
	print(json.dumps(result,indent=2));return code
if __name__=='__main__':raise SystemExit(main())
