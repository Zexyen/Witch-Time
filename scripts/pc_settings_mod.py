#!/usr/bin/env python3
"""The text of the PC settings rows of the game's Options screen (runtime/options_row.c): a RomFS mod made from YOUR dump.

    python scripts/pc_settings_mod.py [--romfs <folder>] [--dump <folder>] [--out <mods folder>]

The Options screen's labels are messages of ui/ui_menu[_<lang>].dat -> messmenu.mcd, the values of its list rows messages
of a bank named by the row: the language names are in ui/ui_core[_<lang>].dat -> messcore.mcd (PlatinumGames MCD,
docs/RE_NOTES.md section 8). This adds the rows' labels (MENU_OPTION_LIST_PC_*) to the 13 messmenu.mcd and their values
(CORE_PC_*; messcore has the digits) to the 13 messcore.mcd, spelled with each file's own glyphs, and writes
<out>/witchtime-pc-settings/romfs/ui_000.pkz: the dump's archive byte for byte, with those 26 entries appended stored ("None")
and their entry records pointed at the copies. The decompressed entries come from --dump (a `witch-time-game --pkz-dump` folder),
else from build/witch-time-game --pkz-dump into a temporary folder.
Defaults: --romfs targets/romfs, --out mods (beside build/, where the runtime looks). The output holds the game's files:
it stays on your PC (git ignores mods/); never share it.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'scripts'))
import pgdat  # noqa: E402
import pkz    # noqa: E402

BRIGHTNESS = 0x5f236d04                                      # MENU_OPTION_LIST_016: its font is the list's
ON = 0x39bcb833                                              # CORE_SETTING_003 ("ON"): the font of the values
EN = ('us', 'gb', 'nl', 'pt')                                # nl and pt hold the English text


def T(en, ja, de, fr, es, it, ru, cn, tw, kr):
    return dict({'': ja, 'de': de, 'fr': fr, 'es': es, 'it': it, 'ru': ru, 'cn': cn, 'tw': tw, 'kr': kr}, **{l: en for l in EN})


def ALL(*texts):                                             # the same in every language; the first a file can spell
    return {l: texts for l in ('', 'de', 'fr', 'es', 'it', 'ru', 'cn', 'tw', 'kr') + EN}


# Where a file has not the glyphs of a word (a file only has those its own text uses), another word or the English one;
# the Chinese, Korean and Japanese files have Latin letters. A value has room for about 10 Latin or 9 Cyrillic letters.
# Names: runtime/options_row.c k_pc.
LA, VSYNC, ASPECT, FL = 'Anisotropic Filtering', 'VSync', 'Aspect Ratio', 'Frameless'
MENU = [
    ('MENU_OPTION_LIST_PC_SETTINGS', T('PC Settings', 'PC設定', 'PC-Einstellungen', 'Paramètres PC', 'Ajustes de PC', 'Impostazioni PC',
                                       'Настройки ПК', 'PC设置', 'PC設定', 'PC 설정')),
    ('MENU_OPTION_LIST_PC_RES', T('Internal Resolution (next start)', '画面サイズ (スタート時に変更)', 'Interne Auflösung (nach Neustart)',
                                  'Résolution interne (au redémarrage)', 'Resolución interna (al reiniciar)', 'Risoluzione interna (al riavvio)',
                                  'Разрешение (после перезапуска)', '画面大小 (Restart)', '畫面大小 (Restart)',
                                  ('내부 해상도 (다음 시작 시)', '해상도 (다음 시작 시)'))),     # applies at the next start
    ('MENU_OPTION_LIST_PC_CUTSCENE', T('Cutscene Frame Rate', 'イベントシーンのFPS', ('Bildrate der Zwischensequenzen', 'Bildrate der Filmszenen', 'Bildrate in Filmen'), 'Fréquence des cinématiques',
                                       'Fotogramas de las cinemáticas', 'Frame rate dei filmati', 'Частота кадров в роликах', '动画FPS',
                                       '動畫FPS', '무비 프레임 레이트')),
    ('MENU_OPTION_LIST_PC_VSYNC', T('Vertical Sync', 'Vシンク', 'Vertikale Synchronisation', 'Synchro verticale', 'Sincronización vertical',
                                    'Sincronizzazione verticale', 'Вертикальная синхронизация', VSYNC, VSYNC, VSYNC)),
    ('MENU_OPTION_LIST_PC_WINDOW', T('Display Mode', '表示モード', 'Anzeigemodus', "Mode d'affichage", 'Modo de pantalla', 'Modalità schermo',
                                     'Режим экрана', '显示模式', '顯示模式', '화면 모드')),
    ('MENU_OPTION_LIST_PC_ANISO', T(LA, LA, 'Anisotrope Filterung', 'Filtrage anisotrope', 'Filtrado anisótropo', 'Filtro anisotropico',
                                    'Анизотропная фильтрация', LA, LA, LA)),
    ('MENU_OPTION_LIST_PC_TEXTURE', T('Texture Detail', 'テクスチャ調整', 'Texturdetails', 'Détail des textures', 'Nivel de detalle',
                                      ('Dettaglio texture', 'Livello di dettaglio'), 'Детализация текстур', 'LOD调整', 'LOD設定', 'LOD 조정')),
    ('MENU_OPTION_LIST_PC_ASPECT', T('Aspect Ratio (next start)', (ASPECT + ' (スタート時に変更)', ASPECT), 'Seitenverhältnis (nach Neustart)',
                                     "Format d'image (au redémarrage)", 'Relación de aspecto (al reiniciar)', 'Proporzioni (al riavvio)',
                                     ('Формат экрана (после перезапуска)', 'Формат экрана'), ASPECT + ' (Restart)', ASPECT + ' (Restart)',
                                     (ASPECT + ' (다음 시작 시)', ASPECT))),     # aspect_ratio: applies at the next start
    ('MENU_OPTION_LIST_PC_HUD', T('HUD Layout', ('HUDの配置', 'HUD'), 'HUD-Anordnung', 'Disposition du HUD', 'Disposición del HUD',
                                  'Layout HUD', 'Расположение HUD', ('HUD布局', 'HUD'), ('HUD佈局', 'HUD'), ('HUD 배치', 'HUD'))),
    ('MENU_OPTION_LIST_PC_FPS', T('Experimental Frame Rate (next start)',      # frame_rate: experimental above 60, at the next start; no commas
                                  # (no menu file has one) and no 'x' in the ja / cn / tw files: 'beta' there
                                  ('フレームレート 実験的 (スタート時に変更)', 'FPS ベータ版 (スタート時に変更)', 'FPS Beta (スタート時に変更)'),
                                  'Experimentelle Bildrate (nach Neustart)', "Fréquence d'images expérimentale (au redémarrage)",
                                  ('Fotogramas por segundo experimentales (al reiniciar)', 'Fotogramas por segundo de prueba (al reiniciar)'),
                                  'Frame rate sperimentale (al riavvio)', 'Частота кадров (эксперимент, после перезапуска)',
                                  ('实验性帧率 (Restart)', 'FPS 试验版 (Restart)', 'FPS Beta (Restart)'),
                                  ('實驗性幀率 (Restart)', 'FPS Beta (Restart)'),
                                  ('실험적 프레임 레이트 (다음 시작 시)', '테스트 프레임 레이트 (다음 시작 시)', 'Experimental FPS (다음 시작 시)'))),
]
CORE = [
    ('CORE_PC_AUTO', T('Auto', '自動', 'Auto', 'Auto', 'Automática', 'Automatica', 'Авто', ('自动', 'Auto'), ('自動', 'Auto'), '자동')),
    ('CORE_PC_MONITOR', T('Monitor', ('モニター', 'Monitor'), 'Monitor', 'Moniteur', 'Monitor', 'Monitor', 'Монитор', ('显示器', 'Monitor'),
                          ('顯示器', 'Monitor'), ('모니터', 'Monitor'))),           # vsync = display: frames at the display's refresh
    ('CORE_PC_TIMER', T('Timer', ('タイマー', 'Timer'), 'Timer', 'Minuterie', 'Reloj', 'Timer', 'Таймер', ('计时器', 'Timer'),
                        ('計時器', 'Timer'), ('타이머', 'Timer'))),                  # vsync = clock: the program's 60 Hz clock
    ('CORE_PC_WINDOWED', T('Windowed', 'ウィンドウ', 'Fenster', 'Fenêtré', 'Ventana', 'Finestra', 'Окно', 'Windowed', 'Windowed', 'Windowed')),
    ('CORE_PC_BORDERLESS', T('Borderless', ('ボーダーレス', FL), 'Randlos', 'Sans cadre', 'Sin bordes', 'Senza bordi', 'Без рамки',
                             ('Borderless', FL), ('Borderless', FL), ('테두리 없음', FL))),
    ('CORE_PC_FULLSCREEN', T('Fullscreen', 'Fullscreen', 'Vollbild', 'Plein écran', 'Completa', 'Intero', 'Полный',
                             'Fullscreen', 'Fullscreen', '전체 화면')),
    ('CORE_PC_DEFAULT', T('Default', '初期値', 'Standard', 'Par défaut', 'Estándar', 'Predefinito', 'Стандарт', '标准', '標準', '기본')),
    ('CORE_PC_FIXED', T('Fixed', ('固定', 'Fixed'), 'Fest', 'Fixe', 'Fijo', 'Fisso', 'Фиксир.', ('固定', 'Fixed'), ('固定', 'Fixed'),
                        ('고정', 'Fixed'))),                                        # aspect_ratio = 16:9
    ('CORE_PC_UNLOCKED', T('Unlocked', ('自由', 'Unlocked'), 'Frei', 'Libre', 'Libre', 'Libero', 'Свободно', ('自由', 'Unlocked'),
                           ('自由', 'Unlocked'), ('자유', 'Unlocked'))),                # aspect_ratio = auto: the window's shape
    ('CORE_PC_HUD_FULL', T('Full Screen', ('全画面', 'Full Screen'), 'Vollbild', 'Plein écran', 'Completa', 'Intero', 'Весь экран',
                           ('全屏', 'Full Screen'), ('全螢幕', 'Full Screen'), ('전체 화면', 'Full Screen'))),       # hud_area = screen
    ('CORE_PC_HUD_CENTER', T('Centered', ('中央', 'Centered'), 'Zentriert', 'Centré', 'Centrado', 'Centrato', 'По центру',
                             ('居中', 'Centered'), ('置中', 'Centered'), ('가운데', 'Centered'))),               # hud_area = 16:9
    ('CORE_PC_CUSTOM', T('Custom', 'カスタム', 'Eigene', 'Autre', 'Otro', 'Altro', 'Свой', '其他', '其他', '사용자 지정')),
    ('CORE_PC_FPS_30', ALL('30')), ('CORE_PC_FPS_60', ALL('60')),
    ('CORE_PC_AF_2', ALL('2x', '2')), ('CORE_PC_AF_4', ALL('4x', '4')), ('CORE_PC_AF_8', ALL('8x', '8')), ('CORE_PC_AF_16', ALL('16x', '16')),
    ('CORE_PC_LOD_M1', ALL('-1')), ('CORE_PC_LOD_M05', ALL('-0.5')), ('CORE_PC_LOD_0', ALL('0')), ('CORE_PC_LOD_P05', ALL('+0.5')),
    ('CORE_PC_LOD_P1', ALL('+1')),
]
CORE += [('CORE_PC_RES_' + v.replace('.', ''), ALL(v)) for v in ('1.0', '1.2', '1.6', '2.4')]
CORE += [('CORE_PC_FPS_90', ALL('90')), ('CORE_PC_FPS_120', ALL('120'))]
LANGS = list(MENU[0][1])
MOD = 'witchtime-pc-settings'
OLD_MOD = 'b3recomp-pc-settings'                 # its name before the rename to Witch Time (2026-10-27)


# ---- MCD: header {u32 offset, count} x 5 (messages, symbols, glyphs, fonts, events); message {paragraphs, count, seq,
# event hash}; paragraph {lines, count, v, h, font}; line {text, 0, units, units, below, horiz}; text = u16 pairs
# (symbol, kerning), 0x8001 <width> = a space, 0x8000 = end; symbol {u16 font, u16 char, u32 glyph}; event {hash, message,
# char name[32]} sorted by hash. Sections in this order, each but the last followed by 4 zero bytes (the text by 1-4, to a
# multiple of 4).
def mcd_read(d):
    mo, mc, so, sc, go, gc, fo, fc, eo, ec = struct.unpack_from('<10I', d, 0)
    msgs = []
    for i in range(mc):
        po, pc, seq, ev = struct.unpack_from('<4I', d, mo + 16 * i)
        pars = []
        for j in range(pc):
            lo, lc, vp, hp, font = struct.unpack_from('<5I', d, po + 20 * j)
            lines = []
            for k in range(lc):
                off, pad, n, n2, below, horiz = struct.unpack_from('<4I2f', d, lo + 24 * k)
                lines.append([list(struct.unpack_from('<%dH' % n, d, off)), pad, n2 - n, below, horiz, off])
            pars.append([vp, hp, font, lines])
        msgs.append([seq, ev, pars])
    return dict(msgs=msgs, syms=[struct.unpack_from('<2HI', d, so + 8 * i) for i in range(sc)],
                glyphs=[d[go + 40 * i:go + 40 * i + 40] for i in range(gc)], fonts=[d[fo + 20 * i:fo + 20 * i + 20] for i in range(fc)],
                evs=[struct.unpack_from('<II32s', d, eo + 40 * i) for i in range(ec)])


def mcd_write(m):
    lines = [ln for msg in m['msgs'] for p in msg[2] for ln in p[3]]
    out = bytearray(40)
    toff = {}
    for i in sorted(range(len(lines)), key=lambda i: lines[i][5]):     # texts in their original order, new ones last
        toff[i] = len(out)
        out += struct.pack('<%dH' % len(lines[i][0]), *lines[i][0])
    out += bytes(((len(out) + 4) & ~3) - len(out))
    npar = sum(len(msg[2]) for msg in m['msgs'])
    mo = len(out); out += bytes(16 * len(m['msgs']) + 4)
    po = len(out); out += bytes(20 * npar + 4)
    lo = len(out); out += bytes(24 * len(lines) + 4)
    pi = li = 0
    for i, (seq, ev, pars) in enumerate(m['msgs']):
        struct.pack_into('<4I', out, mo + 16 * i, po + 20 * pi, len(pars), seq, ev)
        for vp, hp, font, ls in pars:
            struct.pack_into('<5I', out, po + 20 * pi, lo + 24 * li, len(ls), vp, hp, font)
            pi += 1
            for us, pad, extra, below, horiz, _ in ls:
                struct.pack_into('<4I2f', out, lo + 24 * li, toff[li], pad, len(us), len(us) + extra, below, horiz)
                li += 1
    heads = []
    for key, fmt in (('syms', '<2HI'), ('glyphs', None), ('fonts', None), ('evs', '<II32s')):
        heads += [len(out), len(m[key])]
        for x in m[key]:
            out += struct.pack(fmt, *x) if fmt else x
        out += bytes(4 if key != 'evs' else 0)
    struct.pack_into('<10I', out, 0, mo, len(m['msgs']), *heads)
    return bytes(out)


def add_msgs(d, msgs, lang, font_of):
    """d (a messmenu.mcd or messcore.mcd) with the messages msgs = [(name, {lang: text or (texts...)})], in the font of the
    message font_of; returns (the file, [what could not be spelled])"""
    m = mcd_read(d)
    assert mcd_write(m) == d, 'MCD: this layout is not the one known (the file would not be rebuilt as it is)'
    syms, bad = m['syms'], []
    font = m['msgs'][next(i for h, i, _ in m['evs'] if h == font_of)][2][0][2]
    kern, kern1, space = {}, {}, {}
    for _, _, pars in m['msgs']:                             # kerning after a symbol (before the next), space widths, in this font
        for _, _, f, ls in pars:
            for us, *_ in ls:
                if f != font:
                    continue
                pairs = [(us[i], us[i + 1]) for i in range(0, len(us) - 1, 2)]
                for (a, ka), (b, _) in zip(pairs, pairs[1:] + [(0x8000, 0)]):
                    if a == 0x8001:
                        space[ka] = space.get(ka, 0) + 1
                    elif a < 0x8000:
                        kern[a, b] = ka
                        kern1.setdefault(a, {}).setdefault(ka, 0)
                        kern1[a][ka] += 1
    order = [font, 7] + sorted({f for f, _, _ in syms} - {font, 7})      # 7: the Latin font (the Chinese / Korean files' "PC")

    def sym(ch, fonts=order):                               # another font's glyph has its own size: the last resort
        return next((i for f in fonts for i, (sf, c, _) in enumerate(syms) if sf == f and c == ord(ch)), None)
    have = {h for h, _, _ in m['evs']}
    for name, texts in msgs:
        h = zlib.crc32(name.lower().encode()) & 0x7fffffff
        if h in have:
            continue
        cands = texts[lang] if isinstance(texts[lang], tuple) else (texts[lang],)
        text = next((t for k in (order[:2], order) for t in cands if all(ch == ' ' or sym(ch, k) is not None for ch in t)), cands[0])
        missing = ''.join(sorted({ch for ch in text if ch != ' ' and sym(ch) is None}))
        other = ''.join(sorted({ch for ch in text if ch != ' ' and sym(ch) is not None and sym(ch, order[:2]) is None}))
        if missing:                                          # (nl / pt messcore: no digits) spelled without them
            bad.append('%s %r without %r' % (name, text, missing))
        if other:
            bad.append('%s %r: %r in another font' % (name, text, other))
        seq = [0x8001 if ch == ' ' else sym(ch) for ch in text if ch == ' ' or sym(ch) is not None] + [0x8000]
        us = []
        for a, b in zip(seq, seq[1:] + [None]):
            if a == 0x8000:
                us.append(a)
            elif a == 0x8001:
                us += [a, max(space, key=space.get) if space else 7]
            else:
                us += [a, kern.get((a, b), max(kern1[a], key=kern1[a].get) if a in kern1 else 0)]
        m['msgs'].append([len(m['msgs']) + 1, h, [[0, 0, font, [[us, 0, 0, 0.0, 0.0, 1 << 40]]]]])
        m['evs'].append((h, len(m['msgs']) - 1, name.encode()))
    m['evs'].sort(key=lambda e: e[0])
    return mcd_write(m), bad


def dat_replace(blob, member, data):
    """the DAT with `member` appended at the end (16-byte aligned) and its offset / size pointed there"""
    _, count, off_t, _, _, size_t, _ = struct.unpack_from('<4s6I', blob, 0)
    i = next(k for k, (name, _, _, _) in enumerate(pgdat.entries(blob)) if name == member)
    out = bytearray(blob) + bytes(-len(blob) % 16)
    struct.pack_into('<I', out, off_t + 4 * i, len(out))
    struct.pack_into('<I', out, size_t + 4 * i, len(data))
    out += data + bytes(-len(data) % 16)
    return bytes(out)


def main(argv):
    args = dict(zip(argv[::2], argv[1::2]))
    if set(args) - {'--romfs', '--dump', '--out'} or len(argv) % 2:
        sys.exit(__doc__)
    romfs = args.get('--romfs', os.path.join(REPO, 'targets', 'romfs'))
    arc = os.path.join(romfs, 'ui_000.pkz')
    out = os.path.join(args.get('--out', os.path.join(REPO, 'mods')), MOD, 'romfs', 'ui_000.pkz')
    fsize, entries = pkz.read_pkz(arc)
    names = ['ui/ui_%s%s.dat' % (k, '_' + l if l else '') for l in LANGS for k in ('menu', 'core')]
    assert all(any(e[0] == n for e in entries) for n in names), 'ui_000.pkz: not the archive known'
    dump, tmp = args.get('--dump'), None
    if not dump:
        tmp = dump = tempfile.mkdtemp(prefix='wtpcmod_')
        exe = os.path.join(REPO, 'build', 'witch-time-game' + ('.exe' if os.name == 'nt' else ''))
        r = subprocess.run([exe, os.path.join(REPO, 'targets', 'exefs', 'main.elf'), '--pkz-dump', tmp, 'ui/ui_menu', 'ui/ui_core'],
                           cwd=os.path.join(REPO, 'build'), env=dict(os.environ, SSBU_ROMFS=romfs), capture_output=True, text=True)
        if r.returncode:
            sys.exit('witch-time-game --pkz-dump failed:\n' + r.stdout[-2000:] + r.stderr[-2000:])
    try:
        new, bad = {}, []
        for n in names:
            lang, kind = n[len('ui/ui_menu_'):-4] if '_' in n[6:] else '', n[6:10]
            blob = open(os.path.join(dump, n), 'rb').read()
            member = 'mess%s.mcd' % kind
            mcd = next(blob[o:o + s] for name, _, o, s in pgdat.entries(blob) if name == member)
            d, b = add_msgs(mcd, MENU if kind == 'menu' else CORE, lang, BRIGHTNESS if kind == 'menu' else ON)
            new[n] = dat_replace(blob, member, d)
            bad += ['%s: %s' % (n, x) for x in b]
    finally:
        if tmp:
            shutil.rmtree(tmp)
    with open(arc, 'rb') as f:
        head = f.read(0x20)
        _, _, _, count, toff, nsize, align = struct.unpack_from('<4sIQIIII', head)
        f.seek(toff)
        table = bytearray(f.read(count * 0x20 + nsize))
    none = bytes(table[count * 0x20:]).index(b'None\0')
    assert none == 0 or table[count * 0x20 + none - 1] == 0
    # the same mod under its old name would serve ui_000.pkz first (mods sort by name), and options_row.c looks for MOD
    shutil.rmtree(os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(out))), OLD_MOD), ignore_errors=True)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    shutil.copyfile(arc, out + '.tmp')
    pos = (fsize + align - 1) // align * align
    with open(out + '.tmp', 'r+b') as f:
        for i, e in enumerate(entries):
            if e[0] in new:
                d = new[e[0]]
                no, = struct.unpack_from('<I', table, i * 0x20)
                struct.pack_into('<IIQQQ', table, i * 0x20, no, none, len(d), pos, len(d))
                f.seek(pos)
                f.write(d)
                pos = (pos + len(d) + align - 1) // align * align
        f.truncate(pos)
        f.seek(0)
        f.write(struct.pack('<4sIQIIII', b'pkzl', 0x20000, pos, count, toff, nsize, align) + table)
    os.replace(out + '.tmp', out)
    for n in sorted({x.split(':')[0] for x in bad}):          # (nl / pt messcore: no digits, not even the game's ON)
        xs = [x.split(': ', 1)[1] for x in bad if x.startswith(n + ':')]
        print('  %s: %d texts not as written (glyphs the file has not, or of another font): %s' % (n, len(xs), '; '.join(xs) if len(xs) < 4 else xs[0] + '; ...'))
    print('%s: %d bytes, %d text files with the PC settings rows\' text (%d labels, %d values)' % (out, pos, len(new), len(MENU), len(CORE)))


if __name__ == '__main__':
    main(sys.argv[1:])
