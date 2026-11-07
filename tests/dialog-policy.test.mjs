import test from 'node:test';
import assert from 'node:assert/strict';
import '../public/dialog-policy.js';
test('dialog choices preserve Win32 IDs and cancellation rules', () => {
  const sets = [[1],[1,2],[3,4,5],[6,7,2],[6,7],[4,2],[2,10,11]];
  for (let kind=0;kind<sets.length;kind++) {
    const policy=MoviesDialogPolicy(kind|0x1000);
    assert.deepEqual(policy.buttons.map(b=>b.id),sets[kind]);
    assert.equal(policy.escapeId,sets[kind].includes(2)?2:kind===0?1:null);
    for(let i=0;i<sets[kind].length;i++)assert.equal(MoviesDialogPolicy(kind|(i<<8)).defaultIndex,i);
  }
  assert.throws(()=>MoviesDialogPolicy(7));
});
