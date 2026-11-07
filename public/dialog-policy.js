'use strict';
(function(root) {
  const sets = [
    [[1,'OK']], [[1,'OK'],[2,'Cancel']], [[3,'Abort'],[4,'Retry'],[5,'Ignore']],
    [[6,'Yes'],[7,'No'],[2,'Cancel']], [[6,'Yes'],[7,'No']],
    [[4,'Retry'],[2,'Cancel']], [[2,'Cancel'],[10,'Try Again'],[11,'Continue']],
  ];
  root.MoviesDialogPolicy = flags => {
    const items = sets[flags & 15];
    if (!items) throw new Error('Unsupported message box buttons');
    const buttons = items.map(([id,label]) => ({id,label}));
    const requested = (flags >>> 8) & 3;
    return {buttons, defaultIndex: requested < buttons.length ? requested : 0,
      escapeId: buttons.some(b=>b.id===2) ? 2 : (buttons.length===1 ? 1 : null)};
  };
})(globalThis);
