#!/usr/bin/env python3
"""Download one pinned official package locally, verify and run bounded references."""
import argparse,base64,hashlib,shutil,subprocess,tarfile,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2];CACHE=ROOT/'.cache/media-probe'
p=argparse.ArgumentParser();p.add_argument('input',type=Path);p.add_argument('--ffmpeg',default='ffmpeg');args=p.parse_args()
data=args.input.read_bytes()
assert len(data)==1235199 and hashlib.sha256(data).hexdigest()=='1ef433c169969e03e01869e0cf076bf89e7e09fa6d141259835c8790f9755c09','Unexpected original clip'
CACHE.mkdir(parents=True,exist_ok=True);archive=CACHE/'core-0.12.10.tgz'
if not archive.exists():
 with urllib.request.urlopen('https://registry.npmjs.org/@ffmpeg/core/-/core-0.12.10.tgz',timeout=60) as r: archive.write_bytes(r.read(30*1024*1024))
assert base64.b64encode(hashlib.sha512(archive.read_bytes()).digest()).decode()=='dzNplnn2Nxle2c2i2rrDhqcB19q9cglCkWnoMTDN9Q9l3PvdjZWd1HfSPjCNWc/p8Q3CT+Es9fWOR0UhAeYQZA=='
with tarfile.open(archive) as t:t.extractall(CACHE/'core',filter='data')
(CACHE/'input.wmv').write_bytes(data)
subprocess.run(['node','scripts/media-probe/decode.cjs'],cwd=ROOT,check=True,timeout=60)
with (CACHE/'reference.log').open('w') as log:
 subprocess.run([args.ffmpeg,'-nostdin','-y','-i',str(CACHE/'input.wmv'),'-map','0:v:0','-frames:v','1','-pix_fmt','yuv420p','-f','rawvideo',str(CACHE/'reference.yuv'),'-map','0:a:0','-t','1','-acodec','pcm_s16le','-f','s16le',str(CACHE/'reference.pcm')],check=True,stdout=log,stderr=log,timeout=60)
subprocess.run(['node','scripts/media-probe/encode.cjs'],cwd=ROOT,check=True,timeout=60)
subprocess.run(['node','scripts/media-probe/stream-node.cjs'],cwd=ROOT,check=True,timeout=60)
subprocess.run(['node','scripts/media-probe/independent-node.cjs'],cwd=ROOT,check=True,timeout=60)
subprocess.run(['python3','scripts/media-probe/stream-reference.py'],cwd=ROOT,check=True,timeout=60)
subprocess.run([args.ffmpeg,'-v','error','-nostdin','-y','-i',str(CACHE/'input.wmv'),'-map','0:v:0','-frames:v','1','-pix_fmt','bgr24','-f','rawvideo',str(CACHE/'service-frame.bgr')],check=True,timeout=60)
subprocess.run(['node','scripts/media-probe/service-test.cjs'],cwd=ROOT,check=True,timeout=60)
assert args.input.read_bytes()==data,'Original changed during probe'
subprocess.run(['python3','scripts/media-probe/stage.py'],cwd=ROOT,check=True)
