# Botones del TFT MKS de la Robin Nano (fork klipper-hellbot)
#
# Configuracion en printer.cfg:
#
# [hbtft_buttons]
# btn1_label: HOME
# btn1_gcode: G28
# btn2_label: UNLOAD
# btn2_gcode: M702
# ...
#
# Los labels (max 16 caracteres, mayusculas) se dibujan en los botones
# del TFT; al tocar un boton se ejecuta su gcode.
#
# Calibracion del touch (TODO en host, sin recompilar el firmware):
# el MCU reporta coordenadas crudas del XPT2046 en el mensaje hb_button,
# y este modulo las mapea con estos parametros:
#
#   xpt_x_min / xpt_x_max : rango ADC crudo del eje X (tocar extremos y
#       anotar los valores que aparecen en klippy.log: "mensaje recibido")
#   xpt_y_min / xpt_y_max : idem eje Y
#   flip_x / flip_y       : 1 si el eje va invertido
#
# Con el rango mal calibrado los botones igual funcionan si los tocas
# cerca del centro del boton; ajusta min/max hasta que los bordes
# respondan bien.
import logging


class HBTFTButtons:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.gcode = self.printer.lookup_object('gcode')
        self.reactor = self.printer.get_reactor()
        self.labels = []
        self.gcodes = []
        self.colors = []
        for i in range(6):
            self.labels.append(config.get(
                'btn%d_label' % (i + 1), 'BTN%d' % (i + 1)).upper()[:16])
            self.gcodes.append(config.get('btn%d_gcode' % (i + 1), ''))
            self.colors.append(self._rgb565(
                config.get('btn%d_color' % (i + 1), None)))
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
        self.printer.register_event_handler('klippy:ready', self._handle_ready)
        self.printer.register_event_handler(
            'klippy:disconnect', self._handle_disconnect)
        self.gcode.register_command(
            'HBTFT_TOUCH_DEBUG', self._cmd_touch_debug,
            desc='Loguea toques del TFT con coordenadas crudas y mapeadas')

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

    def _handle_ready(self):
        try:
            self.mcu = self.printer.lookup_object('mcu')
            self.cmd_label = self.mcu.lookup_command(
                'hb_label idx=%u w0=%u w1=%u w2=%u w3=%u color=%u')
            for i, label in enumerate(self.labels):
                self.cmd_label.send(
                    [i] + self._pack(label) + [self.colors[i]])
            reg_ok = self._register_response()
            if not reg_ok:
                logging.warning(
                    'hbtft_buttons: no encontre la API de respuestas en esta '
                    'version de klipper; los labels se dibujan pero los '
                    'toques no ejecutan gcode')
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
                    'boton=%s', params.get('x'), params.get('y'),
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
                logging.info('hbtft_buttons: MCU dijo btn=%s, calibracion '
                             'host dice btn=%s (se usa la del host)',
                             btn, raw_btn)
                btn = raw_btn
            if btn > 5:
                return
            script = self.gcodes[btn]
            if not script:
                return
            logging.info('hbtft_buttons: btn%d (%s) -> %s',
                         btn + 1, self.labels[btn], script)
            # El callback de serie corre en el thread de fondo: agendar en
            # el reactor con register_async_callback (patron de [buttons])
            self.reactor.register_async_callback(
                (lambda e, s=script: self._run_script(s)))
        except Exception:
            logging.exception('hbtft_buttons: error en evento de boton')

    def _run_script(self, script):
        try:
            self.gcode.run_script(script)
        except Exception:
            logging.exception('hbtft_buttons: error ejecutando gcode')


def load_config(config):
    return HBTFTButtons(config)
