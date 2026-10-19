# Buttons for the MKS TFT of the Robin Nano (klipper-hellbot fork)
#
# Configuration in printer.cfg:
#
# [hbtft_buttons]
# btn1_label: HOME
# btn1_gcode: G28
# btn2_label: UNLOAD
# btn2_gcode: M702
# ...
#
# Labels (max 16 chars, displayed uppercase) are drawn on the TFT
# buttons; tapping a button runs its gcode.
#
# MENUS / PAGES (host-only, no firmware rebuild):
#   - Page 1 is the classic btnN_* set above.
#   - Extra pages: pageN_btnM_label / pageN_btnM_gcode /
#     pageN_btnM_color (e.g. page2_btn1_label: HEAT).
#   - A button with gcode "PAGE:2" switches to page 2; to go back,
#     put a button on page 2 with gcode "PAGE:1".
#   - You can also switch from the console or a macro: HBTFT_PAGE PAGE=2
#
# TOUCH CALIBRATION (all in host, no firmware rebuild):
# the MCU reports raw XPT2046 coordinates in the hb_button message,
# and this module maps them using these parameters:
#
#   xpt_x_min / xpt_x_max : raw ADC range of the X axis (tap the edges
#       and note the values logged as "message received" in klippy.log)
#   xpt_y_min / xpt_y_max : same for the Y axis
#   flip_x / flip_y       : 1 if the axis is inverted
#
# With a rough calibration the buttons still work when tapped near the
# center; adjust min/max until the edges respond correctly.
import logging
import re


class HBTFTButtons:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.gcode = self.printer.lookup_object('gcode')
        self.reactor = self.printer.get_reactor()
        self.labels = []
        self.gcodes = []
        self.colors = []
        for i in range(6):
            self.labels.append(
                config.get('btn%d_label' % (i + 1), 'BTN%d' % (i + 1)))
            self.gcodes.append(config.get('btn%d_gcode' % (i + 1), ''))
            self.colors.append(self._rgb565(
                config.get('btn%d_color' % (i + 1), None)))
        # Sistema de paginas / menus: la pagina 1 son los btnN_* clasicos.
        # Paginas extra: pageN_btnM_label/gcode/color en printer.cfg.
        # Un boton cuyo gcode es "PAGE:N" cambia a la pagina N (el host
        # reenvia los 6 hb_label y el TFT se redibuja solo).
        self.pages = {1: list(zip(self.labels, self.gcodes, self.colors))}
        self.page = 1
        options = config.get_prefix_options('page')
        extra_pages = set()
        for opt in options:
            try:
                extra_pages.add(int(opt.split('_')[0][4:]))
            except ValueError:
                pass
        for p in sorted(extra_pages):
            entries = []
            for i in range(6):
                entries.append((
                    config.get('page%d_btn%d_label' % (p, i + 1),
                               'BTN%d' % (i + 1)),
                    config.get('page%d_btn%d_gcode' % (p, i + 1), ''),
                    self._rgb565(config.get(
                        'page%d_btn%d_color' % (p, i + 1), None))))
            self.pages[p] = entries
        # Calibracion touch (mapeo crudo->pantalla hecho aqui, en host)
        self.xpt_x_min = config.getint('xpt_x_min', 150)
        self.xpt_x_max = config.getint('xpt_x_max', 1850)
        self.xpt_y_min = config.getint('xpt_y_min', 220)
        self.xpt_y_max = config.getint('xpt_y_max', 1830)
        self.flip_x = config.getboolean('flip_x', False)
        self.flip_y = config.getboolean('flip_y', True)
        # Grilla de botones (3 columnas x 2 filas), misma del firmware
        self.cols = config.getint('cols', 3, minval=1, maxval=4)
        self.rows = config.getint('rows', 2, minval=1, maxval=3)
        self.mcu = None
        self.cmd_label = None
        self._resp_wrapper = None
        self.touch_debug = False
        # Resolucion del panel (MKS TFT35 ILI9488, landscape)
        self.tft_w = config.getint('tft_width', 480)
        self.tft_h = config.getint('tft_height', 320)
        # Dynamic labels: templates like "HEAT {extruder.temperature}"
        # and the PAUSE_BUTTON auto pause/resume button
        self.big_font = config.getboolean('big_font', False)
        # Per-button big font override: btnN_big_font (page 1) and
        # pageP_btnQ_big_font (extra pages)
        self.big_override = {}
        for i in range(6):
            if config.getboolean('btn%d_big_font' % (i + 1), None) is not None:
                self.big_override[(1, i)] = config.getboolean(
                    'btn%d_big_font' % (i + 1))
        for p in range(2, 100):
            for i in range(6):
                v = config.getboolean('page%d_btn%d_big_font' % (p, i + 1),
                                      None)
                if v is not None:
                    self.big_override[(p, i)] = v
        self._template_re = re.compile(r'\{([a-zA-Z_0-9.]+)\}')
        self._paused = False
        self._update_timer = None
        self.printer.register_event_handler('klippy:ready', self._handle_ready)
        self.printer.register_event_handler(
            'klippy:disconnect', self._handle_disconnect)
        self.gcode.register_command(
            'HBTFT_TOUCH_DEBUG', self._cmd_touch_debug,
            desc='Loguea toques del TFT con coordenadas crudas y mapeadas')
        self.gcode.register_command(
            'HBTFT_PAGE', self._cmd_hbtft_page,
            desc='Switch the TFT button page (PAGE=N)')
        self.cmd_label2 = None

    def _register_response(self):
        # Estrategia: registrar DIRECTO en serialhdl (mcu._serial), que es
        # el nivel mas bajo y estable — es exactamente lo que hace klipper
        # internamente. El wrapper mcu.register_serial_response puede dejar
        # un placeholder que se traga mensajes si el callback post-init no
        # llega a disparar.
        serial = getattr(self.mcu, '_serial', None)
        if serial is not None and hasattr(serial, 'register_response'):
            serial.register_response(self._handle_button, 'hb_button')
            logging.info('hbtft_buttons: registrado directo en '
                         'mcu._serial.register_response')
            return True
        reg = getattr(self.mcu, 'register_serial_response', None)
        if reg is not None:
            self._resp_wrapper = reg(
                self._handle_button, 'hb_button btn=%u x=%u y=%u')
            logging.info('hbtft_buttons: registrado via '
                         'mcu.register_serial_response')
            return True
        for owner in ('serial',):
            r = getattr(getattr(self.mcu, owner, None),
                        'register_response', None)
            if r is not None:
                r(self._handle_button, 'hb_button')
                logging.info('hbtft_buttons: registrado via mcu.%s.'
                             'register_response', owner)
                return True
        logging.warning('hbtft_buttons: NINGUNA ruta de registro disponible')
        return False

    def _pixel_x(self, raw):
        v = max(self.xpt_x_min, min(self.xpt_x_max, raw))
        px = (v - self.xpt_x_min) * self.tft_w // (
            self.xpt_x_max - self.xpt_x_min)
        return self.tft_w - px if self.flip_x else px

    def _pixel_y(self, raw):
        v = max(self.xpt_y_min, min(self.xpt_y_max, raw))
        py = (v - self.xpt_y_min) * self.tft_h // (
            self.xpt_y_max - self.xpt_y_min)
        return self.tft_h - py if self.flip_y else py

    def _cmd_touch_debug(self, gcmd):
        self.touch_debug = not self.touch_debug
        gcmd.respond_info('hbtft touch debug: %s' % (
            'ON' if self.touch_debug else 'OFF'))

    def _render(self, label):
        # Resuelve plantillas: "HEAT {extruder.temperature}" -> "HEAT 205"
        # Sintaxis: {objeto.campo} con objeto = klippy printer object
        # (extruder, heater_bed, ...) y campo = clave de get_status()
        def repl(m):
            parts = m.group(1).split('.', 1)
            try:
                obj = self.printer.lookup_object(parts[0])
                val = obj.get_status(self.reactor.monotonic())[parts[1]]
                return str(int(val))
            except Exception:
                return '??'
        return self._template_re.sub(repl, label)

    def _button_label(self, page, i, labels):
        label = self._render(labels[i])
        if self.pages[page][1][i].strip().upper() == 'PAUSE_BUTTON':
            label = 'RESUME' if self._paused else 'PAUSE'
        return label

    def _send_button(self, i, label, color, page=None):
        if page is None:
            page = self.page
        big = self.big_override.get((page, i), self.big_font)
        line1, _, line2 = label.partition('\n')
        line1 = line1.upper()[:16]
        line2 = line2.upper()[:16]
        if self.cmd_label2 is not None and (line2 or big):
            self.cmd_label2.send(
                [i, 1 if big else 0] + self._pack(line1)
                + self._pack(line2) + [color])
        else:
            self.cmd_label.send([i] + self._pack(line1) + [color])

    def _send_labels(self):
        # Envia los 6 labels/gcodes/colores de la pagina actual al MCU
        # (el firmware redibuja cada boton al recibir hb_label)
        labels, gcodes, colors = self.pages[self.page]
        for i, label in enumerate(labels):
            self._send_button(i, self._button_label(self.page, i, labels),
                              colors[i])
        logging.info('hbtft_buttons: page %d sent (%s)', self.page,
                     ', '.join(self._button_label(self.page, i, labels)
                               for i in range(6)))

    def _update_dynamic(self, eventtime):
        # Refresco periodico: botones con plantilla y el PAUSE_BUTTON
        try:
            pr = self.printer.lookup_object('pause_resume', None)
            paused = bool(pr.get_status(eventtime).get('is_paused')) \
                if pr is not None else False
            if paused != self._paused:
                self._paused = paused
            labels, gcodes, colors = self.pages[self.page]
            for i in range(6):
                dyn = '{' in labels[i] \
                    or gcodes[i].strip().upper() == 'PAUSE_BUTTON'
                if dyn and self.cmd_label is not None:
                    self._send_button(
                        i, self._button_label(self.page, i, labels),
                        colors[i])
        except Exception:
            logging.exception('hbtft_buttons: dynamic update error')
        return eventtime + 1.0

    def _set_page(self, page):
        if page not in self.pages:
            logging.warning('hbtft_buttons: page %d not defined', page)
            return
        self.page = page
        if self.cmd_label is not None:
            self._send_labels()

    def _cmd_hbtft_page(self, gcmd):
        page = gcmd.get_int('PAGE', 1, minval=1, maxval=99)
        self._set_page(page)
        gcmd.respond_info('hbtft: page %d (%s)' % (
            self.page, ', '.join(self.pages[self.page][0])))

    def _handle_ready(self):
        try:
            self.mcu = self.printer.lookup_object('mcu')
            self.cmd_label = self.mcu.lookup_command(
                'hb_label idx=%u w0=%u w1=%u w2=%u w3=%u color=%u')
            try:
                self.cmd_label2 = self.mcu.lookup_command(
                    'hb_label2 idx=%u big=%u w0=%u w1=%u w2=%u w3=%u '
                    'w4=%u w5=%u w6=%u w7=%u color=%u')
            except self.printer.lookup_object('gcode').error as e:
                logging.info('hbtft_buttons: firmware sin hb_label2 '
                             '(labels de 1 linea): %s', e)
                self.cmd_label2 = None
            self.page = 1
            self._send_labels()
            # 1 s timer: refresh dynamic labels (temps, pause/resume)
            self._update_timer = self.reactor.register_timer(
                self._update_dynamic, self.reactor.monotonic() + 1.0)
            reg_ok = self._register_response()
            if not reg_ok:
                logging.warning(
                    'hbtft_buttons: no response registration API found in this '
                    'klipper version; labels are drawn but taps do not '
                    'run gcode')
        except Exception:
            logging.exception('hbtft_buttons: init error')

    @staticmethod
    def _rgb565(val):
        # formato: 0xRRGGBB (siempre). RGB565 directo ya no es aceptado
        # porque es ambiguo (ej. 0x00AA00 < 0xFFFF se confundia con 565).
        if not val:
            return 0
        v = int(str(val), 0) & 0xFFFFFF
        r, g, b = (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF
        return (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)

    @staticmethod
    def _pack(label):
        # 16 bytes -> 4 uint32 (byte bajo primero), igual que desempaqueta el MCU
        raw = label.encode('ascii', 'replace')[:16].ljust(16, b' ')
        return [int.from_bytes(raw[w*4:w*4+4], 'little') for w in range(4)]

    def _handle_disconnect(self):
        if self._update_timer is not None:
            self.reactor.unregister_timer(self._update_timer)
            self._update_timer = None
        if self._resp_wrapper is not None:
            try:
                self._resp_wrapper.unregister()
            except Exception:
                pass
            self._resp_wrapper = None
        self.mcu = None
        self.cmd_label = None

    def _raw_to_button(self, rx, ry):
        # Mapea coordenadas crudas del XPT2046 a indice de boton (0-5)
        # usando la calibracion de printer.cfg. Espejo exacto de
        # xpt_map_x/xpt_map_y del firmware, pero sin recompilar.
        def norm(v, lo, hi):
            v = max(lo, min(hi, v))
            return (v - lo) / float(hi - lo)
        nx = norm(rx, self.xpt_x_min, self.xpt_x_max)
        ny = norm(ry, self.xpt_y_min, self.xpt_y_max)
        if self.flip_x:
            nx = 1.0 - nx
        if self.flip_y:
            ny = 1.0 - ny
        col = min(self.cols - 1, int(nx * self.cols))
        row = min(self.rows - 1, int(ny * self.rows))
        return row * self.cols + col

    def _handle_button(self, params):
        try:
            logging.info('hbtft_buttons: mensaje recibido %s', params)
            if self.touch_debug:
                logging.info(
                    'hbtft_buttons: TOUCH raw=(%s,%s) -> pixel=(%s,%s) '
                    'button=%s', params.get('x'), params.get('y'),
                    self._pixel_x(params.get('x')),
                    self._pixel_y(params.get('y')),
                    self._raw_to_button(params.get('x', 0),
                                        params.get('y', 0)))
            btn = params.get('btn', 255)
            # La calibracion del host manda: si el MCU y el host no
            # concuerdan (fw sin calibrar), usamos el mapeo del host
            raw_btn = self._raw_to_button(params.get('x', 0),
                                          params.get('y', 0))
            if raw_btn != btn:
                logging.info('hbtft_buttons: MCU said btn=%s, host '
                             'calibration says btn=%s (host wins)',
                             btn, raw_btn)
                btn = raw_btn
            if btn > 5:
                return
            labels, gcodes, colors = self.pages[self.page]
            script = gcodes[btn]
            if not script:
                return
            if script.strip().upper() == 'PAUSE_BUTTON':
                script = 'RESUME' if self._paused else 'PAUSE'
            # Gcode especial: cambiar de pagina de menu
            if script.strip().upper().startswith('PAGE:'):
                try:
                    newpage = int(script.split(':', 1)[1].strip())
                except ValueError:
                    logging.warning('hbtft_buttons: invalid PAGE: %s',
                                    script)
                    return
                logging.info('hbtft_buttons: btn%d -> page %d',
                             btn + 1, newpage)
                self.reactor.register_async_callback(
                    (lambda e, p=newpage: self._set_page(p)))
                return
            logging.info('hbtft_buttons: btn%d (%s) -> %s',
                         btn + 1, labels[btn], script)
            # El callback de serie corre en el thread de fondo: agendar en
            # el reactor con register_async_callback (patron de [buttons])
            self.reactor.register_async_callback(
                (lambda e, s=script: self._run_script(s)))
        except Exception:
            logging.exception('hbtft_buttons: error in button event')

    def _run_script(self, script):
        try:
            self.gcode.run_script(script)
        except Exception:
            logging.exception('hbtft_buttons: error ejecutando gcode')


def load_config(config):
    return HBTFTButtons(config)
