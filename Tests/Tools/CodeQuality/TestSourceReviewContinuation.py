"""Bounded negative contracts for the maintained continuation verifier.

These isolated artificial source roots exercise validation mechanics with the
actual native lexer. They are never a real source-review acceptance receipt.
"""
import argparse,copy,gzip,hashlib,importlib.util,json,os,re,subprocess,tempfile
from pathlib import Path

def main():
	parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--validator',type=Path,required=True);parser.add_argument('--audit-executable',type=Path,required=True);parser.add_argument('--report',type=Path);parser.add_argument('--minimum-cases',type=int,default=55);a=parser.parse_args();
	if a.minimum_cases<1:parser.error('--minimum-cases must be positive')
	if a.report is None:a.report=Path(tempfile.mkdtemp(prefix='TerraEngine-continuation-contract-report-'))/'contracts.json'
	spec=importlib.util.spec_from_file_location('continuation',a.validator);v=importlib.util.module_from_spec(spec);spec.loader.exec_module(v);results=[]
	with tempfile.TemporaryDirectory(prefix='TerraEngine-continuation-contract-')as directory:
		# Temporary roots may be aliases (Windows junctions or macOS /var).
		# Compare verifier outputs against the same resolved trusted root.
		root=Path(directory).resolve();repo=root/'repo';history=root/'history';lf=root/'lf';crlf=root/'crlf';base=root/'proof'
		for r in(repo,history,lf,crlf,base):r.mkdir()
		before=b'// Returns the bounded fixture input.\nint ReadFixture(int value) { return value; }\n';after=before.replace(b'return value;',b'return value + 1;');path='Engine/Fixture.cpp';other='Editor/Other.cpp'
		for r in(repo,history,lf,crlf):
			for relative in(path,other):
				p=r/relative;p.parent.mkdir(exist_ok=True);p.write_bytes(after if r==repo and relative==path else before)
		historical={'files':[{'path':x,'role':'production'}for x in(path,other)]};diff=v.diff_bytes(path,before,after);ref={'path':'change.diff','sha256':v.digest(diff),'bytes':len(diff)};(base/'change.diff').write_bytes(diff)
		row={'path':path,'changeType':'modified','nativeScannerEligible':True,'beforeSha256':v.digest(before),'afterSha256':v.digest(after),'beforeBytes':len(before),'afterBytes':len(after),'diffSha256':v.digest(diff),'diffArtifact':ref,'classification':'functional','review':{'status':'reviewed','reviewer':'isolated-contract','reviewedDiffSha256':v.digest(diff),'commentIntent':'Existing bounded input description remains applicable.','ownershipDecision':'Scalar return owns no resource; no native lifetime acceptance.','findings':[{'message':'Reviewed actual scalar return change.','reviewedHunkHeaders':re.findall(r'^@@.*@@.*$',diff.decode(),re.M),'introducedIssue':False}],'artifactReferences':[ref]}}
		manifest={'schemaVersion':'source-review-continuation-v1','historicalRevision':v.REVISION,'historicalTree':v.TREE,'structuralOnly':True,'semanticCompletenessProven':False,'lifetimeAcceptance':False,'currentPolicies':[],'reviewArtifacts':[ref],'files':[row]}
		def verify(m):return v.verify_reviews(repo,history,lf,crlf,historical,m,a.audit_executable,base)
		def check(name,action,should_fail=True):
			try:action();failed=False
			except (v.Failure,FileExistsError):failed=True
			except Exception as error:
				# A harness crash is never evidence of an expected verifier rejection.
				results.append({'name':name,'passed':False,'crash':type(error).__name__+': '+str(error)});return
			results.append({'name':name,'passed':failed==should_fail})
		# Exercise the actual negative-case handler with an unexpected parser crash.
		def crash():raise json.JSONDecodeError('controlled malformed harness input','',0)
		check('temporary-crash-control',crash);observed=results.pop();check('unexpected-crash-is-not-rejection',lambda:v.require(not observed['passed'] and 'JSONDecodeError' in observed['crash'],'crash was mistaken for expected rejection'),False)
		check('actual-lexer-reviewed-functional-control',lambda:verify(manifest),False)
		# Compressed evidence must expand to exactly the same independently
		# computed diff; corrupt headers, truncation and extra payloads fail closed.
		def compressed(data,compression='gzip'):
			stored=base/'change.diff.gz';stored.write_bytes(data);m=copy.deepcopy(manifest)
			m['files'][0]['diffArtifact']={'path':stored.name,'sha256':v.digest(data),'bytes':len(data),'compression':compression}
			return m
		packed=gzip.compress(diff,mtime=0)
		check('compressed-exact-diff-control',lambda:verify(compressed(packed)),False)
		for name,data,kind in [('compressed-wrong-diff',gzip.compress(b'wrong',mtime=0),'gzip'),('compressed-extra-payload',gzip.compress(diff+b'extra',mtime=0),'gzip'),('compressed-concatenated-payload',packed+gzip.compress(b'extra',mtime=0),'gzip'),('compressed-truncated-trailer',packed[:-4],'gzip'),('compressed-invalid-header',b'not gzip','gzip'),('compressed-unknown-format',packed,'zip'),('compressed-null-format',packed,None),('compressed-expansion-over-bound',gzip.compress(diff*1000,mtime=0),'gzip')]:
			check(name,lambda data=data,kind=kind:verify(compressed(data,kind)))
		m=compressed(packed);(base/'change.diff.gz').write_bytes(packed+b'corruption');check('compressed-stored-hash-mismatch',lambda:verify(m))
		check('compressed-invalid-deflate',lambda:verify(compressed(packed[:10]+b'\x06'+packed[11:])))
		def change(key,value):
			m=copy.deepcopy(manifest);target=m['files'][0]
			for part in key[:-1]:target=target[part]
			target[key[-1]]=value;return m
		for name,key,value in [('stale-after',['afterSha256'],'0'*64),('stale-before',['beforeSha256'],'0'*64),('stale-diff',['diffSha256'],'0'*64),('stale-byte-count',['afterBytes'],0),('unreviewed',['review','status'],'pending'),('missing-reviewer',['review','reviewer'],''),('missing-comment-intent',['review','commentIntent'],''),('missing-ownership',['review','ownershipDecision'],''),('missing-hunk-coverage',['review','findings'],[{'message':'Missing actual hunk notes'}]),('unresolved-introduced-issue',['review','findings'],[{'message':'Defect','introducedIssue':True}]),('false-comment-only',['classification'],'comment-only'),('wrong-native-qualification',['nativeScannerEligible'],False),('missing-review-artifacts',['review','artifactReferences'],[])]:
			check(name,lambda key=key,value=value:verify(change(key,value)))
		m=copy.deepcopy(manifest);m['files']=[];check('omitted-delta',lambda:verify(m))
		m=copy.deepcopy(manifest);m['files']*=2;check('duplicate-delta',lambda:verify(m))
		m=copy.deepcopy(manifest);m['files'][0]['path']=other;check('extra-nondelta-review',lambda:verify(m))
		m=copy.deepcopy(manifest);m['semanticCompletenessProven']=True;check('false-semantic-acceptance',lambda:verify(m))
		m=copy.deepcopy(manifest);m['currentPolicies']=[{'path':'.gitattributes','textSha256':'0'*64}];check('stale-checkout-policy',lambda:verify(m))
		original=(base/'change.diff').read_bytes();(base/'change.diff').write_bytes(b'changed');check('mutated-review-artifact',lambda:verify(manifest));(base/'change.diff').write_bytes(original)
		pins,_,_=verify(manifest)
		p=repo/path;p.write_bytes(before);check('source-mutated-after-review',lambda:v.verify_source_pins(repo,pins,a.audit_executable,historical));p.write_bytes(after)
		extra=repo/'Engine/Added.cpp';extra.write_bytes(before);check('source-added-after-review',lambda:v.verify_source_pins(repo,pins,a.audit_executable,historical));check('unreviewed-added-source',lambda:verify(manifest));extra.unlink()
		p.unlink();check('source-deleted-after-review',lambda:v.verify_source_pins(repo,pins,a.audit_executable,historical));p.write_bytes(after)
		check('escaping-artifact-path',lambda:v.safe(base,'../escape'))
		# Use actual directory redirects, not mocked resolve results. Windows
		# junctions provide the same boundary check without symlink privileges.
		links=[]
		def directory_link(link,target):
			try:link.symlink_to(target,target_is_directory=True)
			except OSError:
				if os.name!='nt':raise
				created=subprocess.run(['cmd','/c','mklink','/J',str(link),str(target)],capture_output=True,timeout=15)
				v.require(created.returncode==0,'Cannot create redirect contract fixture: '+created.stderr.decode(errors='replace'))
			links.append(link)
		try:
			alias=root/'proof-alias';directory_link(alias,base)
			check('trusted-symlink-root-control',lambda:v.require(v.safe(alias,'change.diff')==base/'change.diff','root alias rejected'),False)
			directory_link(base/'inside-link',base)
			check('interior-same-root-redirect-rejected',lambda:v.safe(base,'inside-link/change.diff'))
			directory_link(base/'outside-link',repo)
			check('interior-escaping-redirect-rejected',lambda:v.safe(alias,'outside-link/'+path))
		finally:
			for link in reversed(links):
				if link.is_symlink():link.unlink()
				else:os.rmdir(link)
		duplicate=base/'duplicate.json';duplicate.write_text('{"x":1,"x":2}');check('duplicate-json-property',lambda:v.load(duplicate))
		output=base/'exclusive.json';output.write_bytes(b'original');check('competing-report-writer',lambda:v.write_exclusive(output,{}));check('competing-writer-preserved',lambda:v.require(output.read_bytes()==b'original','overwrite'),False)
		# Exercise the actual private Git clean path, including exact cleaned
		# bytes. These artificial roots never touch the main Git object store.
		attributes=repo/'.gitattributes';attributes.write_text('*.cpp text\n')
		blob_id=hashlib.sha1(b'blob '+str(len(before)).encode()+b'\0'+before).hexdigest();inputs=root/'raw-input';(inputs/'blobs').mkdir(parents=True);(inputs/'blobs'/blob_id).write_bytes(before)
		git_history={'files':[{'path':x,'role':'production','mode':'100644','blobId':blob_id,'blobSha256':v.digest(before),'bytes':len(before)}for x in(path,other)]}
		work=root/'clean-work';work.mkdir();_,_,_,_,clean,representations=v.materialize(repo,a.audit_executable,'git',work,git_history,inputs)
		mixed=before.replace(b'\n',b'\r\n',1)
		check('declared-text-mixed-newline-content-identity',lambda:v.require(clean(path,mixed),'identity'),False)
		check('git-clean-code-byte-change-rejected',lambda:v.require(clean(path,mixed.replace(b'return value;',b'return other;')),'identity'))
		check('git-clean-bare-cr-change-rejected',lambda:v.require(clean(path,mixed.replace(b'value;',b'va\rlue;')),'identity'))
		check('mixed-identity-receipt-retains-raw-pin',lambda:v.require(representations[0]['actualSha256']==v.digest(mixed)and representations[0]['actualBytes']==len(mixed),'raw pin'),False)
		(work/'current-policy/.gitattributes').write_text('*.cpp -text\n')
		check('binary-policy-does-not-normalize-mixed-bytes',lambda:v.require(clean(path,mixed),'identity'))
		for name,policy in [('filter-driver-never-executed','*.cpp filter=forbidden\n'),('working-tree-encoding-rejected','*.cpp working-tree-encoding=UTF-16\n')]:
			attributes.write_text(policy);bad_work=root/name;bad_work.mkdir();check(name,lambda bad_work=bad_work:v.materialize(repo,a.audit_executable,'git',bad_work,git_history,inputs))
		attributes.unlink()
		accounting={'schemaVersion':'historical-source-comment-accounting-v1','mode':'HistoricalAccounting','status':'Passed','stagePerformed':False,'currentSourceAcceptance':False,'tokenProofExecuted':False,'semanticCompletenessProven':False,'lifetimeAcceptance':False,'structuralOnly':True,'commentOnlyFiles':181,'originalOccurrences':2345,'batchCount':6,'historicalStageMismatch':{'path':'preserved-failure'}}
		check('distinct-historical-accounting-control',lambda:v.verify_historical_accounting(accounting),False)
		for name,key,value in [('portable-not-historical-accounting','schemaVersion','source-review-quality-v1'),('historical-alone-not-current-acceptance','currentSourceAcceptance',True),('historical-accounting-cannot-claim-stage','stagePerformed',True)]:
			bad=copy.deepcopy(accounting);bad[key]=value;check(name,lambda bad=bad:v.verify_historical_accounting(bad))
		old=os.environ.get('GIT_DIR');os.environ['GIT_DIR']='hostile';os.environ['GIT_CONFIG_COUNT']='99'
		try:check('ambient-git-routing-sanitized',lambda:v.require('GIT_DIR'not in v.git_environment()and'GIT_CONFIG_COUNT'not in v.git_environment(),'ambient Git escaped'),False)
		finally:
			os.environ.pop('GIT_CONFIG_COUNT',None)
			if old is None:os.environ.pop('GIT_DIR',None)
			else:os.environ['GIT_DIR']=old
	result={'schemaVersion':'source-review-continuation-contracts-v1','status':'Passed'if len(results)>=a.minimum_cases and all(x['passed']for x in results)else'Failed','structuralOnly':True,'semanticCompletenessProven':False,'lifetimeAcceptance':False,'realSourceAcceptance':False,'selectedCases':len(results),'executedCases':len(results),'failedCases':sum(not x['passed']for x in results),'cases':results,'validatorSha256':v.digest(a.validator.read_bytes()),'auditSha256':v.digest(a.audit_executable.read_bytes())};v.write_exclusive(a.report,result);print('Retained contract receipt: '+str(a.report.resolve()));print(json.dumps(result,indent=2));print(str(len(results))+' selected tests, '+str(result['failedCases'])+' failures');return 0 if result['status']=='Passed'else 1
if __name__=='__main__':raise SystemExit(main())
