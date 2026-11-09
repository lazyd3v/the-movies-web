'use strict';
(function(root) {
  /* KeyboardEvent.code -> Windows virtual-key code (layout-independent positions,
   * US labels). https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes */
  const VK = {Backspace:0x08,Tab:0x09,Enter:0x0d,NumpadEnter:0x0d,ShiftLeft:0x10,ShiftRight:0x10,ControlLeft:0x11,ControlRight:0x11,
    AltLeft:0x12,AltRight:0x12,Pause:0x13,CapsLock:0x14,Escape:0x1b,Space:0x20,PageUp:0x21,PageDown:0x22,End:0x23,Home:0x24,
    ArrowLeft:0x25,ArrowUp:0x26,ArrowRight:0x27,ArrowDown:0x28,PrintScreen:0x2c,Insert:0x2d,Delete:0x2e,
    NumpadMultiply:0x6a,NumpadAdd:0x6b,NumpadSubtract:0x6d,NumpadDecimal:0x6e,NumpadDivide:0x6f,NumLock:0x90,ScrollLock:0x91,
    Semicolon:0xba,Equal:0xbb,Comma:0xbc,Minus:0xbd,Period:0xbe,Slash:0xbf,Backquote:0xc0,BracketLeft:0xdb,Backslash:0xdc,
    BracketRight:0xdd,Quote:0xde,IntlBackslash:0xe2,MetaLeft:0x5b,MetaRight:0x5c,ContextMenu:0x5d};
  for (let i = 0; i < 26; i++) VK['Key' + String.fromCharCode(65 + i)] = 65 + i;
  for (let i = 0; i < 10; i++) { VK['Digit' + i] = 0x30 + i; VK['Numpad' + i] = 0x60 + i; }
  for (let i = 1; i <= 12; i++) VK['F' + i] = 0x6f + i;
  root.MoviesVirtualKeys = Object.freeze(VK);
  /* WM_KEYDOWN/WM_KEYUP for the game's WndProc, which
   * turns key-downs into characters with ToUnicode/ToAscii (text entry). lParam:
   * repeat 1, scan code, extended flag, previous state, transition. */
  root.MoviesKeyMessage = (e, hwnd, x = 0, y = 0) => {
    const vk = VK[e.code]; if (vk === undefined) return null;
    const scan = root.MoviesDirectInputKeys?.[e.code] ?? 0, up = e.type === 'keyup';
    const lParam = (1 | ((scan & 0x7f) << 16) | (scan & 0x80 ? 1 << 24 : 0) | (up || e.repeat ? 1 << 30 : 0) | (up ? 1 << 31 : 0)) >>> 0;
    // Always WM_KEYDOWN/UP: WM_SYSKEY* would reach DefWindowProc's unmodelled menu semantics.
    return {hwnd, message: 0x100 + (up ? 1 : 0), wParam: vk, lParam, time: Math.floor(performance.timeOrigin + performance.now()) >>> 0, x, y};
  };
  // A surface owns an actual canvas; rendering is supplied separately by D3D.
  root.MoviesWindowSurface = class {
    constructor(container, document, onInput = null, pointerPosition = null) {
      this.container = container;
      this.document = document;
      this.windows = new Map();
      this.cursors = new Map();
      this.geometry = new Map();
      // The game hides the system cursor and draws its own. That only works while
      // the mouse is captured; once released (Escape), show the system cursor so the
      // player can reach the page and browser UI.
      document.addEventListener?.('pointerlockchange', () => this.syncCursor());
      this.restoreButtons = new Map();
      this.onInput = onInput; this.pointerPosition = pointerPosition;
    }
    syncCursor() {
      for (const [hwnd, canvas] of this.windows) canvas.style.cursor = this.document.pointerLockElement === canvas ? (this.cursors.get(hwnd) ?? 'default') : 'default';
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
          const keyboard = e => {
            const state = this.geometry.get(event.hwnd); const message = root.MoviesKeyMessage(e, event.hwnd, state?.clientX ?? 0, state?.clientY ?? 0);
            if (!message) return; this.onInput(message);
            if (e.code === 'Tab' || e.code === 'Backspace' || e.code === 'Space' || e.code.startsWith('Arrow') || e.altKey) e.preventDefault();
          };
          canvas.addEventListener('keydown', keyboard); canvas.addEventListener('keyup', keyboard);
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
          this.cursors.set(event.hwnd, event.cursor); this.syncCursor();
        } else if (event.type === 'destroy') {
          existing.remove();
          this.restoreButtons.get(event.hwnd)?.remove();
          this.restoreButtons.delete(event.hwnd);
          this.windows.delete(event.hwnd); this.cursors.delete(event.hwnd);
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
