"""Measured, bounded pointer input for QEMU integration tests (no blind aiming).
Mixin expects cmd/key/mem/num methods and a symbols dict on the guest object.
"""
import struct
import time

class MouseGUI:
    def move(self, x, y):
        for _ in range(12):
            dx, dy = x - self.num('mx'), y - self.num('my')
            if abs(dx) <= 2 and abs(dy) <= 2:
                return
            self.cmd(f'mouse_move {max(-100,min(100,dx))} {max(-100,min(100,dy))}')
            time.sleep(.045)
        raise AssertionError(f'Pointer failed to reach measured target {x,y}')

    def click(self, x, y):
        self.move(x, y)
        self.cmd('mouse_button 1'); time.sleep(.10)
        self.cmd('mouse_button 0'); time.sleep(.12)

    def windows(self):
        out = []
        for i, fields in enumerate(struct.iter_unpack('<4i32s6i', self.mem('wins', 8*72))):
            x,y,w,h,title,used,visible,minimized,app,accent,scroll = fields
            if used:
                out.append(dict(id=i,x=x,y=y,w=w,h=h,app=app,visible=visible,minimized=minimized))
        return out

    def window(self, app):
        found = [w for w in self.windows() if w['app'] == app]
        assert len(found) == 1, (app, self.windows())
        return found[0]

    def menu(self, app=None, item=None):
        h = self.num('scr_h')
        self.click(40, h-14)
        assert self.num('launcher_open') == 1
        icons = list(struct.iter_unpack('<3i', self.mem('icons', 10*12)))
        row = next(i for i, (_,_,a) in enumerate(icons) if a == app) if app is not None else item
        self.click(130, h-28-(30+12*26)+30+row*26+13)
        time.sleep(.15)

    def open_app(self, app):
        if app == 10:
            w=self.window(app)
            order=struct.unpack('<8i',self.mem('z_order',32))[:self.num('win_count')]
            if order[-1]!=w['id'] or w['minimized']:
                self.click(94+order.index(w['id'])*138+66,self.num('scr_h')-14)
        else:
            self.menu(app=app)
        assert self.window(app)['minimized'] == 0

    def hits(self, app=None):
        win = None if app is None else self.window(app)['id']
        for _ in range(25):
            count=self.num('hit_count')
            table=list(struct.iter_unpack('<6i',self.mem('hits',count*24))) if count else []
            filtered=[t for t in table if win is None or t[5]==win]
            ids={t[4] for t in filtered}
            valid=count==self.num('hit_count') and len({(t[4],t[5]) for t in table})==len(table)
            if app in (7,9):
                mode=self.mode(app)
                expected={82,83} if mode else ({92} if app==9 and self.num('dev_help') else {40,88})
                valid=valid and expected.issubset(ids)
                if app==9 and self.num('dev_help') and not mode: valid=valid and ids=={92}
            if valid: return filtered
            time.sleep(.025)
        raise AssertionError(('GUI did not finish building widgets',app,count,filtered))

    def widget(self, identifier, app=None):
        # HMP memory reads can sample the GUI while it rebuilds its hit list.
        # Wait on the measured widget, never retry a blind click.
        for _ in range(15):
            found = [t for t in self.hits(app) if t[4] == identifier]
            if found: break
            time.sleep(.08)
        assert found, ('widget not visible', identifier, app, self.hits(app))
        x,y,w,h,_,_ = found[-1]
        self.click(x+w//2, y+h//2)

    def close_app(self, app, discard=False):
        self.open_app(app)
        w = self.window(app)
        self.click(w['x']+w['w']-13, w['y']+11)
        if discard and any(t[4] == 84 for t in self.hits(app)):
            self.widget(84, app)

    def exit_gui(self):
        self.menu(item=11)
        for _ in range(2):
            if not self.num('running'):
                break
            self.widget(84)  # explicit Don't save on a private test document
        time.sleep(.2)
        assert self.num('running') == 0

    def document(self, app=7):
        symbol = 'dev_editor' if app == 9 else 'note_editor'
        raw = self.mem(symbol, 128*121+9*4)
        lines,cy,cx,top,left,dirty,ay,ax,selecting = struct.unpack_from('<9i', raw, 128*121)
        text = b'\n'.join(raw[r*121:(r+1)*121].split(b'\0')[0] for r in range(lines))
        return dict(text=text,lines=lines,cy=cy,cx=cx,top=top,left=left,dirty=dirty,ay=ay,ax=ax,selecting=selecting)

    def mode(self, app=7):
        symbol = 'dev_editor' if app == 9 else 'note_editor'
        return struct.unpack('<i', self.mem(self.symbols[symbol]+2*(128*121+9*4)+8, 4))[0]

    def choose_name(self, text):
        # Text entry only. Navigation and confirmation use actual clickable widgets.
        self.key('ctrl-u')
        self.text(text)

    def screenshot(self, path):
        from PIL import Image
        from pathlib import Path
        path=Path(path); path.parent.mkdir(parents=True,exist_ok=True)
        ppm=path.with_suffix('.ppm')
        self.cmd(f'screendump "{ppm}"')
        Image.open(ppm).save(path); ppm.unlink()
