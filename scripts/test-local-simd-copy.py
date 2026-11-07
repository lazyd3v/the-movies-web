#!/usr/bin/env python3
"""Synthetic local/guest copy classification and native sanitizer equivalence."""
from pathlib import Path
import importlib.util,subprocess,json,re,sys,os
ROOT=Path(__file__).resolve().parents[1];HERE=ROOT/'.cache/local-simd-tests'
HERE.mkdir(parents=True,exist_ok=True)
upstream=ROOT/'.cache/retro-port/plugins/retro-port/skills/port-pc-game-to-browser/template/recomp/translator/x86toc.py'
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
wrapper=load('translation_wrapper',ROOT/'scripts/translate-game.py')
base=load('local_base',upstream)
candidate=load('local_candidate',wrapper.prepare_translator(upstream.parents[1],HERE/'patched-tool'))
codes=['660ffec1','660ffe00','660f72e004','660fe2c1','660fe200','660f73f804','660f70c11b','660f70001b','660f6ec0','660f7ec0','0f6fc1','0f6f00','0f7f00','660ff5c1','660ff500','0f70c11b','660fc5c003','660fc4c003','660f6f00','660f7f00','f30fd6c0']
output=[];counts=[]
for index,hexcode in enumerate(codes):
 versions=[]
 for label,t in [('baseline',base),('candidate',candidate)]:
  start=0x1000;data=bytes.fromhex(hexcode+'c3');p=t.Program.__new__(t.Program);p.base=start;p.img=data;p.text_end=start+len(data);p.trace=False;p.entries={start};p.ghidra_entries={start};p.jumptables={};p.md=t.Cs(t.CS_ARCH_X86,t.CS_MODE_32);p.md.detail=True
  ft=t.FunctionTranslator(p,{'entry':start,'name':'fixture','ranges':[[start,start+len(data)]]});body,unsupported,_=ft.translate();assert not unsupported,(hexcode,unsupported)
  # Unrecognized destination identifiers may never silently become an unguarded copy.
  if label=='candidate':
   ins=next(p.md.disasm(data,start))
   try:ft.vload(ins,ins.operands[0],'HP(0x100)',16);raise AssertionError('unsafe destination accepted')
   except t.Unsupported:pass
  versions.append(body);output.append(body.replace('f_00001000',f'{label}_{index}'))
 a,b=versions
 # Exact guest-copy line fragments must remain normal memcpy, not builtins.
 guest_a=re.findall(r'(?<!_)memcpy\([^;]*HP\([^;]*;',a);guest_b=re.findall(r'(?<!_)memcpy\([^;]*HP\([^;]*;',b)
 assert guest_a==guest_b,(hexcode,guest_a,guest_b)
 assert not re.search(r'__builtin_memcpy\([^;]*HP\(',b),hexcode
 assert re.findall(r'\b(?:R|W|RF|WF)(?:8|16|32|64)\([^;]*',a)==re.findall(r'\b(?:R|W|RF|WF)(?:8|16|32|64)\([^;]*',b),(hexcode,'guest accessor changed')
 counts.append({'code':hexcode,'baselineCopies':len(re.findall(r'\bmemcpy\(',a)),'candidateProtectedCopies':len(re.findall(r'\bmemcpy\(',b)),'candidateLocalCopies':b.count('__builtin_memcpy('),'guestCopies':len(guest_a)})
(HERE/'generated.inc').write_text('\n'.join(output))
(HERE/'functions.inc').write_text('static GuestFn baselines[]={'+','.join('baseline_'+str(i) for i in range(len(codes)))+'};\nstatic GuestFn candidates[]={'+','.join('candidate_'+str(i) for i in range(len(codes)))+'};\n')
runtime=ROOT/'.cache/recomp-full/bootstrap-wasm-o1-threads/runtime'
subprocess.run(['clang','-O1','-g','-pthread','-fsanitize=address,undefined','-DMOVIES_GUEST_THREADS=1','-I'+str(runtime),'-I'+str(HERE),str(ROOT/'tests/test_local_simd_copy.c'),'-o',str(HERE/'fixture')],check=True)
subprocess.run([str(HERE/'fixture')],check=True)
(HERE/'classification.json').write_text(json.dumps(counts,indent=2)+'\n')
print('Classification tests passed for',len(codes),'register/memory instruction forms')

if '--wasm' in sys.argv:
 env=os.environ.copy();env['EM_CACHE']=str(ROOT/'.cache/recomp-full/emscripten-cache')
 emcc=ROOT/'.cache/runtime-build/emsdk-main/upstream/emscripten/emcc'
 subprocess.run([str(emcc),'-O1','-DMOVIES_GUEST_THREADS=1','-I'+str(runtime),'-I'+str(HERE),str(ROOT/'tests/test_local_simd_copy.c'),'-o',str(HERE/'fixture.cjs')],env=env,check=True)
 subprocess.run(['node',str(HERE/'fixture.cjs')],check=True)
