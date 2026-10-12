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
        self.mcu = None
        self.cmd_label = None
        self._resp_wrapper = None
        self.printer.register_event_handler('klippy:ready', self._handle_ready)
        self.printer.register_event_handler(
            'klippy:disconnect', self._handle_disconnect)

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

    def _handle_button(self, params):
        try:
            logging.info('hbtft_buttons: mensaje recibido %s', params)
            btn = params.get('btn', 255)
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
