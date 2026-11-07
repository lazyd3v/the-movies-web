'use strict';
(function(root) {
  // A surface owns an actual canvas; rendering is supplied separately by D3D.
  root.MoviesWindowSurface = class {
    constructor(container, document, onInput = null, pointerPosition = null) {
      this.container = container;
      this.document = document;
      this.windows = new Map();
      this.geometry = new Map();
      this.restoreButtons = new Map();
      this.onInput = onInput; this.pointerPosition = pointerPosition;
    }
    apply(event) {
      if (!Number.isInteger(event.hwnd) || event.hwnd <= 0) throw new Error('Invalid window handle');
      const existing = this.windows.get(event.hwnd);
      if (event.type === 'create' || event.type === 'show') {
        for (const dimension of [event.clientWidth, event.clientHeight]) {
          if (!Number.isInteger(dimension) || dimension < 1 || dimension > 8192) throw new Error('Unsupported window dimensions');
        }
      }
      if (event.type === 'create') {
        if (existing || this.windows.size) throw new Error('Only one game window is supported');
        const canvas = this.document.createElement('canvas');
        canvas.width = event.clientWidth;
        canvas.height = event.clientHeight;
        canvas.tabIndex = 0;
        canvas.setAttribute('aria-label', String(event.title || 'The Movies'));
        canvas.style.cursor = 'default';
        canvas.hidden = !event.visible || !!event.minimized;
        this.container.append(canvas);
        this.windows.set(event.hwnd, canvas);
        this.geometry.set(event.hwnd, event);
        if (this.onInput) {
          const restore = this.document.createElement('button');
          restore.textContent = 'Restore game window';
          restore.hidden = !event.minimized;
          restore.addEventListener('click', () => {
            if (!this.geometry.get(event.hwnd)?.minimized) return;
            this.onInput({hwnd:event.hwnd,message:0x112,wParam:0xf120,lParam:0,
              time:Math.floor(performance.timeOrigin+performance.now())>>>0,x:0,y:0});
          });
          this.container.append(restore);
          this.restoreButtons.set(event.hwnd,restore);
          const pointer = e => {
            if (e.pointerType !== 'mouse') return;
            const state = this.geometry.get(event.hwnd);
            const rect = canvas.getBoundingClientRect();
            if (!state || !rect.width || !rect.height) return;
            const message = e.type === 'pointermove' ? 0x200 :
              ({pointerdown: [0x201, 0x207, 0x204], pointerup: [0x202, 0x208, 0x205]})[e.type]?.[e.button];
            if (!message) return;
            const virtual=this.pointerPosition?.();
            const x = virtual ? virtual.x-state.clientX : Math.floor((e.clientX - rect.left) * canvas.width / rect.width);
            const y = virtual ? virtual.y-state.clientY : Math.floor((e.clientY - rect.top) * canvas.height / rect.height);
            const keys = (e.buttons & 3) | ((e.buttons & 4) ? 0x10 : 0) | (e.shiftKey ? 4 : 0) | (e.ctrlKey ? 8 : 0);
            this.onInput({hwnd: event.hwnd, message, wParam: keys, lParam: ((y & 65535) << 16) | (x & 65535),
              time: Math.floor(performance.timeOrigin + performance.now()) >>> 0, x: state.clientX + x, y: state.clientY + y});
            e.preventDefault();
            if (e.type === 'pointerdown') { canvas.focus(); canvas.setPointerCapture(e.pointerId); }
            if (e.type === 'pointerup' && !e.buttons && canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);
          };
          for (const name of ['pointermove', 'pointerdown', 'pointerup']) canvas.addEventListener(name, pointer);
          canvas.addEventListener('contextmenu', e => e.preventDefault());
        }
      } else {
        if (!existing) throw new Error('Unknown game window');
        if (event.type === 'show') {
          // Assigning either canvas dimension clears its backing store.
          if (existing.width !== event.clientWidth) existing.width = event.clientWidth;
          if (existing.height !== event.clientHeight) existing.height = event.clientHeight;
          existing.hidden = !event.visible || !!event.minimized;
          this.geometry.set(event.hwnd, event);
          const restore=this.restoreButtons.get(event.hwnd);
          if (restore) restore.hidden=!event.minimized;
        }
        else if (event.type === 'cursor') {
          if (!['default', 'none'].includes(event.cursor)) throw new Error('Unsupported cursor');
          existing.style.cursor = event.cursor;
        } else if (event.type === 'destroy') {
          existing.remove();
          this.restoreButtons.get(event.hwnd)?.remove();
          this.restoreButtons.delete(event.hwnd);
          this.windows.delete(event.hwnd);
          this.geometry.delete(event.hwnd);
        } else throw new Error('Unsupported window event');
      }
      return 1;
    }
    clear() {
      for (const canvas of this.windows.values()) canvas.remove();
      this.windows.clear();
      for (const button of this.restoreButtons.values()) button.remove();
      this.restoreButtons.clear();
      this.geometry.clear();
    }
  };
})(globalThis);
