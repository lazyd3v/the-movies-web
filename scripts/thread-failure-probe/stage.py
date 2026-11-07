from pathlib import Path
import shutil,hashlib,json
root=Path(__file__).resolve().parents[2];source=Path(__file__).parent;out=root/'public/runtime/thread-failure-probe';out.mkdir(parents=True,exist_ok=True)
text=(root/'public/static-probe.js').read_text();helper=text.split('// BEGIN THREAD FAILURE WATCHDOG\n')[1].split('// END THREAD FAILURE WATCHDOG')[0]
(out/'watchdog.js').write_text(helper)
for name in ('index.html','parent.js','child.js','page.js'):shutil.copyfile(source/name,out/name)
(out/'manifest.json').write_text(json.dumps({'productionWatchdogSHA256':hashlib.sha256(helper.encode()).hexdigest(),'ownedWorkerHostSHA256':hashlib.sha256((root/'public/owned-worker-host.js').read_bytes()).hexdigest(),'ownedWorkerClientSHA256':hashlib.sha256((root/'public/owned-worker-client.js').read_bytes()).hexdigest(),'scope':'Synthetic shared child failure; exact production watcher and page-owned workers; no original game execution'},indent=2)+'\n')
print('http://127.0.0.1:4173/runtime/thread-failure-probe/')
