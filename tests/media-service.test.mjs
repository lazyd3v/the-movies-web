import test from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
const root=fileURLToPath(new URL('../',import.meta.url));
for(const fixture of ['service-shared-buffer-test.cjs','page-lifecycle-test.cjs']){
 test(`media service: ${fixture}`,()=>{
  const result=spawnSync(process.execPath,[`scripts/media-probe/${fixture}`],{cwd:root,encoding:'utf8',timeout:10000});
  assert.equal(result.error,undefined,String(result.error));
  assert.equal(result.status,0,result.stdout+result.stderr);
 });
}
